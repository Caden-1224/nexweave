// RKNN Zipformer 引擎实现。只在本仓库开启 RKNN 适配器构建时编译；
// 依赖 rknn_api.h 与板端 librknnrt.so。本文件不接触 Session、回调或音频帧领域对象。

#include "rknn_zipformer_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#include "rknn_api.h"

namespace nexweave::backend {
namespace {

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

bool file_readable(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

std::size_t tensor_elements(const rknn_tensor_attr& attr) {
  return static_cast<std::size_t>(attr.n_elems);
}

bool tensor_is_float(const rknn_tensor_attr& attr) {
  return attr.type == RKNN_TENSOR_FLOAT16 || attr.type == RKNN_TENSOR_FLOAT32;
}

}  // namespace

// 一个 RKNN context 的 RAII 包装：加载模型、预分配输入输出、执行单次 run。
// 输入输出缓冲区由本对象拥有；release_outputs 是 outputs_get 后唯一允许释放的位置。
class RknnModel {
 public:
  RknnModel() = default;
  ~RknnModel() {
    if (outputs_acquired_) {
      (void)rknn_outputs_release(context_, io_num_.n_output, outputs_.data());
    }
    if (context_ != 0) {
      (void)rknn_destroy(context_);
    }
  }

  RknnModel(const RknnModel&) = delete;
  RknnModel& operator=(const RknnModel&) = delete;

  domain::OperationResult load(const std::string& path,
                               const std::string& role,
                               std::uint32_t timeout_ms) {
    timeout_ms_ = timeout_ms;
    if (!file_readable(path)) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kInvalidInput,
          "RKNN 模型不可读: " + role);
    }
    const int init_result =
        rknn_init(&context_, const_cast<char*>(path.c_str()), 0, 0, nullptr);
    if (init_result != RKNN_SUCC) {
      context_ = 0;
      return rknn_failure(init_result, "init");
    }

    int result = rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (result != RKNN_SUCC) {
      return rknn_failure(result, "query_in_out_num");
    }
    if (io_num_.n_input == 0 || io_num_.n_output == 0) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN 模型没有输入或输出");
    }

