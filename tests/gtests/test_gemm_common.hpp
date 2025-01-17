/*******************************************************************************
* Copyright 2018 Intel Corporation
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

#ifndef TEST_GEMM_COMMON_H
#define TEST_GEMM_COMMON_H

#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"

#include "mkldnn_types.h"
#include "mkldnn.h"

#include <type_traits>
#include <utility>
#include <vector>

#define CONCAT_WITH_UNDERSCORE_(a,b) a ## _ ## b
#define CONCAT_WITH_UNDERSCORE(a,b) CONCAT_WITH_UNDERSCORE_(a,b)

#define INST_TEST_CASE_(str, ...) \
    INSTANTIATE_TEST_SUITE_P(str, gemm_test, ::testing::Values(__VA_ARGS__))
#define INST_TEST_CASE(str, ...) \
    INST_TEST_CASE_(             \
            CONCAT_WITH_UNDERSCORE(str, TEST_CASE_NAME_PREFIX), __VA_ARGS__)

#define CPU_INST_TEST_CASE_(str, ...) CPU_INSTANTIATE_TEST_SUITE_P( \
        str, gemm_test, ::testing::Values(__VA_ARGS__))
#define CPU_INST_TEST_CASE(str, ...) CPU_INST_TEST_CASE_( \
        CONCAT_WITH_UNDERSCORE(str,TEST_CASE_NAME_PREFIX), __VA_ARGS__)

#if MKLDNN_WITH_OPENCL

// Declare OpenCL GEMM interfaces for testing
extern "C" {
mkldnn_status_t mkldnn_ocl_sgemm(cl_command_queue queue, const char *transa,
        const char *transb, mkldnn_dim_t m, mkldnn_dim_t n, mkldnn_dim_t k,
        cl_float alpha, cl_mem a, mkldnn_dim_t offset_a, mkldnn_dim_t lda,
        cl_mem b, mkldnn_dim_t offset_b, mkldnn_dim_t ldb, cl_float beta,
        cl_mem c, mkldnn_dim_t offset_c, mkldnn_dim_t ldc);

mkldnn_status_t mkldnn_ocl_hgemm(cl_command_queue queue, const char *transa,
        const char *transb, mkldnn_dim_t m, mkldnn_dim_t n, mkldnn_dim_t k,
        cl_float alpha, cl_mem a, mkldnn_dim_t offset_a, mkldnn_dim_t lda,
        cl_mem b, mkldnn_dim_t offset_b, mkldnn_dim_t ldb, cl_float beta,
        cl_mem c, mkldnn_dim_t offset_c, mkldnn_dim_t ldc);
}
#endif

namespace mkldnn {

struct test_igemm_params {
    char offsetc;
    bool zero_oa;
    bool zero_ob;
    bool zero_oc;

    int8_t oa() const { return (int8_t)(zero_oa ? 0 : 4); }
    int8_t ob() const { return (int8_t)(zero_ob ? 0 : 3); }
};

struct gemm_offset {
    int64_t a;
    int64_t b;
    int64_t c;
};

struct test_params {
    char transA;
    char transB;
    int64_t M;
    int64_t N;
    int64_t K;
    float alpha;
    float beta;
    int64_t lda;
    int64_t ldb;
    int64_t ldc;

    test_igemm_params igemm_params;
    bool expect_to_fail;
    mkldnn_status_t expected_status;

    gemm_offset off;

    bool tr_a() const { return transA == 'T' || transA == 't'; }
    bool tr_b() const { return transB == 'T' || transB == 't'; }
    int64_t sizeC() const { return N * ldc; }

    bool oc_is_R() const
    { auto c = igemm_params.offsetc; return c == 'R' || c == 'r'; }
    bool oc_is_C() const
    { auto c = igemm_params.offsetc; return c == 'C' || c == 'c'; }
    int64_t size_oc() const { return oc_is_R() ? N : oc_is_C() ? M : 1; }
};

template <typename... TArgs>
inline test_params make_test_params_with_offset(
        const gemm_offset &off, TArgs &&... args) {
    test_params params{ std::forward<TArgs>(args)... };
    params.off = off;
    return params;
}

/* Test implementation description.
 *
 * To reduce the time spent in GEMM validation the test matrices A, B, and C
 * are generated from sub-matrices (A', B', and C') of smaller size:
 * - A(M, K) <-> A'(M_test, K)
 * - B(K, N) <-> B'(K, N_test)
 * - C(M, N) <-> C'(M_test, N_test)
 *
 * The matrices A', B', and C' are generated randomly. Then:
 * - A(m, k) := A'(mapper_m[m], k),
 * - B(k, n) := B'(k, mapper_n[n]),
 * - C(m, n) := C'(mapper_m[m], mapper_n[n]);
 *
 * Here `mapper_x[]` is surjection of {0, ..., X-1} onto {0, ..., X_test-1}.
 * For simplicity mapper_x[x] = x, for x in {0, ..., X_test-1}.
 *
 * This technique allows reducing the complexity of the validation code from
 * O(M*N*K) to O(M_test * N_test * K).
 *
 * X_test := min(X, X_test_max), where X_test_max is prime number around 50.
 *
 * To make the test robust the surjective functions mapper_m and mapper_n
 * should randomly map the elements {X_test, ..., X-1} onto {0, ..., X_test-1}.
 *
 * The validation itself looks as follows:
 * 0.  Prepare mapper_m and mapper_n
 * 1.a Generate random matrices A', B', C'
 * 1.b Prepare matrices A, B, C based on A', B', and C' respectively
 * 2.  Compute C_calc := Op(M, N, K, A, B, C)
 * 3.  Compute C'_ref := Op_REF(M_test, N_test, K, A', B', C')
 * 4.  Expand C'_ref to C_ref, by applying mapper_m and mapper_n
 * 5.  Compare C_calc and C_ref
 */

