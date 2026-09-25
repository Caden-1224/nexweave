// MeloTTS 文本前端单元测试：保护中英混合 token 化、最长词典匹配、词典外缩写
// 字母回退、词条超预算拆分和空输入错误语义。测试使用临时小词表，不加载真实模型。
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "../test_support.hpp"
#include "melotts/melotts_text_frontend.hpp"

using namespace nexweave;

namespace {

std::string g_temp_counter = "0";

struct TempFile {
  std::string path;

  explicit TempFile(std::string file_path) : path(std::move(file_path)) {}
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

TempFile WriteTempFile(const std::string& suffix, const std::string& content) {
  const std::string name = "nexweave_melotts_" +
                           std::to_string(static_cast<long>(::getpid())) + "_" +
                           g_temp_counter + suffix;
  g_temp_counter = std::to_string(std::stoul(g_temp_counter) + 1U);
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("无法创建测试临时文件");
  }
  output << content;
  output.close();
  return TempFile(path.string());
}

std::string TokensFixture() {
  return "_ 0\n"
         "UNK 1\n"
         "b 2\n"
         "ang 3\n"
         "g 4\n"
         "ey 5\n"
         "t 6\n"
         "w 7\n"
         ", 8\n"
         "r 9\n"
         "k 10\n"
         "l 11\n"
         "m 12\n";
}

std::string LexiconFixture() {
  return "帮 b ang 1 1\n"
         "gateway g ey t w ey 7 9 7 7 10\n"
         "， , 0\n"
         "r r 9\n"
         "k k 7\n"
         "l l 9\n"
         "m m 10\n";
}

backend::MeloTextFrontend MakeFrontend() {
  TempFile tokens = WriteTempFile(".tokens", TokensFixture());
  TempFile lexicon = WriteTempFile(".lexicon", LexiconFixture());
  auto loaded = backend::MeloTextFrontend::create(lexicon.path, tokens.path);
  CHECK(loaded.ok());
  return std::move(*loaded.value);
}

std::size_t CountNonBlank(const std::vector<std::int32_t>& phones) {
  std::size_t count = 0;
  for (const std::int32_t phone : phones) {
    if (phone != 0) {
      ++count;
    }
  }
  return count;
}

void TestMixedChineseEnglish() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  auto converted = frontend.convert("帮 gateway", 240U);
  CHECK(converted.ok());
  CHECK(converted.value->size() == 1U);
  const backend::MeloPhoneChunk& chunk = converted.value->front();
  CHECK(chunk.phones.size() == 15U);
  CHECK(chunk.tones.size() == 15U);
  CHECK(chunk.languages.size() == 15U);
  CHECK(chunk.word_phone_counts.size() == 2U);
  CHECK(chunk.word_phone_counts[0] == 5U);
  CHECK(chunk.word_phone_counts[1] == 10U);
  CHECK(chunk.phones[0] == 0);
  CHECK(chunk.phones[1] == 2);
  CHECK(chunk.phones[3] == 3);
  CHECK(chunk.phones[5] == 4);
  CHECK(chunk.phones[7] == 5);
  CHECK(chunk.phones[9] == 6);
  CHECK(chunk.phones[11] == 7);
  CHECK(chunk.phones[13] == 5);
  CHECK(chunk.tones[1] == 1);
  CHECK(chunk.tones[3] == 1);
  CHECK(chunk.tones[5] == 7);
  CHECK(chunk.tones[7] == 9);
  CHECK(chunk.tones[9] == 7);
  CHECK(chunk.tones[11] == 7);
  CHECK(chunk.tones[13] == 10);
  CHECK(chunk.unknown_units == 0U);
  for (std::int32_t language : chunk.languages) {
    CHECK(language == 3);
  }
}

