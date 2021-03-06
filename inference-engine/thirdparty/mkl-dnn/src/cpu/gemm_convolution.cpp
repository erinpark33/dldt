/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "mkldnn_types.h"

#include "c_types_map.hpp"
#include "gemm_convolution.hpp"
#include "utils.hpp"
#include "type_helpers.hpp"
#include "mkldnn_thread.hpp"
#include "ref_eltwise.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::status;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::memory_tracking::names;
using namespace mkldnn::impl::utils;

void gemm_convolution_fwd_t::execute_forward() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto weights = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto bias = reinterpret_cast<const data_t *>(this->input_memory(2));
    auto dst = reinterpret_cast<data_t*>(this->memory());

    auto col = scratchpad().get<data_t>(key_conv_gemm_col);

    const auto &jcp = this->pd()->jcp_;
    const int MB = pd()->MB();

    const memory_desc_wrapper src_d(pd()->src_pd());
    const memory_desc_wrapper dst_d(pd()->dst_pd());

    const int M = jcp.os * jcp.od;
    const size_t src_step = (src_d.blk_off(1) - src_d.off_l(0)) / jcp.ngroups;
    const size_t dst_step = (dst_d.blk_off(1) - dst_d.off_l(0)) / jcp.ngroups;
    const size_t weights_g_size = jcp.ic * jcp.oc * jcp.ks;
    src += src_d.off_l(0);
    dst += dst_d.off_l(0);

    assert(IMPLICATION(
            jcp.id != 1, jcp.oh_block == jcp.oh && jcp.ow_block == jcp.ow));
    assert(IMPLICATION(jcp.ow_block != jcp.ow, jcp.oh_block == 1));

    const int K = jcp.ic * jcp.ks;
    const int N = jcp.oc;

    if (jcp.im2col_sz && jcp.id != 1)
        parallel_nd(jcp.im2col_sz * jcp.nthr,
                [&](ptrdiff_t i) { col[i] = (data_t)0; });

    const int nb_oh = div_up(jcp.oh, jcp.oh_block);
    const int nb_ow = div_up(jcp.ow, jcp.ow_block);
    const size_t work_amount = jcp.ngroups * MB * jcp.od * nb_oh * nb_ow;
    parallel(jcp.nthr, [&](const int ithr, const int nthr) {
        data_t *_col = col + (ptrdiff_t)ithr * jcp.im2col_sz;

        int g{ 0 }, n{ 0 }, od{ 0 }, ohb{ 0 }, owb{ 0 };
        size_t start = 0, end = 0;

        balance211(work_amount, nthr, ithr, start, end);
        nd_iterator_init(start, g, jcp.ngroups, n, MB, od, jcp.od, ohb,
                nb_oh, owb, nb_ow);
        for (size_t iwork = start; iwork < end; ++iwork) {
            int oh = ohb * jcp.oh_block;
            int ow = owb * jcp.ow_block;
            const data_t *_src = src + (n * jcp.ngroups + g) * src_step;
            const data_t *_weights = weights + g * weights_g_size;
            data_t *_dst_im = dst + (n * jcp.ngroups + g) * dst_step;
            const int h_step = nstl::min(jcp.oh_block, jcp.oh - oh);
            const int w_step = nstl::min(jcp.ow_block, jcp.ow - ow);
            if (jcp.im2col_sz) {
                if (jcp.id == 1)
                    jit_gemm_convolution_utils::im2col(
                            jcp, _src, _col, oh, h_step, ow, w_step);
                else
                    jit_gemm_convolution_utils::im2col_3d(jcp, _src, _col, od);
            }

            const data_t one = 1.0;

            const int m = h_step * w_step;
            const int LDA = jcp.im2col_sz ? m : M;
            data_t *_dst = _dst_im + od * jcp.os + oh * jcp.ow + ow;

            extended_sgemm("N", "N", &m, &N, &K, &one,
                    jcp.im2col_sz ? _col : _src + od * m, &LDA, _weights, &K,
                    &this->beta_, _dst, &M);

            data_t *d = _dst;
            const auto &p = pd()->attr()->post_ops_;
            bool need_bias = jcp.with_bias;
            if (use_fast_relu) {
                parallel_nd(jcp.oc, [&](const int oc) {
                    data_t b = need_bias ? bias[g * jcp.oc + oc] : 0;
                    data_t *d_ = d + oc * M;
                    PRAGMA_OMP_SIMD()
                    for (int oS = 0; oS < m; ++oS) {
                        d_[oS] += b;
                        if (d_[oS] < 0) d_[oS] *= fast_relu_ns;
                    }
                });

                need_bias = false;
            } else if (p.len_ > 0) {
                int eltwise_inj_idx = 0;
                int depthwise_inj_idx = 0;

                for (int i = 0; i < p.len_; i++) {
                    auto& post_op = p.entry_[i];
                    if (post_op.is_eltwise()) {
                        parallel_nd(jcp.oc, [&](const int oc) {
                            data_t b = need_bias ? bias[g * jcp.oc + oc] : 0;
                            data_t *d_ = d + oc * M;
                            PRAGMA_OMP_SIMD()
                            for (int oS = 0; oS < m; ++oS) {
                                d_[oS] += b;
                                d_[oS] = eltwise_injectors[eltwise_inj_idx]->compute_scalar(d_[oS]);
                            }
                        });

                        eltwise_inj_idx++;
                        need_bias = false;
                    } else if (post_op.is_depthwise()) {
                        auto depthwise_weights = post_op.depthwise.weights_data;
                        auto depthwise_bias = post_op.depthwise.biases_data;

                        parallel_nd(jcp.oc, [&](const int oc) {
                            data_t b = need_bias ? bias[g * jcp.oc + oc] : 0;
                            data_t *d_ = d + oc * M;
                            PRAGMA_OMP_SIMD()
                            for (int oS = 0; oS < m; ++oS) {
                                d_[oS] += b;
                                d_[oS] = depthwise_injectors[depthwise_inj_idx]->compute_scalar(d_[oS],
                                                                  depthwise_weights + g * jcp.oc + oc,
                                                                  depthwise_bias + g * jcp.oc + oc);
                            }
                        });

                        depthwise_inj_idx++;
                        need_bias = false;
                    }
                }
            }

            if (need_bias) {
                parallel_nd(jcp.oc, [&](const int oc) {
                    data_t b = bias[g * jcp.oc + oc];
                    data_t *d_ = d + oc * M;
                    PRAGMA_OMP_SIMD()
                    for (int oS = 0; oS < m; ++oS) {
                        d_[oS] += b;
                    }
                });
            }

            nd_iterator_step(g, jcp.ngroups, n, MB, od, jcp.od, ohb, nb_oh,
                    owb, nb_ow);
        }
    });
}

