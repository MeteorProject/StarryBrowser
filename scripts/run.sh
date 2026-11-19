#!/usr/bin/env bash
# 启动编出来的浏览器。
#
#   scripts/run.sh [Release|Dev] [-- 参数…]
#
# 默认每次用一个新的临时数据目录；要沿用就设 STARRY_USER_DATA_DIR。
# 加了 --use-mock-keychain：不碰钥匙串（新 Profile 第一次会弹钥匙串授权，远程运行时会卡住）。
# STARRY_LLDB=1：在 lldb 里跑浏览器进程（批处理），崩溃时打印带函数名的调用栈
# （Release 没有调试信息，但符号表还在；Chromium 自己打的栈只有地址）。
# STARRY_OPEN_LOG=<文件>（macOS）：用 open（LaunchServices）启动并等它退出，输出写进这个
# 文件。从后台进程直接执行时 macOS 不让 App 到前台。
set -euo pipefail
source "$(dirname "$0")/env.sh"

config=Release
if [ $# -gt 0 ] && [ "$1" != "--" ]; then
    config="$1"
    shift
fi
[ "${1:-}" = "--" ] && shift

binary="$(starry_browser_binary "$config")"
app=""
[ "$(uname -s)" = Darwin ] && app="$CHROMIUM_SRC/out/$config/$STARRY_PRODUCT_NAME.app"
[ -x "$binary" ] || starry_die "没有 ${binary}，先运行 scripts/build.sh $config"

data_dir="${STARRY_USER_DATA_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/starry-chromium.XXXXXX")}"
starry_log "数据目录：$data_dir"
args=(
    --user-data-dir="$data_dir"
    --no-first-run
    --no-default-browser-check
    --use-mock-keychain
)

# Linux：不用系统的密钥环（没有桌面会话时会卡住或弹窗）。
[ "$(uname -s)" = Linux ] && args+=(--password-store=basic)

if [ -n "$app" ] && [ -n "${STARRY_OPEN_LOG:-}" ] && [ "${STARRY_LLDB:-}" != 1 ]; then
    exec open -n -W --stdout "${STARRY_OPEN_LOG}" --stderr "${STARRY_OPEN_LOG}" -a "$app" --args \
        "${args[@]}" "$@"
fi

debugger=()
if [ "${STARRY_LLDB:-}" = 1 ]; then
    debugger=(lldb --batch
        -o "process handle SIGPIPE -n false -p true -s false"
        -o "process launch --shell-expand-args false"
        -k "bt 60"
        -k "kill"
        -k "quit"
        --)
fi

exec ${debugger[@]+"${debugger[@]}"} "$binary" "${args[@]}" "$@"
