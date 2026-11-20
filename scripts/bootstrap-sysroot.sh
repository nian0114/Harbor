#!/usr/bin/env bash
# Harbor: 无 root 构建 sysroot 引导脚本（Ubuntu 24.04, amd64）。
#
# 原理：apt-get download 以普通用户身份下载 .deb（不触碰系统 dpkg 数据库），
#       dpkg -x 解包到用户目录 sysroot，再做少量修正（merged-usr、gcc 包装器、
#       缺失运行库符号链接）。
#
# 用法:  scripts/bootstrap-sysroot.sh       # 幂等，可重复执行
# 然后:  source scripts/env.sh
set -euo pipefail

SYSROOT="${HARBOR_SYSROOT:-$HOME/.local/harbor-sysroot}"
DEBCACHE="${HARBOR_DEBCACHE:-$HOME/.cache/harbor-debs}"

# 种子包；递归 Depends 会自动带出全部传递依赖（含运行时库）。
SEEDS=(
    gcc g++ make pkgconf pkg-config meson ninja-build
    libgtk-4-dev libadwaita-1-dev libvte-2.91-dev
    libssh2-1-dev libssl-dev libsodium-dev libsqlite3-dev
    libvterm-dev libvncserver-dev
    freerdp3-dev libwinpr3-dev
    libjson-glib-dev libsoup-3.0-dev libsecret-1-dev
    libglib2.0-dev libglib2.0-dev-bin
    xvfb xauth xfonts-base
    zsh fish
)

APT_LINE="sudo apt install build-essential pkg-config meson ninja-build \
libgtk-4-dev libadwaita-1-dev libvte-2.91-dev libssh2-1-dev libssl-dev \
libsodium-dev libsqlite3-dev libvterm-dev libvncserver-dev freerdp3-dev \
libwinpr3-dev libjson-glib-dev libsoup-3.0-dev libsecret-1-dev libglib2.0-dev \
xvfb xauth"

echo "==> 若本机可用 root，等价做法是:"
echo "    $APT_LINE"
echo

mkdir -p "$SYSROOT" "$DEBCACHE"

echo "==> 解析递归依赖 ..."
mapfile -t PKGS < <(
    apt-cache depends --recurse \
        --no-recommends --no-suggests --no-conflicts \
        --no-breaks --no-replaces --no-enhances \
        "${SEEDS[@]}" 2>/dev/null \
    | grep -oE '^[a-zA-Z0-9][a-zA-Z0-9+._-]*' | sort -u
)
echo "    候选包: ${#PKGS[@]}"

echo "==> 下载 .deb 到 $DEBCACHE（apt 自身会跳过已下载的文件）..."
cd "$DEBCACHE"
failed=()
for p in "${PKGS[@]}"; do
    # 虚包没有候选版本，跳过
    apt-cache policy "$p" 2>/dev/null | grep -q 'Candidate: [0-9]' || continue
    apt-get download "$p" >/dev/null 2>&1 || failed+=("$p")
done
if ((${#failed[@]})); then
    echo "    ! 下载失败: ${failed[*]}" >&2
fi

echo "==> 解包所有 .deb 到 $SYSROOT ..."
for f in "$DEBCACHE"/*.deb; do
    dpkg -x "$f" "$SYSROOT"
done

# --- merged-usr 修正 -------------------------------------------------------
# noble 的 .deb 把库放在 usr/lib/...，但 libc.so 链接脚本引用 /lib/...、
# /lib64/... 绝对路径。真实系统上 /lib 是指向 usr/lib 的符号链接，
# sysroot 里补出同样的布局即可。
for d in bin sbin lib lib64; do
    if [ -d "$SYSROOT/$d" ] && [ ! -L "$SYSROOT/$d" ]; then
        mkdir -p "$SYSROOT/usr/$d"
        cp -a "$SYSROOT/$d/." "$SYSROOT/usr/$d/" && rm -rf "$SYSROOT/$d"
    fi
    [ -e "$SYSROOT/$d" ] || ln -s "usr/$d" "$SYSROOT/$d"
done

# --- gcc/g++ 包装器：注入 --sysroot ----------------------------------------
mkdir -p "$SYSROOT/tool-bin"
for t in cc gcc gcc-13 g++ g++-13 cpp \
         x86_64-linux-gnu-gcc x86_64-linux-gnu-gcc-13 \
         x86_64-linux-gnu-g++ x86_64-linux-gnu-g++-13 \
         x86_64-linux-gnu-cpp x86_64-linux-gnu-cpp-13; do
    real="$t"
    [ "$t" = "cc" ] && real="gcc"
    [ "$t" = "cpp" ] && real="x86_64-linux-gnu-cpp"
    if [ -x "$SYSROOT/usr/bin/$real" ]; then
        printf '#!/bin/sh\nexec "%s/usr/bin/%s" --sysroot="%s" "$@"\n' \
            "$SYSROOT" "$real" "$SYSROOT" > "$SYSROOT/tool-bin/$t"
        chmod +x "$SYSROOT/tool-bin/$t"
    fi
done

# --- runtime-libs：系统缺失的库 --------------------------------------------
# .deb 总会把 SONAME 以实体文件或符号链接的形式装出来，所以直接遍历文件名
# 与系统 ldconfig 对比即可（无需 readelf —— readelf 自身也依赖这里的库）。
# 结果只有叶子库（libssh2、libvterm、libvncclient、libbfd 等），不含 libc。
mkdir -p "$SYSROOT/runtime-libs"
ldconfig -p | awk '{print $1}' | sort -u > "$SYSROOT/.syslibs.tmp"
find "$SYSROOT/usr/lib/x86_64-linux-gnu" -maxdepth 1 -name 'lib*.so*' \
    | while read -r f; do
    base=$(basename "$f")
    # 只要 SONAME 层的名字（.so / .so.N），跳过 .so.N.M 版本实体文件
    echo "$base" | grep -qE '\.so(\.[0-9]+)?$' || continue
    grep -qx "$base" "$SYSROOT/.syslibs.tmp" && continue
    tgt=$(readlink -f "$f")
    # 裸 .so 名字：-dev 包的开发链接（libfoo.so 是指向 .so.N 的符号链接，
    # 运行时不会被查找）跳过；只保留实体 ELF 文件（如 libbfd-2.42-system.so）
    case "$base" in
        *.so) [ -L "$f" ] && continue
              head -c4 "$tgt" 2>/dev/null | grep -q 'ELF' || continue ;;
    esac
    ln -sf "$tgt" "$SYSROOT/runtime-libs/$base"
done
rm -f "$SYSROOT/.syslibs.tmp"

echo
echo "==> 完成。使用:  source $(dirname "$0")/env.sh"
