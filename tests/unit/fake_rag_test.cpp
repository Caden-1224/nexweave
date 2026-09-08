#include "../test_support.hpp"
#include "fake_rag.hpp"
using namespace nexweave;
namespace {
void TestRetrievalAndRoutes() {
  backend::FakeRag rag({{"z", "灯光亮度设置为百分之五十", 0.5},
                        {"a", "灯光亮度设置为百分之五十", 0.9}});
  const auto hits = rag.retrieve("灯光", 2);
  CHECK(hits.ok());
  CHECK(hits.value->size() == 2);
  CHECK((*hits.value)[0].id == "a");
  CHECK((*hits.value)[1].id == "z");
  backend::FakeRagRouter router(rag);
  const auto l0 = router.route("停止");
  CHECK(l0.ok() && l0.value->level == backend::RagRouteLevel::kL0);
  CHECK(l0.value->hits.empty());
  const auto l1 = router.route("灯光");
  CHECK(l1.ok() && l1.value->level == backend::RagRouteLevel::kL1);
  CHECK(l1.value->direct_answer == "灯光亮度设置为百分之五十");
  backend::FakeRag context({{"ctx", "温度", 0.5}});
  backend::FakeRagRouter context_router(context);
  CHECK(context_router.route("温度").value->level == backend::RagRouteLevel::kL2);
  CHECK(context_router.route("未知").value->level == backend::RagRouteLevel::kL3);
}
void TestFailuresAndReplay() {
  backend::FakeRag rag({{"id", "query", 0.5}});
  CHECK(rag.retrieve("", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve("query", 0).ok());
  CHECK(rag.retrieve("absent", 1).value->empty());
  backend::FakeRagRouter router(rag);
  CHECK(router.cancel().ok());
  CHECK(router.route("query").error.code == domain::ErrorCode::kCancelled);
  CHECK(router.reset().ok());
  CHECK(router.route("query").ok());
}
}
int main() {
  TestRetrievalAndRoutes();
  TestFailuresAndReplay();
}
