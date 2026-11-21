#!/usr/bin/env bash
# CI 编译前准备好 Chromium 源码（.github/workflows/release.yml 用）。尽量不碰没变的文件，
# 保住编译机上的增量编译：
#   - chromium.version 或 STARRY_EXTRA_TARGET_OS 变了（或者还没有源码）才 gclient sync，
#     src 里原有的改动全部丢掉
#   - 否则只把和 patches/ 不一致的文件改过来（scripts/patches.sh sync）
#   - Linux 上检查系统依赖：Chromium 升级后可能要新的包，缺了就停下
set -euo pipefail
source "$(dirname "$0")/../env.sh"

# actions/checkout 每次都会清空工作区（git clean -ffdx），源码放在里面每次都要重新同步、重新编
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    for dir in "$CHROMIUM_ROOT" "$DEPOT_TOOLS"; do
        case "$dir/" in
            "$STARRY_ROOT"/*)
                starry_die "$dir 在工作区里，每次 checkout 都会被清掉：编译机要把 STARRY_CHROMIUM_ROOT 和 STARRY_DEPOT_TOOLS 设到工作区外面"
                ;;
        esac
    done
fi
case " ${STARRY_EXTRA_TARGET_OS:-} " in
    *" win "*)
        [ -n "${DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL:-}" ] ||
            starry_die "交叉编译 Windows 要 VS 工具链：设好 DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL 和 GYP_MSVS_HASH_*（见 docs/development.md「发版」）"
        ;;
esac

[ -x "$DEPOT_TOOLS/gclient" ] || "$STARRY_ROOT/scripts/bootstrap.sh"

stamp="$CHROMIUM_ROOT/.starry-synced"
want="$CHROMIUM_VERSION ${STARRY_EXTRA_TARGET_OS:-}"
if [ -d "$CHROMIUM_SRC/.git" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$want" ]; then
    starry_log "Chromium $CHROMIUM_VERSION 已同步，只对齐补丁"
    "$STARRY_ROOT/scripts/patches.sh" sync
else
    if [ -d "$CHROMIUM_SRC/.git" ]; then
        # sync.sh 只撤 patches/ 里现有补丁碰过的文件；已经删掉的补丁留下的改动在这里一起撤掉
        starry_log "撤掉 src 里的全部改动"
        git -C "$CHROMIUM_SRC" checkout -- .
        git -C "$CHROMIUM_SRC" ls-files --others --exclude-standard -z |
            (cd "$CHROMIUM_SRC" && xargs -0 rm -f --)
    fi
    "$STARRY_ROOT/scripts/sync.sh" --force
    printf '%s\n' "$want" > "$stamp"
fi

if [ "$(uname -s)" = Linux ] &&
    ! python3 "$CHROMIUM_SRC/build/install-build-deps.py" --quick-check; then
    starry_die "编译机缺 Chromium $CHROMIUM_VERSION 要的系统包（见上面的列表）：重建编译机的镜像后重跑"
fi
starry_log "源码就绪：$CHROMIUM_SRC"
