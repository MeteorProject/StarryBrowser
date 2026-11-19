#!/usr/bin/env bash
# 编译。
#
#   scripts/build.sh [Release|Dev] [目标…]     默认 Release、chrome
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_depot_tools
starry_require_src

config="${1:-Release}"
[ $# -gt 0 ] && shift
[ $# -gt 0 ] || set -- chrome

[ -f "$CHROMIUM_SRC/out/$config/build.ninja" ] || "$STARRY_ROOT/scripts/gen.sh" "$config"

cd "$CHROMIUM_SRC"
autoninja -C "out/$config" "$@"
