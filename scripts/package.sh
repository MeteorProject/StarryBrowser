#!/usr/bin/env bash
# 打发布包（说明见 docs/development.md）。三个平台同一个入口，产物放在 dist/：
#
#   scripts/package.sh [选项]
#     --config <名字>      构建配置，默认 Release（Dev 是组件构建，不能打包）
#     --skip-build         不编译，直接用 out/<配置> 里现有的
#     --keep-dist          打包前不清空 dist/（连续打多个平台的包时用）
#
# macOS：签名、打成 DMG（Chromium 自带的签名流程 chrome/installer/mac/signing）。
#     --identity <证书>    签名用的证书（钥匙串里 Developer ID Application 的名字或 SHA-1）。
#                          不给时是 ad-hoc 签名：本机能打开，发给别人会被 Gatekeeper 拦
#     --notary-profile <名字>
#                          公证：xcrun notarytool store-credentials 存的钥匙串配置名。
#                          要配合 Developer ID 证书，会等 Apple 的结果并把票据钉进 DMG
# Windows：安装程序 setup.exe（starry/installer/win，外面一层窗口，里面是 Chromium 的
#     mini_installer.exe；还没有签名）和免安装的 zip。在 Linux 上交叉编译用 --config Release-Win。
# Linux：Chromium 的 .deb 和 .rpm（装 rpmbuild 才有 .rpm）。
#   平台标识从 starry/app/theme/BRANDING 生成。
# 打哪个平台的包看构建目录的 target_os / target_cpu，没写就是编译机自己的平台。
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_src
starry_require_depot_tools

config=Release
build=1
keep_dist=0
identity=""
notary_profile=""
while [ $# -gt 0 ]; do
    case "$1" in
        --config) config="$2"; shift 2 ;;
        --skip-build) build=0; shift ;;
        --keep-dist) keep_dist=1; shift ;;
        --identity) identity="$2"; shift 2 ;;
        --notary-profile) notary_profile="$2"; shift 2 ;;
        *) starry_die "Unknown argument: $1 (see the usage at the top of this script)" ;;
    esac
done
[ "$config" != Dev ] || starry_die "Dev is a component build and cannot be packaged; use Release"

out="$CHROMIUM_SRC/out/$config"
if [ ! -f "$out/build.ninja" ]; then
    [ "$build" = 1 ] || starry_die "${out} not found; run scripts/gen.sh $config first"
    "$STARRY_ROOT/scripts/gen.sh" "$config"
fi
# gn 要在源码目录里跑：depot_tools 的 gn 包装按当前目录找 gn。
gn_args="$(cd "$CHROMIUM_SRC" && gn args "out/$config" --list --short --overrides-only)" ||
    starry_die "Cannot read the gn args of out/$config"
gn_value() { printf '%s\n' "$gn_args" | sed -n "s/^$1 = \"\(.*\)\"\$/\1/p"; }
if printf '%s\n' "$gn_args" | grep -q '^is_component_build = true'; then
    starry_die "$config is a component build and cannot be packaged"
fi
if printf '%s\n' "$gn_args" | grep -q '^starry_devtools = true'; then
    starry_die "$config enables starry_devtools, and release packages must not contain debug code; run scripts/gen.sh $config --no-local first"
fi

# 版本号取自 starry/VERSION（SemVer，与 Chromium 内核版本分离）。
version="$STARRY_VERSION"
[ -n "$version" ] || starry_die "starry/VERSION is empty"
linux_version="$(starry_linux_package_version "$version")"
target_os="$(gn_value target_os)"
if [ -z "$target_os" ]; then
    case "$(uname -s)" in
        Darwin) target_os=mac ;;
        Linux) target_os=linux ;;
        MINGW* | MSYS* | CYGWIN*) target_os=win ;;
        *) starry_die "Unsupported OS: $(uname -s)" ;;
    esac
fi
target_cpu="$(gn_value target_cpu)"
case "${target_cpu:-$(uname -m)}" in
    arm64 | aarch64) arch=arm64 ;;
    x64 | x86_64 | amd64 | AMD64) arch=x64 ;;
    *) arch="${target_cpu:-$(uname -m)}" ;;
esac
name="$(printf '%s' "$STARRY_PRODUCT_NAME" | tr -d ' ')"
dist="$STARRY_ROOT/dist"

build_targets() {
    [ "$build" = 1 ] || return 0
    starry_log "Building: $*"
    "$STARRY_ROOT/scripts/build.sh" "$config" "$@"
}