    input_attrs_.resize(io_num_.n_input);
    output_attrs_.resize(io_num_.n_output);
    for (std::size_t index = 0; index < input_attrs_.size(); ++index) {
      input_attrs_[index] = {};
      input_attrs_[index].index = static_cast<std::uint32_t>(index);
      result = rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[index],
                          sizeof(rknn_tensor_attr));
      if (result != RKNN_SUCC) {
        return rknn_failure(result, "query_input_attr");
      }
    }
    for (std::size_t index = 0; index < output_attrs_.size(); ++index) {
      output_attrs_[index] = {};
      output_attrs_[index].index = static_cast<std::uint32_t>(index);
      result = rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[index],
                          sizeof(rknn_tensor_attr));
      if (result != RKNN_SUCC) {
        return rknn_failure(result, "query_output_attr");
      }
    }

    inputs_.resize(io_num_.n_input);
    outputs_.resize(io_num_.n_output);
    input_float_storage_.resize(io_num_.n_input);
    input_int64_storage_.resize(io_num_.n_input);
    output_float_storage_.resize(io_num_.n_output);
    output_int64_storage_.resize(io_num_.n_output);

    for (std::size_t index = 0; index < inputs_.size(); ++index) {
      const rknn_tensor_attr& attr = input_attrs_[index];
      rknn_input& input = inputs_[index];
      input = {};
      input.index = static_cast<std::uint32_t>(index);
      input.pass_through = 0;
      input.fmt = attr.fmt;
      if (attr.type == RKNN_TENSOR_INT64) {
        input.type = RKNN_TENSOR_INT64;
        input_int64_storage_[index].assign(tensor_elements(attr), 0);
        input.size = static_cast<std::uint32_t>(
            input_int64_storage_[index].size() * sizeof(std::int64_t));
        input.buf = input_int64_storage_[index].data();
      } else if (tensor_is_float(attr)) {
        input.type = RKNN_TENSOR_FLOAT32;
        input_float_storage_[index].assign(tensor_elements(attr), 0.0F);
        input.size = static_cast<std::uint32_t>(
            input_float_storage_[index].size() * sizeof(float));
        input.buf = input_float_storage_[index].data();
      } else {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "RKNN 输入类型不受支持");
      }
    }

    for (std::size_t index = 0; index < outputs_.size(); ++index) {
      const rknn_tensor_attr& attr = output_attrs_[index];
      rknn_output& output = outputs_[index];
      output = {};
      output.index = static_cast<std::uint32_t>(index);
      output.is_prealloc = 1;
      if (attr.type == RKNN_TENSOR_INT64) {
        output.want_float = 0;
        output_int64_storage_[index].assign(tensor_elements(attr), 0);
        output.size = static_cast<std::uint32_t>(
            output_int64_storage_[index].size() * sizeof(std::int64_t));
        output.buf = output_int64_storage_[index].data();
      } else if (tensor_is_float(attr)) {
        output.want_float = 1;
        output_float_storage_[index].assign(tensor_elements(attr), 0.0F);
        output.size = static_cast<std::uint32_t>(
            output_float_storage_[index].size() * sizeof(float));
        output.buf = output_float_storage_[index].data();
      } else {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "RKNN 输出类型不受支持");
      }
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult run() {
    if (context_ == 0) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                              "RKNN context 未加载");
    }
    int result = rknn_inputs_set(context_, io_num_.n_input, inputs_.data());
    if (result != RKNN_SUCC) {
      return rknn_failure(result, "inputs_set");
    }
    rknn_run_extend extend = {};
    extend.timeout_ms = static_cast<std::int32_t>(timeout_ms_);
    result = rknn_run(context_, &extend);
    if (result != RKNN_SUCC) {
      return rknn_failure(result, "run");
    }
    result = rknn_outputs_get(context_, io_num_.n_output, outputs_.data(), nullptr);
    if (result != RKNN_SUCC) {
      return rknn_failure(result, "outputs_get");
    }
    outputs_acquired_ = true;
    return domain::OperationResult::success();
  }

  domain::OperationResult release_outputs() {
    if (!outputs_acquired_) {
      return domain::OperationResult::success();
    }
    const int result =
        rknn_outputs_release(context_, io_num_.n_output, outputs_.data());
    outputs_acquired_ = false;
    if (result != RKNN_SUCC) {
      return rknn_failure(result, "outputs_release");
    }
    return domain::OperationResult::success();
  }

  std::size_t input_count() const noexcept { return io_num_.n_input; }
  std::size_t output_count() const noexcept { return io_num_.n_output; }
  rknn_context context() const noexcept { return context_; }

  const rknn_tensor_attr& input_attr(std::size_t index) const {
    return input_attrs_.at(index);
  }
  const rknn_tensor_attr& output_attr(std::size_t index) const {
    return output_attrs_.at(index);
  }

  float* input_float(std::size_t index) { return input_float_storage_.at(index).data(); }
  std::int64_t* input_int64(std::size_t index) {
    return input_int64_storage_.at(index).data();
  }
  const float* output_float(std::size_t index) const {
    return output_float_storage_.at(index).data();
  }
  const std::int64_t* output_int64(std::size_t index) const {
    return output_int64_storage_.at(index).data();
  }

  void zero_input(std::size_t index) {
    const rknn_tensor_attr& attr = input_attrs_.at(index);
    if (attr.type == RKNN_TENSOR_INT64) {
      std::fill(input_int64_storage_.at(index).begin(),
                input_int64_storage_.at(index).end(), 0);
    } else {
      std::fill(input_float_storage_.at(index).begin(),
                input_float_storage_.at(index).end(), 0.0F);
    }
  }

  domain::OperationResult copy_output_to_input(std::size_t index) {
    const rknn_tensor_attr& input_attr = input_attrs_.at(index);
    const rknn_tensor_attr& output_attr = output_attrs_.at(index);
    if (input_attr.type == RKNN_TENSOR_INT64 &&
        output_attr.type == RKNN_TENSOR_INT64) {
      std::copy_n(output_int64(index), tensor_elements(input_attr), input_int64(index));
      return domain::OperationResult::success();
    }
    if (!tensor_is_float(input_attr) || !tensor_is_float(output_attr)) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN 缓存输入输出类型不匹配");
    }
    if (input_attr.n_dims == 4 && output_attr.n_dims == 4 &&
        input_attr.fmt == RKNN_TENSOR_NHWC && output_attr.fmt == RKNN_TENSOR_NCHW) {
      const std::size_t n = input_attr.dims[0];
      const std::size_t h = input_attr.dims[1];
      const std::size_t w = input_attr.dims[2];
      const std::size_t c = input_attr.dims[3];
      if (n * c * h * w != tensor_elements(input_attr)) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "RKNN NHWC 输入形状不一致");
      }
      const float* source = output_float(index);
      float* destination = input_float(index);
      for (std::size_t ni = 0; ni < n; ++ni) {
        for (std::size_t ci = 0; ci < c; ++ci) {
          for (std::size_t hi = 0; hi < h; ++hi) {
            for (std::size_t wi = 0; wi < w; ++wi) {
              destination[ni * h * w * c + hi * w * c + wi * c + ci] =
                  source[ni * c * h * w + ci * h * w + hi * w + wi];
            }
          }
        }
      }
      return domain::OperationResult::success();
    }
    std::copy_n(output_float(index), tensor_elements(input_attr), input_float(index));
    return domain::OperationResult::success();
  }

 private:
  rknn_context context_ = 0;
  rknn_input_output_num io_num_ = {};
  std::uint32_t timeout_ms_ = 0;
  std::vector<rknn_tensor_attr> input_attrs_;
  std::vector<rknn_tensor_attr> output_attrs_;
  std::vector<rknn_input> inputs_;
  std::vector<rknn_output> outputs_;
  std::vector<std::vector<float>> input_float_storage_;
  std::vector<std::vector<std::int64_t>> input_int64_storage_;
  std::vector<std::vector<float>> output_float_storage_;
  std::vector<std::vector<std::int64_t>> output_int64_storage_;
  bool outputs_acquired_ = false;
};

