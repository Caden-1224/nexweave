#include "session_app.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../backend/fake_rag.hpp"
#include "../domain/audio_frame.hpp"
#include "interaction_contract.hpp"
#include "speech_segmentation.hpp"

namespace nexweave::runtime {
namespace {

using domain::Error;
using domain::ErrorCode;
using domain::OperationResult;

// 文件音频的固定格式合同。v1 只接受这一种组合，不做隐式重采样、混声道或位深转换：
// 转换责任属于明确的适配器，而不是“读文件”这件事本身。任一字段不符即 kInvalidInput。
constexpr std::uint16_t kWavPcmFormat = 1;
constexpr std::uint16_t kWavChannels = 1;
constexpr std::uint16_t kWavBitsPerSample = 16;
constexpr std::uint32_t kWavSampleRateHz = 16000;

// 诊断文本只用于定位；机器决策一律使用错误码。
constexpr char kInvalidConfig[] = "Session 应用配置非法";
constexpr char kResidentMissing[] = "模拟常驻模式必须注入音频输入";
constexpr char kResidentUnexpected[] = "非模拟常驻模式不应注入音频输入";
constexpr char kTextModeEmpty[] = "文本模式缺少非空识别文本";
constexpr char kFileModeEmpty[] = "文件音频模式缺少 WAV 路径";
constexpr char kStreamIdEmpty[] = "输入流标识不能为空";
constexpr char kPumpBudgetInvalid[] = "常驻输入的取段泵入预算必须大于 0";
constexpr char kWavOpenFailed[] = "无法打开音频文件";
constexpr char kWavReadFailed[] = "读取音频文件失败";
constexpr char kWavTooShort[] = "音频文件过短，缺少 RIFF/WAVE 头";
constexpr char kWavNotRiff[] = "音频文件不是 RIFF 容器";
constexpr char kWavNotWave[] = "音频文件不是 WAVE 格式";
constexpr char kWavMissingFmt[] = "音频文件缺少 fmt 块";
constexpr char kWavNotPcm[] = "音频文件不是未压缩 PCM";
constexpr char kWavBadChannels[] = "音频文件不是单声道";
constexpr char kWavBadBits[] = "音频文件不是 16 位样本";
constexpr char kWavBadRate[] = "音频文件不是 16 kHz";
constexpr char kWavBadBlockAlign[] = "音频文件块对齐与 16 位单声道不符";
constexpr char kWavTruncatedChunk[] = "音频文件的块声明长度超出文件范围";
constexpr char kWavEmptyData[] = "音频文件不含任何样本";
constexpr char kPumpBudgetExhausted[] = "一次取段未能在泵入预算内取得语音段";
constexpr char kResidentEnded[] = "常驻输入已经结束";
constexpr char kStopped[] = "已请求停止，不再等待新的语音段";

// 小端整数读取：只在已确认长度足够的缓冲区上调用，因此不再重复做越界判断。
std::uint16_t read_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes.at(offset)) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(offset + 1)) << 8U);
}

std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes.at(offset + static_cast<std::size_t>(index)))
             << (index * 8);
  }
  return value;
}

bool matches_tag(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* tag) {
  for (std::size_t index = 0; index < 4; ++index) {
    if (bytes.at(offset + index) != static_cast<std::uint8_t>(tag[index])) {
      return false;
    }
  }
  return true;
}

// 一次性读入整个文件。失败分为“打不开”（kInvalidInput，属于路径/权限问题）与“读坏了”
// （kDeviceFailure，属于 I/O 问题），两者不能混为一谈：前者调用方可以改路径重试，
// 后者说明介质出了问题。
domain::Result<std::vector<std::uint8_t>> read_whole_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return domain::Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavOpenFailed);
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buffer[4096];
  for (;;) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      bytes.insert(bytes.end(), buffer, buffer + read);
    }
    if (read < sizeof(buffer)) {
      // 短读既可能是文件结束，也可能是读取错误；必须分开，否则坏文件会被当成一个空音频
      // 文件，最终只表现为“音频为空”，真正的 I/O 原因从证据里消失。
      if (std::ferror(file) != 0) {
        std::fclose(file);
        return domain::Result<std::vector<std::uint8_t>>::failure(ErrorCode::kDeviceFailure,
                                                                 kWavReadFailed);
      }
      break;
    }
  }
  std::fclose(file);
  return domain::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

