// MeloTTS 真实推理引擎实现：ONNX Runtime C API 编码器 + RKNN C API 解码器。
//
// 该文件只在 NEXWEAVE_ENABLE_MELOTTS=ON 时参与构建，默认 WSL 回归不依赖
// ONNX Runtime 或 RKNN。所有厂商类型都留在这里，公共头文件与 Session 不可见。
#include "melotts_real_engines.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "onnxruntime_c_api.h"
#include "rknn_api.h"

namespace nexweave::backend {
namespace {

domain::OperationResult ort_status(const OrtApi* api,
                                   OrtStatus* status,
                                   const std::string& stage) {
  if (status == nullptr) {
    return domain::OperationResult::success();
  }
  const char* message = api == nullptr ? nullptr : api->GetErrorMessage(status);
  std::string detail = stage;
  if (message != nullptr && message[0] != '\0') {
    detail += ": ";
    detail += message;
  }
  if (api != nullptr) {
    api->ReleaseStatus(status);
  }
  return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure, detail);
}

bool readable_file(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

domain::ErrorCode rknn_error_code(int result) {
  switch (result) {
    case RKNN_ERR_TIMEOUT:
      return domain::ErrorCode::kTimeout;
    case RKNN_ERR_DEVICE_UNAVAILABLE:
    case RKNN_ERR_DEVICE_UNMATCH:
    case RKNN_ERR_TARGET_PLATFORM_UNMATCH:
      return domain::ErrorCode::kDeviceFailure;
    case RKNN_ERR_PARAM_INVALID:
    case RKNN_ERR_INPUT_INVALID:
      return domain::ErrorCode::kInvalidInput;
    default:
      return domain::ErrorCode::kBackendFailure;
  }
}

domain::OperationResult rknn_failure(int result, const char* stage) {
  return domain::OperationResult::failure(
      rknn_error_code(result),
      std::string("RKNN ") + stage + " 失败，错误码 " + std::to_string(result));
}

std::size_t tensor_elements(const rknn_tensor_attr& attr) {
  return static_cast<std::size_t>(attr.n_elems);
}

}  // namespace

struct MeloOrtEncoder::Impl {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSessionOptions* options = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* memory_info = nullptr;
  std::array<std::size_t, 8> input_index{};
  std::array<std::size_t, 3> output_index{};

