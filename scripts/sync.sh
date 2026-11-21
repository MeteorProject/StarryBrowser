#!/usr/bin/env bash
# 把 chromium/src 同步到 chromium.version 锁定的版本，然后链接 starry/、打上 patches/。
#
#   scripts/sync.sh            同步并打补丁
#   scripts/sync.sh --force    src 里有没导出成补丁的改动也照样重置（这些改动会丢）
#   scripts/sync.sh --nohooks  不跑 gclient runhooks。Linux 上第一次要先装系统依赖
#                              （chromium/src/build/install-build-deps.sh）才能跑 hooks：
#                              先 --nohooks，装完依赖再不带参数跑一遍
#
# 交叉编译别的平台时，把它加进 STARRY_EXTRA_TARGET_OS（空格分隔），gclient 才会拉它的依赖。
# 比如在 Linux 上编 Windows：STARRY_EXTRA_TARGET_OS=win（还要 VS 工具链，见 docs/development.md）
#
# 第一次是浅克隆（--no-history），几十 GB，要几个小时。
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_depot_tools

force=0
hooks=1
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        --nohooks) hooks=0 ;;
        *) starry_die "不认识的参数：$arg" ;;
    esac
done

case "$(uname -s)" in
    Darwin) target_os=mac ;;
    Linux) target_os=linux ;;
    MINGW* | MSYS* | CYGWIN*) target_os=win ;;
    *) starry_die "不认识的系统：$(uname -s)" ;;
esac
target_os_list="\"$target_os\""
for extra in ${STARRY_EXTRA_TARGET_OS:-}; do
    [ "$extra" = "$target_os" ] || target_os_list="$target_os_list, \"$extra\""
done

mkdir -p "$CHROMIUM_ROOT"
cat > "$CHROMIUM_ROOT/.gclient" <<EOF
# 由 scripts/sync.sh 生成，改 chromium.version 而不是这里
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git@refs/tags/$CHROMIUM_VERSION",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = [$target_os_list]
EOF

if [ -d "$CHROMIUM_SRC/.git" ]; then
    if [ "$force" = 0 ] && ! "$STARRY_ROOT/scripts/patches.sh" check; then
        starry_die "src 里有没导出成补丁的改动：先运行 scripts/patches.sh export，或者加 --force 丢掉它们"
    fi
    starry_log "撤掉旧补丁"
    "$STARRY_ROOT/scripts/patches.sh" revert
fi

cd "$CHROMIUM_ROOT"
starry_log "gclient sync 到 $CHROMIUM_VERSION"
gclient sync --nohooks --no-history -D --force
if [ "$hooks" = 1 ]; then
    starry_map_win_toolchain
    starry_log "gclient runhooks"
    gclient runhooks
fi

"$STARRY_ROOT/scripts/patches.sh" apply
starry_log "完成：${CHROMIUM_SRC}（${CHROMIUM_VERSION}）"