const int M_test_max = 47;
const int N_test_max = 53;

/** Mapper:
 * a surjective function from {0, ..., dim-1} onto {0, ..., dim_test-1}.
 */
struct mapper_t {
    mapper_t(int64_t dim, int64_t dim_test_max,
            int64_t gen = 7, int64_t gen_start = 13)
        : dim_(dim), dim_test_(std::min(dim, dim_test_max))
        , gen_(gen), gen_start_(gen_start)
        , mapper_(dim)
    {
        for (int64_t d = 0; d < dim_test_; ++d) mapper_[d] = d;
        for (int64_t g = gen_start_ % dim_test_, d = dim_test_; d < dim_; ++d) {
            mapper_[d] = mapper_[g];
            g = g * gen_ % dim_test_;
        }
    }

    int64_t dim() const { return dim_; }
    int64_t dim_test() const { return dim_test_; }
    int64_t operator[](int64_t d) const { return mapper_[d]; }

  private:
    const int64_t dim_;
    const int64_t dim_test_;
    const int64_t gen_, gen_start_;
    std::vector<int64_t> mapper_;
};

enum class layout_t { ROW_MAJOR, COL_MAJOR };

/** Prepares matrix A or B according to the dimension mapper.
 * The K dimension is always assumed to be columns, hence:
 * - A layout = A_is_transposed ? ROW_MAJOR : COL_MAJOR
 * - B layout = B_is_transposed ? COL_MAJOR : ROW_MAJOR
 */
