#!/bin/bash
# Harbor macOS 工具链 bootstrap —— 在 Mac 上运行（无 root，可重复执行）。
#
#   scripts/bootstrap-macos.sh            # 全部构建（幂等）
#   scripts/bootstrap-macos.sh --force X  # 强制重建某一项
#
# 目录布局（${HARBOR_MACOS_SYSROOT:-$HOME/.local/harbor-macos}）：
#   tools/      构建工具（meson+ninja venv、CMake.app、pkgconf）
#   x86_64/     x86_64 安装前缀（静态库 + 头 + .pc）
#   arm64/      arm64 安装前缀
#   universal/  lipo 合并后的静态库 + 头 + .pc（供 -arch 双架构链接）
#   work/       下载缓存与构建目录（可整体删除重建）
#
# 全部第三方包固定版本并校验 sha256；MACOSX_DEPLOYMENT_TARGET=13.0。
set -euo pipefail

SYSROOT="${HARBOR_MACOS_SYSROOT:-$HOME/.local/harbor-macos}"
TOOLS="$SYSROOT/tools"
WORK="$SYSROOT/work"
ARCHES="x86_64 arm64"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
export MACOSX_DEPLOYMENT_TARGET=13.0

mkdir -p "$TOOLS" "$WORK"
for a in $ARCHES; do mkdir -p "$SYSROOT/$a"; done
mkdir -p "$SYSROOT/universal/lib" "$SYSROOT/universal/lib/pkgconfig" \
         "$SYSROOT/universal/include"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# 每包完成标记：构建成功 touch $PREFIX/.stamp-<name>；--force <name>
# 删 stamp + work 目录即可触发重建（幂等粒度与包对应）。
stamp_done() { [ -f "$PREFIX/.stamp-$1" ]; }
stamp_hit()  { touch "$PREFIX/.stamp-$1"; }
# 旧 sysroot 迁移：产物已装但无 stamp → 补 stamp，不重建。
# 例外：--force 指定的包不补（stamp+work 已删，就是要重建它）。
seed_stamp() {  # seed_stamp <name> <artifact-path>...
    local n="$1"; shift
    [ "$n" = "${FORCED:-}" ] && return 0
    stamp_done "$n" && return 0
    local f
    for f in "$@"; do [ -f "$PREFIX/$f" ] || return 0; done
    stamp_hit "$n"
}

# ---------------------------------------------------------------- 下载/校验

fetch() {  # fetch <file> <url> <sha256>
    local f="$1" url="$2" sum="$3" out
    out="$WORK/$f"
    if [ -f "$out" ]; then
        if [ "$(shasum -a 256 "$out" | awk '{print $1}')" = "$sum" ]; then
            return 0
        fi
        warn "$f checksum mismatch, re-downloading"
        rm -f "$out"
    fi
    log "download $f"
    curl -fL --retry 3 --connect-timeout 20 -o "$out" "$url" \
        || die "download failed: $url"
    [ "$(shasum -a 256 "$out" | awk '{print $1}')" = "$sum" ] \
        || die "$f: sha256 mismatch"
}

extract() {  # extract <file> <dest-dir> → prints top dir
    local f="$1" dst="$2" top
    rm -rf "$dst"
    mkdir -p "$dst"
    tar -xzf "$WORK/$f" -C "$dst" 2>/dev/null \
        || tar -xJf "$WORK/$f" -C "$dst" \
        || die "extract failed: $f"
    top=$(find "$dst" -mindepth 1 -maxdepth 1 -type d | head -1)
    echo "$top"
}

host_for() {  # autoconf --host triple
    case "$1" in
        x86_64) echo x86_64-apple-darwin ;;
        arm64)  echo aarch64-apple-darwin ;;
    esac
}
ssl_target() {  # OpenSSL Configure target
    case "$1" in
        x86_64) echo darwin64-x86_64-cc ;;
        arm64)  echo darwin64-arm64-cc ;;
    esac
}