// 把 WAV 字节流解码成 16 kHz/单声道/16 位 PCM 样本。
//
// 顺序理由：先校验容器、再校验 fmt 块、最后校验并复制 data 块。这样“文件是别的格式”与
// “文件被截断”得到不同的错误码，调用方不需要靠猜；任何失败都不会产生半截样本，因此坏
// 文件不可能变成一个已经开始又中途失败的轮次。
// 未知块（例如 LIST/INFO 元数据）按 RIFF 规则跳过而不是判为损坏：附加元数据不影响样本
// 正确性；但声明长度超出文件范围的块仍然被拒绝——那是真正的截断。
domain::Result<std::vector<std::int16_t>> decode_wav(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 12) {
    return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavTooShort);
  }
  if (!matches_tag(bytes, 0, "RIFF")) {
    return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavNotRiff);
  }
  if (!matches_tag(bytes, 8, "WAVE")) {
    return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavNotWave);
  }

  bool have_format = false;
  std::size_t data_offset = 0;
  std::size_t data_bytes = 0;
  std::size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const std::uint32_t declared = read_le32(bytes, offset + 4);
    const std::size_t body = offset + 8;
    if (body > bytes.size() || static_cast<std::size_t>(declared) > bytes.size() - body) {
      return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                               kWavTruncatedChunk);
    }
    if (matches_tag(bytes, offset, "fmt ")) {
      if (declared < 16) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavMissingFmt);
      }
      if (read_le16(bytes, body) != kWavPcmFormat) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavNotPcm);
      }
      if (read_le16(bytes, body + 2) != kWavChannels) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavBadChannels);
      }
      if (read_le32(bytes, body + 4) != kWavSampleRateHz) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavBadRate);
      }
      // 块对齐 = 声道数 × 位深/8。它与声道、位深互为冗余校验：单独字段都合法、组合起来
      // 却不是 16 位单声道时（例如两个字段被同时改坏），这里仍然能拒绝。
      if (read_le16(bytes, body + 12) != kWavChannels * (kWavBitsPerSample / 8U)) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavBadBlockAlign);
      }
      if (read_le16(bytes, body + 14) != kWavBitsPerSample) {
        return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                                 kWavBadBits);
      }
      have_format = true;
    } else if (matches_tag(bytes, offset, "data")) {
      data_offset = body;
      data_bytes = declared;
    }
    // 块体长度为奇数时补一个填充字节，这是 RIFF 的通用规则，与块类型无关。
    offset = body + declared + (declared % 2U);
  }

  if (!have_format) {
    return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavMissingFmt);
  }
  if (data_bytes < 2) {
    // 0 表示没有 data 块，1 表示连一个 16 位样本都放不下。两者都不含任何可识别音频，
    // 因此都按“没有数据”拒绝，而不是补一个零样本当成有效输入。
    return domain::Result<std::vector<std::int16_t>>::failure(ErrorCode::kInvalidInput,
                                                             kWavEmptyData);
  }

  const std::size_t sample_count = data_bytes / 2;
  std::vector<std::int16_t> samples;
  samples.reserve(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    samples.push_back(static_cast<std::int16_t>(read_le16(bytes, data_offset + index * 2)));
  }
  return domain::Result<std::vector<std::int16_t>>::success(std::move(samples));
}

