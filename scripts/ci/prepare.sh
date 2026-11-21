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
                starry_die "$dir is inside the workspace, which every checkout wipes: on the build machine, set STARRY_CHROMIUM_ROOT and STARRY_DEPOT_TOOLS to paths outside it"
                ;;
        esac
    done
fi
case " ${STARRY_EXTRA_TARGET_OS:-} " in
    *" win "*)
        [ -n "${DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL:-}" ] ||
            starry_die "Cross-compiling for Windows needs the VS toolchain: put the packaged zip in a directory and point DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL at it (see the release section of docs/development.md)"
        # Windows SDK 要放在不区分大小写的目录里。Chromium 用的是 src 里自带的 depot_tools，
        # 这个目录不是挂载点时会去挂 ciopfs，没有 FUSE（比如容器里）就挂不上
        vs_files="$CHROMIUM_SRC/third_party/depot_tools/win_toolchain/vs_files"
        if [ "$(uname -s)" = Linux ] && [ ! -e /dev/fuse ] && [ -d "$CHROMIUM_SRC/.git" ] &&
            ! python3 -c 'import os, sys; sys.exit(not os.path.ismount(sys.argv[1]))' "$vs_files"; then
            starry_die "$vs_files is not a mount point and FUSE is unavailable: mount a casefold-enabled directory there (see the release section of docs/development.md)"
        fi
        ;;
esac

[ -x "$DEPOT_TOOLS/gclient" ] || "$STARRY_ROOT/scripts/bootstrap.sh"

stamp="$CHROMIUM_ROOT/.starry-synced"
want="$CHROMIUM_VERSION ${STARRY_EXTRA_TARGET_OS:-}"
if [ -d "$CHROMIUM_SRC/.git" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$want" ]; then
    starry_log "Chromium $CHROMIUM_VERSION is already synced; syncing patches only"
    "$STARRY_ROOT/scripts/patches.sh" sync
else
    if [ -d "$CHROMIUM_SRC/.git" ]; then
        # sync.sh 只撤 patches/ 里现有补丁碰过的文件；已经删掉的补丁留下的改动在这里一起撤掉
        starry_log "Discarding all changes in src"
        git -C "$CHROMIUM_SRC" checkout -- .
        git -C "$CHROMIUM_SRC" ls-files --others --exclude-standard -z |
            (cd "$CHROMIUM_SRC" && xargs -0 rm -f --)
    fi
    "$STARRY_ROOT/scripts/sync.sh" --force
    printf '%s\n' "$want" > "$stamp"
fi

if [ "$(uname -s)" = Linux ]; then
    # install-build-deps.py 靠 apt 的软件包列表判断该用哪个包名（libpng16-16t64 还是 libpng12-0），
    # 列表被删掉（镜像里常见）时会全部报成缺包，只好跳过
    if ! ls /var/lib/apt/lists/ 2>/dev/null | grep -q '_Packages'; then
        starry_log "No apt package lists; skipping the system dependency check"
    elif ! python3 "$CHROMIUM_SRC/build/install-build-deps.py" --quick-check; then
        starry_die "The build machine is missing system packages that Chromium $CHROMIUM_VERSION needs (listed above): rebuild the build machine image and run again"
    fi
fi
starry_log "Source ready: $CHROMIUM_SRC"