ARCH="" PREFIX=""
for_arch() {  # for_arch <fn> — 在两架构下执行 fn（ARCH/PREFIX 已设）
    local fn="$1" a
    for a in $ARCHES; do
        ARCH="$a"; PREFIX="$SYSROOT/$a"
        "$fn"
    done
    ARCH=""; PREFIX=""
}

# ------------------------------------------------------------------- 版本表

PKGCONF_VER=3.0.11;     PKGCONF_FILE=pkgconf-3.0.11.tar.xz
PKGCONF_URL="https://distfiles.ariadne.space/pkgconf/$PKGCONF_FILE"
PKGCONF_SHA=d0cd841bc8e55b9da888106c00dc256d05b7d1d8e5a46f4be4eb4c5125bd2dc4

CMAKE_VER=3.31.10;      CMAKE_FILE=cmake-3.31.10-macos-universal.tar.gz
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v3.31.10/$CMAKE_FILE"
CMAKE_SHA=be9f3faeeaf7921cc2d77cea711dd5e6f72c63af2810cacd9205b3ce8d1593c9

OPENSSL_VER=3.5.9;      OPENSSL_FILE=openssl-3.5.9.tar.gz
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-3.5.9/$OPENSSL_FILE"
OPENSSL_SHA=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

LIBSODIUM_VER=1.0.20;   LIBSODIUM_FILE=libsodium-1.0.20.tar.gz
LIBSODIUM_URL="https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/$LIBSODIUM_FILE"
LIBSODIUM_SHA=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19

LIBSSH2_VER=1.11.1;     LIBSSH2_FILE=libssh2-1.11.1.tar.gz
LIBSSH2_URL="https://libssh2.org/download/$LIBSSH2_FILE"
LIBSSH2_SHA=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7

LIBVTERM_VER=0.3.3;     LIBVTERM_FILE=libvterm-0.3.3.tar.gz
LIBVTERM_URL="https://www.leonerd.org.uk/code/libvterm/$LIBVTERM_FILE"
LIBVTERM_SHA=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0

LIBJPEG_VER=3.2.1;      LIBJPEG_FILE=libjpeg-turbo-3.2.1.tar.gz
LIBJPEG_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/$LIBJPEG_FILE"
LIBJPEG_SHA=c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f

LIBVNC_VER=0.9.15;      LIBVNC_FILE=LibVNCServer-0.9.15.tar.gz
LIBVNC_URL="https://github.com/LibVNC/libvncserver/archive/refs/tags/$LIBVNC_FILE"
LIBVNC_SHA=62352c7795e231dfce044beb96156065a05a05c974e5de9e023d688d8ff675d7

FREERDP_VER=3.32.2;     FREERDP_FILE=freerdp-3.32.2.tar.gz
FREERDP_URL="https://pub.freerdp.com/releases/$FREERDP_FILE"
FREERDP_SHA=e95a505c0057e4d06a3b120477c421028769deba4db9c639ef3465d050f0955c

MESON_VER=1.9.1
NINJA_VER=1.13.2

# ------------------------------------------------------------------- 工具层

tools_done() { [ -x "$TOOLS/venv/bin/meson" ] && [ -x "$TOOLS/venv/bin/ninja" ]; }

