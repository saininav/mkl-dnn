Data Types {#dev_guide_data_types}
==================================

Intel MKL-DNN functionality supports a number of numerical
data types. IEEE single precision floating point (fp32) is considered
to be the golden standard in deep learning applications and is supported
in all the library functions. The purpose of low precision data types
support is to improve performance of compute intensive operations, such as
convolutions, inner product, and recurrent neural network cells
in comparison to fp32.

| Data type | Description
| :---      | :---
| f32       | [IEEE single precision floating point](https://en.wikipedia.org/wiki/Single-precision_floating-point_format#IEEE_754_single-precision_binary_floating-point_format:_binary32)
| bf16      | [non-IEEE 16-bit floating point](https://software.intel.com/en-us/download/bfloat16-hardware-numerics-definition)
| f16       | [IEEE half precision floating point](https://en.wikipedia.org/wiki/Half-precision_floating-point_format#IEEE_754_half-precision_binary_floating-point_format:_binary16)
| s8/u8     | signed/unsigned 8-bit integer

## Inference and Training

Intel MKL-DNN supports training and inference with the following data types:

| Usage mode | CPU                | GPU        |
| :---       | :---               | :---       |
| Inference  | f32, bf16, s8/u8   | f32, f16   |
| Training   | f32, bf16          | f32        |

Note, that using lower precision arithmetic requires changes
in the deep learning model implementation. See topics for the corresponding
data types for the details:
 * @ref dev_guide_inference_int8
   * @ref dev_guide_attributes_quantization
 * @ref dev_guide_training_bfp16

Individual primitives may have additional limitations with respect to data type
support based on the precision requirements. The list of data types supported
by each primitive is included into corresponding sections of the developer
guide.

## Hardware Limitations

While all the platforms Intel MKL-DNN supports have hardware acceleration for
fp32 arithmetics, that is not the case for other data types. Considering that
performance is the main purpose of the low precision data types support,
Intel MKL-DNN implements this functionality only for the platforms that have
hardware acceleration for these data types. The table below summarizes the
current support matrix:

| Data type | CPU                                | GPU           |
| :---      | :---                               | :---          |
| f32       | any                                | any           |
| bf16      | Intel(R) DL-Boost with bfloat16    | not supported |
| f16       | not supported                      | any           |
| s8, u8    | Intel(R) AVX512, Intel DL-Boost    | not supported |

@note
  Intel MKL-DNN can simulate the blfoat16 data type on CPUs with
  Intel(R) AVX512 BW. The performance of primitives in this case is
  approximately 3-4x times lower than the corresponding f32.
  The primary goal is to allow users to try using bfloat16 before the
  actual HW will become available.
