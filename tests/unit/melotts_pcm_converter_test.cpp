// MeloTtsPcmConverter 单元测试：保护重采样连续性、分块等价、满量程转换与
// 尾部样本计数四组不变量。测试只调用公开值接口，不依赖 SDK、线程或真实时钟。
#include <cstdint>
#include <vector>

#include "../test_support.hpp"
#include "melotts/melotts_pcm_converter.hpp"

using namespace nexweave;

namespace {

void TestInvalidRatesAreInert() {
  backend::MeloTtsPcmConverter converter(0, 16000);
  CHECK(!converter.valid());
  std::vector<std::int16_t> output;
  const float sample = 0.5F;
  converter.push(&sample, 1, output);
  converter.flush(output);
  CHECK(output.empty());
}

// 常量输入在 44.1k -> 16k 下应保持常量；输出样本数由输入时长 floor 决定，
// 说明转换不是改帧头，也不是按固定倍数复制。
void TestConstantDownsampleCount() {
  backend::MeloTtsPcmConverter converter(4, 2);
  CHECK(converter.valid());
  const std::vector<float> input(4, 0.5F);
  std::vector<std::int16_t> output;
  converter.push(input.data(), input.size(), output);
  converter.flush(output);
  CHECK(output.size() == 2U);
  CHECK(output[0] == 16384);
  CHECK(output[1] == 16384);
  CHECK(converter.total_input_samples() == 4U);
  CHECK(converter.total_output_samples() == 2U);
}

// 上采样时最后一个输出样本落在最后一个输入区间内，按零阶保持使用末尾样本；
// 这是 flush 的确定性边界，不能悄悄丢掉最后一个时间区间。
void TestUpsampleHoldsLastSampleAtFlush() {
  backend::MeloTtsPcmConverter converter(2, 4);
  CHECK(converter.valid());
  const std::vector<float> input = {0.0F, 0.25F, 0.5F, 0.75F};
  std::vector<std::int16_t> output;
  converter.push(input.data(), input.size(), output);
  converter.flush(output);
  CHECK(output.size() == 8U);
  CHECK(output[0] == 0);
  CHECK(output[1] == 4096);
  CHECK(output[3] == 12288);
  CHECK(output[6] == 24576);
  CHECK(output[7] == 24576);
}

// 分块送入与一次性送入必须逐样本一致：跨 push 只保留尚未消费的输入样本与
// 下一个输出序号，不保留“当前块本地相位”这类会产生拼接跳变的状态。
void TestChunkedEqualsSinglePush() {
  std::vector<float> input(37, 0.0F);
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<float>(index) / 100.0F;
  }

  backend::MeloTtsPcmConverter single(44100, 16000);
  std::vector<std::int16_t> expected;
  single.push(input.data(), input.size(), expected);
  single.flush(expected);

  backend::MeloTtsPcmConverter chunked(44100, 16000);
  std::vector<std::int16_t> actual;
  chunked.push(input.data(), 3, actual);
  chunked.push(input.data() + 3, 0, actual);
  chunked.push(input.data() + 3, 11, actual);
  chunked.push(input.data() + 14, input.size() - 14, actual);
  chunked.flush(actual);
  CHECK(actual == expected);
}

// 输入超出 [-1,1] 时先裁剪再量化：正负边界使用 int16 两端，0 映射为 0，
// 避免整数溢出和“把非法浮点直接当 PCM”的未定义行为。
void TestClippingAndNan() {
  backend::MeloTtsPcmConverter converter(1, 1);
  const std::vector<float> input = {-2.0F, 0.0F, 2.0F, 1.0F, -1.0F};
  std::vector<std::int16_t> output;
  converter.push(input.data(), input.size(), output);
  converter.flush(output);
  CHECK(output.size() == input.size());
  CHECK(output[0] == -32768);
  CHECK(output[1] == 0);
  CHECK(output[2] == 32767);
  CHECK(output[3] == 32767);
  CHECK(output[4] == -32768);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  backend::MeloTtsPcmConverter nan_converter(1, 1);
  std::vector<std::int16_t> nan_output;
  nan_converter.push(&nan, 1, nan_output);
  nan_converter.flush(nan_output);
  CHECK(nan_output.size() == 1U);
  CHECK(nan_output[0] == 0);
}

}  // namespace

int main() {
  TestInvalidRatesAreInert();
  TestConstantDownsampleCount();
  TestUpsampleHoldsLastSampleAtFlush();
  TestChunkedEqualsSinglePush();
  TestClippingAndNan();
  return 0;
}