struct RknnZipformerEngine::Impl {
  domain::OperationResult load(const RknnZipformerEngineOptions& options) {
    auto status = encoder.load(options.encoder_model_path, "encoder",
                               options.run_timeout_ms);
    if (!status.ok()) {
      return status;
    }
    status = decoder.load(options.decoder_model_path, "decoder",
                          options.run_timeout_ms);
    if (!status.ok()) {
      return status;
    }
    status = joiner.load(options.joiner_model_path, "joiner", options.run_timeout_ms);
    if (!status.ok()) {
      return status;
    }

    rknn_sdk_version version = {};
    const int version_result =
        rknn_query(encoder.context(), RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    if (version_result != RKNN_SUCC) {
      return rknn_failure(version_result, "query_sdk_version");
    }
    api_version = version.api_version;
    driver_version = version.drv_version;

    if (encoder.input_count() < 2 || encoder.input_count() != encoder.output_count()) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN encoder 输入输出数量不匹配");
    }
    if (decoder.input_count() != 1 || decoder.output_count() != 1 ||
        joiner.input_count() != 2 || joiner.output_count() != 1) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN decoder/joiner 输入输出数量不匹配");
    }

    const rknn_tensor_attr& encoder_input = encoder.input_attr(0);
    if (encoder_input.n_dims != 3 || encoder_input.dims[0] != 1) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN encoder 输入形状不支持");
    }
    info.feature_dim = encoder_input.dims[2];
    info.chunk_feature_frames = encoder_input.dims[1];

    const rknn_tensor_attr& encoder_output = encoder.output_attr(0);
    if (encoder_output.n_dims != 3 || encoder_output.dims[0] != 1) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN encoder 输出形状不支持");
    }
    info.encoder_output_frames = encoder_output.dims[1];
    info.decoder_output_dim = encoder_output.dims[2];

    const rknn_tensor_attr& decoder_input = decoder.input_attr(0);
    info.decoder_context_tokens = tensor_elements(decoder_input);
    info.decoder_output_dim = decoder.output_attr(0).n_elems;
    if (joiner.input_attr(0).n_elems != info.decoder_output_dim ||
        joiner.input_attr(1).n_elems != info.decoder_output_dim) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "RKNN joiner 输入维度与 decoder 输出不一致");
    }
    info.joiner_output_classes = tensor_elements(joiner.output_attr(0));
    info.blank_token_id = options.blank_token_id;
    info.unk_token_id = options.unk_token_id;
    info.encoder_subsampling_factor = options.encoder_subsampling_factor;
    info.feature_frame_shift_samples = options.feature_frame_shift_samples;

    const auto validation = zipformer_detail::validate_zipformer_model_info(info);
    if (!validation.ok()) {
      return validation;
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult reset_round() {
    for (std::size_t index = 1; index < encoder.input_count(); ++index) {
      encoder.zero_input(index);
    }
    return domain::OperationResult::success();
  }

  RknnModel encoder;
  RknnModel decoder;
  RknnModel joiner;
  zipformer_detail::ZipformerModelInfo info;
  std::string api_version;
  std::string driver_version;
};

