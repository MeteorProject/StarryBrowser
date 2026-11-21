#!/usr/bin/env bash
# 取 depot_tools（放在 vendor/depot_tools，不入库）并让它准备好自己的 Python、cipd 等工具。
set -euo pipefail
source "$(dirname "$0")/env.sh"

if [ ! -d "$DEPOT_TOOLS/.git" ]; then
    starry_log "Cloning depot_tools into $DEPOT_TOOLS"
    mkdir -p "$(dirname "$DEPOT_TOOLS")"
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_TOOLS"
fi

# 第一次运行会下载 depot_tools 自带的 Python 和 cipd
starry_log "Preparing depot_tools"
gclient --version >/dev/null
starry_log "depot_tools ready: $DEPOT_TOOLS"
