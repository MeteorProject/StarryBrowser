#!/usr/bin/env bash
# 应用图标：从 starry/app/icon/ 的 SVG 生成三个
# 平台的图标文件，换掉 chromium/src 里 Chromium 的图标，再导出成补丁。
#
#   scripts/app-icon.sh        生成、写进 src、scripts/patches.sh export
#
# 图标是二进制补丁（patches/chrome/app/theme/ 下），平时不用跑；改了 SVG 之后跑一次。
# 只能在 macOS 上跑：SVG 用 WebKit 渲染（render.swift），macOS 的图标要 actool、iconutil。
# 128 px 以上用 icon.svg，64、32、16 px 各有一张精简版本。
set -euo pipefail
source "$(dirname "$0")/env.sh"
starry_require_src
[ "$(uname -s)" = Darwin ] || starry_die "只能在 macOS 上运行（要用 WebKit、actool、iconutil）"

src="$STARRY_ROOT/starry/app/icon"
theme="$CHROMIUM_SRC/chrome/app/theme"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

starry_log "编译 SVG 渲染工具"
xcrun swiftc -O -swift-version 5 "$src/render.swift" -o "$tmp/render"

# 每个尺寸用哪张 SVG：不大于 16 用 icon-16，不大于 32 用 icon-32，不大于 64 用 icon-64。
svg_for() {
    if [ "$1" -le 16 ]; then echo "$src/icon-16.svg"
    elif [ "$1" -le 32 ]; then echo "$src/icon-32.svg"
    elif [ "$1" -le 64 ]; then echo "$src/icon-64.svg"
    else echo "$src/icon.svg"
    fi
}

# Windows 开始菜单的磁贴：图标只占中间一半，四周透明（同 Chromium 的 Logo.png）。
sed 's|viewBox="0 0 1024 1024"|viewBox="-512 -512 2048 2048"|' "$src/icon.svg" > "$tmp/tile.svg"

jobs=()
for size in 16 20 22 24 32 40 44 48 64 96 128 256 512 1024; do
    jobs+=("$(svg_for "$size")" "$tmp/$size.png" "$size")
done
jobs+=("$tmp/tile.svg" "$tmp/tile-600.png" 600 "$tmp/tile.svg" "$tmp/tile-176.png" 176)
starry_log "渲染 PNG"
perl -e 'alarm 300; exec @ARGV' "$tmp/render" "${jobs[@]}" >/dev/null

# macOS：Assets.car（AppIcon 是 App 的图标，Icon 是文档类型的角标）和老系统用的 app.icns。
starry_log "macOS：Assets.car、app.icns"
assets="$tmp/Assets.xcassets"
mkdir -p "$assets/AppIcon.appiconset" "$assets/Icon.iconset" "$tmp/app.iconset" "$tmp/car"
printf '{\n  "info" : {\n    "author" : "xcode",\n    "version" : 1\n  }\n}\n' > "$assets/Contents.json"
entries=""
for spec in 16:1 16:2 32:1 32:2 128:1 128:2 256:1 256:2 512:1 512:2; do
    point="${spec%%:*}"
    scale="${spec##*:}"
    pixels=$((point * scale))
    name="icon_${point}x${point}"
    [ "$scale" = 2 ] && name="${name}@2x"
    cp "$tmp/$pixels.png" "$assets/AppIcon.appiconset/$name.png"
    cp "$tmp/$pixels.png" "$tmp/app.iconset/$name.png"
    [ -n "$entries" ] && entries="$entries,"
    entries="$entries
    { \"filename\" : \"$name.png\", \"idiom\" : \"mac\", \"scale\" : \"${scale}x\", \"size\" : \"${point}x${point}\" }"
done
printf '{\n  "images" : [%s\n  ],\n  "info" : {\n    "author" : "xcode",\n    "version" : 1\n  }\n}\n' \
    "$entries" > "$assets/AppIcon.appiconset/Contents.json"
cp "$tmp/256.png" "$assets/Icon.iconset/icon_256x256.png"
cp "$tmp/512.png" "$assets/Icon.iconset/icon_256x256@2x.png"
xcrun actool --compile "$tmp/car" --platform macosx --minimum-deployment-target 12.0 \
    --app-icon AppIcon --output-partial-info-plist "$tmp/partial.plist" \
    --output-format human-readable-text --notices --warnings "$assets" >/dev/null
iconutil -c icns -o "$tmp/app.icns" "$tmp/app.iconset"
cp "$tmp/car/Assets.car" "$theme/chromium/mac/Assets.car"
cp "$tmp/app.icns" "$theme/chromium/mac/app.icns"

# Windows：程序的图标（chromium.ico）、开始菜单的磁贴。
starry_log "Windows：chromium.ico、磁贴"
python3 "$src/make_ico.py" "$theme/chromium/win/chromium.ico" \
    "$tmp/16.png" "$tmp/20.png" "$tmp/24.png" "$tmp/32.png" "$tmp/40.png" \
    "$tmp/48.png" "$tmp/64.png" "$tmp/96.png" "$tmp/128.png" "$tmp/256.png"
cp "$tmp/tile-600.png" "$theme/chromium/win/tiles/Logo.png"
cp "$tmp/tile-176.png" "$theme/chromium/win/tiles/SmallLogo.png"

# Linux 的应用图标，以及 Chrome 界面里用到产品图标的地方（关于页、通知……）。
starry_log "Linux 和界面里的产品图标"
for size in 24 48 64 128 256; do
    cp "$tmp/$size.png" "$theme/chromium/linux/product_logo_$size.png"
done
for size in 16 24 48 64 128 256; do
    cp "$tmp/$size.png" "$theme/chromium/product_logo_$size.png"
done
for size in 16 32; do
    cp "$tmp/$size.png" "$theme/default_100_percent/chromium/product_logo_$size.png"
    cp "$tmp/$size.png" "$theme/default_100_percent/chromium/linux/product_logo_$size.png"
    cp "$tmp/$((size * 2)).png" "$theme/default_200_percent/chromium/product_logo_$size.png"
done

# 支付对话框里的产品标识（原来的图标带 Chromium 字样），用 Starry 的应用图标。
for variant in "" _white; do
    cp "$tmp/22.png" "$theme/default_100_percent/chromium/product_logo_name_22${variant}.png"
    cp "$tmp/44.png" "$theme/default_200_percent/chromium/product_logo_name_22${variant}.png"
done

# 界面里的单色矢量产品图标（默认浏览器提示、命令栏等）。
for icon in components/vector_icons/chromium/product.icon \
            components/vector_icons/chromium/product_refresh.icon \
            components/omnibox/browser/vector_icons/chrome_product.icon \
            components/omnibox/browser/vector_icons/product_chrome_refresh_old.icon \
            components/omnibox/browser/vector_icons/product_old.icon; do
    cp "$src/product.icon" "$CHROMIUM_SRC/$icon"
done

"$STARRY_ROOT/scripts/patches.sh" export
starry_log "完成。macOS 换了图标后 Dock 可能还显示旧的：touch 一下 .app 刷新图标缓存"