package_mac() {
    build_targets chrome chrome/installer/mac
    local app="$out/$STARRY_PRODUCT_NAME.app"
    local packaging="$out/$STARRY_PRODUCT_NAME Packaging"
    [ -d "$app" ] || starry_die "${app} not found"
    [ -d "$packaging" ] || starry_die "${packaging} not found (chrome/installer/mac was not built)"

    # 构建时就要设好默认数据目录和 Starry 版本号，直接打开 out 里的 App 和打包后保持一致。
    local product_dir
    product_dir="$(/usr/libexec/PlistBuddy -c 'Print :CrProductDirName' \
        "$app/Contents/Info.plist")" || starry_die "The app has no product data directory set; rebuild it"
    [ "$product_dir" = "$STARRY_PRODUCT_NAME" ] ||
        starry_die "The app's data directory is not ${STARRY_PRODUCT_NAME}; rebuild it"
    local app_version
    app_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$app/Contents/Info.plist")" || starry_die "The app has no version; rebuild it"
    [ "$app_version" = "$version" ] ||
        starry_die "The app version is ${app_version}, but starry/VERSION is ${version}; rebuild it"

    # 不用 local：退出时的 trap 要用到它。
    work="$(mktemp -d "${TMPDIR:-/tmp}/starry-package.XXXXXX")"
    trap 'rm -rf "$work"' EXIT
    mkdir -p "$work/input" "$work/output"
    starry_log "Copying the app"
    ditto "$app" "$work/input/$STARRY_PRODUCT_NAME.app"
    ditto "$packaging" "$work/input/$STARRY_PRODUCT_NAME Packaging"

    # 签名：Chromium 的签名流程（按顺序签框架、各 Helper 和它们的 entitlements、主程序）。
    # DMG 自己打：它的 pkg-dmg 做的是 HFS+ 的镜像，给里面每个文件加上 com.apple.FinderInfo，
    # 拷出来的 App 过不了 codesign --strict（也就过不了公证）。
    if [ -n "$notary_profile" ] && [ -z "$identity" ]; then
        starry_die "Notarization needs a Developer ID signature (--identity)"
    fi
    local signed
    if [ -n "$identity" ]; then
        # 签名流程要 Python 3.11 以上（系统自带的是 3.9）：用 depot_tools 带的。
        local python="python3"
        if [ -f "$DEPOT_TOOLS/python3_bin_reldir.txt" ]; then
            python="$DEPOT_TOOLS/$(cat "$DEPOT_TOOLS/python3_bin_reldir.txt")/python3"
        fi
        starry_log "Signing ($identity)"
        "$python" "$work/input/$STARRY_PRODUCT_NAME Packaging/sign_chrome.py" \
            --input "$work/input" --output "$work/output" --disable-packaging \
            --identity "$identity" >"$work/sign.log" 2>&1 ||
            { tail -30 "$work/sign.log" >&2; starry_die "Signing failed"; }
        signed="$(find "$work/output" -maxdepth 3 -type d -name "$STARRY_PRODUCT_NAME.app" | head -1)"
        [ -n "$signed" ] || starry_die "Signing produced no app (output is in $work/output)"
    else
        # 没有证书：整个包 ad-hoc 签名。不开 hardened runtime：开了就要做库校验，ad-hoc 的
        # 框架没有 Team ID，主程序载入不了它（Chromium 的签名流程用 ad-hoc 签出来的就是这样）。
        starry_log "No --identity: signing ad hoc, so the app only opens on this Mac"
        signed="$work/input/$STARRY_PRODUCT_NAME.app"
        codesign --force --deep --sign - "$signed" 2>"$work/sign.log" ||
            { tail -30 "$work/sign.log" >&2; starry_die "Signing failed"; }
    fi
    codesign --verify --deep --strict "$signed" || starry_die "Signature verification failed"

    # DMG：App 和「应用程序」的替身，APFS、LZMA 压缩（macOS 10.15 以后都能打开）。
    starry_log "Creating the DMG"
    local stage="$work/dmg"
    mkdir -p "$stage"
    ditto "$signed" "$stage/$STARRY_PRODUCT_NAME.app"
    ln -s /Applications "$stage/Applications"
    local target="$dist/$name-$version-mac-$arch.dmg"
    rm -f "$target"
    hdiutil create -quiet -volname "$STARRY_PRODUCT_NAME" -srcfolder "$stage" -fs APFS \
        -format ULMO "$target"
    if [ -n "$identity" ]; then
        codesign --sign "$identity" --timestamp "$target"
    fi

    # 公证：交 DMG（里面的 App 一起公证），等结果，把票据钉进 DMG 和 App。
    if [ -n "$notary_profile" ]; then
        starry_log "Notarizing (takes a few minutes)"
        xcrun notarytool submit "$target" --keychain-profile "$notary_profile" --wait
        xcrun stapler staple "$target"
        xcrun stapler staple "$signed"
    fi

    # 签好的 App 也压一份 zip（自动更新、直接拷贝用）。
    ditto -c -k --keepParent "$signed" "$dist/$name-$version-mac-$arch.zip"

    # 核对 DMG 里的 App：签名完整，用户数据目录是自己的。
    local mount="$work/mount"
    mkdir -p "$mount"
    hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mount" "$target" >/dev/null
    codesign --verify --deep --strict "$mount/$STARRY_PRODUCT_NAME.app" ||
        starry_die "Signature verification failed for the app in the DMG"
    codesign -dv "$mount/$STARRY_PRODUCT_NAME.app" 2>&1 |
        grep -E '^(Identifier|Authority|Signature|TeamIdentifier)' >&2 || true
    /usr/libexec/PlistBuddy -c "Print :CrProductDirName" \
        "$mount/$STARRY_PRODUCT_NAME.app/Contents/Info.plist" >&2
    /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
        "$mount/$STARRY_PRODUCT_NAME.app/Contents/Info.plist" >&2
    /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" \
        "$mount/$STARRY_PRODUCT_NAME.app/Contents/Info.plist" >&2
    hdiutil detach "$mount" >/dev/null
    starry_log "Done: $target"
    starry_log "      $dist/$name-$version-mac-$arch.zip"
}

