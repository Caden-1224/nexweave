#include <stdexcept>
#include <string>
#include <vector>
#include "../test_support.hpp"
#include "fake_llm.hpp"
using namespace nexweave;
namespace {
void Ordered() { backend::FakeLlm llm({"你好", "，世界"}); std::vector<capability::TextEvent> e; CHECK(llm.set_callback([&](const auto& x){e.push_back(x);}).ok()); CHECK(llm.generate("问候").ok()); CHECK(e.size()==3 && e[0].kind==capability::TextEventKind::kToken && e[2].kind==capability::TextEventKind::kDone); }
void Cancel() { backend::FakeLlm llm({"答"}); std::vector<capability::TextEvent> e; CHECK(llm.generate("问").error.code==domain::ErrorCode::kInvalidInput); CHECK(llm.set_callback([&](const auto& x){e.push_back(x);}).ok()); CHECK(llm.generate("").error.code==domain::ErrorCode::kInvalidInput); CHECK(llm.cancel().ok()); CHECK(llm.generate("问").error.code==domain::ErrorCode::kCancelled); CHECK(llm.set_callback([&](const auto& x){e.push_back(x);}).ok()); CHECK(llm.generate("问").ok()); }
void CallbackFailure() { backend::FakeLlm llm({"答"}); CHECK(llm.set_callback([](const auto&){ throw std::runtime_error("x"); }).ok()); bool threw=false; try {(void)llm.generate("问");} catch(const std::runtime_error&){threw=true;} CHECK(threw); CHECK(llm.generate("问").error.code==domain::ErrorCode::kCancelled); }
}
int main(){ Ordered(); Cancel(); CallbackFailure(); }