template <typename data_t>
void prepare_matrix(const test_memory &M_mem, int64_t off_beg, layout_t layout, int64_t R,
        int64_t C, int64_t LD, const mapper_t &mapper) {
    auto M = map_memory<data_t>(M_mem);
    auto dt = data_traits<data_t>::data_type;
    bool is_fp = (dt == memory::data_type::f16 || dt == memory::data_type::f32);
    const data_t mean = (data_t)(is_fp ? 1.f : 4);
    const data_t var = (data_t)(is_fp ? 2e-1f : 3);

    ASSERT_EQ(R, mapper.dim());
    const int R_test = mapper.dim_test();

    if (layout == layout_t::COL_MAJOR) {
        mkldnn::impl::parallel_nd(C, R_test, [&](int64_t c, int64_t r) {
            const int64_t off = c * LD + r;
            M[off_beg + off] = set_value<data_t>(off, mean, var, 1.);
        });
        if (R > R_test) {
            const int64_t R_rest = R - R_test;
            mkldnn::impl::parallel_nd(C, R_rest, [&](int64_t c, int64_t r_) {
                const int64_t r = R_test + r_;
                const int64_t off = c * LD + r;
                const int64_t off0 = c * LD + mapper[r];
                M[off_beg + off] = M[off_beg + off0];
            });
        }
    } else {
        mkldnn::impl::parallel_nd(R_test, C, [&](int64_t r, int64_t c) {
            const int64_t off = r * LD + c;
            M[off_beg + off] = set_value<data_t>(off, mean, var, 1.);
        });
        if (R > R_test) {
            const int64_t R_rest = R - R_test;
            mkldnn::impl::parallel_nd(R_rest, C, [&](int64_t r_, int64_t c) {
                const int64_t r = R_test + r_;
                const int64_t off = r * LD + c;
                const int64_t off0 = mapper[r] * LD + c;
                M[off_beg + off] = M[off_beg + off0];
            });
        }
    }
}

/** Extends columns of the matrix M according to the mapper_c */
template <typename data_t>
void extend_matrix_cols(const test_memory &M_mem, int64_t off, int64_t R, int64_t C,
        int64_t LD, const mapper_t &mapper_c) {
    auto M = map_memory<data_t>(M_mem);
    ASSERT_EQ(C, mapper_c.dim());
    const int64_t C_test = mapper_c.dim_test();
    if (C_test == C) return;

    mkldnn::impl::parallel_nd(C - C_test, [&](int64_t c_) {
        const int64_t c = C_test + c_;
        const int64_t c0 = mapper_c[c];
        for (int64_t r = 0; r < R; ++r)
            M[off + c * LD + r] = M[off + c0 * LD + r];
    });
}

/** Extends rows of the matrix M according to the mapper_r */
template <typename data_t>
void extend_matrix_rows(const test_memory &M_mem, int64_t off, int64_t R, int64_t C,
        int64_t LD, const mapper_t &mapper_r) {
    auto M = map_memory<data_t>(M_mem);
    ASSERT_EQ(R, mapper_r.dim());
    const int64_t R_test = mapper_r.dim_test();
    if (R_test == R) return;

    mkldnn::impl::parallel_nd(C, R - R_test, [&](int64_t c, int64_t r_) {
        const int64_t r = R_test + r_;
        const int64_t r0 = mapper_r[r];
        M[off + c * LD + r] = M[off + c * LD + r0];
    });
}

/** Extends matrix M according to the mapper_r and mapper_c */
template <typename data_t>
void extend_matrix(const test_memory &M_mem, int64_t off, int64_t R, int64_t C, int64_t LD,
        const mapper_t &mapper_r, const mapper_t &mapper_c) {
    ASSERT_EQ(R, mapper_r.dim());
    ASSERT_EQ(C, mapper_c.dim());
    extend_matrix_rows<data_t>(M_mem, off, R, C, LD, mapper_r);
    extend_matrix_cols<data_t>(M_mem, off, R, C, LD, mapper_c);
}