  ~Impl() {
    if (memory_info != nullptr && api != nullptr) {
      api->ReleaseMemoryInfo(memory_info);
    }
    if (session != nullptr && api != nullptr) {
      api->ReleaseSession(session);
    }
    if (options != nullptr && api != nullptr) {
      api->ReleaseSessionOptions(options);
    }
    if (env != nullptr && api != nullptr) {
      api->ReleaseEnv(env);
    }
  }
};

MeloOrtEncoder::MeloOrtEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MeloOrtEncoder::~MeloOrtEncoder() = default;

domain::Result<std::unique_ptr<MeloOrtEncoder>> MeloOrtEncoder::create(
    const MeloOrtEncoderConfig& config) {
  if (!readable_file(config.model_path)) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 编码器 ONNX 文件不可读");
  }
  if (config.intra_op_num_threads <= 0) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS ONNX intra-op 线程数必须为正");
  }

  const OrtApiBase* base = OrtGetApiBase();
  if (base == nullptr) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        domain::ErrorCode::kBackendFailure, "ONNX Runtime API 不可用");
  }
  const OrtApi* api = base->GetApi(ORT_API_VERSION);
  if (api == nullptr) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        domain::ErrorCode::kBackendFailure, "ONNX Runtime API 版本不兼容");
  }

  auto impl = std::make_unique<Impl>();
  impl->api = api;

  OrtStatus* status = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "nexweave-melotts", &impl->env);
  auto checked = ort_status(api, status, "CreateEnv");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  status = api->CreateSessionOptions(&impl->options);
  checked = ort_status(api, status, "CreateSessionOptions");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  status = api->SetIntraOpNumThreads(impl->options, config.intra_op_num_threads);
  checked = ort_status(api, status, "SetIntraOpNumThreads");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  status = api->SetSessionGraphOptimizationLevel(impl->options, ORT_ENABLE_ALL);
  checked = ort_status(api, status, "SetSessionGraphOptimizationLevel");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  status = api->CreateSession(impl->env, config.model_path.c_str(), impl->options,
                              &impl->session);
  checked = ort_status(api, status, "CreateSession");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }

  std::size_t input_count = 0;
  std::size_t output_count = 0;
  status = api->SessionGetInputCount(impl->session, &input_count);
  checked = ort_status(api, status, "SessionGetInputCount");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  status = api->SessionGetOutputCount(impl->session, &output_count);
  checked = ort_status(api, status, "SessionGetOutputCount");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }
  if (input_count != 8U || output_count != 3U) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS ONNX 编码器输入/输出数量不符合约定");
  }

  OrtAllocator* allocator = nullptr;
  status = api->GetAllocatorWithDefaultOptions(&allocator);
  checked = ort_status(api, status, "GetAllocatorWithDefaultOptions");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }

  const std::array<const char*, 8> expected_inputs = {
      "phone", "tone", "language", "g", "noise_scale",
      "noise_scale_w", "length_scale", "sdp_ratio"};
  const std::array<const char*, 3> expected_outputs = {
      "z_p", "pronoun_lens", "audio_len"};
  std::array<bool, 8> found_inputs{};
  std::array<bool, 3> found_outputs{};
  for (std::size_t index = 0; index < input_count; ++index) {
    char* raw_name = nullptr;
    OrtStatus* name_status =
        api->SessionGetInputName(impl->session, index, allocator, &raw_name);
    auto name_checked = ort_status(api, name_status, "SessionGetInputName");
    if (!name_checked.ok()) {
      return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
          name_checked.error.code, name_checked.error.message);
    }
    const std::string name = raw_name == nullptr ? std::string() : std::string(raw_name);
    if (raw_name != nullptr) {
      OrtStatus* free_status = api->AllocatorFree(allocator, raw_name);
      auto free_checked = ort_status(api, free_status, "AllocatorFree input name");
      if (!free_checked.ok()) {
        return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
            free_checked.error.code, free_checked.error.message);
      }
    }
    for (std::size_t expected = 0; expected < expected_inputs.size(); ++expected) {
      if (name == expected_inputs[expected]) {
        impl->input_index[expected] = index;
        found_inputs[expected] = true;
      }
    }
  }
  for (std::size_t index = 0; index < output_count; ++index) {
    char* raw_name = nullptr;
    OrtStatus* name_status =
        api->SessionGetOutputName(impl->session, index, allocator, &raw_name);
    auto name_checked = ort_status(api, name_status, "SessionGetOutputName");
    if (!name_checked.ok()) {
      return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
          name_checked.error.code, name_checked.error.message);
    }
    const std::string name = raw_name == nullptr ? std::string() : std::string(raw_name);
    if (raw_name != nullptr) {
      OrtStatus* free_status = api->AllocatorFree(allocator, raw_name);
      auto free_checked = ort_status(api, free_status, "AllocatorFree output name");
      if (!free_checked.ok()) {
        return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
            free_checked.error.code, free_checked.error.message);
      }
    }
    for (std::size_t expected = 0; expected < expected_outputs.size(); ++expected) {
      if (name == expected_outputs[expected]) {
        impl->output_index[expected] = index;
        found_outputs[expected] = true;
      }
    }
  }
  for (const bool found : found_inputs) {
    if (!found) {
      return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS ONNX 编码器缺少约定输入");
    }
  }
  for (const bool found : found_outputs) {
    if (!found) {
      return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS ONNX 编码器缺少约定输出");
    }
  }
  status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl->memory_info);
  checked = ort_status(api, status, "CreateCpuMemoryInfo");
  if (!checked.ok()) {
    return domain::Result<std::unique_ptr<MeloOrtEncoder>>::failure(
        checked.error.code, checked.error.message);
  }

  return domain::Result<std::unique_ptr<MeloOrtEncoder>>::success(
      std::unique_ptr<MeloOrtEncoder>(new MeloOrtEncoder(std::move(impl))));
}

