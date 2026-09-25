// MeloTTS 真实适配器 factory。调用方只在 backend 初始化代码中触碰本入口；
// Session 只接收它返回的 capability::ITts 引用，不包含 MeloTTS 或 SDK 类型。
#pragma once

#include <memory>

#include "melotts_tts.hpp"
#include "melotts_types.hpp"

namespace nexweave::backend {

// 按 config 加载文本前端、ONNX Runtime 编码器与 RKNN 解码器，构造真实 MeloTTS
// 适配器。任一步失败都释放已成功加载的部分并返回结构化错误；成功后对象拥有全部
// 模型、映射表和推理资源。返回值暴露 MeloTtsTts 以便板端证据读取统计；调用方若
// 只关心能力契约，可立即移动到 std::unique_ptr<capability::ITts>。
domain::Result<std::unique_ptr<MeloTtsTts>> create_melotts_tts_adapter(
    const MeloTtsConfig& config);

}  // namespace nexweave::backend