template <typename a_dt, typename b_dt, typename c_dt>
struct ref_gemm {
    static void call(const test_params &p, int64_t M, int64_t N,
            const test_memory &a_mem, const test_memory &b_mem,
            const test_memory &c_mem, const test_memory &) {
        auto a = map_memory<a_dt>(a_mem);
        auto b = map_memory<b_dt>(b_mem);
        auto c = map_memory<c_dt>(c_mem);

        const bool tr_a = p.transA && (p.transA == 'T' || p.transA == 't');
        const bool tr_b = p.transB && (p.transB == 'T' || p.transB == 't');

        auto pa = [&](int64_t i, int64_t j) { return a[p.off.a + j * p.lda + i]; };
        auto pb = [&](int64_t i, int64_t j) { return b[p.off.b + j * p.ldb + i]; };
        auto pc = [&](int64_t i, int64_t j) { return c[p.off.c + j * p.ldc + i]; };

        mkldnn::impl::parallel_nd(M, N, [&](int64_t im, int64_t in) {
            c_dt c_elem = (p.beta == 0.) ? 0. : pc(im, in) * p.beta;

            for (int64_t ik = 0; ik < p.K; ik++) {
                const a_dt a_elem = tr_a ? pa(ik, im) : pa(im, ik);
                const b_dt b_elem = tr_b ? pb(in, ik) : pb(ik, in);
                c_elem += p.alpha * a_elem * b_elem;
            }
            c[p.off.c + in * p.ldc + im] = c_elem;
        });
    }
};

template <typename b_dt>
struct ref_gemm<int8_t, b_dt, int32_t> {
    static void call(const test_params &p, int64_t M, int64_t N,
            const test_memory &a_mem, const test_memory &b_mem,
            const test_memory &c_mem, const test_memory &oc_mem) {
        auto A = map_memory<int8_t>(a_mem);
        auto B = map_memory<b_dt>(b_mem);
        auto C = map_memory<int32_t>(c_mem);
        auto oc = map_memory<int32_t>(oc_mem);

        const bool tr_a = p.transA && (p.transA == 'T' || p.transA == 't');
        const bool tr_b = p.transB && (p.transB == 'T' || p.transB == 't');
        bool OCisR = (p.igemm_params.offsetc == 'R'
                || p.igemm_params.offsetc == 'r');
        bool OCisC = (p.igemm_params.offsetc == 'C'
                || p.igemm_params.offsetc == 'c');

        auto pa = [&](int64_t i, int64_t j) {
            return (double)A[p.off.a + j * p.lda + i];
        };
        auto pb = [&](int64_t i, int64_t j) {
            return (double)B[p.off.b + j * p.ldb + i];
        };
        auto pc = [&](int64_t i, int64_t j) {
            return (double)C[p.off.c + j * p.ldc + i];
        };

        int8_t oa = p.igemm_params.oa();
        int8_t ob = p.igemm_params.ob();

        mkldnn::impl::parallel_nd(M, N, [&](int64_t m, int64_t n) {
            double c_elem = 0;
            for (int64_t k = 0; k < p.K; k++) {
                const double a_elem = (tr_a ? pa(k, m) : pa(m, k)) + oa;
                const double b_elem = (tr_b ? pb(n, k) : pb(k, n)) + ob;
                c_elem += a_elem * b_elem;
            }

            double coffset = OCisR ? oc[n] : OCisC ? oc[m] : oc[0];
            double val = (p.beta == 0.f ? 0. : p.beta * pc(m, n))
                    + p.alpha * c_elem + coffset;
            C[p.off.c + n * p.ldc + m] = static_cast<int32_t>(
                    nearbyint(saturate<int32_t, double>(val)));
        });
    }
};

