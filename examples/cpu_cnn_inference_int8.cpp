/*******************************************************************************
* Copyright 2018-2019 Intel Corporation
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

/// @example cpu_cnn_inference_int8.cpp
/// Annotated version: @ref cpu_cnn_inference_int8_cpp
///
/// @page cpu_cnn_inference_int8_cpp CNN int8 inference example
/// Full example text: @ref cpu_cnn_inference_int8.cpp
///
/// This C++ API example demonstrates how to run AlexNet's conv3 and relu3
/// with int8 data type.

#include "mkldnn.hpp"
#include <iostream>
#include <numeric>
#include <string>

using namespace mkldnn;

memory::dim product(const memory::dims &dims) {
    return std::accumulate(dims.begin(), dims.end(), (memory::dim)1,
            std::multiplies<memory::dim>());
}

void simple_net_int8() {
    using tag = memory::format_tag;
    using dt = memory::data_type;

    auto cpu_engine = engine(engine::kind::cpu, 0);
    stream s(cpu_engine);

    const int batch = 8;

/// Configure tensor shapes
/// @snippet cpu_cnn_inference_int8.cpp Configure tensor shapes
//[Configure tensor shapes]
    // AlexNet: conv3
    // {batch, 256, 13, 13} (x)  {384, 256, 3, 3}; -> {batch, 384, 13, 13}
    // strides: {1, 1}
    memory::dims conv_src_tz = { batch, 256, 13, 13 };
    memory::dims conv_weights_tz = { 384, 256, 3, 3 };
    memory::dims conv_bias_tz = { 384 };
    memory::dims conv_dst_tz = { batch, 384, 13, 13 };
    memory::dims conv_strides = { 1, 1 };
    memory::dims conv_padding = { 1, 1 };
//[Configure tensor shapes]

/// Next, the example configures the scales used to quantize fp32 data
/// into int8. For this example, the scaling value is chosen as an
/// arbitrary number, although in a realistic scenario, it should be
/// calculated from a set of precomputed values as previously mentioned.
/// @snippet cpu_cnn_inference_int8.cpp Choose scaling factors
//[Choose scaling factors]
    // Choose scaling factors for input, weight, output and bias quantization
    const std::vector<float> src_scales = { 1.8f };
    const std::vector<float> weight_scales = { 2.0f };
    const std::vector<float> bias_scales = { 1.0f };
    const std::vector<float> dst_scales = { 0.55f };

    // Choose channel-wise scaling factors for convolution
    std::vector<float> conv_scales(384);
    const int scales_half = 384 / 2;
    std::fill(conv_scales.begin(), conv_scales.begin() + scales_half, 0.3f);
    std::fill(conv_scales.begin() + scales_half + 1, conv_scales.end(), 0.8f);
//[Choose scaling factors]

/// The *source, weights, bias* and *destination* datasets use the single-scale
/// format with mask set to '0', while the *output* from the convolution
/// (conv_scales) will use the array format where mask = 2 corresponding
/// to the output dimension.
/// @snippet cpu_cnn_inference_int8.cpp Set scaling mask
//[Set scaling mask]
    const int src_mask = 0;
    const int weight_mask = 0;
    const int bias_mask = 0;
    const int dst_mask = 0;
    const int conv_mask = 2; // 1 << output_channel_dim
//[Set scaling mask]

    // Allocate input and output buffers for user data
    std::vector<float> user_src(batch * 256 * 13 * 13);
    std::vector<float> user_dst(batch * 384 * 13 * 13);

    // Allocate and fill buffers for weights and bias
    std::vector<float> conv_weights(product(conv_weights_tz));
    std::vector<float> conv_bias(product(conv_bias_tz));

/// Create the memory primitives for user data (source, weights, and bias).
/// The user data will be in its original 32-bit floating point format.
/// @snippet cpu_cnn_inference_int8.cpp Allocate buffers
//[Allocate buffers]
    auto user_src_memory = memory({ { conv_src_tz }, dt::f32, tag::nchw },
            cpu_engine, user_src.data());
    auto user_weights_memory
            = memory({ { conv_weights_tz }, dt::f32, tag::oihw }, cpu_engine,
                    conv_weights.data());
    auto user_bias_memory = memory({ { conv_bias_tz }, dt::f32, tag::x },
            cpu_engine, conv_bias.data());
//[Allocate buffers]

/// Create a memory descriptor for each convolution parameter.
/// The convolution data uses 8-bit integer values, so the memory
/// descriptors are configured as:
///
/// * 8-bit unsigned (u8) for source and destination.
/// * 8-bit signed (s8) for bias and weights.
///
///  > **Note**
///  > The destination type is chosen as *unsigned* because the
///  > convolution applies a ReLU operation where data results \f$\geq 0\f$.
/// @snippet cpu_cnn_inference_int8.cpp Create convolution memory descriptors
//[Create convolution memory descriptors]
    auto conv_src_md = memory::desc({ conv_src_tz }, dt::u8, tag::any);
    auto conv_bias_md = memory::desc({ conv_bias_tz }, dt::s8, tag::any);
    auto conv_weights_md = memory::desc({ conv_weights_tz }, dt::s8, tag::any);
    auto conv_dst_md = memory::desc({ conv_dst_tz }, dt::u8, tag::any);
//[Create convolution memory descriptors]

/// Create a convolution descriptor passing the int8 memory
/// descriptors as parameters.
/// @snippet cpu_cnn_inference_int8.cpp Create convolution descriptor
//[Create convolution descriptor]
    auto conv_desc = convolution_forward::desc(prop_kind::forward,
            algorithm::convolution_direct, conv_src_md, conv_weights_md, conv_bias_md,
            conv_dst_md, conv_strides, conv_padding, conv_padding);
//[Create convolution descriptor]

/// Configuring int8-specific parameters in an int8 primitive is done
/// via the Attributes Primitive. Create an attributes object for the
/// convolution and configure it accordingly.
/// @snippet cpu_cnn_inference_int8.cpp Configure scaling
//[Configure scaling]
    primitive_attr conv_attr;
    conv_attr.set_output_scales(conv_mask, conv_scales);
//[Configure scaling]

/// The ReLU layer from Alexnet is executed through the PostOps feature. Create
/// a PostOps object and configure it to execute an _eltwise relu_ operation.
/// @snippet cpu_cnn_inference_int8.cpp Configure post-ops
//[Configure post-ops]
    const float ops_scale = 1.f;
    const float ops_alpha = 0.f; // relu negative slope
    const float ops_beta = 0.f;
    post_ops ops;
    ops.append_eltwise(ops_scale, algorithm::eltwise_relu, ops_alpha, ops_beta);
    conv_attr.set_post_ops(ops);
//[Configure post-ops]

    // check if int8 convolution is supported
    try {
        auto conv_prim_desc = convolution_forward::primitive_desc(
                conv_desc, conv_attr, cpu_engine);
    } catch (error &e) {
        if (e.status == mkldnn_unimplemented) {
            std::cerr << "Intel MKL-DNN does not have int8 convolution "
            "implementation that supports this system. Please refer to "
            "the developer guide for details." << std::endl;
        }
        throw;
    }

/// Create a primitive descriptor using the convolution descriptor
/// and passing along the int8 attributes in the constructor. The primitive
/// descriptor for the convolution will contain the specific memory
/// formats for the computation.
/// @snippet cpu_cnn_inference_int8.cpp Create convolution primitive descriptor
//[Create convolution primitive descriptor]
    auto conv_prim_desc = convolution_forward::primitive_desc(
            conv_desc, conv_attr, cpu_engine);
//[Create convolution primitive descriptor]

/// Create a memory for each of the convolution's data input
/// parameters (source, bias, weights, and destination). Using the convolution
/// primitive descriptor as the creation parameter enables Intel MKL-DNN
/// to configure the memory formats for the convolution.
///
/// Scaling parameters are passed to the reorder primitive via the attributes
/// primitive.
///
/// User memory must be transformed into convolution-friendly memory
/// (for int8 and memory format). A reorder layer performs the data
/// transformation from fp32 (the original user data) into int8 format
/// (the data used for the convolution). In addition, the reorder
/// transforms the user data into the required memory format (as explained
/// in the simple_net example).
///
/// @snippet cpu_cnn_inference_int8.cpp Quantize data and weights
//[Quantize data and weights]
    auto conv_src_memory = memory(conv_prim_desc.src_desc(), cpu_engine);
    primitive_attr src_attr;
    src_attr.set_output_scales(src_mask, src_scales);
    auto src_reorder_pd = reorder::primitive_desc(cpu_engine,
            user_src_memory.get_desc(), cpu_engine,
            conv_src_memory.get_desc(), src_attr);
    auto src_reorder = reorder(src_reorder_pd);
    src_reorder.execute(s, user_src_memory, conv_src_memory);

    auto conv_weights_memory
            = memory(conv_prim_desc.weights_desc(), cpu_engine);
    primitive_attr weight_attr;
    weight_attr.set_output_scales(weight_mask, weight_scales);
    auto weight_reorder_pd = reorder::primitive_desc(cpu_engine,
            user_weights_memory.get_desc(), cpu_engine,
            conv_weights_memory.get_desc(), weight_attr);
    auto weight_reorder = reorder(weight_reorder_pd);
    weight_reorder.execute(s, user_weights_memory, conv_weights_memory);

    auto conv_bias_memory = memory(conv_prim_desc.bias_desc(), cpu_engine);
    primitive_attr bias_attr;
    bias_attr.set_output_scales(bias_mask, bias_scales);
    auto bias_reorder_pd = reorder::primitive_desc(cpu_engine,
            user_bias_memory.get_desc(), cpu_engine,
            conv_bias_memory.get_desc(), bias_attr);
    auto bias_reorder = reorder(bias_reorder_pd);
    bias_reorder.execute(s, user_bias_memory, conv_bias_memory);
//[Quantize data and weights]

    auto conv_dst_memory = memory(conv_prim_desc.dst_desc(), cpu_engine);

/// Create the convolution primitive and add it to the net. The int8 example
/// computes the same Convolution +ReLU layers from AlexNet simple-net.cpp
/// using the int8 and PostOps approach. Although performance is not
/// measured here, in practice it would require less computation time to achieve
/// similar results.
/// @snippet cpu_cnn_inference_int8.cpp Create convolution primitive
//[Create convolution primitive]
    auto conv = convolution_forward(conv_prim_desc);
    conv.execute(s,
            { { MKLDNN_ARG_SRC, conv_src_memory },
                    { MKLDNN_ARG_WEIGHTS, conv_weights_memory },
                    { MKLDNN_ARG_BIAS, conv_bias_memory },
                    { MKLDNN_ARG_DST, conv_dst_memory } });
//[Create convolution primitive]

/// @page cpu_cnn_inference_int8_cpp
/// Finally, *dst memory* may be dequantized from int8 into the original
/// fp32 format. Create a memory primitive for the user data in the original
/// 32-bit floating point format and then apply a reorder to transform the
/// computation output data.
/// @snippet cpu_cnn_inference_int8.cpp Dequantize the result
//[Dequantize the result]
    auto user_dst_memory = memory({ { conv_dst_tz }, dt::f32, tag::nchw },
            cpu_engine, user_dst.data());
    primitive_attr dst_attr;
    dst_attr.set_output_scales(dst_mask, dst_scales);
    auto dst_reorder_pd = reorder::primitive_desc(cpu_engine,
            conv_dst_memory.get_desc(), cpu_engine,
            user_dst_memory.get_desc(), dst_attr);
    auto dst_reorder = reorder(dst_reorder_pd);
    dst_reorder.execute(s, conv_dst_memory, user_dst_memory);
//[Dequantize the result]

    s.wait();
}

int main(int argc, char **argv) {
    try {
        simple_net_int8();
        std::cout << "Simple-net-int8 example passed!" << std::endl;
    } catch (error &e) {
        std::cerr << "status: " << e.status << std::endl;
        std::cerr << "message: " << e.message << std::endl;
    }
    return 0;
}
