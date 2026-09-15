#include "../test_support.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "fake_rag.hpp"

using namespace nexweave;
namespace {

// 成功不变量：命中只包含查询相关条目，按分数降序、同分 id 字典序稳定返回。
// 同时覆盖 direct/context 阈值边界，证明分数只驱动确定性分支而不是概率结论。
void TestRetrievalAndThresholdRoutes() {
  backend::FakeRag rag({{"z", "灯光亮度设置为百分之五十", 0.5},
                        {"a", "灯光亮度设置为百分之五十", 0.9}});
  const auto hits = rag.retrieve("灯光", 2);
  CHECK(hits.ok());
  CHECK(hits.value->size() == 2);
  CHECK((*hits.value)[0].id == "a");
  CHECK((*hits.value)[1].id == "z");

  backend::FakeRagRouter router(rag);
  const auto l0 = router.route("停止");
  CHECK(l0.ok());
  CHECK(l0.value->level == backend::RagRouteLevel::kL0);
  CHECK(l0.value->hits.empty());
  const auto l1 = router.route("灯光");
  CHECK(l1.ok());
  CHECK(l1.value->level == backend::RagRouteLevel::kL1);
  CHECK(l1.value->direct_answer == "灯光亮度设置为百分之五十");

  backend::FakeRag context({{"ctx", "温度", 0.5}});
  backend::FakeRagRouter context_router(context);
  CHECK(context_router.route("温度").value->level == backend::RagRouteLevel::kL2);
  CHECK(context_router.route("未知").value->level == backend::RagRouteLevel::kL3);
}

// 非法输入不消费夹具；空白、零 top_k、非法记录和非法阈值都必须有明确结果。
// 取消后旧轮次封锁，reset 后同一输入可重复得到同样的成功结果。
void TestFailuresBoundariesAndReplay() {
  backend::FakeRag rag({{"id", "query", 0.5}});
  CHECK(rag.retrieve("", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve("   ", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve("query", 0).ok());
  CHECK(rag.retrieve("absent", 1).value->empty());

  bool invalid_chunk = false;
  try {
    backend::FakeRag invalid({{"", "text", 0.1}});
  } catch (const std::invalid_argument&) {
    invalid_chunk = true;
  }
  CHECK(invalid_chunk);

  bool invalid_config = false;
  try {
    backend::FakeRagRouter invalid_router(
        rag, backend::FakeRagRouter::Config{0.4, 0.5, 1});
  } catch (const std::invalid_argument&) {
    invalid_config = true;
  }
  CHECK(invalid_config);

  backend::FakeRagRouter router(rag);
  CHECK(router.cancel().ok());
  CHECK(router.route("query").error.code == domain::ErrorCode::kCancelled);
  CHECK(router.reset().ok());
  CHECK(router.route("query").ok());
}

}  // namespace

// 同分排序不变量：分数相同时按 id 字典序返回，结果与夹具里的写入顺序无关，top_k 截断
// 发生在排序之后。此前没有任何同分夹具，改坏 tie-break 全量回归也不会红。
void TestEqualScoresBreakTiesByLexicographicId() {
  backend::FakeRag rag({{"z", "灯光亮度", 0.5},
                        {"a", "灯光亮度", 0.5},
                        {"m", "灯光亮度", 0.5}});
  const auto hits = rag.retrieve("灯光", 3);
  CHECK(hits.ok());
  CHECK(hits.value->size() == 3);
  CHECK((*hits.value)[0].id == "a");
  CHECK((*hits.value)[1].id == "m");
  CHECK((*hits.value)[2].id == "z");

  // 反转写入顺序后结果必须一致：排序结果不能依赖夹具的书写顺序。
  backend::FakeRag reversed({{"m", "灯光亮度", 0.5},
                             {"z", "灯光亮度", 0.5},
                             {"a", "灯光亮度", 0.5}});
  const auto again = reversed.retrieve("灯光", 3);
  CHECK(again.ok());
  CHECK(again.value->size() == 3);
  for (std::size_t index = 0; index < 3; ++index) {
    CHECK((*again.value)[index].id == (*hits.value)[index].id);
  }

  // 截断发生在排序之后：留下的是字典序最小的两条，而不是最先写进去的两条。
  const auto limited = rag.retrieve("灯光", 2);
  CHECK(limited.ok());
  CHECK(limited.value->size() == 2);
  CHECK((*limited.value)[0].id == "a");
  CHECK((*limited.value)[1].id == "m");
}


// 保护不变量：路由器只把能完整装入显式上下文预算的命中交给 L2；一条都装不下时改走
// L3 且不携带命中，不能把空上下文或越界上下文交给模型。L0 使用可配置的精确控制词，
// 包含控制词的完整知识问题不会被误判为控制意图。
void TestConfigurableControlAndContextBudget() {
  backend::FakeRag rag({{"long", "query plus a lot of bytes", 0.6},
                        {"short", "query ok", 0.6}});

  backend::FakeRagRouter::Config fitting;
  fitting.direct_answer_threshold = 0.9;
  fitting.context_threshold = 0.5;
  fitting.top_k = 2;
  fitting.max_context_bytes = 10;
  fitting.l0_keywords = {"halt"};
  backend::FakeRagRouter fitting_router(rag, fitting);
  const auto fitted = fitting_router.route("query");
  CHECK(fitted.ok());
  CHECK(fitted.value->level == backend::RagRouteLevel::kL2);
  CHECK(fitted.value->hits.size() == 1);
  CHECK(fitted.value->hits.front().id == "short");
  CHECK(fitted.value->hits.front().text.size() <= fitting.max_context_bytes);
  CHECK(fitting_router.route("halt").value->level == backend::RagRouteLevel::kL0);
  CHECK(fitting_router.route("取消后旧数据怎么处理").value->level !=
        backend::RagRouteLevel::kL0);

  backend::FakeRagRouter::Config too_small = fitting;
  too_small.max_context_bytes = 1;
  backend::FakeRagRouter too_small_router(rag, too_small);
  const auto no_context = too_small_router.route("query");
  CHECK(no_context.ok());
  CHECK(no_context.value->level == backend::RagRouteLevel::kL3);
  CHECK(no_context.value->hits.empty());
  CHECK(no_context.value->reason.find("context byte budget") != std::string::npos);

  bool invalid_budget = false;
  try {
    backend::FakeRagRouter invalid(rag, backend::FakeRagRouter::Config{0.9, 0.5, 1, 0});
  } catch (const std::invalid_argument&) {
    invalid_budget = true;
  }
  CHECK(invalid_budget);

  bool invalid_keyword = false;
  try {
    backend::FakeRagRouter invalid(rag,
                                   backend::FakeRagRouter::Config{0.9, 0.5, 1, 8, {"  "}});
  } catch (const std::invalid_argument&) {
    invalid_keyword = true;
  }
  CHECK(invalid_keyword);
}

int main() {
  TestRetrievalAndThresholdRoutes();
  TestEqualScoresBreakTiesByLexicographicId();
  TestFailuresBoundariesAndReplay();
  TestConfigurableControlAndContextBudget();
}
