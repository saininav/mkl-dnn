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

#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"

#include "mkldnn.hpp"

namespace mkldnn {

struct sum_test_params {
    std::vector<memory::format_tag> srcs_format;
    memory::format_tag dst_format;
    memory::dims dims;
    std::vector<float> scale;
    bool is_output_omitted;
    bool expect_to_fail;
    mkldnn_status_t expected_status;
};


template <typename data_t, typename acc_t>
class sum_test: public ::testing::TestWithParam<sum_test_params> {
    void check_data(const std::vector<memory> &srcs,
                    const std::vector<float> scale,
                    const memory &dst)
    {
        auto dst_data = map_memory<const data_t>(dst);
        const auto &dst_d = dst.get_desc();
        const auto dst_dims = dst_d.data.dims;
        const mkldnn::impl::memory_desc_wrapper dst_mdw(dst_d.data);

        std::vector<mapped_ptr_t<const data_t>> mapped_srcs;
        for (auto &src: srcs)
            mapped_srcs.emplace_back(map_memory<const data_t>(src));

        mkldnn::impl::parallel_nd(
            dst_dims[0], dst_dims[1], dst_dims[2], dst_dims[3],
            [&](memory::dim n, memory::dim c, memory::dim h, memory::dim w) {
            if (is_current_test_failed())
                return;

            acc_t src_sum = 0.0;
            for (size_t num = 0; num < srcs.size(); num++) {
                auto &src_data = mapped_srcs[num];
                const auto &src_d = srcs[num].get_desc();
                const auto src_dims = src_d.data.dims;
                const mkldnn::impl::memory_desc_wrapper src_mdw(src_d.data);

                auto src_idx = w
                    + src_dims[3]*h
                    + src_dims[2]*src_dims[3]*c
                    + src_dims[1]*src_dims[2]*src_dims[3]*n;
                if (num == 0) {
                    src_sum = data_t(scale[num])
                            * src_data[src_mdw.off_l(src_idx, false)];
                } else {
                    src_sum += data_t(scale[num])
                            * src_data[src_mdw.off_l(src_idx, false)];
                }

                src_sum = std::max(std::min(src_sum,
                            std::numeric_limits<acc_t>::max()),
                        std::numeric_limits<acc_t>::lowest());

            }

            auto dst_idx = w
                + dst_dims[3]*h
                + dst_dims[2]*dst_dims[3]*c
                + dst_dims[1]*dst_dims[2]*dst_dims[3]*n;

            auto dst_val = dst_mdw.off_l(dst_idx, false);

            ASSERT_EQ(src_sum, dst_data[dst_val]);
            }
        );
    }

protected:
    virtual void SetUp() {
        sum_test_params p
            = ::testing::TestWithParam<sum_test_params>::GetParam();
        catch_expected_failures([=](){Test();}, p.expect_to_fail,
                    p.expected_status);
    }

    void Test() {
        sum_test_params p
            = ::testing::TestWithParam<sum_test_params>::GetParam();

        const auto num_srcs = p.srcs_format.size();

        auto eng = engine(get_test_engine_kind(), 0);
        auto strm = stream(eng);

        memory::data_type data_type = data_traits<data_t>::data_type;

        std::vector<memory::desc> srcs_md;
        std::vector<memory> srcs;

        for (size_t i = 0; i < num_srcs; i++) {
            auto desc = memory::desc(p.dims, data_type, p.srcs_format[i]);
            auto src_memory = memory(desc, eng);
            const size_t sz =
                src_memory.get_desc().get_size() / sizeof(data_t);
            fill_data<data_t>(sz, src_memory);

            // Keep few mantissa digits for fp types to avoid round-off errors
            // With proper scalars the computations give exact results
            if (!std::is_integral<data_t>::value) {
                using uint_type = typename data_traits<data_t>::uint_type;
                int mant_digits
                        = mkldnn::impl::nstl::numeric_limits<data_t>::digits;
                int want_mant_digits = 3;
                auto src_ptr = map_memory<data_t>(src_memory);
                for (size_t i = 0; i < sz; i++) {
                    uint_type mask = (uint_type)-1
                            << (mant_digits - want_mant_digits);
                    *((uint_type *)&src_ptr[i]) &= mask;
                }
            }

            srcs_md.push_back(desc);
            srcs.push_back(src_memory);
        }

        memory dst;
        sum::primitive_desc sum_pd;

        if (p.is_output_omitted) {
            ASSERT_NO_THROW(sum_pd = sum::primitive_desc(p.scale, srcs_md, eng));
        } else {
            auto dst_desc = memory::desc(p.dims, data_type, p.dst_format);
            sum_pd = sum::primitive_desc(dst_desc, p.scale, srcs_md, eng);

            ASSERT_EQ(sum_pd.dst_desc().data.ndims, dst_desc.data.ndims);
        }
        ASSERT_NO_THROW(dst = memory(sum_pd.dst_desc(), eng));

        {
            auto dst_data = map_memory<data_t>(dst);
            const size_t sz =
                dst.get_desc().get_size() / sizeof(data_t);
            // overwriting dst to prevent false positives for test cases.
            mkldnn::impl::parallel_nd((ptrdiff_t)sz,
                [&](ptrdiff_t i) { dst_data[i] = -32; }
            );
        }

        sum c(sum_pd);
        std::unordered_map<int, memory> args = {
            {MKLDNN_ARG_DST, dst}};
        for (int i = 0; i < (int)num_srcs; i++) {
            args.insert({MKLDNN_ARG_MULTIPLE_SRC + i, srcs[i]});
        }
        c.execute(strm, args);
        strm.wait();

        check_data(srcs, p.scale, dst);
    }
};

/* corner cases */
#define CASE_CC(ifmt0, ifmt1, ofmt, dims_, ef, st) \
    sum_test_params{\
        {memory::format_tag::ifmt0, memory::format_tag::ifmt1}, memory::format_tag::ofmt, \
        memory::dims dims_, {1.0f, 1.0f}, 0, ef, st}

static auto simple_test_cases = [](bool omit_output) { return ::testing::Values(
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {0, 7, 4, 4}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {1, 0, 4, 4}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {1, 8, 0, 4}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {-1, 8, 4, 4}, {1.0f, 1.0f}, omit_output, true, mkldnn_invalid_arguments},
 
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {1, 1024, 38, 50}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nchw}, memory::format_tag::nchw,
    {2, 8, 2, 2}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nChw8c, memory::format_tag::nChw8c}, memory::format_tag::nChw8c,
    {2, 16, 3, 4}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nchw}, memory::format_tag::nChw8c,
    {2, 16, 2, 2}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nChw8c, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {2, 16, 3, 4}, {1.0f, 1.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nchw}, memory::format_tag::nchw,
    {2, 8, 2, 2}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nChw8c, memory::format_tag::nChw8c}, memory::format_tag::nChw8c,
    {2, 16, 3, 4}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nchw}, memory::format_tag::nChw8c,
    {2, 16, 2, 2}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nChw8c, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {2, 16, 3, 4}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {5, 8, 3, 3}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw,
    {32, 32, 13, 14}, {2.0f, 3.0f}, omit_output},
    sum_test_params{
    {memory::format_tag::nChw16c, memory::format_tag::nChw8c},
    memory::format_tag::nChw16c,
    {2, 16, 3, 3}, {2.0f, 3.0f}, omit_output});
};

