#!/usr/bin/env bash
# 把 Chromium（Release，没有调试信息）崩溃时在日志里打的地址栈还原成函数名。
#
#   scripts/symbolize.sh <日志文件>
#
# 要求日志里有 StarryBrowserMainExtraParts 打的「[starry] framework <路径> loaded at 0x…」
# （启动时需开启 --enable-logging=stderr --log-level=0）。
# 只还原浏览器进程里 Framework（<产品名> Framework）的地址；系统库的地址原样输出。
set -euo pipefail
log="${1:?日志文件}"

line=$(grep -m1 '\[starry\] framework .* loaded at 0x' "${log}" || true)
[ -n "${line}" ] || { echo "日志里没有框架的载入地址" >&2; exit 1; }
framework=$(echo "${line}" | sed -E 's/.*\[starry\] framework (.*) loaded at 0x[0-9a-f]+.*/\1/')
base=$(echo "${line}" | sed -E 's/.*loaded at (0x[0-9a-f]+).*/\1/')

# 崩溃信息和之后的地址栈。
awk '/Received signal|Check failed|FATAL/{p=1} p' "${log}" | grep -E 'Received signal|Check failed|FATAL' | head -3
addresses=$(awk '/Received signal|FATAL/{p=1} p' "${log}" | grep -oE '\[0x[0-9a-f]+\]' | tr -d '[]' | head -60)
[ -n "${addresses}" ] || { echo "日志里没有地址栈" >&2; exit 1; }
# shellcheck disable=SC2086
atos -o "${framework}" -arch arm64 -l "${base}" ${addresses} | cut -c1-220