void gemm_convolution_bwd_data_t::execute_backward_data() const {
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto weights = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t*>(this->memory());

    auto col = scratchpad().get<data_t>(key_conv_gemm_col);

    const auto &jcp = this->pd()->jcp_;
    const int MB = pd()->MB();

    const int M = jcp.os * jcp.od;
    const size_t src_step_to_clean = jcp.ic * jcp.ih * jcp.iw * jcp.id;
    const memory_desc_wrapper diff_src_d(pd()->diff_src_pd());
    const memory_desc_wrapper diff_dst_d(pd()->diff_dst_pd());
    const size_t src_step = diff_src_d.blk_off(1) / jcp.ngroups;
    const size_t dst_step = diff_dst_d.blk_off(1) / jcp.ngroups;
    const size_t weights_g_size = jcp.ic * jcp.oc * jcp.ks;

    const int m = jcp.os;
    const int K = jcp.oc;
    const int N = jcp.ic * jcp.ks;
    const int LDC = jcp.im2col_sz ? m : M;

    const size_t work_amount = (size_t)jcp.ngroups * MB;

    if (jcp.id > 1) {
        for (size_t j = 0; j < work_amount; j++) {
            int j_step = src_step * j;
            const ptrdiff_t diff_src_sz = (ptrdiff_t)(src_step_to_clean);
            parallel_nd(diff_src_sz, [&](ptrdiff_t i) { diff_src[j_step + i] = (data_t)0; });
        }
    }

    parallel(jcp.nthr, [&](const int ithr, const int nthr) {
        data_t *_col = col + (ptrdiff_t)ithr * jcp.im2col_sz;

        int g{0}, n{0};
        size_t start = 0, end = 0;
        balance211(work_amount, nthr, ithr, start, end);
        nd_iterator_init(start, g, jcp.ngroups, n, MB);
        for (size_t iwork = start; iwork < end; ++iwork) {

            data_t *_diff_src = diff_src + (n * jcp.ngroups + g) * src_step;
            const data_t *_weights = weights + g * weights_g_size;
            for (int od = 0; od < jcp.od; ++od) {
                const data_t *_diff_dst = diff_dst + (n * jcp.ngroups + g)
                    *dst_step + od * m;

                const data_t zero = 0.0, one = 1.0;
                extended_sgemm("N", "T", &m, &N, &K, &one, _diff_dst, &M,
                    _weights, &N, &zero,
                    jcp.im2col_sz ? _col:_diff_src + od * m, &LDC);

                if (jcp.im2col_sz) {
                    if (jcp.id == 1)
                        jit_gemm_convolution_utils::col2im(jcp, _col,
                            _diff_src);
                    else
                        jit_gemm_convolution_utils::col2im_3d(jcp, _col,
                            _diff_src, od);
                }
            }
            nd_iterator_step(g, jcp.ngroups, n, MB);
        }
    });
}

