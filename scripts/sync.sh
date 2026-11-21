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
        *) starry_die "Unknown argument: $arg" ;;
    esac
done

case "$(uname -s)" in
    Darwin) target_os=mac ;;
    Linux) target_os=linux ;;
    MINGW* | MSYS* | CYGWIN*) target_os=win ;;
    *) starry_die "Unsupported OS: $(uname -s)" ;;
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

tag="refs/tags/$CHROMIUM_VERSION"
revision="$tag"
if [ -d "$CHROMIUM_SRC/.git" ]; then
    if ! git -C "$CHROMIUM_SRC" rev-parse --verify --quiet "$tag^{commit}" >/dev/null; then
        git -C "$CHROMIUM_SRC" fetch --no-tags --depth=1 origin "$tag:$tag"
    fi
    # gclient treats ref names as unresolved on an existing checkout and fetches
    # all remote branches. Pass the commit we just obtained from the release tag.
    revision="$(git -C "$CHROMIUM_SRC" rev-parse --verify "$tag^{commit}")"
    if [ -n "$(git -C "$CHROMIUM_SRC" status --porcelain --ignore-submodules=dirty)" ]; then
        patches="$STARRY_ROOT/patches"
        if [ "$force" = 0 ] && ! "$STARRY_ROOT/scripts/patches.sh" check; then
            # After updating Starry, the applied patches can still match the
            # last Starry revision on the old Chromium version.
            old_tag="$(git -C "$CHROMIUM_SRC" describe --tags --exact-match HEAD 2>/dev/null || true)"
            previous=""
            if [ -n "$old_tag" ]; then
                previous="$(git -C "$STARRY_ROOT" log -1 --format=%H -S"$old_tag" -- chromium.version)"
            fi
            baseline=HEAD
            if [ "$(git -C "$STARRY_ROOT" show HEAD:chromium.version)" != "$old_tag" ]; then
                baseline="$previous^"
            fi
            if [ -z "$old_tag" ] || ! git -C "$STARRY_ROOT" cat-file -e "$baseline:patches" 2>/dev/null ||
                [ "$(git -C "$STARRY_ROOT" show "$baseline:chromium.version")" != "$old_tag" ]; then
                starry_die "src has unexported changes; save them before syncing"
            fi
            old_patches="$(mktemp -d)"
            trap 'rm -rf "$old_patches"' EXIT
            git -C "$STARRY_ROOT" archive "$baseline" patches | tar -x -C "$old_patches"
            patches="$old_patches/patches"
            "$STARRY_ROOT/scripts/patches.sh" check "$patches" ||
                starry_die "src differs from its recorded patches; save the changes before syncing"
        fi
        starry_log "Reverting old patches"
        "$STARRY_ROOT/scripts/patches.sh" revert "$patches"
    fi
fi

cd "$CHROMIUM_ROOT"
starry_log "gclient sync to $CHROMIUM_VERSION"
gclient sync --nohooks --no-history -D --force --revision "src@$revision"
starry_src_at_pinned_version || starry_die "Chromium HEAD does not match refs/tags/$CHROMIUM_VERSION"
if [ "$hooks" = 1 ]; then
    starry_map_win_toolchain
    starry_log "gclient runhooks"
    gclient runhooks
fi

"$STARRY_ROOT/scripts/patches.sh" apply
starry_log "Done: ${CHROMIUM_SRC} (${CHROMIUM_VERSION})"
