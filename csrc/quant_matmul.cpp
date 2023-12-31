#include <torch/extension.h>
#include <torch/torch.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <vector>

#include "cutlass/numeric_types.h"
#include "cutlass/integer_subbyte.h"

#include "tensorrt_llm/kernels/cutlass_kernels/cutlass_preprocessors.h"
#include "tensorrt_llm/kernels/weightOnlyBatchedGemv/kernelLauncher.h"
#include "tensorrt_llm/kernels/cutlass_kernels/fpA_intB_gemm/fpA_intB_gemm.h"

#define CHECK_SHAPE(x, ...) TORCH_CHECK(x.sizes() == torch::IntArrayRef({__VA_ARGS__}), #x " must have shape (" #__VA_ARGS__ ")")

#define BOOL_SWITCH(COND, CONST_NAME, ...)                                           \
    [&] {                                                                            \
        if (COND) {                                                                  \
            static constexpr bool CONST_NAME = true;                                 \
            return __VA_ARGS__();                                                    \
        } else {                                                                     \
            static constexpr bool CONST_NAME = false;                                \
            return __VA_ARGS__();                                                    \
        }                                                                            \
    }()

template <typename WeightType, cutlass::WeightOnlyQuantOp QuantOp>
void dispatch_to_weight_only_batched_gemv(const half* A, const WeightType* B, const half* weight_scales,
    const half* bias, half* C, int m, int n, int k, int group_size, cudaStream_t stream)
{
    using namespace tensorrt_llm::kernels;

    // Convert WeightType
    WeightOnlyQuantType weight_only_quant_type
        = std::is_same_v<WeightType, cutlass::uint4b_t> ? WeightOnlyQuantType::Int4b : WeightOnlyQuantType::Int8b;

    // Convert QuantType
    WeightOnlyType weight_only_type = QuantOp == cutlass::WeightOnlyQuantOp::PER_COLUMN_SCALE_ONLY
        ? WeightOnlyType::PerChannel
        : WeightOnlyType::GroupWise;

    // https://github.com/NVIDIA/TensorRT-LLM/blob/d37b507f41a87457fe9f10f7459d08f5db235745/cpp/tensorrt_llm/plugins/weightOnlyQuantMatmulPlugin/weightOnlyQuantMatmulPlugin.cpp#L322
    // https://github.com/NVIDIA/TensorRT-LLM/blob/d37b507f41a87457fe9f10f7459d08f5db235745/cpp/tensorrt_llm/plugins/weightOnlyGroupwiseQuantMatmulPlugin/weightOnlyGroupwiseQuantMatmulPlugin.cpp#L363
    WeightOnlyParams params{
        /*qweight=*/reinterpret_cast<const uint8_t*>(B),
        /*scales=*/reinterpret_cast<const ::half*>(weight_scales),
        /*zeros=*/nullptr,
        /*in=*/reinterpret_cast<const ::half*>(A),
        /*act_scale=*/nullptr,
        /*bias=*/reinterpret_cast<const ::half*>(bias),
        /*out=*/reinterpret_cast<::half*>(C),
        m,
        n,
        k,
        group_size,
        weight_only_quant_type,
        weight_only_type,
        WeightOnlyActivationFunctionType::Identity,
        WeightOnlyActivationType::FP16
    };

    weight_only_batched_gemv_launcher(params, stream);
}

template <typename WeightType, cutlass::WeightOnlyQuantOp QuantOp>
void gemm_fp16_int_bias(const half* A, const WeightType* B, const half* weight_scales, const half* bias, half* C,
    int m, int n, int k, int group_size, char* workspace_ptr, size_t workspace_bytes, cudaStream_t stream)
{
    if (m <= 4) {
        dispatch_to_weight_only_batched_gemv<WeightType, QuantOp>(A, B, weight_scales, bias, C, m, n, k,
            group_size, stream);
    } else {
        using namespace tensorrt_llm::kernels::cutlass_kernels;
        CutlassFpAIntBGemmRunner<half, WeightType, QuantOp> runner;
        runner.gemm_bias(A, B, weight_scales, nullptr, bias, C, m, n, k, group_size, workspace_ptr, workspace_bytes, stream);
    }

}

