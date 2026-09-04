#include "session_state_machine.hpp"
#include <cstdio>
#include <thread>
#include <vector>
namespace { int failures = 0;
#define CHECK(value) do { if (!(value)) { std::printf("FAIL: %s\n", #value); ++failures; } } while (0)
// 成功不变量：L0/L1 直答绕过 Thinking，最终回到 Idle。
void TestMainPaths() { using M=nexweave::runtime::SessionStateMachine; M m; CHECK(m.dispatch(M::Event::kAudioStart).ok()); CHECK(m.dispatch(M::Event::kAsrFinal).ok()); CHECK(m.dispatch(M::Event::kRouteL0L1).ok()); CHECK(m.dispatch(M::Event::kTtsDone).ok()); CHECK(m.state()==M::State::kIdle); const auto t=m.trace(); CHECK(t.size()==4 && t[2]=="routing--route_l0_l1-->speaking"); }
// 成功不变量：L2/L3 必须经过 Thinking 后才能进入 Speaking。
void TestThinkingPath() { using M=nexweave::runtime::SessionStateMachine; M m; CHECK(m.dispatch(M::Event::kAudioStart).ok()); CHECK(m.dispatch(M::Event::kAsrFinal).ok()); CHECK(m.dispatch(M::Event::kRouteL2L3).ok()); CHECK(m.state()==M::State::kThinking); CHECK(m.dispatch(M::Event::kLlmDone).ok()); CHECK(m.dispatch(M::Event::kTtsDone).ok()); CHECK(m.state()==M::State::kIdle); }
// 失败/取消不变量：非法事件无副作用，取消必须在 Cancelling 完成后收敛。
void TestInvalidAndCancel() { using M=nexweave::runtime::SessionStateMachine; M m; CHECK(m.dispatch(M::Event::kLlmDone).error.code==nexweave::domain::ErrorCode::kInvalidInput); CHECK(m.trace().empty()); CHECK(!m.dispatch(M::Event::kCancel).ok()); CHECK(m.dispatch(M::Event::kAudioStart).ok()); CHECK(m.dispatch(M::Event::kCancel).ok()); CHECK(m.state()==M::State::kCancelling); CHECK(!m.dispatch(M::Event::kCancel).ok()); CHECK(m.dispatch(M::Event::kCancelComplete).ok()); CHECK(m.state()==M::State::kIdle); }
// 代际不变量：取消前捕获的代际事件必须被拒绝，且不能污染新请求轨迹。
void TestGenerationIsolation() { using M=nexweave::runtime::SessionStateMachine; M m; const auto initial=m.generation(); CHECK(m.dispatch(M::Event::kAudioStart).ok()); const auto active=m.generation(); CHECK(active==initial+1); CHECK(m.dispatch(M::Event::kCancel, active).ok()); const auto cancelled=m.generation(); CHECK(cancelled==active+1); CHECK(!m.dispatch(M::Event::kCancelComplete, active).ok()); CHECK(!m.dispatch(M::Event::kTtsDone, active).ok()); CHECK(m.state()==M::State::kCancelling); m.reset(); CHECK(!m.dispatch(M::Event::kAudioStart, active).ok()); CHECK(m.dispatch(M::Event::kAudioStart, cancelled).ok()); CHECK(m.generation()==cancelled+1); }
void TestConcurrentSnapshots() { using M=nexweave::runtime::SessionStateMachine; M m; std::vector<std::thread> w; for(int i=0;i<4;++i) w.emplace_back([&]{for(int j=0;j<100;++j){(void)m.state();(void)m.state_name();(void)m.trace();}}); for(auto& x:w)x.join(); m.reset(); CHECK(m.state()==M::State::kIdle && m.trace().empty()); }
}
int main(){TestMainPaths();TestThinkingPath();TestInvalidAndCancel();TestGenerationIsolation();TestConcurrentSnapshots();return failures?1:0;}