template <typename b_dt, typename c_dt>
void compare(const test_params &p, const test_memory &c_mem,
        const test_memory &c_ref_mem) {
    using data_type = memory::data_type;
    auto c = map_memory<c_dt>(c_mem);
    auto c_ref = map_memory<c_dt>(c_ref_mem);
    mkldnn::impl::parallel_nd(p.N, p.ldc, [&](int64_t i, int64_t j) {
        if (is_current_test_failed())
            return;

        c_dt ref = c_ref[p.off.c + i * p.ldc + j];
        c_dt got = c[p.off.c + i * p.ldc + j];
        c_dt diff = got - ref;

        if (data_traits<b_dt>::data_type == data_type::f16) {
            const float eps = 1e-3 * p.K;
            float e = (std::abs(ref) > eps) ? diff / ref : float(diff);
            ASSERT_NEAR(e, 0.0, eps) << "Row: " << j << " Col: " << i;
        } else if (data_traits<b_dt>::data_type == data_type::f32) {
            c_dt e = (std::abs(ref) > 1e-4) ? c_dt(diff / ref) : diff;
            ASSERT_NEAR(e, 0.0, 1e-4) << "Row: " << j << " Col: " << i;
        } else {
            // igemm
            if (p.alpha == 1.0f) {
                ASSERT_NEAR(diff, 0, 1) << "Row: " << j << " Col: " << i;
            } else {
                if (data_traits<b_dt>::data_type == data_type::u8) {
                    c_dt eps = p.K / 700 + 1;
                    ASSERT_NEAR(diff, 0, eps) << "Row: " << j << " Col: " << i;
                } else if (data_traits<b_dt>::data_type == data_type::s8) {
                    c_dt eps = p.K / 350 + 1;
                    ASSERT_NEAR(diff, 0, eps) << "Row: " << j << " Col: " << i;
                }
            }
        }
    });
}

inline void get_matrix_size(const test_params &p, size_t &sizeA,
        size_t &sizeB, size_t &sizeC) {
    const bool tr_a = (p.transA == 'T' || p.transA == 't');
    const bool tr_b = (p.transB == 'T' || p.transB == 't');
    sizeA = !tr_a ? p.lda * p.K : p.lda * p.M,
    sizeB = !tr_b ? p.ldb * p.N : p.ldb * p.K,
    sizeC = p.ldc * p.N;
}

template <typename T>
inline test_memory get_matrix_memory(memory::dim n, memory::dim off, engine &eng) {
    auto d = create_md(
            { n + off }, data_traits<T>::data_type, memory::format_tag::x);
    return test_memory(d, eng);
}

template <typename a_dt, typename b_dt, typename c_dt>
void fill_matrices(const test_params &p, const mapper_t &mapper_m,
        const mapper_t &mapper_n, const test_memory &a_mem,
        const test_memory &b_mem, const test_memory &c_mem,
        const test_memory &c_ref_mem, const test_memory &oc_mem) {
    prepare_matrix<a_dt>(a_mem, p.off.a,
            p.tr_a() ? layout_t::ROW_MAJOR : layout_t::COL_MAJOR, p.M, p.K,
            p.lda, mapper_m);
    prepare_matrix<b_dt>(b_mem, p.off.b,
            p.tr_b() ? layout_t::COL_MAJOR : layout_t::ROW_MAJOR, p.N, p.K,
            p.ldb, mapper_n);

    fill_data<c_dt>(p.off.c + p.sizeC(), c_mem.get());
    extend_matrix<c_dt>(c_mem, p.off.c, p.M, p.N, p.ldc, mapper_m, mapper_n);
    {
        auto C = map_memory<c_dt>(c_mem);
        auto C_ref = map_memory<c_dt>(c_ref_mem);
        mkldnn::impl::parallel_nd(
                p.sizeC(), [&](int64_t i) { C_ref[p.off.c + i] = C[p.off.c + i]; });
    }

    if (oc_mem.get_size() == 0)
        return;

    if (p.igemm_params.zero_oc) {
        auto oc = map_memory<c_dt>(oc_mem);
        for (int64_t i = 0; i < p.size_oc(); i++) oc[i] = 0;
    } else {
        fill_data<c_dt>(p.size_oc(), oc_mem.get(), (c_dt)1, (c_dt)0);
        if (p.oc_is_R()) {
            extend_matrix_cols<c_dt>(oc_mem, 0, 1, p.N, 1, mapper_n);
        } else if (p.oc_is_C()) {
            extend_matrix_rows<c_dt>(oc_mem, 0, p.M, 1, p.M, mapper_m);
        }
    }
}

