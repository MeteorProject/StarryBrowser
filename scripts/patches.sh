#!/usr/bin/env bash
# 管理对 Chromium 上游文件的改动。
# 每个改动过的上游文件一个补丁：chrome/BUILD.gn → patches/chrome/BUILD.gn.patch。
# 自己的代码不进补丁，放在 starry/（链接成 src/starry）。
#
#   scripts/patches.sh apply    链接 starry/，把 patches/ 打到 src 上
#   scripts/patches.sh revert   把补丁碰过的文件恢复成上游的样子
#   scripts/patches.sh export   把 src 里现在的改动写回 patches/（增删补丁文件）
#   scripts/patches.sh check    src 里的改动和 patches/ 一致时返回 0
#
# 目前只管 src 这一个 git 仓库；改 v8、skia 这些子仓库时再扩展。
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_src

PATCHES="$STARRY_ROOT/patches"
mkdir -p "$PATCHES"

git_src() { git -C "$CHROMIUM_SRC" -c core.quotepath=off "$@"; }

# 按上游目录存放补丁，递归列出并固定顺序。
patch_files() { find "${1:-$PATCHES}" -type f -name '*.patch' | LC_ALL=C sort; }

# 补丁里涉及的上游文件
patched_files() {
    local p
    while IFS= read -r p; do
        sed -n 's|^diff --git a/\(.*\) b/.*$|\1|p' "$p"
    done < <(patch_files) | sort -u
}

link_overlay() {
    local link="$CHROMIUM_SRC/starry"
    if [ -L "$link" ]; then
        [ "$(readlink "$link")" = "$STARRY_ROOT/starry" ] || { rm "$link"; ln -s "$STARRY_ROOT/starry" "$link"; }
    elif [ -e "$link" ]; then
        starry_die "$link 已经存在而且不是链接"
    else
        ln -s "$STARRY_ROOT/starry" "$link"
    fi
    local exclude="$CHROMIUM_SRC/.git/info/exclude"
    grep -qx '/starry' "$exclude" 2>/dev/null || echo '/starry' >> "$exclude"
}

# 把 src 现在的改动按文件写到目录 $1
write_patches() {
    local dir="$1" f patch
    mkdir -p "$dir"
    {
        git_src diff --name-only HEAD
        git_src ls-files --others --exclude-standard
    } | sort -u | while IFS= read -r f; do
        [ -n "$f" ] || continue
        patch="$dir/$f.patch"
        mkdir -p "$(dirname "$patch")"
        if git_src cat-file -e "HEAD:$f" 2>/dev/null; then
            git_src diff --binary HEAD -- "$f" > "$patch"
        else
            # 新文件：和空文件比
            (cd "$CHROMIUM_SRC" && git -c core.quotepath=off diff --binary --no-index -- /dev/null "$f") \
                > "$patch" || true
        fi
    done
}

cmd_apply() {
    link_overlay
    local p n=0
    while IFS= read -r p; do
        git_src apply --whitespace=nowarn "$p" || starry_die "补丁打不上：${p#"$PATCHES"/}（上游改了，要手动更新）"
        n=$((n + 1))
    done < <(patch_files)
    starry_log "打上了 $n 个补丁"
}

cmd_revert() {
    local f
    patched_files | while IFS= read -r f; do
        [ -n "$f" ] || continue
        if git_src cat-file -e "HEAD:$f" 2>/dev/null; then
            git_src checkout HEAD -- "$f"
        else
            rm -f "$CHROMIUM_SRC/$f"
        fi
    done
}

cmd_export() {
    local tmp
    tmp="$(mktemp -d)"
    write_patches "$tmp"
    find "$PATCHES" -type f -name '*.patch' -delete
    find "$PATCHES" -depth -mindepth 1 -type d -empty -delete
    cp -R "$tmp/." "$PATCHES/"
    rm -rf "$tmp"
    starry_log "导出了 $(patch_files | wc -l | tr -d ' ') 个补丁"
}

cmd_check() {
    local tmp status=0
    tmp="$(mktemp -d)"
    write_patches "$tmp"
    diff -r -q "$tmp" "$PATCHES" -x README.md -x LICENSE >/dev/null 2>&1 || status=1
    rm -rf "$tmp"
    return $status
}

case "${1:-}" in
    apply) cmd_apply ;;
    revert) cmd_revert ;;
    export) cmd_export ;;
    check) cmd_check ;;
    *) starry_die "用法：scripts/patches.sh apply|revert|export|check" ;;
esac