void gemm_convolution_bwd_weights_t::execute_backward_weights() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_weights = reinterpret_cast<data_t*>(this->memory(0));
    auto diff_bias = reinterpret_cast<data_t *>(this->memory(1));

    auto col = scratchpad().get<data_t>(key_conv_gemm_col);
    auto wei_reduction = scratchpad().get<data_t>(key_conv_wei_reduction);

    const jit_gemm_conv_conf_t &jcp = this->pd()->jcp_;

    const int K = jcp.os * jcp.od;
    const size_t src_step = jcp.ic * jcp.ih * jcp.iw * jcp.id;
    const size_t dst_step = jcp.oc * K;
    const size_t weights_g_size = jcp.ic * jcp.oc * jcp.ks;

    const int k = jcp.os;
    const int N = jcp.oc;
    const int M = jcp.ic * jcp.ks;
    const int LDA = jcp.im2col_sz ? k : K;

    parallel_nd(jcp.im2col_sz * jcp.nthr,
            [&](ptrdiff_t i) { col[i] = (data_t)0; });

    parallel(jcp.nthr, [&](const int ithr, const int nthr) {
        int ithr_g, nthr_g, ithr_mb, nthr_mb;
        size_t g_start{0}, g_end{0}, mb_start{0}, mb_end{0};

        const int mb_for_balance = jcp.need_wei_reduction ? jcp.mb : 1;
        jit_gemm_convolution_utils::bwd_weights_balance(ithr, nthr, jcp.ngroups,
                mb_for_balance, ithr_g, nthr_g, ithr_mb, nthr_mb);

        assert(IMPLICATION(!jcp.need_wei_reduction, nthr_mb == 1));
        const int need_reduction = nthr_mb != 1;

        if (ithr_g != -1 && ithr_mb != -1) {
            balance211((size_t)jcp.ngroups, nthr_g, ithr_g, g_start, g_end);
            balance211((size_t)jcp.mb, nthr_mb, ithr_mb, mb_start, mb_end);

            assert(IMPLICATION((g_end - g_start) > 1, need_reduction == 0));

            data_t *_col = col + (ptrdiff_t)ithr * jcp.im2col_sz;
            data_t *weights_reduce_base = wei_reduction
                    + ithr_g * nthr_mb * weights_g_size;
            data_t *weights_reduce = weights_reduce_base
                    + ithr_mb * weights_g_size;

            for (size_t g = g_start; g < g_end; ++g) {
                data_t *_diff_weights = need_reduction
                        ? weights_reduce : (diff_weights + g * weights_g_size);
                for (size_t mb = mb_start; mb < mb_end; ++mb) {
                    const data_t *_src = src + (mb*jcp.ngroups+g)*src_step;
                    for (int od = 0; od < jcp.od; ++od) {
                    const data_t *_diff_dst = diff_dst
                            + (mb*jcp.ngroups+g)*dst_step + od * k;

                    if (jcp.im2col_sz) {
                        if (jcp.id == 1)
                            jit_gemm_convolution_utils::im2col(
                                    jcp, _src, _col, 0, jcp.oh, 0, jcp.ow);
                        else
                            jit_gemm_convolution_utils::im2col_3d(jcp, _src,
                                _col, od);
                    }

                    const data_t zero = 0.0, one = 1.0;
                    extended_sgemm(
                        "T", "N", &M, &N, &k, &one,
                        jcp.im2col_sz ? _col : _src + od * k,
                        &LDA, _diff_dst, &K,
                        mb == mb_start && od == 0 ? &zero : &one,
                        _diff_weights, &M);
                    }
                }
            }
            if (need_reduction) {
                mkldnn_thr_barrier();
                data_t *weights_base = diff_weights + g_start * weights_g_size;
                jit_gemm_convolution_utils::bwd_weights_reduction_par(
                    ithr_mb, nthr_mb, jcp, weights_reduce_base, weights_base);
            }
        } else
            if (need_reduction) { mkldnn_thr_barrier(); }
    });

    if (jcp.with_bias) {
        parallel_nd(jcp.ngroups, jcp.oc, [&](int g, int oc) {
            data_t db = 0;
            size_t offset_ = (size_t)g * dst_step + (size_t)oc * K;
            for (int mb = 0; mb < jcp.mb; ++mb)
            {
                size_t offset = offset_ + (size_t)mb * jcp.ngroups * dst_step;
                for (int od = 0; od < jcp.od; ++od)
                for (int oh = 0; oh < jcp.oh; ++oh)
                PRAGMA_OMP_SIMD(reduction(+:db))
                for (int ow = 0; ow < jcp.ow; ++ow) {
                    db += diff_dst[offset];
                    offset++;
                }
            }
            diff_bias[g*jcp.oc+oc] = db;
        });
    }
}

}
}
}