template <typename a_dt, typename b_dt, typename c_dt>
struct mkldnn_gemm {
    static mkldnn_status_t call(test_params &p, const test_memory &a_mem,
            const test_memory &b_mem, const test_memory &c_mem) {
        throw error(mkldnn_runtime_error, "unknown gemm");
    }
};

template <>
struct mkldnn_gemm<float16_t, float16_t, float16_t> {
    static mkldnn_status_t call(const test_params &p, const test_memory &a_mem,
            const test_memory &b_mem, const test_memory &c_mem, const test_memory &) {
#if MKLDNN_WITH_OPENCL
        if (get_test_engine_kind() == engine::kind::gpu) {
            engine eng(get_test_engine_kind(), 0);
            stream s(eng);
            cl_command_queue q = s.get_ocl_command_queue();
            auto status = mkldnn_ocl_hgemm(q, &p.transA, &p.transB, p.M, p.N,
                    p.K, p.alpha, a_mem.get().get_ocl_mem_object(), p.off.a,
                    p.lda, b_mem.get().get_ocl_mem_object(), p.off.b, p.ldb,
                    p.beta, c_mem.get().get_ocl_mem_object(), p.off.c, p.ldc);
            s.wait();
            return status;
        }
#endif
        throw error(mkldnn_runtime_error, "unknown gemm");
    }
};

template <>
struct mkldnn_gemm<float, float, float> {
    static mkldnn_status_t call(const test_params &p, const test_memory &a_mem,
            const test_memory &b_mem, const test_memory &c_mem,
            const test_memory &) {
#if MKLDNN_WITH_OPENCL
        if (get_test_engine_kind() == engine::kind::gpu) {
            engine eng = a_mem.get().get_engine();
            stream s(eng);
            cl_command_queue q = s.get_ocl_command_queue();
            auto status = mkldnn_ocl_sgemm(q, &p.transA, &p.transB, p.M, p.N,
                    p.K, p.alpha, a_mem.get().get_ocl_mem_object(), p.off.a,
                    p.lda, b_mem.get().get_ocl_mem_object(), p.off.b, p.ldb,
                    p.beta, c_mem.get().get_ocl_mem_object(), p.off.c, p.ldc);
            s.wait();
            return status;
        }
#endif
        auto A = map_memory<float>(a_mem);
        auto B = map_memory<float>(b_mem);
        auto C = map_memory<float>(c_mem);
        return mkldnn_sgemm(&p.transA, &p.transB, &p.M, &p.N, &p.K, &p.alpha, A,
                &p.lda, B, &p.ldb, &p.beta, C, &p.ldc);
    }
};

template <>
struct mkldnn_gemm<int8_t, int8_t, int32_t> {
    static mkldnn_status_t call(const test_params &p, const test_memory &a_mem,
            const test_memory &b_mem, const test_memory &c_mem,
            const test_memory &oc_mem) {

        auto A = map_memory<int8_t>(a_mem);
        auto B = map_memory<int8_t>(b_mem);
        auto C = map_memory<int32_t>(c_mem);
        auto oc = map_memory<int32_t>(oc_mem);
        int8_t oa = p.igemm_params.oa();
        int8_t ob = p.igemm_params.ob();
        return mkldnn_gemm_s8s8s32(&p.transA, &p.transB,
                &p.igemm_params.offsetc, &p.M, &p.N, &p.K, &p.alpha, A, &p.lda,
                &oa, B, &p.ldb, &ob, &p.beta, C, &p.ldc, oc);
    }
};

