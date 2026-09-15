// NexWeave Mock profile 确定性入口：把「一条命令跑一个可复现场景」固定成一层可复用契约。
//
// 它解决什么问题
// --------------
// 单进程会话应用、会话监督器与请求入口各自都已被验收，但把它们串成
// 「外部控制请求 → 会话执行 → 数据面事件」这条完整链路时，还需要三件外面的事：
// 场景怎么选、同一场景怎么每次都跑出同样的结果、跑完之后谁证明资源已经交还。本文件负责
// 这三件事，并且只负责这三件事——它不实现编排、不实现监督、不实现协议，全部复用已有层次。
//
// 为什么要四个场景而不是一个「跑起来就行」
// ----------------------------------------
// mock profile 的价值是**确定性回归**，而不只是能跑。规模要求正常、慢消费、取消与故障
// 都能被显式选择，因为这四种路径的收敛方式完全不同：正常路径要按顺序交付事件并最终完成；
// 慢消费要触发入口的有界发送与慢客户端关闭，而不是让缓冲随输出增长；取消要证明「受理」与
// 「设备静默」是两件事；故障要证明失败会以明确的错误码与失败退出码收尾，并且仍然执行清理。
// 把它们做成四个显式场景，回归时就不会出现「只跑过正常路径就以为其余三条也成立」。
//
// 场景不是脚本语言的替代品
// ------------------------
// 每个场景是一段**确定性的操作序列**：同一配置重复执行得到逐字节一致的输出。因此场景里没有
// 睡眠、没有轮询超时、没有随机数，也没有依赖真实时间的判断。「慢消费」不是靠等得久，而是靠
// 每次只取走固定的小预算，使发送缓冲的增长由契约而不是时序决定。
//
// 确定性针对什么
// --------------
// 确定性针对事件的**内容与因果顺序**，不要求时间戳逐字相同。本层因此不读时钟做决策、不写入
// 时间戳：输出里出现的每一项要么来自配置，要么来自会话自身已经定局的终态。
//
// 资源归属与生命周期
// ------------------
// 本层创建：一个 Supervisor（它在内部创建**恰好一个**工作线程）、一个 Gateway（它只创建
// 内存对象与连接记录）、一条连接、以及可选的输出文件。这些资源全部在 run_mock_profile()
// 返回之前释放：会话在预算内收敛并退出监督器之后才返回；输出文件在返回之前被删除。
// 因此「返回即没有本进程残留」是一条可断言的不变量，而不是一句希望——MockProfileResult
// 里的 ledger 就是它的观测面（创建的线程数、已 join 的线程数、仍打开的连接数）。
//
// 能力对象的归属：本层在内部建立并持有一整套确定性夹具（见实现文件），因此调用方只需要给出
// 配置，不需要注入后端——“这条命令跑的是哪套能力”因此是构造性质，而不是调用方的自律。
//
// 线程归属
// --------
// 全部控制入口（feed/flush/status）在调用 run_mock_profile() 的线程上串行执行，与 Supervisor
// 的工作线程之间只通过监督器的锁与条件变量交接状态。本层不额外创建线程，也不要求调用方
// 加锁；同一份运行同时只允许一次 run_mock_profile()。
//
// 已知限制
// --------
//   - 每次 run_mock_profile() 建立一次全新会话，因此“重复运行”的验证方式是重复执行命令，
//     而不是重复调用它；会话内的常驻输入采集只能开始一次（见 SessionApp 的重复运行说明）。
//   - v1 的控制响应没有承载状态载荷的字段，因此本层只断言受理结果与数据事件，不解析状态详情。
//   - 运行清单里与机器相关的字段（git commit、编译器版本、时间戳）留给后续证据采集填写；
//     本层只填写自己真正知道的部分，其余保持空串而不是编造一个看起来像证据的值。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../domain/error.hpp"