// 文本是否只由空白字符组成。文本模式用它区分“没有输入”与“一次提问”：空白字符串不是
// 提问，把它当成提问会得到一次“识别不到文本”的失败轮次，掩盖“本次运行没有输入”这一事实。
bool is_blank(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// 单轮输入生产者的公共部分：文本与文件模式都只产出一轮，区别只在“这一轮的样本从哪来”。
// 轮次身份（stream_id、generation、request_id）与结束判定的规则因此只有一份实现，不会出现
// 两个模式各自拼一次 request_id 而在将来悄悄不一致。子类只负责给出样本与脚本文本。
class SingleTurnProducer : public SessionApp::TurnProducer {
 public:
  explicit SingleTurnProducer(std::string stream_id) : stream_id_(std::move(stream_id)) {}

  domain::OperationResult next(SessionTurnInput& output) override {
    if (delivered_ || !has_input()) {
      // 已经交付过，或者本次运行根本没有可处理的输入：两者都是“不会有下一轮”，因此按
      // 输入耗尽报告，而不是伪造一个空轮次。
      delivered_ = true;
      return OperationResult::failure(ErrorCode::kAlreadyCompleted);
    }
    delivered_ = true;
    output.stream_id = stream_id_;
    output.generation = 1;
    output.request_id = stream_id_ + "-turn-1";
    output.pcm_samples = turn_samples();
    return OperationResult::success();
  }

  bool input_ended() const noexcept override { return delivered_; }
  // 单轮模式不建立常驻采集，因此没有“输入流关闭”这条生命周期事实。
  bool stream_established() const noexcept override { return false; }
  OperationResult close() noexcept override { return OperationResult::success(); }

 protected:
  // 本次运行是否有可处理的输入。
  virtual bool has_input() const = 0;
  // 本轮要交付的样本（独立副本）。
  virtual std::vector<std::int16_t> turn_samples() const = 0;

 private:
  std::string stream_id_;
  bool delivered_ = false;
};

// 文本模式的生产者：固定文本就是一次识别结果。
// 它只投递一帧静音来驱动“识别完成”这一事件，因此不需要音频源，也不存在第二个生产者。
// 这一帧不是录音、不代表任何音频内容，帧内容因此对结果没有影响（全部为零）；真正的识别
// 文本由 scripted_text() 交给文本注入器，仍然完整经过“识别 → 路由 → 合成 → 播放”。
class TextTurnProducer final : public SingleTurnProducer {
 public:
  TextTurnProducer(std::string stream_id, std::string text)
      : SingleTurnProducer(std::move(stream_id)), text_(std::move(text)) {}

  std::string scripted_text() const override { return text_; }

 protected:
  // 全空白文本与“配置里没有文本”一样表示没有输入：交付它只会得到一次“识别不到文本”的
  // 失败轮次，而调用方真正需要知道的是“这次运行没有可处理的输入”。
  bool has_input() const override { return !is_blank(text_); }

  std::vector<std::int16_t> turn_samples() const override {
    return std::vector<std::int16_t>(domain::kAudioFrameSamples, 0);
  }

 private:
  std::string text_;
};

// 文件音频的生产者：整个文件是**一轮**输入。样本在 run() 建立生产者之前已经读完并校验，
// 因此 next() 不会再失败；尾部不足一帧的部分由会话层的分帧契约补零并保留有效样本数。
class FileTurnProducer final : public SingleTurnProducer {
 public:
  FileTurnProducer(std::string stream_id, std::vector<std::int16_t> samples)
      : SingleTurnProducer(std::move(stream_id)), samples_(std::move(samples)) {}

 protected:
  bool has_input() const override { return !samples_.empty(); }
  std::vector<std::int16_t> turn_samples() const override { return samples_; }

 private:
  std::vector<std::int16_t> samples_;
};

// 模拟常驻输入的生产者：把已经完成的语音段按轮次交给会话，并在“旧回答被新语音打断”时
// 把该段切在打断点上。
//
// 为什么需要切分：常驻输入可以在一轮回答播放期间就攒出**下一段**说话，因此取到的段可能
// 同时包含“还没说完的旧语音”和“打断旧回答的新语音”。整段交给被打断的那一轮，会让新语音
// 的开头随旧输出一起作废；整段留给下一轮，又会把新语音的开头算成上一轮的内容。切分点取
// 打断通知里的 start_sequence（本次说话第一帧在输入流内的序号），因此切分依据是输入自己的
// 序号，而不是播放侧的时间估计。
//
// 切分只影响“这一轮的输入范围”，不丢弃任何帧：前半段交给本轮，后半段留给下一次取段。
// 通知早于本段起点时不切分——那种通知描述的是本轮开始之前就已经出现的说话，把它当成
// 本段内的新语音会让刚取到的这一段立刻取消自己。
class ResidentTurnProducer final : public SessionApp::TurnProducer {
 public:
  ResidentTurnProducer(ResidentAudioInput& resident, std::size_t pump_budget,
                       const std::atomic<bool>* stopped)
      : resident_(resident), pump_budget_(pump_budget), stopped_(stopped) {}

  domain::OperationResult start() override {
    const auto opened = resident_.start();
    if (!opened.ok()) {
      return opened;
    }
    started_ = true;
    return OperationResult::success();
  }

  domain::OperationResult next(SessionTurnInput& output) override {
    if (!started_) {
      return OperationResult::failure(ErrorCode::kInvalidInput, kResidentMissing);
    }
    if (!has_remainder_ && remaining_ == 0 && !resident_.capturing()) {
      // 队列已经取空且采集已经结束：不会再有下一轮。这是正常结束而不是失败。
      return OperationResult::failure(ErrorCode::kAlreadyCompleted);
    }

    SpeechSegment segment;
    if (has_remainder_) {
      segment = std::move(remainder_);
      remainder_ = SpeechSegment{};
      has_remainder_ = false;
    } else {
      auto taken = take_segment_or_fail();
      if (!taken.ok()) {
        return OperationResult::failure(taken.error.code, taken.error.message);
      }
      segment = std::move(*taken.value);
    }
    if (remaining_ > 0) {
      --remaining_;
    }

    // 打断通知只在本段起点之后才有切分意义。通知是“最早一条未消费”的记录，因此消费它
    // 同时也是在清空遗留通知，避免它在下一次取段时被误当成新的打断。
    const auto notice = resident_.take_speech_started();
    if (notice.has_value()) {
      const std::size_t split = split_index(segment, notice->start_sequence);
      if (split > 0 && split < segment.frames.size()) {
        remainder_ = segment;
        remainder_.frames.erase(remainder_.frames.begin(),
                                remainder_.frames.begin() + static_cast<std::ptrdiff_t>(split));
        remainder_.chunk_index += 1;
        has_remainder_ = true;
        segment.frames.resize(split);
      }
    }

    output.stream_id = resident_.stream_id();
    // generation 只作为调用方的输入身份记录：事件过滤使用会话自己持有的代际水位。
    output.generation = segment.segment_id;
    output.request_id = resident_.stream_id() + "-turn-" + std::to_string(segment.segment_id) +
                        "-" + std::to_string(segment.chunk_index + 1);
    output.pcm_samples = segment_samples(segment);
    return OperationResult::success();
  }

  bool input_ended() const noexcept override { return !has_remainder_ && remaining_ == 0; }

  bool stream_established() const noexcept override { return started_; }

  bool stopped_by_request() const noexcept override { return stopped_by_request_; }

  OperationResult close() noexcept override { return resident_.stop(); }

 private:
  // 取一个已经完成的段：先按预算泵入，再非阻塞取段。只有“泵入预算用尽但还没成段”才
  // 返回失败，因为那时源在预算内没有产出任何帧，继续泵入只会空转。
  //
  // 顺序理由：每轮先取段、再泵一帧。反过来的“先无限泵入直到成段”会让泵入量取决于输入
  // 长度，掩盖“消费进度”；而先阻塞等待再泵入则会永久等待——分段器只在静音超时或输入
  // 结束时才交付段，这两件事都需要继续泵帧。
  domain::Result<SpeechSegment> take_segment_or_fail() {
    for (std::size_t pumped = 0; pumped < pump_budget_; ++pumped) {
      auto taken = resident_.take_segment();
      if (taken.availability == SegmentAvailability::kAvailable) {
        remaining_ = resident_.pending_segments();
        return domain::Result<SpeechSegment>::success(std::move(taken.segment));
      }
      if (taken.availability == SegmentAvailability::kEnded) {
        remaining_ = 0;
        return domain::Result<SpeechSegment>::failure(ErrorCode::kAlreadyCompleted,
                                                      kResidentEnded);
      }
      // 停止请求也要在泵入过程中生效，而不只在轮次边界：一条持续几十秒的说话若必须
      // 等它说完才响应退出，进程退出时间就会由输入长度决定。这里报告“本轮没有输入”，
      // 由调用方按“被停止”收敛；此时还没有段被取走，因此不会有半个轮次被提交。
      if (stopped_ != nullptr && stopped_->load()) {
        stopped_by_request_ = true;
        return domain::Result<SpeechSegment>::failure(ErrorCode::kAlreadyCompleted, kStopped);
      }
      // 每帧一次：泵入与消费的粒度都保持在一帧，段边界因此与消费边界一样可观察。
      const auto pumped_frames = resident_.pump(1);
      if (!pumped_frames.ok()) {
        return domain::Result<SpeechSegment>::failure(pumped_frames.error.code,
                                                      pumped_frames.error.message);
      }
    }
    // 预算内没有产出任何帧：这既可能是设备暂时没有数据，也可能是预算配置与场景规模不匹配。
    // 两种情形都属于“这次没等到”，而不是设备或后端坏了，因此用可重试的超时类错误收敛——
    // 报成后端故障会让调用方误以为需要更换或重启后端，也会污染运行证据里的故障统计。
    return domain::Result<SpeechSegment>::failure(ErrorCode::kTimeout, kPumpBudgetExhausted);
  }

  // 找出“本段内新语音从哪里开始”的帧下标。返回 0 表示本段整体都属于新语音（或通知早于
  // 本段起点），返回 frames.size() 表示本段内没有新语音的开头；两种情况调用方都不切分。
  static std::size_t split_index(const SpeechSegment& segment, std::uint64_t start_sequence) {
    if (segment.chunk_index > 0) {
      // 续块是同一次说话的后半段，其中不会出现新的起音：整块都属于新语音。
      return 0;
    }
    if (segment.start_sequence >= start_sequence) {
      return 0;
    }
    return segment.frames.size();
  }

  ResidentAudioInput& resident_;
  std::size_t pump_budget_ = 64;
  // 应用的停止标志（借用，生命周期长于生产者）：泵入过程中据此提前退出。
  const std::atomic<bool>* stopped_ = nullptr;
  bool started_ = false;
  // 队列中已经交付但尚未取走的段数快照。它只用于判断“取空且已结束”，因此允许偏保守：
  // 偏大只会多取一次空，偏小会被接下来的 kEnded 分支立刻纠正。
  std::size_t remaining_ = 0;
  // 本次取段是否因为停止请求而放弃（而不是因为输入结束）。它让“提前停止”与“输入读完”
  // 在结果里保持互斥，调用方不会把一次被打断的运行当成正常收尾。
  bool stopped_by_request_ = false;
  SpeechSegment remainder_;
  bool has_remainder_ = false;
};

// 文本注入器：文本模式下把“固定的识别文本”交给会话，其余情况原样透传。
//
// 为什么用一层包装而不是另建一个识别实现：固定文本必须**经过识别接口**进入会话，否则验收
// 的就不是“识别 → 路由 → 合成 → 播放”这条链路，而是一次手工注入；但也不该因此要求每个
// 识别适配器都实现一个只为文本模式存在的接口。注入器只在结束帧这一点替换文本，其余调用
// （注册回调、送帧、取消）全部原样转发，因此被包装的适配器仍然观察到真实的送帧序列。
//
// 边界：文本为空时是严格透传（注册的就是调用方自己的回调），因此音频模式复用它也不会改变
// 行为；注入器不实现取消封锁，取消语义由被包装的适配器与会话层负责。
class TextEventInjector final : public capability::IAsr {
 public:
  explicit TextEventInjector(capability::IAsr& inner) : inner_(inner) {}

  // 设定本次运行的固定文本；空串表示“识别结果来自音频”。
  void set_text(std::string text) { text_ = std::move(text); }

  // 解除已注册的转发回调。必须在一轮（或一次运行）结束后调用：转发回调按引用捕获本对象
  // 的成员，留着它会让被包装的适配器持有指向下一次运行状态的引用。
  void reset_round() { callback_ = nullptr; }

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return OperationResult::failure(ErrorCode::kInvalidInput);
    }
    if (text_.empty()) {
      callback_ = nullptr;
      return inner_.set_callback(std::move(callback));
    }
    callback_ = std::move(callback);
    return inner_.set_callback([this](const capability::TextEvent& event) {
      if (callback_ == nullptr) {
        return;
      }
      if (event.kind == capability::TextEventKind::kFinal) {
        callback_(capability::TextEvent{capability::TextEventKind::kFinal, text_, event.error});
        return;
      }
      callback_(event);
    });
  }

  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override {
    return inner_.feed(frame, is_last);
  }

  domain::OperationResult cancel() noexcept override { return inner_.cancel(); }

 private:
  capability::IAsr& inner_;
  std::string text_;
  capability::TextEventCallback callback_;
};

}  // namespace