build_tools() {
    tools_done && { log "tools: meson/ninja already installed"; } || {
        log "tools: python venv + meson $MESON_VER / ninja $NINJA_VER"
        python3 -m venv "$TOOLS/venv"
        "$TOOLS/venv/bin/pip" install --quiet \
            "meson==$MESON_VER" "ninja==$NINJA_VER"
    }

    if [ -x "$TOOLS/CMake.app/Contents/bin/cmake" ]; then
        log "tools: cmake already installed"
    else
        fetch "$CMAKE_FILE" "$CMAKE_URL" "$CMAKE_SHA"
        log "tools: unpack cmake $CMAKE_VER"
        rm -rf "$WORK/cmake-x" && mkdir -p "$WORK/cmake-x"
        tar -xzf "$WORK/$CMAKE_FILE" -C "$WORK/cmake-x"
        rm -rf "$TOOLS/CMake.app"
        cp -R "$WORK/cmake-x/CMake-$CMAKE_VER-macos-universal/CMake.app" \
              "$TOOLS/CMake.app"
    fi

    if [ -x "$TOOLS/bin/pkgconf" ]; then
        log "tools: pkgconf already installed"
    else
        fetch "$PKGCONF_FILE" "$PKGCONF_URL" "$PKGCONF_SHA"
        log "tools: build pkgconf $PKGCONF_VER"
        local src
        src=$(extract "$PKGCONF_FILE" "$WORK/pkgconf-x")
        (cd "$src" && ./configure --prefix="$TOOLS" --disable-shared \
            >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi
    # meson/pkg-config 生态只认 'pkg-config' 这个名字
    [ -x "$TOOLS/bin/pkgconf" ] && \
        ln -sf pkgconf "$TOOLS/bin/pkg-config"
    export PATH="$TOOLS/venv/bin:$TOOLS/CMake.app/Contents/bin:$TOOLS/bin:$PATH"
}

# ------------------------------------------------------------------ 库构建

# OpenSSL 3.5 LTS（静态）。Configure 自带 darwin64-*-cc 目标。
build_openssl() {
    stamp_done openssl && return 0
    fetch "$OPENSSL_FILE" "$OPENSSL_URL" "$OPENSSL_SHA"
    log "openssl $OPENSSL_VER [$ARCH]"
    local src
    src=$(extract "$OPENSSL_FILE" "$WORK/$ARCH/openssl")
    (cd "$src" && ./Configure "$(ssl_target "$ARCH")" no-shared no-tests \
        --prefix="$PREFIX" --libdir=lib >/dev/null \
        && make -j"$JOBS" build_libs >/dev/null \
        && make install_dev >/dev/null) || die "openssl $ARCH"
    stamp_hit openssl
}

# libsodium（静态）
build_sodium() {
    stamp_done sodium && return 0
    fetch "$LIBSODIUM_FILE" "$LIBSODIUM_URL" "$LIBSODIUM_SHA"
    log "libsodium $LIBSODIUM_VER [$ARCH]"
    local src
    src=$(extract "$LIBSODIUM_FILE" "$WORK/$ARCH/libsodium")
    (cd "$src" && ./configure --prefix="$PREFIX" --host="$(host_for "$ARCH")" \
        --disable-shared CC="clang -arch $ARCH" >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null) \
        || die "libsodium $ARCH"
    stamp_hit sodium
}

# libssh2（OpenSSL 后端，静态）
build_libssh2() {
    stamp_done libssh2 && return 0
    fetch "$LIBSSH2_FILE" "$LIBSSH2_URL" "$LIBSSH2_SHA"
    log "libssh2 $LIBSSH2_VER [$ARCH]"
    local src
    src=$(extract "$LIBSSH2_FILE" "$WORK/$ARCH/libssh2")
    (cd "$src" && ./configure --prefix="$PREFIX" --host="$(host_for "$ARCH")" \
        --disable-shared --disable-examples-build --disable-docker-tests \
        --with-crypto=openssl --with-libssl-prefix="$PREFIX" \
        CC="clang -arch $ARCH" \
        PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null) \
        || die "libssh2 $ARCH"
    stamp_hit libssh2
}

# libvterm：上游 Makefile 依赖 GNU libtool（macOS 只有 Apple libtool，
# 接口不兼容）—— 直接编译 src/*.c 打静态库，等价于 libtool 产物。
build_vterm() {
    stamp_done vterm && return 0
    fetch "$LIBVTERM_FILE" "$LIBVTERM_URL" "$LIBVTERM_SHA"
    log "libvterm $LIBVTERM_VER [$ARCH]"
    local src f objdir
    src=$(extract "$LIBVTERM_FILE" "$WORK/$ARCH/libvterm")
    objdir="$src/obj"
    mkdir -p "$objdir" "$PREFIX/lib" "$PREFIX/include" \
             "$PREFIX/lib/pkgconfig"
    for f in "$src"/src/*.c; do
        clang -arch "$ARCH" -O2 -Wall -Wpedantic -std=c99 \
            -I"$src/include" -I"$src/src" \
            -c "$f" -o "$objdir/$(basename "${f%.c}").o" \
            || die "libvterm compile $f"
    done
    ar rcs "$PREFIX/lib/libvterm.a" "$objdir"/*.o || die "libvterm ar"
    ranlib "$PREFIX/lib/libvterm.a"
    cp "$src"/include/*.h "$PREFIX/include/"
    # 上游无 .pc：补一个（静态链接只需 -lvterm）
    cat > "$PREFIX/lib/pkgconfig/vterm.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libvterm
Description: abstract terminal emulation library
Version: $LIBVTERM_VER
Libs: -L\${libdir} -lvterm
Cflags: -I\${includedir}
EOF
    stamp_hit vterm
}

# libjpeg-turbo（cmake；无 nasm → WITH_SIMD=0）
build_jpeg() {
    stamp_done jpeg && return 0
    fetch "$LIBJPEG_FILE" "$LIBJPEG_URL" "$LIBJPEG_SHA"
    log "libjpeg-turbo $LIBJPEG_VER [$ARCH]"
    local src
    src=$(extract "$LIBJPEG_FILE" "$WORK/$ARCH/libjpeg")
    cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DENABLE_SHARED=0 -DENABLE_STATIC=1 -DWITH_SIMD=0 \
        -DWITH_TURBOJPEG=1 >/dev/null
    cmake --build "$src/build" -j "$JOBS" >/dev/null
    cmake --install "$src/build" >/dev/null
    stamp_hit jpeg
}

# LibVNCServer（静态 client + server；禁掉 GnuTLS/SASL/systemd 等）
build_libvnc() {
    stamp_done libvnc && return 0
    # WITH_OPENSSL=OFF → 内置 d3des/sha1 后端：0.9.15 的 crypto_openssl.c
    # 每次 DES 后 OSSL_PROVIDER_unload(default)，把进程全局 libctx 毁了
    #（之后 RAND_bytes 全失败 → libssh2 握手 -1）。VncAuth 不需要 TLS。
    fetch "$LIBVNC_FILE" "$LIBVNC_URL" "$LIBVNC_SHA"
    log "libvncserver $LIBVNC_VER [$ARCH]"
    local src
    src=$(extract "$LIBVNC_FILE" "$WORK/$ARCH/libvnc")
    cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF \
        -DWITH_ZLIB=ON -DWITH_JPEG=ON -DWITH_PNG=OFF -DWITH_LZO=OFF \
        -DWITH_OPENSSL=OFF \
        -DWITH_GNUTLS=OFF -DWITH_SASL=OFF -DWITH_SYSTEMD=OFF \
        -DWITH_GCRYPT=OFF -DWITH_FFMPEG=OFF -DWITH_SDL=OFF \
        -DWITH_WEBSOCKETS=OFF -DWITH_IPv6=ON >/dev/null
    cmake --build "$src/build" -j "$JOBS" >/dev/null
    cmake --install "$src/build" >/dev/null
    # 上游 .pc 把依赖名写成 -lz.tbd/-ljpeg.a 等非法 -l 名 → 改成可解析形式
    sed -i '' \
        's|-lz\.tbd|-lz|g; s|-ljpeg\.a|-ljpeg|g; s|-lssl\.a|-lssl|g; s|-lcrypto\.a|-lcrypto|g' \
        "$PREFIX/lib/pkgconfig/libvncclient.pc" \
        "$PREFIX/lib/pkgconfig/libvncserver.pc" || true
    stamp_hit libvnc
}

# FreeRDP 3.32.2 静态 client（rd_rdp.c 只用 cliprdr/disp/drdynvc）。
build_freerdp() {
    stamp_done freerdp && return 0
    fetch "$FREERDP_FILE" "$FREERDP_URL" "$FREERDP_SHA"
    log "freerdp $FREERDP_VER [$ARCH]"
    local src simd_flag
    src=$(extract "$FREERDP_FILE" "$WORK/$ARCH/freerdp")
    # Intel 交叉 arm64 时 FreeRDP 的 SIMD 探测按 build host 选 SSE 源，
    # Apple clang 不接受其 -msse3 —— 只给 x86_64 开 SIMD。
    if [ "$ARCH" = x86_64 ]; then simd_flag=-DWITH_SIMD=ON; else simd_flag=-DWITH_SIMD=OFF; fi
    # WITH_INTERNAL_MD4/RC4：静态 OpenSSL3 没有 legacy provider →
    # 无 MD4/RC4，CredSSP/NTLM(NLA) 起不来；用 WinPR 内置实现。
    cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF -DSTATIC_CHANNELS=ON \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DWITH_CLIENT=ON -DWITH_CLIENT_COMMON=ON \
        -DWITH_SERVER=OFF -DWITH_SAMPLE=OFF -DWITH_MANPAGES=OFF \
        -DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_DIRECTFB=OFF \
        -DWITH_MACAUDIO=OFF -DWITH_IOSAUDIO=OFF \
        -DWITH_ALSA=OFF -DWITH_PULSE=OFF -DWITH_OSS=OFF \
        -DWITH_SNDIO=OFF -DWITH_SOUND=OFF -DWITH_DSP_FFMPEG=OFF \
        -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF -DWITH_CAIRO=OFF \
        -DWITH_CUPS=OFF -DWITH_PCSC=OFF -DWITH_FUSE=OFF \
        -DWITH_KRB5=OFF -DWITH_SMARTCARD_INSPECT=OFF \
        -DWITH_JPEG=ON -DJPEG_INCLUDE_DIR="$PREFIX/include" \
        -DJPEG_LIBRARY="$PREFIX/lib/libjpeg.a" \
        -DWITH_OPENSSL=ON -DOPENSSL_ROOT_DIR="$PREFIX" \
        -DWITH_INTERNAL_MD4=ON -DWITH_INTERNAL_RC4=ON \
        -DWITH_ZLIB=ON -DWITH_WEBVIEW=OFF -DWITH_CLIENT_SDL=OFF \
        -DWITH_INTERWORKING=OFF -DWITH_SHADOW=OFF \
        -DWITH_PROXY=OFF -DWITH_AAD=OFF \
        -DCHANNEL_URBDRC=OFF -DCHANNEL_SERIAL=OFF \
        -DCHANNEL_PARALLEL=OFF -DCHANNEL_PRN=OFF \
        -DCHANNEL_RDPSND=OFF -DCHANNEL_AUDIN=OFF \
        -DCHANNEL_DRIVE=OFF -DCHANNEL_SMARTCARD=OFF \
        $simd_flag >/dev/null
    cmake --build "$src/build" -j "$JOBS" >/dev/null
    cmake --install "$src/build" >/dev/null
    stamp_hit freerdp
}

# 向 .pc 的 Libs.private 幂等追加 token（已存在则不动）—— 修复上游
# pkg-config 元数据，不属于构建本身，故不受 stamp 跳过。
pc_ensure() {  # pc_ensure <pkg> <token>...
    local pc="$PREFIX/lib/pkgconfig/$1.pc" t
    shift
    [ -f "$pc" ] || return 0
    for t in "$@"; do
        grep -qF -- "$t" "$pc" 2>/dev/null || \
            sed -i '' "s|^Libs\.private:.*|& $t|" "$pc"
    done
}
# winpr/freerdp 静态链接还需 Apple 框架/ObjC 运行时，上游 .pc 未列出：
# winpr sysinfo.c 用 IOPS*（IOKit），freerdp keyboard_apple.c 用 TIS*
#（Carbon/HIToolbox），两者都要 CF/CoreServices/Foundation/objc。
# 注意 '-framework X' 必须作为整体追加（grep -F 整串判存）。
fixup_freerdp_pcs() {
    pc_ensure winpr3 "-framework CoreFoundation" "-framework CoreServices" \
        "-framework Foundation" "-framework IOKit" "-lobjc"
    pc_ensure freerdp3 "-framework Carbon"
}

# sqlite3/zlib 用 SDK 的 .tbd：补 .pc 供 meson/pkgconf 解析
#（libssh2.pc 的 Requires: zlib 也要它）。
write_sdk_pcs() {
    mkdir -p "$PREFIX/lib/pkgconfig"
    cat > "$PREFIX/lib/pkgconfig/sqlite3.pc" <<EOF
Name: sqlite3
Description: SQLite database engine (Apple SDK)
Version: 3.50.0
Libs: -lsqlite3
Cflags:
EOF
    cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: zlib compression library (Apple SDK)
Version: 1.2.12
Libs: -lz
Cflags:
EOF
}

build_one_arch() {  # 单架构全量（顺序：ssl 先于依赖它的包）
    # 旧 sysroot 迁移：产物已装但无 stamp → 补 stamp，保持幂等
    seed_stamp openssl  lib/libcrypto.a
    seed_stamp sodium   lib/libsodium.a
    seed_stamp libssh2  lib/libssh2.a
    seed_stamp vterm    lib/libvterm.a
    seed_stamp jpeg     lib/libjpeg.a
    seed_stamp libvnc   lib/libvncclient.a lib/libvncserver.a
    seed_stamp freerdp  lib/libfreerdp-client3.a
    build_openssl
    build_sodium
    build_libssh2
    build_vterm
    build_jpeg
    build_libvnc
    build_freerdp
    fixup_freerdp_pcs
    write_sdk_pcs
}

# ------------------------------------------------------------- universal 合并

make_universal() {
    log "universal: lipo merge"
    local lib name out="$SYSROOT/universal/lib"
    # find 递归：freerdp3/libremdesk-common.a 这类子目录库也要合并
    (cd "$SYSROOT/x86_64/lib" && find . -name '*.a' -type f) | \
    while read -r lib; do
        lib="${lib#./}"
        if [ -f "$SYSROOT/arm64/lib/$lib" ]; then
            mkdir -p "$out/$(dirname "$lib")"
            lipo -create "$SYSROOT/x86_64/lib/$lib" \
                         "$SYSROOT/arm64/lib/$lib" \
                 -output "$out/$lib" || die "lipo $lib"
        else
            warn "arm64 missing $lib, skipped"
        fi
    done
    # 头与 .pc：按 x86_64 侧拷贝，prefix 改到 universal
    cp -R "$SYSROOT/x86_64/include/." "$SYSROOT/universal/include/"
    for pc in "$SYSROOT/x86_64/lib/pkgconfig"/*.pc; do
        name=$(basename "$pc")
        sed -e "s|$SYSROOT/x86_64|$SYSROOT/universal|g" "$pc" \
            > "$SYSROOT/universal/lib/pkgconfig/$name"
    done
}

# --------------------------------------------------------------------- main

FORCED=""
case "${1:-}" in
    --force) [ -n "${2:-}" ] || die "--force needs a name"
             FORCED="$2"
             rm -rf "$WORK"/*/"$2"
             rm -f "$SYSROOT"/*/.stamp-"$2" ;;
    ""|--all) ;;
    *) die "usage: $0 [--force <name>]" ;;
esac

build_tools
for_arch build_one_arch
make_universal
log "done: $SYSROOT"
