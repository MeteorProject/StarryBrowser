#!/usr/bin/env bash
# 把 dist/ 里当前版本（starry/VERSION）的发布包传到 GitHub Release v<版本>，还没有这个 Release 就先建。
# Linux / Windows 包由 CI 传（.github/workflows/release.yml）；macOS 包在 Mac 上打完自己传：
#
#   scripts/package.sh --identity "Developer ID Application: …" --notary-profile <profile>
#   scripts/release.sh
#
# 每个包旁边生成一份 .sha256 一起传；同名文件覆盖，重传不会出重复。
# 要先 gh auth login（或者设 GH_TOKEN）。传到别的仓库：GH_REPO=<owner/name> scripts/release.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"
command -v gh >/dev/null 2>&1 || starry_die "gh (GitHub CLI) not found"
cd "$STARRY_ROOT"

version="$STARRY_VERSION"
tag="v$version"
name="$(printf '%s' "$STARRY_PRODUCT_NAME" | tr -d ' ')"
dist="$STARRY_ROOT/dist"

checksum() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi
}

files=()
for f in "$dist/$name-$version-"*; do
    [ -f "$f" ] || continue
    case "$f" in *.sha256) continue ;; esac
    (cd "$dist" && checksum "${f##*/}") > "$f.sha256"
    files+=("$f" "$f.sha256")
done
[ ${#files[@]} -gt 0 ] || starry_die "No $version packages in dist/; run scripts/package.sh first"

if ! gh release view "$tag" >/dev/null 2>&1; then
    # 发到本仓库时 tag 必须已经推上去，免得 gh 在默认分支上新建一个
    own_repo="$(git remote get-url origin 2>/dev/null | sed -E 's#^.*github\.com[:/]##; s#\.git$##')"
    verify=""
    if [ -z "${GH_REPO:-}" ] || [ "$GH_REPO" = "$own_repo" ]; then
        verify=--verify-tag
    fi
    starry_log "Creating release $tag"
    # 失败多半是另一边（CI 或 Mac）刚好同时建好了
    gh release create "$tag" ${verify:+"$verify"} --title "$STARRY_PRODUCT_NAME $version" --notes "" ||
        gh release view "$tag" >/dev/null
fi
starry_log "Uploading: ${files[*]##*/}"
gh release upload "$tag" "${files[@]}" --clobber
starry_log "Done: $(gh release view "$tag" --json url -q .url)"
