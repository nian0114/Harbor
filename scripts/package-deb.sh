#!/usr/bin/env bash
# package-deb.sh — 构建 harbor / harbor-server 的 .deb（Ubuntu 24.04 amd64）。
#
#   - fresh release builddir: -Dbuildtype=release -Db_ndebug=true
#     --prefix=/usr（meson 顶层守卫会拦 b_ndebug 缺失的 release）
#   - 链接期 strip（系统无可用 strip 二进制时用 -Wl,--strip-all）
#   - 发布检查：二进制 strings 不得含 dev hook 符号
#   - Depends：ldd 解析 soname → 在 sysroot debcache 里按
#     `dpkg-deb -c` 反查属主包 + 版本（dpkg-shlibdeps 不在时的
#     ldd+包表回退路径；属主包集合即运行所须的 Ubuntu 24.04 包）
#   - postinst/prerm 不需要：hicolor icon cache 和 desktop db 由
#     dpkg 自带 file triggers 刷新（见 usr/lib/dpkg/triggers）。
#
# 用法: scripts/package-deb.sh [版本号（默认 meson 项目版本）]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${HARBOR_SYSROOT:-$HOME/.local/harbor-sysroot}"
DEBCACHE="${HARBOR_DEBCACHE:-$HOME/.cache/harbor-debs}"
VER="${1:-$(sed -n "s/^project('harbor', 'c',//;s/.*version: '\([^']*\)'.*/\1/p" \
            "$ROOT/meson.build" | head -1)}"
VER="${VER:-0.1.0}"
WORK="$ROOT/.pkg-deb"
BLD="$WORK/build"
DEST="$WORK/dest"
OUT="$WORK/out"

# 禁止打包的二进制中出现的开发钩子痕迹
FORBIDDEN=(
    HARBOR_DEV_ACTIONS HARBOR_DEV_ACCEPT_HOSTKEY
    HARBOR_DEV_CANCEL_PROMPT HARBOR_RELAY_DROP
    HARBOR_SERVER_FRAME_DUMP on_dev_ sync-auth
)

echo "==> harbor $VER deb 打包（fresh release builddir: $BLD）"
rm -rf "$WORK"; mkdir -p "$DEST" "$OUT"

# shellcheck disable=SC1091
source "$ROOT/scripts/env.sh"

LDFLAGS="${LDFLAGS:-} -Wl,--strip-all" \
    meson setup "$BLD" "$ROOT" \
        -Dbuildtype=release -Db_ndebug=true --prefix=/usr \
        -Dserver=true
ninja -C "$BLD"
DESTDIR="$DEST" ninja -C "$BLD" install

# --- release check：dev hooks 必须被 NDEBUG 编译掉 ------------------------
echo "==> release check"
fail=0
for bin in "$DEST/usr/bin/harbor" "$DEST/usr/bin/harbor-server"; do
    for pat in "${FORBIDDEN[@]}"; do
        if grep -qa "$pat" "$bin"; then
            echo "    !! $bin contains dev hook '$pat'" >&2
            fail=1
        fi
    done
done
[ "$fail" = 0 ] || { echo "release check failed" >&2; exit 1; }
echo "    clean"

# --- strip（链接期 --strip-all 已剥；binutils strip 若在则二次确认） -------
STRIP="$(command -v strip || true)"
if [ -n "$STRIP" ] && "$STRIP" --version >/dev/null 2>&1; then
    "$STRIP" --strip-unneeded "$DEST/usr/bin/harbor" \
            "$DEST/usr/bin/harbor-server" 2>/dev/null || true
else
    echo "    (无可用 strip —— 已依赖链接期 -Wl,--strip-all)"
fi

# --- Depends：ldd soname → debcache 反查属主包 -----------------------------
declare -A SONAME_PKG
build_soname_table() {
    # 扫 debcache 每个 deb 的文件清单，建 /usr/lib 下 .so → 包的表
    local deb files pkg ver
    for deb in "$DEBCACHE"/*.deb; do
        pkg="$(dpkg-deb -f "$deb" Package 2>/dev/null)" || continue
        ver="$(dpkg-deb -f "$deb" Version 2>/dev/null)" || continue
        while IFS= read -r f; do
            case "$f" in
                */lib*.so.*|*/ld-linux*.so.*)
                    SONAME_PKG["$(basename "$f")"]="$pkg|$ver|$deb" ;;
            esac
        done < <(dpkg-deb -c "$deb" | awk '{print $6}')
    done
}

deps_for() {
    local bin="$1" soname dep=""
    declare -A seen=()
    while IFS= read -r soname; do
        soname="$(basename "$soname")"
        case "$soname" in
            linux-vdso*|ld-linux*|libharbor*|harbor) continue ;;
        esac
        local ent="${SONAME_PKG[$soname]:-}"
        if [ -z "$ent" ]; then
            echo "    ? soname $soname 未在 debcache 找到属主包" >&2
            continue
        fi
        local pkg="${ent%%|*}" rest="${ent#*|}"
        local ver="${rest%%|*}"
        seen["$pkg"]="$ver"
    done < <(ldd "$bin" | awk '/=> \//{print $3} /^\t?.*ld-linux/{print $1}')
    local out=""
    for pkg in "${!seen[@]}"; do
        out+=", $pkg (>= ${seen[$pkg]})"
    done
    echo "${out#, }"
}

echo "==> 建立 soname→deb 映射（$(ls "$DEBCACHE"/*.deb | wc -l) 个缓存包）"
build_soname_table

mkdeb() {
    local name="$1" bindir_content="$2" desc="$3" moredirs="$4"
    local root="$WORK/$name"
    mkdir -p "$root/DEBIAN" "$root/usr/bin"
    for f in $bindir_content; do
        cp -a "$DEST/usr/bin/$f" "$root/usr/bin/"
    done
    if [ -n "$moredirs" ]; then
        for d in $moredirs; do
            (cd "$DEST" && cp -a --parents "$d" "$root")
        done
    fi
    local deps
    deps="$(deps_for "$root/usr/bin/$(echo $bindir_content | awk '{print $1}')" \
            || true)"
    # dpkg-deb --root-owner-group：无需 root 即可属 root
    {
        echo "Package: $name"
        echo "Version: $VER"
        echo "Architecture: amd64"
        echo "Maintainer: Harbor <noreply@harbor.dev>"
        echo "Installed-Size: $(du -sk "$root" | cut -f1)"
        echo "Section: net"
        echo "Priority: optional"
        [ -n "$deps" ] && echo "Depends: $deps"
        echo "Description: $desc"
    } > "$root/DEBIAN/control"
    dpkg-deb --root-owner-group --build "$root" "$OUT/${name}_${VER}_amd64.deb"
}

# harbor 客户端包：二进制 + desktop/metainfo/icons
mkdeb harbor harbor \
    "SSH client and remote workspace (GTK4/libadwaita).
 Terminal, SFTP, port forwarding, remote desktop, shared consoles." \
    "usr/share/applications usr/share/metainfo usr/share/icons"

# harbor-server 包：服务器二进制 + systemd 用户单元示例
mkdeb harbor-server harbor-server \
    "Sync/share relay server for the Harbor client." \
    "usr/share/doc"

echo
ls -la "$OUT"
for d in "$OUT"/*.deb; do
    echo "== $d"
    dpkg-deb -I "$d" | sed -n '1,14p'
done
echo "debs in $OUT"