domain::OperationResult MeloOrtEncoder::run(const MeloEncoderRequest& request,
                                            MeloEncoderResponse& response) {
  if (impl_ == nullptr || impl_->api == nullptr || impl_->session == nullptr) {
    return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                            "MeloTTS ONNX 编码器未初始化");
  }
  if (request.phones == nullptr || request.tones == nullptr ||
      request.languages == nullptr || request.g == nullptr ||
      request.phone_count == 0U || request.phone_count != request.tone_count ||
      request.phone_count != request.language_count || request.g_count != 256U) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 编码器输入张量不合法");
  }

  const std::int64_t phone_count = static_cast<std::int64_t>(request.phone_count);
  const std::array<std::int64_t, 1> phone_dims = {phone_count};
  const std::array<std::int64_t, 3> g_dims = {1, 256, 1};
  const std::array<std::int64_t, 1> scalar_dims = {1};
  const float noise_scale = request.noise_scale;
  const float noise_scale_w = request.noise_scale_w;
  const float length_scale = request.length_scale;
  const float sdp_ratio = request.sdp_ratio;

  const std::array<const char*, 8> input_names = {
      "phone", "tone", "language", "g", "noise_scale",
      "noise_scale_w", "length_scale", "sdp_ratio"};
  const std::array<const char*, 3> output_names = {"z_p", "pronoun_lens", "audio_len"};
  std::array<OrtValue*, 8> input_values{};
  std::array<OrtValue*, 3> output_values{};

  const auto release_inputs = [&]() {
    for (OrtValue* value : input_values) {
      if (value != nullptr) {
        impl_->api->ReleaseValue(value);
      }
    }
  };
  const auto release_outputs = [&]() {
    for (OrtValue* value : output_values) {
      if (value != nullptr) {
        impl_->api->ReleaseValue(value);
      }
    }
  };

  OrtStatus* status = impl_->api->CreateTensorWithDataAsOrtValue(
      impl_->memory_info, const_cast<std::int32_t*>(request.phones),
      static_cast<std::size_t>(phone_count) * sizeof(std::int32_t), phone_dims.data(),
      phone_dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &input_values[0]);
  auto checked = ort_status(impl_->api, status, "CreateTensor phone");
  if (!checked.ok()) {
    release_inputs();
    return checked;
  }
  status = impl_->api->CreateTensorWithDataAsOrtValue(
      impl_->memory_info, const_cast<std::int32_t*>(request.tones),
      static_cast<std::size_t>(phone_count) * sizeof(std::int32_t), phone_dims.data(),
      phone_dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &input_values[1]);
  checked = ort_status(impl_->api, status, "CreateTensor tone");
  if (!checked.ok()) {
    release_inputs();
    return checked;
  }
  status = impl_->api->CreateTensorWithDataAsOrtValue(
      impl_->memory_info, const_cast<std::int32_t*>(request.languages),
      static_cast<std::size_t>(phone_count) * sizeof(std::int32_t), phone_dims.data(),
      phone_dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &input_values[2]);
  checked = ort_status(impl_->api, status, "CreateTensor language");
  if (!checked.ok()) {
    release_inputs();
    return checked;
  }
  status = impl_->api->CreateTensorWithDataAsOrtValue(
      impl_->memory_info, const_cast<float*>(request.g),
      static_cast<std::size_t>(request.g_count) * sizeof(float), g_dims.data(),
      g_dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_values[3]);
  checked = ort_status(impl_->api, status, "CreateTensor g");
  if (!checked.ok()) {
    release_inputs();
    return checked;
  }
  const std::array<float, 4> scalars = {noise_scale, noise_scale_w, length_scale, sdp_ratio};
  for (std::size_t index = 0; index < scalars.size(); ++index) {
    status = impl_->api->CreateTensorWithDataAsOrtValue(
        impl_->memory_info, const_cast<float*>(&scalars[index]), sizeof(float),
        scalar_dims.data(), scalar_dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_values[4 + index]);
    checked = ort_status(impl_->api, status, "CreateTensor scalar");
    if (!checked.ok()) {
      release_inputs();
      return checked;
    }
  }

  status = impl_->api->Run(impl_->session, nullptr, input_names.data(), input_values.data(),
                           input_values.size(), output_names.data(), output_values.size(),
                           output_values.data());
  checked = ort_status(impl_->api, status, "Run");
  if (!checked.ok()) {
    release_inputs();
    release_outputs();
    return checked;
  }

  const auto fail_run = [&](domain::ErrorCode code, const std::string& message) {
    release_inputs();
    release_outputs();
    return domain::OperationResult::failure(code, message);
  };

  OrtTensorTypeAndShapeInfo* zp_info = nullptr;
  status = impl_->api->GetTensorTypeAndShape(output_values[0], &zp_info);
  checked = ort_status(impl_->api, status, "GetTensorTypeAndShape z_p");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  std::size_t zp_dims_count = 0;
  status = impl_->api->GetDimensionsCount(zp_info, &zp_dims_count);
  checked = ort_status(impl_->api, status, "GetDimensionsCount z_p");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(zp_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  if (zp_dims_count != 3U) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(zp_info);
    return fail_run(domain::ErrorCode::kBackendFailure, "MeloTTS z_p 维度不是 3");
  }
  std::array<std::int64_t, 3> zp_dims = {0, 0, 0};
  status = impl_->api->GetDimensions(zp_info, zp_dims.data(), zp_dims.size());
  checked = ort_status(impl_->api, status, "GetDimensions z_p");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(zp_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  ONNXTensorElementDataType zp_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  status = impl_->api->GetTensorElementType(zp_info, &zp_type);
  checked = ort_status(impl_->api, status, "GetTensorElementType z_p");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(zp_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  impl_->api->ReleaseTensorTypeAndShapeInfo(zp_info);
  if (zp_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || zp_dims[0] != 1 ||
      zp_dims[1] <= 0 || zp_dims[2] <= 0) {
    return fail_run(domain::ErrorCode::kBackendFailure, "MeloTTS z_p 类型/形状不支持");
  }
  response.channels = static_cast<std::size_t>(zp_dims[1]);
  response.frames = static_cast<std::size_t>(zp_dims[2]);
  response.z_p.resize(response.channels * response.frames);
  void* zp_data = nullptr;
  status = impl_->api->GetTensorMutableData(output_values[0], &zp_data);
  checked = ort_status(impl_->api, status, "GetTensorMutableData z_p");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  std::memcpy(response.z_p.data(), zp_data, response.z_p.size() * sizeof(float));

  OrtTensorTypeAndShapeInfo* lens_info = nullptr;
  status = impl_->api->GetTensorTypeAndShape(output_values[1], &lens_info);
  checked = ort_status(impl_->api, status, "GetTensorTypeAndShape pronoun_lens");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  ONNXTensorElementDataType lens_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  status = impl_->api->GetTensorElementType(lens_info, &lens_type);
  checked = ort_status(impl_->api, status, "GetTensorElementType pronoun_lens");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(lens_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  std::size_t lens_count = 0;
  status = impl_->api->GetTensorShapeElementCount(lens_info, &lens_count);
  checked = ort_status(impl_->api, status, "GetTensorShapeElementCount pronoun_lens");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(lens_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  impl_->api->ReleaseTensorTypeAndShapeInfo(lens_info);
  if (lens_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 ||
      lens_count != request.phone_count) {
    return fail_run(domain::ErrorCode::kBackendFailure,
                    "MeloTTS pronoun_lens 类型/数量不支持");
  }
  response.phone_lengths.resize(lens_count);
  void* lens_data = nullptr;
  status = impl_->api->GetTensorMutableData(output_values[1], &lens_data);
  checked = ort_status(impl_->api, status, "GetTensorMutableData pronoun_lens");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  std::memcpy(response.phone_lengths.data(), lens_data,
              response.phone_lengths.size() * sizeof(std::int32_t));

  OrtTensorTypeAndShapeInfo* audio_info = nullptr;
  status = impl_->api->GetTensorTypeAndShape(output_values[2], &audio_info);
  checked = ort_status(impl_->api, status, "GetTensorTypeAndShape audio_len");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  ONNXTensorElementDataType audio_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  status = impl_->api->GetTensorElementType(audio_info, &audio_type);
  checked = ort_status(impl_->api, status, "GetTensorElementType audio_len");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(audio_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  std::size_t audio_count = 0;
  status = impl_->api->GetTensorShapeElementCount(audio_info, &audio_count);
  checked = ort_status(impl_->api, status, "GetTensorShapeElementCount audio_len");
  if (!checked.ok()) {
    impl_->api->ReleaseTensorTypeAndShapeInfo(audio_info);
    return fail_run(checked.error.code, checked.error.message);
  }
  impl_->api->ReleaseTensorTypeAndShapeInfo(audio_info);
  if (audio_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 || audio_count != 1U) {
    return fail_run(domain::ErrorCode::kBackendFailure,
                    "MeloTTS audio_len 类型/数量不支持");
  }
  void* audio_data = nullptr;
  status = impl_->api->GetTensorMutableData(output_values[2], &audio_data);
  checked = ort_status(impl_->api, status, "GetTensorMutableData audio_len");
  if (!checked.ok()) {
    return fail_run(checked.error.code, checked.error.message);
  }
  response.audio_len_samples = *static_cast<std::int32_t*>(audio_data);

  release_inputs();
  release_outputs();
  return domain::OperationResult::success();
}

struct MeloRknnDecoder::Impl {
  rknn_context context = 0;
  rknn_input_output_num io_num = {};
  std::array<rknn_tensor_attr, 2> input_attrs{};
  rknn_tensor_attr output_attr = {};
  std::vector<rknn_input> inputs;
  std::vector<rknn_output> outputs;
  std::vector<float> z_input;
  std::vector<float> g_input;
  std::vector<float> audio_output;
  MeloDecoderInfo info;
  std::uint32_t run_timeout_ms = 0;
  bool outputs_acquired = false;

  ~Impl() {
    if (outputs_acquired && context != 0 && !outputs.empty()) {
      (void)rknn_outputs_release(context, io_num.n_output, outputs.data());
      outputs_acquired = false;
    }
    if (context != 0) {
      (void)rknn_destroy(context);
    }
  }
};

MeloRknnDecoder::MeloRknnDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MeloRknnDecoder::~MeloRknnDecoder() = default;

domain::Result<std::unique_ptr<MeloRknnDecoder>> MeloRknnDecoder::create(
    const MeloRknnDecoderConfig& config) {
  if (!readable_file(config.model_path)) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 解码器 RKNN 文件不可读");
  }
  if (config.run_timeout_ms >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS RKNN 超时配置溢出");
  }

  auto impl = std::make_unique<Impl>();
  impl->run_timeout_ms = config.run_timeout_ms;
  const int init_result = rknn_init(&impl->context,
                                    const_cast<char*>(config.model_path.c_str()), 0, 0,
                                    nullptr);
  if (init_result != RKNN_SUCC) {
    impl->context = 0;
    const domain::Error error = rknn_failure(init_result, "init").error;
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        error.code, error.message);
  }

  int result = rknn_query(impl->context, RKNN_QUERY_IN_OUT_NUM, &impl->io_num,
                          sizeof(impl->io_num));
  if (result != RKNN_SUCC) {
    const domain::Error error = rknn_failure(result, "query_in_out_num").error;
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        error.code, error.message);
  }
  if (impl->io_num.n_input != 2U || impl->io_num.n_output != 1U) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS RKNN 解码器输入/输出数量不符合约定");
  }
  for (std::size_t index = 0; index < impl->input_attrs.size(); ++index) {
    impl->input_attrs[index] = {};
    impl->input_attrs[index].index = static_cast<std::uint32_t>(index);
    result = rknn_query(impl->context, RKNN_QUERY_INPUT_ATTR, &impl->input_attrs[index],
                        sizeof(rknn_tensor_attr));
    if (result != RKNN_SUCC) {
      const domain::Error error = rknn_failure(result, "query_input_attr").error;
      return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
          error.code, error.message);
    }
  }
  impl->output_attr = {};
  impl->output_attr.index = 0;
  result = rknn_query(impl->context, RKNN_QUERY_OUTPUT_ATTR, &impl->output_attr,
                      sizeof(rknn_tensor_attr));
  if (result != RKNN_SUCC) {
    const domain::Error error = rknn_failure(result, "query_output_attr").error;
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        error.code, error.message);
  }

  const rknn_tensor_attr& z_attr = impl->input_attrs[0];
  const rknn_tensor_attr& g_attr = impl->input_attrs[1];
  if (z_attr.n_dims != 3U || z_attr.dims[0] != 1 || z_attr.dims[1] <= 0 ||
      z_attr.dims[2] <= 0 || g_attr.n_dims != 3U || g_attr.dims[0] != 1 ||
      g_attr.dims[1] != 256 || g_attr.dims[2] != 1) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS RKNN 解码器输入形状不支持");
  }
  if (impl->output_attr.n_elems <= 0) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS RKNN 解码器输出为空");
  }
  const std::size_t output_elements = tensor_elements(impl->output_attr);
  const std::size_t samples_per_frame = output_elements / static_cast<std::size_t>(z_attr.dims[2]);
  if (samples_per_frame == 0U ||
      samples_per_frame * static_cast<std::size_t>(z_attr.dims[2]) != output_elements) {
    return domain::Result<std::unique_ptr<MeloRknnDecoder>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS RKNN 解码器输出形状不支持");
  }
  impl->info.channels_per_frame = static_cast<std::size_t>(z_attr.dims[1]);
  impl->info.frames_per_call = static_cast<std::size_t>(z_attr.dims[2]);
  impl->info.samples_per_frame = samples_per_frame;

  impl->z_input.assign(impl->info.channels_per_frame * impl->info.frames_per_call, 0.0F);
  impl->g_input.assign(256U, 0.0F);
  impl->audio_output.assign(output_elements, 0.0F);

  impl->inputs.resize(2);
  impl->inputs[0] = {};
  impl->inputs[0].index = 0;
  impl->inputs[0].buf = impl->z_input.data();
  impl->inputs[0].size = static_cast<std::uint32_t>(impl->z_input.size() * sizeof(float));
  impl->inputs[0].type = RKNN_TENSOR_FLOAT32;
  impl->inputs[0].fmt = z_attr.fmt;
  impl->inputs[0].pass_through = 0;

  impl->inputs[1] = {};
  impl->inputs[1].index = 1;
  impl->inputs[1].buf = impl->g_input.data();
  impl->inputs[1].size = static_cast<std::uint32_t>(impl->g_input.size() * sizeof(float));
  impl->inputs[1].type = RKNN_TENSOR_FLOAT32;
  impl->inputs[1].fmt = g_attr.fmt;
  impl->inputs[1].pass_through = 0;

  impl->outputs.resize(1);
  impl->outputs[0] = {};
  impl->outputs[0].index = 0;
  impl->outputs[0].want_float = 1;
  impl->outputs[0].is_prealloc = 1;
  impl->outputs[0].buf = impl->audio_output.data();
  impl->outputs[0].size = static_cast<std::uint32_t>(
      impl->audio_output.size() * sizeof(float));

  return domain::Result<std::unique_ptr<MeloRknnDecoder>>::success(
      std::unique_ptr<MeloRknnDecoder>(new MeloRknnDecoder(std::move(impl))));
}