namespace nexweave::app {

// Mock profile 可显式选择的场景。数值不参与线协议，但**不是**自由可重排的：任务身份由它派生
// （见实现文件的场景身份函数），因此新增场景只能往后追加。字符串名是命令行与证据里
// 使用的稳定标识：历史结果与发布证据都按名称关联，因此已发布的名称不得改动。
enum class MockProfileScenario : std::uint8_t {
  // 正常：一次创建被受理，会话执行到成功收敛，事件按发生顺序交付，最后一条终态 end=true。
  kNormal = 0,
  // 慢消费：客户端取走字节的速度跟不上产出，入口的有界发送缓冲达到上限后关闭连接。
  // 它验证的是「缓冲不会随输出无界增长」，而不是「等得久一点会不会好」。
  kSlowConsumer = 1,
  // 取消：会话执行期间受理停止。取消是正常语义：退出码仍为 0，但终态是取消而不是成功，
  // 并且「已写出的帧保留」与「未播出的部分丢弃」两件事都要能从证据里读出来。
  kCancel = 2,
  // 故障：输入在会话开始任何轮次之前就不可用（固定音频路径缺失）。会话以明确错误收敛、
  // 事件流给出失败终态、进程以失败退出码收尾，同时仍然执行同一套清理。
  kFault = 3,
};

// 场景的稳定标识。未知值返回空串（与本项目其他 to_string 重载一致），不抛异常、不分配。
const char* to_string(MockProfileScenario scenario) noexcept;

// 从稳定标识解析场景。非法名称返回 false 且不修改输出参数，使“命令行写错场景名”表现为
// 一次配置错误，而不是悄悄回退到某个默认场景。
bool parse_mock_profile_scenario(std::string_view name, MockProfileScenario& scenario) noexcept;

// 场景固定使用的标识前缀。创建请求、任务与会话身份都由它派生，因此同一场景重复执行时
// 身份完全一致；显式传入 stream_id 时以调用方的值为准。
const char* mock_profile_scenario_slug(MockProfileScenario scenario) noexcept;

// 一次 Mock profile 运行的配置。全部字段都是显式策略：默认值即“进程内 Fake 组合”，
// 任何需要真实硬件或网络的组合都不属于本层。
struct MockProfileConfig {
  MockProfileScenario scenario = MockProfileScenario::kNormal;
  // 会话与输入流身份。必须非空：它是输入流与创建请求的共同归属来源，中途更换会让结果
  // 串到另一条流上。校验失败返回 kInvalidInput，且不建立任何会话。
  std::string stream_id = "mock-normal";
  // 可选的输出目录。为空表示不写任何文件，只在内存里给出结果（测试与进程内调用用）。
  // 非空时：目录不存在则创建，运行结束时目录内本次运行产生的文件被删除。目录本身保留，
  // 因为调用方可能同时持有它。
  std::string output_dir;

  // ---- 场景旋钮：把「打断发生在哪一步」从场景定义里拆出来 ----
  //
  // 场景决定**形态**（该不该成、该不该出现失败、该不该关闭连接），旋钮决定**时序**
  // （停在第几次输出、每次取走多少字节）。拆开之后，同一条不变量可以在不同打断点上回归，
  // 而不需要为每个时机各写一个场景。
  //
  // 取消场景：等播放组件**确实写出** n 帧之后再受理停止。n 必须大于 0。
  //
  // 为什么以“写出多少帧”而不是墙钟为界：这条路径必须能在没有真实时间前进的机器上复现，
  // 而墙钟会让“停在第几帧”随负载飘移。写出的帧数由播放组件自己记账，是已经发生的事实，
  // 因此“等到了”与“没等到”可以被明确区分。
  //
  // 为什么不接受 0（“不等输出就停”）：确定性夹具跑完一轮只要不到一毫秒，不设等待就意味着
  // 取消请求要和会话收尾抢时序——同一份配置时而打断成功、时而打在收尾之后，场景本身就不再
  // 可复现。要表达“用户立刻喊停”，用 n=1（停在下第一帧的瞬间）即可，它同样在会话出声之前
  // 完成受理。非取消场景必须为 0（写了非 0 值即配置错误），否则“场景”与“实际行为”就会脱钩。
  std::size_t cancel_after_pcm_events = 0;
  // 慢消费场景：每次 flush 最多取走的字节数。取值必须小于任何一条事件的编码长度，否则一次
  // 取走就可能把队列排空，慢客户端路径永远不会触发——那种“测试通过”并不证明契约成立。
  // 非慢消费场景必须为 0。
  std::size_t drain_budget_bytes = 0;

