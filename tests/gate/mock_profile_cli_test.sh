#!/usr/bin/env bash
# Mock profile 命令行门禁。
#
# 职责：验收"一条命令跑一个可复现场景"这件事在真实进程边界上的行为——四个场景都能被显式
# 选择、每个场景的退出码语义正确、产物无残留、二进制不依赖任何硬件库。断言只读取进程的
# 退出码与 stdout 上的汇总行，因此不依赖私有实现，也不依赖构建目录里的中间产物。
#
# 保护的不变量：
#   1. 参数错误（未知场景、未知开关）以配置错误退出，不建立任何会话。
#   2. 四个场景都能显式选择；按契约完成的运行退出码为 0，包括"取消"与"预期内的故障"。
#      "预期内的故障"退出 0 不是把失败说成成功：会话失败由事件流里的失败终态回答，退出码
#      回答的是"这条命令有没有按本次验收的目标跑完"。两者混成一个数字就无法区分
#      "按预期失败"与"根本没跑起来"。
#   3. 同一命令重复运行得到逐字节一致的输出（汇总行 + 事件流），因为场景里没有时钟、没有
#      睡眠、没有随机数。
#   4. 输出目录里的产物在命令结束前被删除，目录本身保留；运行账目自证资源已交还
#      （quiesced=true、已创建线程数等于已 join 线程数、仍打开的连接数为 0）。
#   5. 慢消费由有界发送缓冲触发连接关闭，而不是靠等待；关闭原因如实报告为 slow_client。
#   6. 默认构建的 mock profile 二进制不链接任何 NPU SDK 或声卡库。
#
# 夹具：本脚本只在系统临时目录下创建自己的输出目录，由 trap 在退出时删除；不修改源码，
# 不后台启动进程。所有断言读的是被测进程自己的输出。
set -uo pipefail

APP="${1:?需要 mock profile 可执行文件路径}"
if [ ! -x "$APP" ]; then
  echo "FAIL: 应用不可执行: $APP"
  exit 1
fi

failures=0
work="$(mktemp -d /tmp/nexweave-mock-gate.XXXXXX)"
trap 'rm -rf -- "$work"' EXIT

fail() {
  echo "FAIL: $1"
  failures=$((failures + 1))
}

RC=0
run() {
  "$@" >"$work/last.log" 2>&1
  RC=$?
}

expect_rc() {
  if [ "$RC" -ne "$1" ]; then
    fail "$2（期望退出码 $1，实际 $RC）"
    sed -n '1,6p' "$work/last.log"
  fi
}

expect_match() {
  if ! grep -q -- "$2" "$3"; then
    fail "$1"
  fi
}

expect_no_match() {
  if grep -q -- "$2" "$3"; then
    fail "$1"
  fi
}

# 汇总行是最后一行：前面是横幅与逐行报文，只有汇总行以 { 开头且独占一行。
summary() {
  grep '^{' "$1" | tail -1
}

# 从汇总行文本里取一个扁平字段值（字符串或数字）。输入是汇总行本身（见 summary），不是
# 文件名：把整行 JSON 当路径传给 sed 只会得到一个"文件名过长"的噪声错误，断言则静默失效。
#
# 收尾用 "sed -n 1p" 而不是 "head -1"：head 提前退出会给上游 sed 一个 SIGPIPE，在
# set -o pipefail 下整条命令替换的退出码变成非零，值会被静默取成空串。
json_field() {
  local key="$1"
  sed -n "s/.*\"$key\":\(\"[^\"]*\"\|[^,}]*\).*/\1/p" | tr -d '"' | sed -n '1p'
}

# ---- 1. 参数错误：必须以配置错误退出，且不产生任何会话 ---------------------------
run "$APP" --scenario bogus
expect_rc 1 "未知场景应当以配置错误退出"
expect_no_match "未知场景不应当输出汇总" '^{' "$work/last.log"

run "$APP" --scenario normal --unknown-flag
expect_rc 1 "未知开关应当以配置错误退出"

run "$APP"
expect_rc 1 "缺少 --scenario 应当以配置错误退出"

run "$APP" --scenario cancel --drain-budget 8
expect_rc 1 "把慢消费旋钮用在取消场景上应当被拒绝"

# 取消场景不接受 0 打断点：不设等待就会让取消与会话收尾抢时序，同一命令时而生效时而失效。
run "$APP" --scenario cancel --cancel-after-pcm 0
expect_rc 1 "取消打断点为 0 应当被拒绝（它无法给出稳定结果）"

run "$APP" --scenario slow --drain-budget 0
expect_rc 1 "慢消费取字节预算为 0 应当被拒绝"

run "$APP" --help
expect_rc 0 "--help 是正常结束"

