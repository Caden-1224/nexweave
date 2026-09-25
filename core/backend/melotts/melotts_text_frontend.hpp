// MeloTTS 文本前端：把 UTF-8 文本转换为模型可消费的 phone/tone/language 张量。
//
// 本实现不依赖 Python 运行时，也不复制参考工程代码。它从调用方提供的
// lexicon.txt 与 tokens.txt 加载自有映射表，使用最长匹配处理中文词语，使用
// 拉丁词表处理英文单词；遇到词典外英文缩写时按字母回退，避免等整段合成失败。
// 所有切分都保留原始内容：文本过长时按 phone 预算切块，单个词超过预算时只在
// phone 序列上拆开，不做静默截断。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../domain/error.hpp"

namespace nexweave::backend {

// 一个可直接送入编码器的文本片段。phones/tones/languages 已按 MeloTTS VITS
// 约定在音素之间插入 blank；word_phone_counts 是 interspersed 序列上的分组
// 长度，求和必须等于 phones.size()。结构是纯值对象，拥有全部数据。
struct MeloPhoneChunk {
  std::vector<std::int32_t> phones;
  std::vector<std::int32_t> tones;
  std::vector<std::int32_t> languages;
  std::vector<std::size_t> word_phone_counts;
  std::size_t unknown_units = 0;
};

// 线程与资源：create 加载文件并构造不可变映射；convert 只读，同一对象可被多个
// 线程只读共享。对象拥有标准库容器，不创建线程、文件句柄或设备句柄；析构释放
// 全部映射内存。错误语义：路径不可读/为空、token 表缺关键项、lexicon 全无有效
// 行都返回结构化失败；单行坏数据被跳过，不影响剩余条目。
class MeloTextFrontend {
 public:
  // 从 lexicon_path 与 tokens_path 加载映射。两个路径都不允许为空；加载失败
  // 返回 kInvalidInput 或 kBackendFailure，不留下部分构造对象。
  static domain::Result<MeloTextFrontend> create(const std::string& lexicon_path,
                                                 const std::string& tokens_path);

  MeloTextFrontend() = default;

  // 把 text 转成若干 MeloPhoneChunk。max_encoder_phones 是 interspersed phone
  // 数的上限，必须 >= 3 且为奇数语义（实现会取 (n-1)/2 作为实际 phone 预算）。
  // 空文本、全空白或预算非法返回 kInvalidInput；合法文本可能产生多个片段，
  // 拼接所有片段的 phone 预算后仍保持原本的发音内容。
  domain::Result<std::vector<MeloPhoneChunk>> convert(
      const std::string& text,
      std::size_t max_encoder_phones) const;

  // 只读诊断：已加载的有效 lexicon 条目数。用于证据和测试，不参与业务分支。
  std::size_t lexicon_entry_count() const noexcept { return lexicon_.size(); }

 private:
  struct Unit {
    std::vector<std::int32_t> phones;
    std::vector<std::int32_t> tones;
  };

  // 加载 token 表：token 字符串 -> 非负整数 id。关键 token 缺失时返回失败。
  static domain::Result<MeloTextFrontend> load(const std::string& lexicon_path,
                                               const std::string& tokens_path);

  std::unordered_map<std::string, Unit> lexicon_;
  std::unordered_map<std::string, std::int32_t> token_ids_;
  std::int32_t blank_id_ = 0;
  std::int32_t unk_id_ = 0;
  std::size_t max_key_codepoints_ = 1;
  bool loaded_ = false;
};

}  // namespace nexweave::backend