  // 固定音频输入路径的覆盖值。只有故障场景的默认策略会读它（默认指向一个不存在的路径，
  // 使"输入不可用"不依赖本机文件系统内容）。允许覆盖的用途是构造**形态不符**的夹具：
  // 把同一场景指向一个真实存在的合法音频，会话就会正常完成，于是"场景声明的形态"与
  // "实际跑出的形态"不一致——这正是失败退出码要覆盖的路径。为空表示用场景默认值。
  std::string wav_path;
};

// 配置的静态校验：只读配置，不做任何 I/O，不创建目录、不建立会话。场景标识必须合法、
// stream_id 必须非空、旋钮必须与场景自洽。成功不改变输入；失败返回 kInvalidInput，调用方
// 应在建立监督器之前把它当成配置错误处理（与 Supervisor/Gateway 的校验入口保持同一种用法）。
domain::OperationResult validate_mock_profile_config(const MockProfileConfig& config);

// 一个场景预期观察到的因果形态。它只描述**外部可见**的事实，不涉及任何内部字段：
// 每一条都是“这条命令应该交付什么”的验收判据，因此同时可以当成场景文档来读。
struct MockProfileExpectation {
  // 创建请求是否被受理（start 响应 ok）。
  bool session_accepted = true;
  // 客户端是否收到了成功终态（一条 end=true 的 done 事件）。
  //
  // 注意它描述的是"交付"，不是"会话在设备上是否跑成了"：入口的有界发送达到上限时会关闭
  // 连接，此后产生的终态事件没有接收者，会被计入入口的丢弃账目而不是硬塞进已关闭的连接。
  // 因此慢消费场景的该项为假，而会话本身仍然正常收敛——两件事必须分开观测，否则"发不完"
  // 会被误读成"会话失败"。
  bool session_completed = true;
  // 事件流是否给出了失败终态（终态事件是 error 且 end=true）。
  bool failure_event = false;
  // 是否存在被取消的轮次（局部 error 事件或取消终态事件）。
  bool cancellation_observed = false;
  // 会话是否确实把音频写给了播放组件。注意它与“线上是否交付了逐帧音频”不是同一件事：
  // v1 的请求入口只交付逐轮文本与终态，逐帧 PCM 下行属于后续传输任务，因此本项断言的是
  // “回答产生了音频”，而不是“客户端收到了音频”。
  bool audio_rendered = true;
  // 连接是否被服务端主动关闭。慢消费场景为真：入口用关闭表达“我发不完了”。
  bool connection_closed_by_server = false;
  // 连接关闭原因是否为慢客户端。只有慢消费场景为真。
  bool slow_client_close = false;
  // 是否至少有一条轮次文本（token）事件。
  bool text_delivered = true;
};

// 返回一个场景的预期形态。纯函数：不读时钟、不访问外部资源，可从任意线程调用。
MockProfileExpectation mock_profile_expectation(MockProfileScenario scenario) noexcept;

// 运行账目：本层创建与释放的资源的对账依据。
//
// 它存在的理由：验收条件要求“任一场景失败仍执行清理”，而“清理了”必须能被观测，不能靠
// 实现者保证。这里记录的是创建者自己的账目——谁创建、谁释放、还差多少，而不是外部猜测。
struct MockProfileLedger {
  // 监督器为会话创建的工作线程数。一次会话恰好一个；没有受理过创建时为 0。
  std::uint64_t threads_created = 0;
  // 已经在返回之前 join 的线程数。运行正常结束时它与 threads_created 相等。
  std::uint64_t threads_joined = 0;
  // 因超出清理预算而被放弃等待的工作线程数（监督器析构会 detach）。正常收敛时为 0；
  // 非 0 表示“返回后仍可能有后台线程”，必须按资源故障处理。
  std::uint64_t threads_detached = 0;
  std::uint64_t connections_opened = 0;
  // 仍然打开的连接数。返回前必须为 0。
  std::uint64_t connections_open = 0;
  // 输出目录里本次运行产生的文件数（未配置输出目录时为 0）。
  std::size_t artifacts_written = 0;
  // 输出文件是否已经在返回之前全部删除。未配置输出目录时为真（没有可残留的东西）。
  bool artifacts_removed = true;
  // 配置阶段是否合法。为假时其余字段描述的是“一次被拒绝的运行”。
  bool config_valid = true;

