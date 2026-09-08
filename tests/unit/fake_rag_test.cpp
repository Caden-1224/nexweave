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

int main() {
  TestRetrievalAndThresholdRoutes();
  TestFailuresBoundariesAndReplay();
}