const char* to_string(SessionAppInputMode mode) noexcept {
  switch (mode) {
    case SessionAppInputMode::kText:
      return "text";
    case SessionAppInputMode::kWavFile:
      return "file";
    case SessionAppInputMode::kSimulatedResident:
      return "resident";
  }
  return "";
}

domain::OperationResult validate_app_config(const SessionAppConfig& config) {
  if (config.stream_id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, kStreamIdEmpty);
  }
  if (config.pump_budget_per_take == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, kPumpBudgetInvalid);
  }
  switch (config.mode) {
    case SessionAppInputMode::kText:
      // 空文本与全空白文本都等价于“没有输入”：生产者随后把空文本报告成输入结束，而不是
      // 伪造一次空问题。这里只拒绝真正缺失的配置。
      if (config.text.empty()) {
        return OperationResult::failure(ErrorCode::kInvalidInput, kTextModeEmpty);
      }
      break;
    case SessionAppInputMode::kWavFile:
      if (config.wav_path.empty()) {
        return OperationResult::failure(ErrorCode::kInvalidInput, kFileModeEmpty);
      }
      break;
    case SessionAppInputMode::kSimulatedResident:
      break;
  }
  return OperationResult::success();
}

SessionApp::SessionApp(const SessionAppConfig& config, capability::IAsr& asr,
                       capability::IRag& retriever, backend::FakeRagRouter& router,
                       capability::ITts& tts, IAudioPlayback& playback,
                       ResidentAudioInput* resident, capability::ILlm* llm)
    : config_(config),
      asr_(asr),
      retriever_(retriever),
      router_(router),
      tts_(tts),
      playback_(playback),
      resident_(resident),
      llm_(llm) {
  // 会话在整条生命周期内只接收一个识别后端引用：注入对象外面固定包一层文本注入器。代价是
  // 音频模式下多一次直通转发，收益是“固定文本只在一个位置交付”——会话对象不必随运行模式
  // 更换后端引用，也就不会出现悬空引用。注入器是本对象的成员，因此会话持有的引用与它同寿。
  auto injector = std::unique_ptr<TextEventInjector>(new TextEventInjector(asr));
  session_.reset(new SessionRuntime(*injector, retriever, router, tts, playback, llm,
                                    config.session_config));
  injector_ = std::move(injector);
}