  // 资源是否全部交还。它是本层对“返回即无残留”的唯一判定，供调用方与门禁直接使用。
  bool quiesced() const noexcept {
    return threads_detached == 0 && threads_joined == threads_created &&
           connections_open == 0 && artifacts_removed;
  }
};

// 运行结果：场景标识、预期、实际观察与账目。字段刻意分开而不是压成一个成功布尔，因为
// 调用方需要区分“场景没跑成”“跑成了但形态不符”“跑成了但资源没交还”三类结论。
struct MockProfileResult {
  MockProfileScenario scenario = MockProfileScenario::kNormal;
  std::string scenario_name;
  // 运行级错误：配置非法、无法建立会话、清理未完成、取消没有被受理。单轮失败不写这里。
  domain::Error error{};
  // 输出的逐行报文。每项都是**一整行**（不含换行符），顺序即交付顺序：
  // 以 "response " 开头的是控制响应，以 "event " 开头的是数据面事件，以 "metric " 开头的是
  // 指标。三种记录的编码都来自协议层与可观测层，本层不另造一套线格式。
  std::vector<std::string> records;

  // 观察到的因果形态。判定规则见 MockProfileExpectation 与 mock_profile_expectation()。
  MockProfileExpectation observed;
  // 上面每一项是否与预期一致。任何一项为假即“形态不符”。
  bool expectation_matched = true;
  // 第一处不符的字段名（例如 "session_completed"）；一致时为空串。
  std::string mismatch;

  std::size_t response_count = 0;
  std::size_t event_count = 0;
  std::size_t terminal_event_count = 0;
  std::size_t token_event_count = 0;
  // 会话写进播放组件的帧数。它是“回答确实出了声”的证据，来源是最近一次已收敛会话的运行
  // 记录，因此不受“线上是否交付 PCM 事件”影响（v1 不交付）。
  std::size_t rendered_frames = 0;
  // 受理停止那一刻，播放组件已经写出的帧数（非取消场景为 0）。它把"打断发生在已经出声
  // 之后"从一个时序主张变成一条可核对的事实：没有它，取消用例只能证明"会话被取消了"，
  // 无法证明"取消时确实已经有音频"。
  std::size_t cancel_frames = 0;
  domain::Error terminal_error{};
  std::string close_reason;
  MockProfileLedger ledger;

  // 进程退出码：0 表示本次运行按契约收敛（含“取消”与“故障场景按预期失败”），
  // 1 表示运行本身没有按契约完成（配置错误、形态不符、资源未交还）。取消是正常语义，
  // 因此取消场景的成功退出码同样是 0，它的“没成功”体现在终态事件里而不是退出码上。
  int exit_code = 0;

  // 机器可读的汇总。确定性：字段顺序固定、不含时间戳、不含绝对路径。
  std::string summary_json;
  // 运行清单。与上面同一份事实，用于关联配置与输入；具体字段的填写责任见文件头注释。
  std::string manifest_json;
};

// 运行一个 Mock profile 场景。
//
// 输入前提：config 已通过 validate_mock_profile_config()；调用方线程持有本函数直到返回。
// 输出后置：返回时监督器已退出、工作线程已 join、连接已关闭、输出目录里的本次产物已删除；
// 这些事实由 result.ledger 如实报告，不由返回值隐含。
//
// 失败语义：不抛出业务异常。可恢复的失败（配置非法、场景形态不符、取消未被受理）以
// error/mismatch 与 exit_code 表达；只有标准库分配失败会向上传播。
//
// 阻塞与截止时间：本函数会等待会话收敛与监督器清理，等待上界由监督器配置（默认每个 5 秒）
// 决定，因此不会无限阻塞。它在等待期间只做有界轮询与连接读取，不睡眠固定时长。
MockProfileResult run_mock_profile(const MockProfileConfig& config);

}  // namespace nexweave::app