void TestUnknownAcronymLetterFallback() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  auto converted = frontend.convert("RKLLM", 240U);
  CHECK(converted.ok());
  CHECK(converted.value->size() == 1U);
  const backend::MeloPhoneChunk& chunk = converted.value->front();
  CHECK(chunk.word_phone_counts.size() == 5U);
  CHECK(chunk.word_phone_counts[0] == 3U);
  for (std::size_t index = 1; index < chunk.word_phone_counts.size(); ++index) {
    CHECK(chunk.word_phone_counts[index] == 2U);
  }
  CHECK(CountNonBlank(chunk.phones) == 5U);
  CHECK(chunk.unknown_units == 0U);
}

void TestChunkingPreservesAllPhonemes() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  auto converted = frontend.convert("帮 gateway 帮 gateway 帮", 15U);
  CHECK(converted.ok());
  CHECK(converted.value->size() == 3U);
  std::size_t total_phone_count = 0;
  for (const backend::MeloPhoneChunk& chunk : *converted.value) {
    CHECK(chunk.phones.size() <= 15U);
    total_phone_count += CountNonBlank(chunk.phones);
  }
  CHECK(total_phone_count == 16U);
}

void TestOversizedUnitIsSplitNotTruncated() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  auto converted = frontend.convert("gateway", 7U);
  CHECK(converted.ok());
  CHECK(converted.value->size() == 2U);
  std::size_t total_phone_count = 0;
  for (const backend::MeloPhoneChunk& chunk : *converted.value) {
    total_phone_count += CountNonBlank(chunk.phones);
  }
  CHECK(total_phone_count == 5U);
}

// 词典外非 ASCII 码点必须显式回退为 UNK，不能被静默丢弃；emoji 与生僻字
// 在本夹具中没有 lexicon 条目，因此各产生一个 UNK unit，且 chunk 仍可合成。
void TestUnsupportedCharactersBecomeUnk() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  const std::string text = "𠮷😀";
  auto converted = frontend.convert(text, 240U);
  CHECK(converted.ok());
  CHECK(converted.value->size() == 1U);
  const backend::MeloPhoneChunk& chunk = converted.value->front();
  CHECK(CountNonBlank(chunk.phones) == 2U);
  CHECK(chunk.unknown_units == 2U);
  CHECK(chunk.word_phone_counts.size() == 2U);
}

// 超长词典外英文串按字母回退后必须完整切块，不能因超出 phone 预算而静默截断；
// 这里用全部在单字母表内的 k 构造 300 个 unit，验证总音素数守恒且产生多个 chunk。
void TestLongUnknownWordIsNotTruncated() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  const std::string text(300U, 'k');
  auto converted = frontend.convert(text, 15U);
  CHECK(converted.ok());
  CHECK(converted.value->size() > 1U);
  std::size_t total_phone_units = 0;
  std::size_t total_non_blank_phones = 0;
  for (const backend::MeloPhoneChunk& chunk : *converted.value) {
    total_phone_units += chunk.word_phone_counts.size();
    total_non_blank_phones += CountNonBlank(chunk.phones);
  }
  CHECK(total_phone_units == text.size());
  CHECK(total_non_blank_phones == text.size());
}

void TestEmptyAndInvalidInput() {
  backend::MeloTextFrontend frontend = MakeFrontend();
  CHECK(frontend.convert("", 240U).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(frontend.convert("帮", 1U).error.code == domain::ErrorCode::kInvalidInput);

  auto missing = backend::MeloTextFrontend::create("/tmp/does-not-exist-lexicon.txt",
                                                  "/tmp/does-not-exist-tokens.txt");
  CHECK(!missing.ok());
  CHECK(missing.error.code == domain::ErrorCode::kInvalidInput);
}

}  // namespace

int main() {
  TestMixedChineseEnglish();
  TestUnknownAcronymLetterFallback();
  TestChunkingPreservesAllPhonemes();
  TestOversizedUnitIsSplitNotTruncated();
  TestUnsupportedCharactersBecomeUnk();
  TestLongUnknownWordIsNotTruncated();
  TestEmptyAndInvalidInput();
  return 0;
}