void SessionApp::set_generation_observer(capability::IGenerationObserver* observer) noexcept {
  generation_observer_ = observer;
  session_->set_generation_observer(observer);
}

void SessionApp::request_stop() noexcept {
  stop_requested_.store(true);
}

void SessionApp::cancel_turn() noexcept {
  (void)session_->cancel();
}

bool SessionApp::stop_requested() const noexcept {
  return stop_requested_.load();
}

std::vector<ActivityMarker> SessionApp::trace() const {
  return session_->trace();
}

std::unique_ptr<SessionApp::TurnProducer> SessionApp::make_producer(const SessionAppConfig& config,
                                                                   domain::Error& error) {
  switch (config.mode) {
    case SessionAppInputMode::kText: {
      if (resident_ != nullptr) {
        // 输入源互斥是配置错误而不是可忽略的多余参数：两个生产者指向同一条流时，
        // “谁拥有麦克风”会退化成运行时竞态，因此这里明确拒绝而不是静默忽略。
        error = Error{ErrorCode::kInvalidInput, kResidentUnexpected};
        return nullptr;
      }
      // 固定文本由生产者持有：生产者回答“这一轮的文本从哪来”，应用据此把它交给文本注入器。
      return std::unique_ptr<TurnProducer>(new TextTurnProducer(config.stream_id, config.text));
    }
    case SessionAppInputMode::kWavFile: {
      if (resident_ != nullptr) {
        error = Error{ErrorCode::kInvalidInput, kResidentUnexpected};
        return nullptr;
      }
      const auto bytes = read_whole_file(config.wav_path);
      if (!bytes.ok()) {
        error = bytes.error;
        return nullptr;
      }
      auto samples = decode_wav(*bytes.value);
      if (!samples.ok()) {
        error = samples.error;
        return nullptr;
      }
      return std::unique_ptr<TurnProducer>(
          new FileTurnProducer(config.stream_id, std::move(*samples.value)));
    }
    case SessionAppInputMode::kSimulatedResident: {
      if (resident_ == nullptr) {
        error = Error{ErrorCode::kInvalidInput, kResidentMissing};
        return nullptr;
      }
      return std::unique_ptr<TurnProducer>(
          new ResidentTurnProducer(*resident_, config.pump_budget_per_take, &stop_requested_));
    }
  }
  error = Error{ErrorCode::kInvalidInput, kInvalidConfig};
  return nullptr;
}