# ---- 2. 四个场景可显式选择，退出码与形态符合预期 -------------------------------
run "$APP" --scenario normal
expect_rc 0 "正常场景应当按契约完成"
cp "$work/last.log" "$work/normal-a.log"
expect_match "正常场景报告退出码 0" '"exit_code":0' "$work/last.log"
expect_match "正常场景形态与预期一致" '"expectation_matched":true' "$work/last.log"
expect_match "正常场景交付成功终态" '"terminal_error_code":"none"' "$work/last.log"
expect_match "正常场景确实产生了音频" '"audio_rendered":true' "$work/last.log"
expect_match "正常场景资源已交还" '"quiesced":true' "$work/last.log"
expect_match "正常场景交付轮次文本" '"token_events":1' "$work/last.log"
summary_line="$(summary "$work/last.log")"
if [ "$(printf '%s' "$summary_line" | json_field threads_created)" != "1" ] ||
   [ "$(printf '%s' "$summary_line" | json_field threads_joined)" != "1" ]; then
  fail "正常场景应当创建并 join 恰好一个工作线程"
fi

run "$APP" --scenario slow
expect_rc 0 "慢消费场景应当按契约完成"
expect_match "慢消费由有界缓冲触发关闭" '"slow_client_close":true' "$work/last.log"
expect_match "慢消费如实报告关闭原因" '"close_reason":"slow_client"' "$work/last.log"
expect_match "慢消费仍然交还全部资源" '"quiesced":true' "$work/last.log"
expect_match "慢消费没有留下打开的连接" '"connections_open":0' "$work/last.log"

run "$APP" --scenario cancel
expect_rc 0 "取消是正常语义，应当按契约完成"
expect_match "取消场景以取消终态收敛" '"terminal_error_code":"cancelled"' "$work/last.log"
expect_match "取消发生在已经出声之后" '"cancel_frames":1' "$work/last.log"
expect_match "取消场景交还全部资源" '"quiesced":true' "$work/last.log"

run "$APP" --scenario cancel --cancel-after-pcm 3
expect_rc 0 "指定打断点的取消场景应当按契约完成"
expect_match "打断点随配置移动" '"cancel_frames":3' "$work/last.log"

run "$APP" --scenario fault
expect_rc 0 "故障场景按预期失败，命令本身按契约完成"
expect_match "故障场景如实报告失败终态" '"terminal_error_code":"invalid_input"' "$work/last.log"
expect_match "故障场景形态与预期一致" '"expectation_matched":true' "$work/last.log"
expect_match "故障场景没有产生音频" '"audio_rendered":false' "$work/last.log"
expect_match "故障场景仍然交还全部资源" '"quiesced":true' "$work/last.log"

# ---- 3. 重复运行确定性 -----------------------------------------------------------
run "$APP" --scenario normal
cp "$work/last.log" "$work/repeat-a.log"
run "$APP" --scenario normal
cp "$work/last.log" "$work/repeat-b.log"
if ! cmp -s "$work/repeat-a.log" "$work/repeat-b.log"; then
  fail "同一命令两次运行的输出应当逐字节一致"
fi
run "$APP" --scenario slow --events
cp "$work/last.log" "$work/slow-r1.log"
run "$APP" --scenario slow --events
if ! cmp -s "$work/slow-r1.log" "$work/last.log"; then
  fail "带事件输出的慢消费场景两次运行也应当逐字节一致"
fi

# ---- 4. 产物写入与清理 -----------------------------------------------------------
out="$work/artifacts"
run "$APP" --scenario normal --out-dir "$out"
expect_rc 0 "带输出目录的运行应当按契约完成"
expect_match "产物数量如实记账" '"artifacts_written":3' "$work/last.log"
expect_match "产物已删除" '"artifacts_removed":true' "$work/last.log"
if [ -n "$(ls -A "$out" 2>/dev/null)" ]; then
  fail "输出目录在命令结束后应当是空的（实际残留：$(ls -A "$out" | tr '\n' ' ')）"
fi

# ---- 5. 显式诊断输出：横幅与退出码一致 -------------------------------------------
run "$APP" --scenario normal
expect_match "横幅报告场景与退出码" 'mock_profile: scenario=normal' "$work/last.log"
expect_rc 0 "横幅与退出码一致"

# ---- 6. 不依赖硬件库 -------------------------------------------------------------
if command -v ldd >/dev/null 2>&1; then
  if ldd "$APP" 2>/dev/null | grep -qiE 'rkllm|rknn|sherpa|onnx|asound|sndfile|zmq'; then
    fail "mock profile 二进制不应当链接 NPU、声卡或传输库"
  fi
fi

if [ "$failures" -ne 0 ]; then
  echo "mock_profile_cli: $failures check(s) failed"
  for f in normal-a.log repeat-a.log repeat-b.log slow-r1.log last.log; do
    if [ -f "$work/$f" ]; then
      echo "--- $f ---"
      sed -n '1,8p' "$work/$f"
    fi
  done
  exit 1
fi
echo "mock_profile_cli: 全部检查通过"
exit 0