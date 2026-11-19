# 公共环境，其他脚本 source 它：
#   source "$(dirname "$0")/env.sh"
#
# 可以用环境变量改位置：
#   STARRY_CHROMIUM_ROOT  gclient 根目录（默认 <仓库>/chromium，源码在它下面的 src/）
#   STARRY_DEPOT_TOOLS    depot_tools 目录（默认 <仓库>/vendor/depot_tools）

STARRY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STARRY_VERSION="$(tr -d ' \r\n' < "$STARRY_ROOT/starry/VERSION")"
CHROMIUM_VERSION="$(tr -d ' \r\n' < "$STARRY_ROOT/chromium.version")"
CHROMIUM_ROOT="${STARRY_CHROMIUM_ROOT:-$STARRY_ROOT/chromium}"
CHROMIUM_SRC="$CHROMIUM_ROOT/src"
DEPOT_TOOLS="${STARRY_DEPOT_TOOLS:-$STARRY_ROOT/vendor/depot_tools}"
STARRY_LOGS="$STARRY_ROOT/.logs"

export PATH="$DEPOT_TOOLS:$PATH"
# 不往 Google 发 depot_tools 的使用统计
export DEPOT_TOOLS_METRICS=0

# 产品名（starry/app/theme/BRANDING 的 PRODUCT_FULLNAME）。macOS 上 .app、主程序、Helper、
# Framework 的名字都从它来。
STARRY_PRODUCT_NAME="$(sed -n 's/^PRODUCT_FULLNAME=//p' "$STARRY_ROOT/starry/app/theme/BRANDING")"
STARRY_PRODUCT_ID="$(sed -n 's/^PRODUCT_ID=//p' "$STARRY_ROOT/starry/app/theme/BRANDING")"

# 把 SemVer 映射成 Debian / RPM 包版本：去掉 +build，-prerelease 变成 ~prerelease
# （内部 '-' 转 '.'），让包管理器把预发布版排在正式版之前。
starry_linux_package_version() {
    local v="${1%%+*}"
    case "$v" in
        *-*)
            local base="${v%%-*}"
            local pre="${v#*-}"
            printf '%s~%s' "$base" "$(printf '%s' "$pre" | tr '-' '.')"
            ;;
        *)
            printf '%s' "$v"
            ;;
    esac
}

# 浏览器的主程序：starry_browser_binary <配置>
starry_browser_binary() {
    case "$(uname -s)" in
        Darwin) printf '%s' "$CHROMIUM_SRC/out/$1/$STARRY_PRODUCT_NAME.app/Contents/MacOS/$STARRY_PRODUCT_NAME" ;;
        MINGW* | MSYS* | CYGWIN*) printf '%s' "$CHROMIUM_SRC/out/$1/$STARRY_PRODUCT_ID.exe" ;;
        *) printf '%s' "$CHROMIUM_SRC/out/$1/chrome" ;;
    esac
}

starry_log() { printf '[starry] %s\n' "$*" >&2; }
starry_die() { printf '[starry] 错误：%s\n' "$*" >&2; exit 1; }

starry_require_depot_tools() {
    [ -x "$DEPOT_TOOLS/gclient" ] || starry_die "没有 depot_tools，先运行 scripts/bootstrap.sh"
}

starry_require_src() {
    [ -d "$CHROMIUM_SRC/.git" ] || starry_die "没有 Chromium 源码，先运行 scripts/sync.sh"
}
