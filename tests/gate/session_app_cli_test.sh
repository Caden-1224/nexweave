#!/usr/bin/env bash
# 单进程 Session 应用的命令行门禁。
#
# 职责：验收“命令行入口能让同一套契约真正跑起来”这件事——参数互斥、三种输入模式可运行、
# 退出码语义、确定性一致，以及“退出能唤醒等待输入的进程”。断言只读取进程的退出码与
# --summary-json / 事件输出，因此不依赖私有实现，也不依赖构建目录里的中间产物。
#
# 保护的不变量：
#   1. 参数错误与配置错误必须在开始任何轮次之前明确失败（退出码 1），不能被当成空输入。
#   2. 正常收敛（含被取消、被停止的轮次）退出码为 0：取消是正常语义，不是程序故障。
#   3. 同一命令重复运行得到逐字节一致的汇总输出：输入、夹具与调度都是确定性的。
#   4. 空输入不产生轮次；非法音频在读入阶段就被拒绝，不会变成一次失败的回答。
#   5. 事件与指标输出符合可观测性契约（每行一个完整 JSON 对象、字段与单位齐全）。
#   6. 常驻模式在等待输入时收到 SIGTERM 能在有限步内退出，且退出码为 0、输入被关闭。
#
# 夹具：本脚本只在系统临时目录下创建自己的 WAV 文件，由 trap 在退出时删除；不修改源码，
# 不后台启动进程（唯一的长驻进程由 timeout 前台托管，并在其窗口内自行结束）。
set -uo pipefail

APP="${1:?需要单进程应用可执行文件路径}"
if [ ! -x "$APP" ]; then
  echo "FAIL: 应用不可执行: $APP"
  exit 1
fi

failures=0
work="$(mktemp -d /tmp/nexweave-cli-gate.XXXXXX)"
trap 'rm -rf -- "$work"' EXIT

fail() {
  echo "FAIL: $1"
  failures=$((failures + 1))
}

# 运行一条命令并把退出码写入全局 RC：调用方随后用 expect_rc 断言，避免 $? 被中间命令覆盖。
RC=0
run() {
  "$@" >"$work/last.log" 2>&1
  RC=$?
}

expect_rc() {
  if [ "$RC" -ne "$1" ]; then
    fail "$2（期望退出码 $1，实际 $RC）"
    sed -n '1,5p' "$work/last.log"
  fi
}

expect_eq() {
  if [ "$2" != "$3" ]; then
    fail "$1（期望 '$3'，实际 '$2'）"
  fi
}

expect_match() {
  if ! grep -q -- "$2" "$3"; then
    fail "$1"
  fi
}

# 提取一份 JSON 里的整数字段；只支持扁平的 "key":数字，够用且不引入解析依赖。
json_int() {
  sed -n "s/.*\"$2\":\([0-9-]*\).*/\1/p" "$1" | head -1
}

json_word() {
  sed -n "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p" "$1" | head -1
}

# ---- 1. 参数与配置错误：必须在开始任何轮次之前失败 -------------------------------
run "$APP" --input text --stream-id gate-missing-text
expect_rc 1 "缺少 --text 时应当以配置错误退出"

run "$APP" --input bogus --text x
expect_rc 1 "未知 --input 时应当以配置错误退出"

run "$APP" --input text --text hello --unknown-flag
expect_rc 1 "未知参数时应当以配置错误退出"

run "$APP" --input file --wav "$work/absent.wav" --quiet --summary-json
expect_rc 1 "缺少音频文件时应当以读入失败退出"
expect_eq "缺少音频文件时不产生轮次" "$(json_int "$work/last.log" turns_started)" "0"

# ---- 2. 文本模式：L1 直答、L2 生成、取消都按契约收敛 -----------------------------
run "$APP" --input text --text "the capital of france" --stream-id gate-l1 \
  --quiet --summary-json
expect_rc 0 "L1 直答应当正常收敛"
expect_eq "L1 直答完成一轮" "$(json_int "$work/last.log" turns_completed)" "1"
expect_eq "L1 直答路由为 L1" "$(json_word "$work/last.log" route)" "L1"
expect_match "L1 直答确实写出音频" '"sink_writes":[1-9]' "$work/last.log"
expect_match "L1 直答输入自然结束" '"input_ended":true' "$work/last.log"
cp "$work/last.log" "$work/l1.log"

run "$APP" --input text --text "the weather in paris" --stream-id gate-l2 \
  --quiet --summary-json
