#include "backend.hpp"
#include <cstdio>
using namespace nexweave;
namespace { int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL: %s\n", #x); ++failures; } } while (0)
class Fixture final : public capability::IAsr, public capability::IRag, public capability::ILlm, public capability::ITts, public capability::IAudioSource, public capability::IAudioSink { public:
 domain::OperationResult set_callback(capability::TextEventCallback c) override { tcb=std::move(c); return domain::OperationResult::success(); } domain::OperationResult set_callback(capability::AudioEventCallback c) override { acb=std::move(c); return domain::OperationResult::success(); }
 domain::OperationResult feed(const domain::AudioFrame&, bool) override { return domain::OperationResult::success(); } domain::Result<std::vector<capability::RetrievedChunk>> retrieve(const std::string&,std::size_t k) override { std::vector<capability::RetrievedChunk> v; if(k)v.push_back({"id","text",1}); return domain::Result<std::vector<capability::RetrievedChunk>>::success(std::move(v)); }
 domain::OperationResult generate(const std::string&) override{return domain::OperationResult::success();} domain::OperationResult synthesize(const std::string& s) override{return s.empty()?domain::OperationResult::failure(domain::ErrorCode::kInvalidInput):domain::OperationResult::success();} domain::OperationResult cancel() noexcept override{return domain::OperationResult::success();}
 domain::OperationResult open() override{o=true;return domain::OperationResult::success();} domain::Result<domain::AudioFrame> read() override{return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kDeviceFailure); } domain::OperationResult write(const domain::AudioFrame&) override{return o?domain::OperationResult::success():domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure);} domain::OperationResult close() noexcept override{o=false;return domain::OperationResult::success();}
 private: bool o=false; capability::TextEventCallback tcb; capability::AudioEventCallback acb; }; }
// 夹具保护契约级不变量：成功检索可消费载荷；零 top_k、空文本、未打开输出和
// 设备读取失败必须返回可诊断结果。其余生命周期由后续 Fake/硬件契约测试覆盖。
int main(){ Fixture f; const auto empty=f.retrieve("q",0); CHECK(empty.ok()); const auto hit=f.retrieve("q",1); CHECK(hit.ok()); CHECK(hit.value.has_value() && hit.value->size()==1); CHECK(!f.synthesize("").ok()); CHECK(!f.read().ok()); CHECK(!f.write(domain::AudioFrame{}).ok()); return failures?1:0; }