at::Tensor preprocess_weight(at::Tensor quantized_weight, int bits, int arch) {

    TORCH_CHECK(bits == 4 || bits == 8);
    TORCH_CHECK(arch >= 70 && arch < 90);
    int rows = quantized_weight.size(0);
    int elts_per_byte = 8 / bits;
    int cols = quantized_weight.size(1) * elts_per_byte;

    TORCH_CHECK(quantized_weight.dtype() == torch::kInt8);
    TORCH_CHECK(quantized_weight.is_cpu());
    TORCH_CHECK(quantized_weight.is_contiguous());
    CHECK_SHAPE(quantized_weight, rows, cols / elts_per_byte);

    auto opts = quantized_weight.options();
    auto out = at::empty({cols, rows / elts_per_byte}, opts);

    using namespace tensorrt_llm::kernels::cutlass_kernels;
    QuantType qtype = bits == 4 ? QuantType::PACKED_INT4_WEIGHT_ONLY : QuantType::INT8_WEIGHT_ONLY;
    std::vector<size_t> shape{rows, cols};
    preprocess_weights_for_mixed_gemm(out.data_ptr<int8_t>(), quantized_weight.data_ptr<int8_t>(), shape, qtype);
    return out;
}

at::Tensor quant_matmul(const at::Tensor input, const at::Tensor weight, const at::Tensor weight_scales,
                             const c10::optional<at::Tensor> bias_, int bits) {

    TORCH_CHECK(bits == 4 || bits == 8);
    const int m = input.size(0);
    const int k = input.size(1);
    const int n = weight.size(0);
    const bool is_finegrained = weight_scales.dim() == 2;
    const int group_size = !is_finegrained ? k : k / weight_scales.size(0);
    TORCH_CHECK(k % group_size == 0);
    if (is_finegrained) { TORCH_CHECK(group_size == 64 || group_size == 128); }
    TORCH_CHECK(n % 8 == 0);

    TORCH_CHECK(input.dtype() == torch::kFloat16);
    TORCH_CHECK(weight.dtype() == torch::kInt8);
    TORCH_CHECK(weight_scales.dtype() == torch::kFloat16);
    TORCH_CHECK(input.is_cuda());
    TORCH_CHECK(weight.is_cuda());
    TORCH_CHECK(weight_scales.is_cuda());
    TORCH_CHECK(input.is_contiguous());
    TORCH_CHECK(weight.is_contiguous());
    TORCH_CHECK(weight_scales.is_contiguous());
    CHECK_SHAPE(input, m, k);
    CHECK_SHAPE(weight, n, k / (8 / bits));
    if (!is_finegrained) {
        CHECK_SHAPE(weight_scales, n);
    } else {
        CHECK_SHAPE(weight_scales, k / group_size, n);
    }

    if (bias_.has_value()) {
        auto bias = bias_.value();
        TORCH_CHECK(bias.dtype() == torch::kFloat16);
        TORCH_CHECK(bias.is_cuda());
        TORCH_CHECK(bias.is_contiguous());
        CHECK_SHAPE(bias, n);
    }

    // Otherwise the kernel will be launched from cuda:0 device
    // Cast to char to avoid compiler warning about narrowing
    at::cuda::CUDAGuard device_guard{(char)input.get_device()};

    // create output/workspace tensor
    auto opts = input.options();
    auto out = at::empty({m, n}, opts);
    at::Tensor workspace;
    bool has_workspace = m > 4;  // m <= 4 dispatches to batched gemv which doesn't need workspace.
    if (has_workspace) { workspace = at::empty({1 << 22}, opts.dtype(torch::kInt8)); }

    BOOL_SWITCH(bits == 4, kIs4Bits, [&] {
        using WeightType = std::conditional_t<kIs4Bits, cutlass::uint4b_t, uint8_t>;
        BOOL_SWITCH(is_finegrained, kIsFinegrained, [&] {
            static constexpr auto QuantOp = !kIsFinegrained ? cutlass::WeightOnlyQuantOp::PER_COLUMN_SCALE_ONLY : cutlass::WeightOnlyQuantOp::FINEGRAINED_SCALE_ONLY;
            gemm_fp16_int_bias<WeightType, QuantOp>(
                reinterpret_cast<half *>(input.data_ptr()),
                reinterpret_cast<WeightType *>(weight.data_ptr()),
                reinterpret_cast<half *>(weight_scales.data_ptr()),
                bias_.has_value() ? reinterpret_cast<half *>(bias_.value().data_ptr()) : nullptr,
                reinterpret_cast<half *>(out.data_ptr()),
                m,
                n,
                k,
                group_size,
                has_workspace ? reinterpret_cast<char *>(workspace.data_ptr()) : nullptr,
                has_workspace ? 1 << 22 : 0,
                at::cuda::getCurrentCUDAStream());
        });
    });

    return out;
}


PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("preprocess_weight", &preprocess_weight, "Preprocess weight");
  m.def("quant_matmul", &quant_matmul, "Quant matmul");
}