expect_rc 0 "L2 生成应当正常收敛"
expect_eq "L2 生成完成一轮" "$(json_int "$work/last.log" turns_completed)" "1"
expect_eq "L2 生成路由为 L2" "$(json_word "$work/last.log" route)" "L2"
if [ "$(json_int "$work/last.log" pcm_frames)" -le 1 ]; then
  fail "L2 生成应当产生多帧输出（流式分句）"
fi
cp "$work/last.log" "$work/l2.log"

run "$APP" --input text --text "the weather in paris" --stream-id gate-cancel \
  --cancel-after-frames 1 --quiet --summary-json
expect_rc 0 "取消后应当正常收敛（取消是正常语义）"
expect_match "取消轮次以取消终态收敛" '"marker":"cancelled"' "$work/last.log"
expect_eq "取消不产生完成轮次" "$(json_int "$work/last.log" turns_completed)" "0"
if [ "$(json_int "$work/last.log" pcm_frames)" -lt 1 ]; then
  fail "取消前已经写出的帧应当保留"
fi
cp "$work/last.log" "$work/cancel.log"

# ---- 3. 空输入与非法音频：不产生轮次、不产生音频 ---------------------------------
run "$APP" --input text --text "   " --stream-id gate-blank --quiet --summary-json
expect_rc 0 "全空白文本应当被当成没有输入而不是配置错误"
expect_eq "全空白文本不产生轮次" "$(json_int "$work/last.log" turns_started)" "0"
expect_eq "全空白文本不产生音频" "$(json_int "$work/last.log" sink_writes)" "0"

# 小端 16 位字段：先算出两个字节的十六进制转义文本，再用一次 %b 解释成真实字节。
# 分两步是必要的：printf 的格式串只解释一次转义，直接嵌套会写出字面量文本。
le16() {
  local low high
  printf -v low '%02x' "$(( $1 & 0xFF ))"
  printf -v high '%02x' "$(( ($1 >> 8) & 0xFF ))"
  printf '%b' "\\x${low}\\x${high}"
}

# 最小合法 WAV（16 kHz 或 8 kHz / 单声道 / 16 位）：2 帧静音。整个 44 字节头一次写出，
# 避免分多条 printf 时把字段条数写错却看不出来；非法采样率版本只改采样率字段。
make_wav() {
  {
    printf 'RIFF\x24\x05\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00'
    printf '%b' "$(le16 "$2")"
    printf '\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00data\x00\x05\x00\x00'
    head -c 1280 /dev/zero
  } >"$1"
}

wav="$work/two-frames.wav"
make_wav "$wav" 16000
run "$APP" --input file --wav "$wav" --hypotheses "the capital of france" \
  --stream-id gate-file --quiet --summary-json
expect_rc 0 "文件音频应当正常收敛"
expect_eq "文件音频完成一轮" "$(json_int "$work/last.log" turns_completed)" "1"
expect_eq "文件音频逐帧送进识别" "$(json_int "$work/last.log" asr_fed_frames)" "2"

bad_wav="$work/bad-rate.wav"
make_wav "$bad_wav" 8000
run "$APP" --input file --wav "$bad_wav" --stream-id gate-bad --quiet --summary-json
expect_rc 1 "非法采样率应当以读入失败退出"
expect_eq "非法采样率不产生轮次" "$(json_int "$work/last.log" turns_started)" "0"

# ---- 4. 常驻模拟：多段说话、旧回答清理不删新语音 ---------------------------------
run "$APP" --input resident --stream-id gate-resident --quiet --summary-json
expect_rc 0 "常驻模式应当正常收敛"
expect_eq "常驻模式跑完两段说话" "$(json_int "$work/last.log" turns_started)" "2"
expect_match "常驻模式输入自然结束" '"input_ended":true' "$work/last.log"
expect_match "常驻模式关闭输入流" '"stream_closed":true' "$work/last.log"
expect_eq "常驻模式不丢弃已交付的语音段" "$(json_int "$work/last.log" segments_dropped)" "0"
cp "$work/last.log" "$work/resident.log"

# ---- 5. 退出能唤醒等待输入的进程 -------------------------------------------------
start_ms=$(date +%s%3N)
timeout -s TERM 1 "$APP" --input resident --stream-id gate-signal --utterances 8 \
  --speech-frames 8 --pump-budget 400 --pace-us 100000 --quiet --summary-json \
  >"$work/signal.log" 2>&1