package_win() {
    build_targets chrome mini_installer starry/installer/win:starry_setup
    # 安装程序：mini_installer.exe 嵌在里面，不再单独发布（批量部署用 setup.exe --silent）。
    local setup="$out/starry_setup.exe"
    [ -f "$setup" ] || starry_die "${setup} not found"
    cp "$setup" "$dist/$name-$version-win-$arch-setup.exe"
    starry_log "Done: $dist/$name-$version-win-$arch-setup.exe (unsigned)"

    # 免安装版：mini_installer 打包前的暂存目录，和安装后的 Application 目录是同一套文件，
    # 解压就能运行。用户数据还是在 %LOCALAPPDATA% 下，和安装版共用。
    local bin="$out/gen/chrome/installer/mini_installer/mini_installer/temp_installer_archive/Chrome-bin"
    [ -f "$bin/$STARRY_PRODUCT_ID.exe" ] ||
        starry_die "$bin/$STARRY_PRODUCT_ID.exe not found (did the mini_installer staging directory move?)"
    local zip="$dist/$name-$version-win-$arch-portable.zip"
    rm -f "$zip"
    python3 - "$bin" "$zip" "$name" <<'EOF'
import os, sys, zipfile

src, dst, top = sys.argv[1:]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(root, f)
            z.write(path, os.path.join(top, os.path.relpath(path, src)))
EOF
    starry_log "Done: $zip"
}

package_linux() {
    local targets=(chrome chrome/installer/linux:stable_deb)
    if command -v rpmbuild >/dev/null 2>&1; then
        targets+=(chrome/installer/linux:stable_rpm)
    else
        starry_log "rpmbuild not found; skipping the .rpm"
    fi
    build_targets "${targets[@]}"
    local file found=0
    for file in "$out/${STARRY_PRODUCT_ID}-stable_${linux_version}-1_"*.deb \
                "$out/${STARRY_PRODUCT_ID}-stable-${linux_version}-1."*.rpm; do
        [ -f "$file" ] || continue
        cp "$file" "$dist/$name-$version-linux-$arch.${file##*.}"
        starry_log "Done: $dist/$name-$version-linux-$arch.${file##*.}"
        found=1
    done
    [ "$found" = 1 ] || starry_die "No .deb or .rpm found"
}

case "$target_os" in
    mac) [ "$(uname -s)" = Darwin ] || starry_die "The macOS package can only be built on a Mac" ;;
    linux | win) ;;
    *) starry_die "Unknown target_os: $target_os" ;;
esac

if [ "$keep_dist" = 0 ]; then
    rm -rf "$dist"
fi
mkdir -p "$dist"

case "$target_os" in
    mac) package_mac ;;
    linux) package_linux ;;
    win) package_win ;;
esac
