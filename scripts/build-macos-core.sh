#!/bin/bash
# 在 Mac 上构建 universal libharbor-core.a（x86_64 native + arm64 cross）。
#
#   scripts/build-macos-core.sh [SRC_DIR] [BUILD_DIR]
#
# 默认 SRC_DIR=$HOME/harbor，BUILD_DIR=$HOME/harbor-build。
# 产物：$BUILD_DIR/universal/libharbor-core.a + $BUILD_DIR/link-line.txt
set -euo pipefail

SRC="${1:-$HOME/harbor}"
BUILD="${2:-$HOME/harbor-build}"
SYSROOT="${HARBOR_MACOS_SYSROOT:-$HOME/.local/harbor-macos}"
# 第三参 release：release 目录 + -Db_ndebug（产物 NDEBUG，测试钩子
# 与 [sftpdbg] 不进发布二进制）；否则沿用既有 debug 目录。
SUF=""
if [ "${3:-}" = "release" ]; then
    SUF="-rel"
fi

source "$SRC/scripts/env-macos.sh"

# rsync 从 Linux 同步来的 mtime 可能比本机时钟快几十秒 → ninja
# clock-skew 报错；全部 mtime 归一到现在。
find "$SRC" -type f -exec touch {} + 2>/dev/null || true

UNILIB="$BUILD/universal"
mkdir -p "$UNILIB"

# ---------------------------------------------------------------- x86_64

if [ ! -f "$BUILD/x86_64$SUF/build.ninja" ]; then
    meson setup "$BUILD/x86_64$SUF" "$SRC" \
        -Dlinux=false -Dserver=false -Dtests=true \
        --pkg-config-path "$SYSROOT/x86_64/lib/pkgconfig" \
        ${SUF:+-Dbuildtype=release -Db_ndebug=true}
fi
ninja -C "$BUILD/x86_64$SUF"

# ------------------------------------------------------------------ arm64

sed -e "s|@HARBOR_MACOS_SYSROOT@|$SYSROOT|g" \
    "$SRC/macos/meson/darwin-arm64.ini.in" > "$BUILD/darwin-arm64.ini"
if [ ! -f "$BUILD/arm64$SUF/build.ninja" ]; then
    meson setup "$BUILD/arm64$SUF" "$SRC" \
        --cross-file "$BUILD/darwin-arm64.ini" \
        -Dlinux=false -Dserver=false -Dtests=true \
        ${SUF:+-Dbuildtype=release -Db_ndebug=true}
fi
ninja -C "$BUILD/arm64$SUF"

# -------------------------------------------------------------- universal

lipo -create "$BUILD/x86_64$SUF/core/libharbor-core.a" \
             "$BUILD/arm64$SUF/core/libharbor-core.a" \
     -output "$UNILIB/libharbor-core.a"
lipo -info "$UNILIB/libharbor-core.a"

# Swift app（M1）需要的完整静态链接行：以 universal 前缀的 .pc
# 做 pkgconf --static 闭包解析（含 Requires.private/Libs.private
# 的 -framework/-l 项），免得手工列表漏包。
# pkgconf --libs 逐 token 改写：universal 里存在 .a 的 -lfoo 换成全路径；
# -L 与 -framework/系统 -l 原样保留（bash3.2 兼容：不在 $() 里写注释）。
rewrite_tokens() {
    local tok n
    while read -r tok; do
        case "$tok" in
        -L*)
            printf '%s\n' "$tok" ;;
        -l*)
            n="${tok#-l}"
            if [ -f "$SYSROOT/universal/lib/lib$n.a" ]; then
                printf '%s\n' "$SYSROOT/universal/lib/lib$n.a"
            else
                printf '%s\n' "$tok"
            fi ;;
        *)
            printf '%s\n' "$tok" ;;
        esac
    done
}
DEPLIBS=$(PKG_CONFIG_LIBDIR="$SYSROOT/universal/lib/pkgconfig" \
    "$SYSROOT/tools/bin/pkgconf" --static --libs \
    libsodium vterm libssh2 libcrypto libvncclient \
    freerdp-client3 winpr3 2>/dev/null | tr -s ' ' '\n' | \
    rewrite_tokens | paste -sd' ' -)
{
    echo "$UNILIB/libharbor-core.a \\"
    echo "$DEPLIBS -lsqlite3"
} > "$BUILD/link-line.txt"
cat "$BUILD/link-line.txt"
