#!/usr/bin/env bash
# 生成构建目录 chromium/src/out/<配置>，参数在 starry/build/args/<配置>.gn。
# 根目录若存在不入库的 args.local.gn，会追加进 args.gn（加 --no-local 忽略）。
#
#   scripts/gen.sh [Release|Dev] [--no-local]     默认 Release
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_depot_tools
starry_require_src

config=Release
use_local=1
for arg in "$@"; do
    case "$arg" in
        --no-local) use_local=0 ;;
        *) config="$arg" ;;
    esac
done
[ "${STARRY_NO_LOCAL_ARGS:-0}" = 1 ] && use_local=0

name="$(printf '%s' "$config" | tr '[:upper:]' '[:lower:]')"
[ -f "$STARRY_ROOT/starry/build/args/$name.gn" ] || starry_die "Unknown config: ${config} (no starry/build/args/$name.gn)"

out="$CHROMIUM_SRC/out/$config"
mkdir -p "$out"
want="import(\"//starry/build/args/$name.gn\")"
if [ "$use_local" = 1 ] && [ -f "$STARRY_ROOT/args.local.gn" ]; then
    want="${want}
$(cat "$STARRY_ROOT/args.local.gn")"
fi
if [ ! -f "$out/args.gn" ] || [ "$(cat "$out/args.gn")" != "$want" ]; then
    printf '%s\n' "$want" > "$out/args.gn"
fi

cd "$CHROMIUM_SRC"
gn gen "out/$config"