RknnZipformerEngine::RknnZipformerEngine(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RknnZipformerEngine::~RknnZipformerEngine() = default;

domain::Result<std::unique_ptr<RknnZipformerEngine>> RknnZipformerEngine::create(
    const RknnZipformerEngineOptions& options) {
  if (options.encoder_model_path.empty() || options.decoder_model_path.empty() ||
      options.joiner_model_path.empty()) {
    return domain::Result<std::unique_ptr<RknnZipformerEngine>>::failure(
        domain::ErrorCode::kInvalidInput, "RKNN 模型路径为空");
  }
  if (options.run_timeout_ms > static_cast<std::uint32_t>(
                                  std::numeric_limits<std::int32_t>::max())) {
    return domain::Result<std::unique_ptr<RknnZipformerEngine>>::failure(
        domain::ErrorCode::kInvalidInput, "RKNN 超时配置溢出");
  }
  auto impl = std::make_unique<Impl>();
  const auto loaded = impl->load(options);
  if (!loaded.ok()) {
    return domain::Result<std::unique_ptr<RknnZipformerEngine>>::failure(
        loaded.error.code, loaded.error.message);
  }
  return domain::Result<std::unique_ptr<RknnZipformerEngine>>::success(
      std::unique_ptr<RknnZipformerEngine>(
          new RknnZipformerEngine(std::move(impl))));
}

const zipformer_detail::ZipformerModelInfo& RknnZipformerEngine::info() const noexcept {
  return impl_->info;
}

domain::OperationResult RknnZipformerEngine::reset_round() {
  return impl_->reset_round();
}

domain::OperationResult RknnZipformerEngine::encode_chunk(
    const float* features,
    std::size_t frame_count,
    std::vector<float>& encoder_output) {
  if (features == nullptr || frame_count != impl_->info.chunk_feature_frames) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKNN encoder 输入窗口非法");
  }
  const std::size_t feature_values = frame_count * impl_->info.feature_dim;
  std::copy_n(features, feature_values, impl_->encoder.input_float(0));
  const auto ran = impl_->encoder.run();
  if (!ran.ok()) {
    return ran;
  }

  try {
    const std::size_t output_values =
        impl_->info.encoder_output_frames * impl_->info.decoder_output_dim;
    encoder_output.assign(impl_->encoder.output_float(0),
                          impl_->encoder.output_float(0) + output_values);
    for (std::size_t index = 1; index < impl_->encoder.input_count(); ++index) {
      const auto copied = impl_->encoder.copy_output_to_input(index);
      if (!copied.ok()) {
        (void)impl_->encoder.release_outputs();
        return copied;
      }
    }
  } catch (...) {
    (void)impl_->encoder.release_outputs();
    throw;
  }
  return impl_->encoder.release_outputs();
}

domain::OperationResult RknnZipformerEngine::decode(
    const std::int64_t* context,
    std::size_t context_size,
    std::vector<float>& decoder_output) {
  if (context == nullptr || context_size != impl_->info.decoder_context_tokens) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKNN decoder 上下文长度非法");
  }
  std::copy_n(context, context_size, impl_->decoder.input_int64(0));
  const auto ran = impl_->decoder.run();
  if (!ran.ok()) {
    return ran;
  }
  try {
    decoder_output.assign(impl_->decoder.output_float(0),
                          impl_->decoder.output_float(0) +
                              impl_->info.decoder_output_dim);
  } catch (...) {
    (void)impl_->decoder.release_outputs();
    throw;
  }
  return impl_->decoder.release_outputs();
}

domain::OperationResult RknnZipformerEngine::join(
    const float* encoder_output,
    const float* decoder_output,
    std::vector<float>& logits) {
  if (encoder_output == nullptr || decoder_output == nullptr) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKNN joiner 输入为空");
  }
  std::copy_n(encoder_output, impl_->info.decoder_output_dim,
              impl_->joiner.input_float(0));
  std::copy_n(decoder_output, impl_->info.decoder_output_dim,
              impl_->joiner.input_float(1));
  const auto ran = impl_->joiner.run();
  if (!ran.ok()) {
    return ran;
  }
  try {
    logits.assign(impl_->joiner.output_float(0),
                  impl_->joiner.output_float(0) +
                      impl_->info.joiner_output_classes);
  } catch (...) {
    (void)impl_->joiner.release_outputs();
    throw;
  }
  return impl_->joiner.release_outputs();
}

std::string RknnZipformerEngine::sdk_api_version() const {
  return impl_->api_version;
}

std::string RknnZipformerEngine::sdk_driver_version() const {
  return impl_->driver_version;
}

}  // namespace nexweave::backend