template <>
struct mkldnn_gemm<int8_t, uint8_t, int32_t> {
    static mkldnn_status_t call(const test_params &p, const test_memory &a_mem,
            const test_memory &b_mem, const test_memory &c_mem,
            const test_memory &oc_mem) {

        auto A = map_memory<int8_t>(a_mem);
        auto B = map_memory<uint8_t>(b_mem);
        auto C = map_memory<int32_t>(c_mem);
        auto oc = map_memory<int32_t>(oc_mem);
        int8_t oa = p.igemm_params.oa();
        int8_t ob = p.igemm_params.ob();
        return mkldnn_gemm_s8u8s32(&p.transA, &p.transB,
                &p.igemm_params.offsetc, &p.M, &p.N, &p.K, &p.alpha, A, &p.lda,
                &oa, B, &p.ldb, &ob, &p.beta, C, &p.ldc, oc);
    }
};

template <typename a_dt, typename b_dt, typename c_dt>
struct run_test_gemm {
    static void call(const test_params &p) {
        if (p.expect_to_fail) {
            engine eng(get_test_engine_kind(), 0);
            test_memory zero_mem({}, eng);
            auto status = mkldnn_gemm<a_dt, b_dt, c_dt>::call(
                    p, zero_mem, zero_mem, zero_mem, zero_mem);
            if (status != mkldnn_success)
                throw error(status, "mkldnn gemm returned error");
            return;
        }

        size_t sizeA, sizeB, sizeC;
        get_matrix_size(p, sizeA, sizeB, sizeC);

        engine eng(get_test_engine_kind(), 0);
        test_memory a_mem = get_matrix_memory<a_dt>(sizeA, p.off.a, eng);
        test_memory b_mem = get_matrix_memory<b_dt>(sizeB, p.off.b, eng);
        test_memory c_mem = get_matrix_memory<c_dt>(sizeC, p.off.c, eng);
        test_memory c_ref_mem = get_matrix_memory<c_dt>(sizeC, p.off.c, eng);
        test_memory oc_mem = get_matrix_memory<c_dt>(p.size_oc(), 0, eng);

        mapper_t mapper_m(p.M, M_test_max), mapper_n(p.N, N_test_max);
        const int64_t M_test = mapper_m.dim_test();
        const int64_t N_test = mapper_n.dim_test();

        fill_matrices<a_dt, b_dt, c_dt>(
                p, mapper_m, mapper_n, a_mem, b_mem, c_mem, c_ref_mem, oc_mem);

        auto status = mkldnn_gemm<a_dt, b_dt, c_dt>::call(
                p, a_mem, b_mem, c_mem, oc_mem);

        if (status == mkldnn_success) {
            ref_gemm<a_dt, b_dt, c_dt>::call(
                    p, M_test, N_test, a_mem, b_mem, c_ref_mem, oc_mem);
            extend_matrix<c_dt>(c_ref_mem, p.off.c, p.M, p.N, p.ldc, mapper_m, mapper_n);
            compare<b_dt, c_dt>(p, c_mem, c_ref_mem);
        }

        if (status != mkldnn_success)
            throw error(status, "mkldnn gemm returned error");
    }
};

template <typename a_dt, typename b_dt, typename c_dt>
class gemm_test_common: public ::testing::TestWithParam<test_params> {
protected:
    virtual void SetUp() {
        const auto &p = ::testing::TestWithParam<test_params>::GetParam();

        bool zero_off = (p.off.a == 0 && p.off.b == 0 && p.off.c == 0);
        SKIP_IF(!zero_off && get_test_engine_kind() == engine::kind::cpu,
                "CPU does not support non-zero offsets.");

        bool is_f16 = (data_traits<c_dt>::data_type == memory::data_type::f16);
        SKIP_IF(is_f16 && get_test_engine_kind() == engine::kind::cpu,
                "CPU does not support f16 data type.");

        catch_expected_failures([=](){Test();}, p.expect_to_fail,
                    p.expected_status);
    }
    void Test() {
        const auto &p = ::testing::TestWithParam<test_params>::GetParam();
        run_test_gemm<a_dt, b_dt, c_dt>::call(p);
    }
};
}
#endif