#define CPU_INST_TEST_CASE(test, omit_output) \
CPU_TEST_P(test, TestsSum) {} \
CPU_INSTANTIATE_TEST_SUITE_P(TestSum, test, simple_test_cases(omit_output)); \
\
CPU_INSTANTIATE_TEST_SUITE_P(TestSumEF, test, ::testing::Values( \
    sum_test_params{\
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw, \
    {1, 8, 4 ,4}, {1.0f}, 0, true, mkldnn_invalid_arguments}, \
    sum_test_params{\
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw, \
    {2, 8, 4 ,4}, {0.1f}, 0, true, mkldnn_invalid_arguments} \
));

#define GPU_INST_TEST_CASE(test, omit_output) \
GPU_TEST_P(test, TestsSum) {} \
GPU_INSTANTIATE_TEST_SUITE_P(TestSum, test, simple_test_cases(omit_output)); \
\
GPU_INSTANTIATE_TEST_SUITE_P(TestSumEF, test, ::testing::Values( \
    sum_test_params{\
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw, \
    {1, 8, 4 ,4}, {1.0f}, 0, true, mkldnn_invalid_arguments}, \
    sum_test_params{\
    {memory::format_tag::nchw, memory::format_tag::nChw8c}, memory::format_tag::nchw, \
    {2, 8, 4 ,4}, {0.1f}, 0, true, mkldnn_invalid_arguments} \
));

#define INST_TEST_CASE(test, omit_output) \
    CPU_INST_TEST_CASE(test, omit_output) \
    GPU_INST_TEST_CASE(test, omit_output)

using sum_test_float_omit_output = sum_test<float,float>;
using sum_test_u8_omit_output = sum_test<uint8_t,float>;
using sum_test_s8_omit_output = sum_test<int8_t,float>;
using sum_test_s32_omit_output = sum_test<int32_t,float>;
using sum_test_f16_omit_output = sum_test<float16_t,float>;

using sum_test_float = sum_test<float,float>;
using sum_test_u8 = sum_test<uint8_t,float>;
using sum_test_s8 = sum_test<int8_t,float>;
using sum_test_s32 = sum_test<int32_t,float>;
using sum_test_f16 = sum_test<float16_t,float>;

using sum_cc_f32 = sum_test<float,float>;
TEST_P(sum_cc_f32, TestSumCornerCases) {}
INSTANTIATE_TEST_SUITE_P(TestSumCornerCases, sum_cc_f32, ::testing::Values(
    CASE_CC(nchw, nChw8c, nchw, ({0, 7, 4, 4}), false, mkldnn_success),
    CASE_CC(nchw, nChw8c, nchw, ({1, 0, 4, 4}), false, mkldnn_success),
    CASE_CC(nchw, nChw8c, nchw, ({1, 8, 0, 4}), false, mkldnn_success),
    CASE_CC(nchw, nChw8c, nchw, ({-1, 8, 4, 4}), true, mkldnn_invalid_arguments)
    ));
#undef CASE_CC

INST_TEST_CASE(sum_test_float_omit_output, 1)
INST_TEST_CASE(sum_test_u8_omit_output, 1)
INST_TEST_CASE(sum_test_s8_omit_output, 1)
INST_TEST_CASE(sum_test_s32_omit_output, 1)
GPU_INST_TEST_CASE(sum_test_f16_omit_output, 1)

INST_TEST_CASE(sum_test_float, 0)
INST_TEST_CASE(sum_test_u8, 0)
INST_TEST_CASE(sum_test_s8, 0)
INST_TEST_CASE(sum_test_s32, 0)
GPU_INST_TEST_CASE(sum_test_f16, 0)

#undef CPU_INST_TEST_CASE
#undef GPU_INST_TEST_CASE
}