bool SessionApp::take_next_input(SessionTurnInput& output, bool& ended, domain::Error& error) {
  // 停止请求的检查点固定在轮次边界：这样停止不会打断一个正在收敛的轮次，也不会把已经
  // 完成的轮次变成半个结果。常驻输入侧的“等不到新语音”由取段泵入预算兜底，因此本检查点
  // 不需要额外的唤醒机制就能在有限步内到达。
  if (stop_requested_.load()) {
    ended = false;
    return false;
  }
  if (config_.max_turns != 0 && result_turns_ >= config_.max_turns) {
    ended = false;
    return false;
  }
  const auto taken = producer_->next(output);
  if (taken.ok()) {
    return true;
  }
  if (taken.error.code == ErrorCode::kAlreadyCompleted) {
    ended = true;
    return false;
  }
  error = taken.error;
  return false;
}

void SessionApp::account_resident_input(SessionAppRunResult& result) const {
  if (resident_ == nullptr) {
    return;
  }
  result.frames_pumped = resident_->frames_pumped();
  result.segments_queued = resident_->queued_segments();
  result.segments_dropped = resident_->dropped_segments();
}

SessionAppRunResult SessionApp::run(SessionAppObserver* observer) {
  SessionAppRunResult result;
  // 每次运行都是一次独立的轮次序列：上一次运行的停止请求不能让下一次运行立刻退出。
  stop_requested_.store(false);
  result_turns_ = 0;
  producer_.reset();

  const auto valid = validate_app_config(config_);
  if (!valid.ok()) {
    result.error = valid.error;
    return result;
  }

  domain::Error producer_error;
  producer_ = make_producer(config_, producer_error);
  if (producer_ == nullptr) {
    result.error = producer_error;
    return result;
  }

  // 把本次运行的固定文本交给注入器。音频模式下生产者返回空串，这一层于是成为严格透传，
  // 因此不需要按模式分支；每次运行都重新设定并清掉上一轮回调，避免上次运行的状态留下。
  auto* const injector = static_cast<TextEventInjector*>(injector_.get());
  injector->reset_round();
  injector->set_text(producer_->scripted_text());

  // 生成进度是可选接缝：后端同时实现 IGenerationProbe 时，会话层会把自己包装成观察者并
  // 转发本对象注册的观察者。未注册观察者时这一路完全不存在，生成与取消语义不变；
  // 构造时补登一次，使“先建应用再注册观察者”的顺序也成立。
  session_->set_generation_observer(generation_observer_);

  // 输入建立失败与轮次失败分开报告：此时还没有任何轮次，因此它是运行级错误。
  const auto started = producer_->start();
  if (!started.ok()) {
    result.error = started.error;
    producer_.reset();
    return result;
  }

  for (;;) {
    SessionTurnInput input;
    bool ended = false;
    domain::Error take_error;
    if (!take_next_input(input, ended, take_error)) {
      if (!take_error.ok()) {
        // 取值失败只保留首个原因：后续失败通常只是它的后果，覆盖它会丢掉根因。
        if (result.error.ok()) {
          result.error = take_error;
        }
      } else if (!ended) {
        result.stopped_early = true;
      }
      break;
    }

    auto turn = session_->run(input);
    ++result_turns_;
    if (turn.completed) {
      ++result.turns_completed;
    } else {
      ++result.turns_failed;
    }
    if (observer != nullptr) {
      observer->on_turn(turn);
    }
    result.turns.push_back(std::move(turn));
  }

  // 两个结束原因互斥：因停止请求收敛不是“输入读完”，输入读完也不是被停止。生产者报出
  // 的“因停止而放弃”优先于轮次边界的停止检查，因为前者可能发生在任何一次取段中。
  if (producer_->stopped_by_request()) {
    result.stopped_early = true;
  }
  result.input_ended = result.stopped_early ? false : producer_->input_ended();
  // 会话侧的输入流只在真的取到过轮次时才需要结束：没有任何轮次时会话从未开始这条流，
  // 对它调用 finish_stream() 只会得到“流已经关闭”的失败，那是噪声而不是事实。
  const bool session_stream_open = !result.turns.empty();
  if (session_stream_open) {
    const auto closed = session_->finish_stream();
    if (!closed.ok() && result.cleanup_error.ok()) {
      result.cleanup_error = closed.error;
    }
  }
  // 输入本身的生命周期由生产者拥有：只要它建立过采集，本次运行结束时就一定已经停止它。
  // 这条事实与“是否取到过轮次”无关——被立即停止的运行同样需要报告输入已经关闭，
  // 否则调用方无法判断“退出后不再有后台采集”是否成立。
  const bool input_established = producer_->stream_established();
  const auto released = producer_->close();
  if (!released.ok() && result.cleanup_error.ok()) {
    result.cleanup_error = released.error;
  }
  producer_.reset();
  static_cast<TextEventInjector*>(injector_.get())->reset_round();
  account_resident_input(result);

  if (input_established && observer != nullptr) {
    observer->on_stream_closed(config_.stream_id);
  }
  return result;
}

}  // namespace nexweave::runtime