const MeloDecoderInfo& MeloRknnDecoder::info() const noexcept {
  return impl_->info;
}

domain::OperationResult MeloRknnDecoder::decode(const float* z_p,
                                                std::size_t total_z_frames,
                                                std::size_t frame_offset,
                                                std::size_t frame_count,
                                                std::vector<float>& audio) {
  if (impl_ == nullptr || impl_->context == 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                            "MeloTTS RKNN 解码器未初始化");
  }
  if (z_p == nullptr || frame_count == 0U ||
      frame_count > impl_->info.frames_per_call ||
      frame_offset + frame_count > total_z_frames) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS RKNN 解码切片参数非法");
  }

  std::fill(impl_->z_input.begin(), impl_->z_input.end(), 0.0F);
  const std::size_t channels = impl_->info.channels_per_frame;
  const std::size_t decoder_frames = impl_->info.frames_per_call;
  for (std::size_t channel = 0; channel < channels; ++channel) {
    const float* source = z_p + channel * total_z_frames + frame_offset;
    float* destination = impl_->z_input.data() + channel * decoder_frames;
    std::copy_n(source, frame_count, destination);
  }
  impl_->inputs[0].buf = impl_->z_input.data();
  impl_->inputs[0].size = static_cast<std::uint32_t>(impl_->z_input.size() * sizeof(float));

  int result = rknn_inputs_set(impl_->context, impl_->io_num.n_input, impl_->inputs.data());
  if (result != RKNN_SUCC) {
    return rknn_failure(result, "inputs_set");
  }
  rknn_run_extend extend = {};
  extend.timeout_ms = static_cast<std::int32_t>(impl_->run_timeout_ms);
  result = rknn_run(impl_->context, &extend);
  if (result != RKNN_SUCC) {
    return rknn_failure(result, "run");
  }
  result = rknn_outputs_get(impl_->context, impl_->io_num.n_output, impl_->outputs.data(),
                            nullptr);
  if (result != RKNN_SUCC) {
    return rknn_failure(result, "outputs_get");
  }
  impl_->outputs_acquired = true;
  const std::size_t samples = frame_count * impl_->info.samples_per_frame;
  audio.assign(impl_->audio_output.begin(),
               impl_->audio_output.begin() + static_cast<std::ptrdiff_t>(samples));
  result = rknn_outputs_release(impl_->context, impl_->io_num.n_output,
                                impl_->outputs.data());
  if (result != RKNN_SUCC) {
    return rknn_failure(result, "outputs_release");
  }
  impl_->outputs_acquired = false;
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend
