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

# 交叉编译 Windows 用自己打包的 VS 工具链（DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL 指向放 zip 的目录）时，
# Chromium 要 GYP_MSVS_HASH_<它要的哈希>=<zip 的哈希> 把两者对上。没设而目录里只有一个 zip 时自动对上，
# 免得 Chromium 换工具链版本（TOOLCHAIN_HASH 变了）就要改编译机的环境变量。要在 gclient sync 之后调用。
starry_map_win_toolchain() {
    local dir="${DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL:-}" want var zips got
    [ -n "$dir" ] && [ -d "$dir" ] || return 0
    want="$(sed -n "s/^TOOLCHAIN_HASH = '\([0-9a-f]*\)'.*/\1/p" "$CHROMIUM_SRC/build/vs_toolchain.py" 2>/dev/null)"
    [ -n "$want" ] || return 0
    var="GYP_MSVS_HASH_$want"
    [ -z "${!var:-}" ] || return 0
    [ ! -f "$dir/$want.zip" ] || return 0
    zips=("$dir"/*.zip)
    if [ ${#zips[@]} -eq 1 ] && [ -f "${zips[0]}" ]; then
        got="$(basename "${zips[0]}" .zip)"
        export "$var=$got"
        starry_log "Windows toolchain: $var=$got"
    else
        starry_die "$dir must contain exactly one toolchain zip (or set $var=<zip hash>); found: $(ls "$dir" | tr '\n' ' ')"
    fi
}

starry_log() { printf '[starry] %s\n' "$*" >&2; }
starry_die() { printf '[starry] error: %s\n' "$*" >&2; exit 1; }

starry_require_depot_tools() {
    [ -x "$DEPOT_TOOLS/gclient" ] || starry_die "depot_tools not found; run scripts/bootstrap.sh first"
}

starry_src_at_pinned_version() {
    local expected actual
    expected="$(git -C "$CHROMIUM_SRC" rev-parse --verify "refs/tags/$CHROMIUM_VERSION^{commit}" 2>/dev/null)" || return 1
    actual="$(git -C "$CHROMIUM_SRC" rev-parse --verify HEAD)" || return 1
    [ "$actual" = "$expected" ]
}

starry_require_src() {
    [ -d "$CHROMIUM_SRC/.git" ] || starry_die "Chromium source not found; run scripts/sync.sh first"
}