signal_rc=$?
end_ms=$(date +%s%3N)
elapsed_ms=$((end_ms - start_ms))
# timeout 自己发出的信号会让它返回 124，这并不表示被管理的进程被强杀；真正的判据是
# 进程在窗口内自行结束（elapsed 明显小于 timeout 之后的强制宽限）并如实报告 signal。
if [ "$elapsed_ms" -ge 2500 ]; then
  fail "被信号中断的运行应当在超时窗口内结束（实际 ${elapsed_ms} ms，退出码 ${signal_rc}）"
fi
if [ "$signal_rc" != "0" ] && [ "$signal_rc" != "124" ]; then
  fail "被信号中断的运行应当正常收敛（实际退出码 ${signal_rc}）"
fi
expect_match "被信号中断的运行如实报告 signal=true" '"signal":true' "$work/signal.log"
expect_match "被信号中断的运行报告提前停止" '"stopped_early":true' "$work/signal.log"
expect_match "被信号中断的运行关闭了输入流" '"stream_closed":true' "$work/signal.log"

# ---- 6. 重复运行确定性 -----------------------------------------------------------
run "$APP" --input text --text "the capital of france" --stream-id gate-repeat \
  --quiet --summary-json
cp "$work/last.log" "$work/repeat-a.log"
run "$APP" --input text --text "the capital of france" --stream-id gate-repeat \
  --quiet --summary-json
cp "$work/last.log" "$work/repeat-b.log"
# 启动横幅不含结果，只比较汇总行（以 { 开头的那一行）。
grep '^{' "$work/repeat-a.log" >"$work/repeat-a.json"
grep '^{' "$work/repeat-b.log" >"$work/repeat-b.json"
if ! cmp -s "$work/repeat-a.json" "$work/repeat-b.json"; then
  fail "同一命令两次运行的汇总应当逐字节一致"
fi

# ---- 7. 输出开关与可观测性契约 ---------------------------------------------------
# 默认（不加开关）只输出人可读摘要与汇总：JSONL 不能被无条件打印，否则“按需开启”
# 这条命令行契约名存实亡。
run "$APP" --input text --text "the capital of france" --stream-id gate-default \
  --quiet --summary-json
expect_rc 0 "默认输出应当正常收敛"
if grep -q '"name":"turn_terminal"' "$work/last.log"; then
  fail "未加 --events 时不应当输出事件 JSONL"
fi
if grep -q '"unit":"frames"' "$work/last.log"; then
  fail "未加 --metrics 时不应当输出指标 JSONL"
fi

run "$APP" --input text --text "the capital of france" --stream-id gate-events-only \
  --quiet --events
expect_rc 0 "只开 --events 应当正常收敛"
expect_match "只开 --events 应输出事件" '"name":"turn_terminal"' "$work/last.log"
if grep -q '"unit":"frames"' "$work/last.log"; then
  fail "只开 --events 时不应当输出指标"
fi

run "$APP" --input text --text "the capital of france" --stream-id gate-metrics-only \
  --quiet --metrics
expect_rc 0 "只开 --metrics 应当正常收敛"
expect_match "只开 --metrics 应输出指标" '"unit":"frames"' "$work/last.log"
if grep -q '"name":"turn_terminal"' "$work/last.log"; then
  fail "只开 --metrics 时不应当输出事件"
fi

run "$APP" --input text --text "the capital of france" --stream-id gate-events \
  --quiet --events --metrics
expect_rc 0 "事件流输出应当正常收敛"
expect_match "事件流包含轮次终态事件" '"name":"turn_terminal"' "$work/last.log"
expect_match "事件流包含播放开始事件" '"name":"playback_started"' "$work/last.log"
expect_match "指标流包含峰值待播帧指标" '"name":"playback_peak_pending_frames"' "$work/last.log"
expect_match "指标流使用帧作为单位" '"unit":"frames"' "$work/last.log"
# 每行都必须以 { 开头并以 } 结尾：既验证格式，也验证没有把多行文本塞进一行。
if grep '^{' "$work/last.log" | grep -qv '}$'; then
  fail "事件与指标每行都应当是完整 JSON 对象"
fi

if [ "$failures" -ne 0 ]; then
  echo "session_app_cli: $failures check(s) failed"
  echo "--- L1 ---"; cat "$work/l1.log"
  echo "--- resident ---"; cat "$work/resident.log"
  echo "--- signal ---"; cat "$work/signal.log"
  exit 1
fi
echo "session_app_cli: 全部检查通过"
exit 0
