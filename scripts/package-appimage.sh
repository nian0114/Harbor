#!/usr/bin/env bash
# package-appimage.sh — 构建 Harbor-<ver>-x86_64.AppImage（rootless）。
#
#   - fresh release builddir: -Dbuildtype=release -Db_ndebug=true
#     --prefix=/usr（meson 顶层守卫会拦 b_ndebug 缺失的 release）
#   - 发布检查：二进制 strings 不得含 dev hook 符号
#   - 工具链下载到 ~/.cache/harbor-tools（sha256 校验，见下方常量）：
#       linuxdeploy        continuous build（GitHub 唯一发布渠道）
#       linuxdeploy-plugin-gtk.sh  git 仓库无 release，钉 commit raw
#       appimagetool       continuous build
#   - linuxdeploy + gtk 插件打包；gtk 插件钉在
#     7a3fbc31a9e5075073ff8790f26effbac5f84453（HEAD 即支持 GTK4）。
#   - 插件对 sysroot 的 schemasdir 用绝对路径拷贝（AppDir/home/x/...），
#     打包后在本脚本里归位到 usr/share/glib-2.0/schemas 并改写 hook。
#   - sysroot 的 gdk-pixbuf 无 svg loader，补进系统 librsvg 的
#     libpixbufloader-svg.so 并重建 loaders.cache（harbor 图标是 SVG）。
#
# 用法: scripts/package-appimage.sh [版本号（默认 meson 项目版本）]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${HARBOR_SYSROOT:-$HOME/.local/harbor-sysroot}"
TOOLS="${HARBOR_TOOLS:-$HOME/.cache/harbor-tools}"
VER="${1:-$(sed -n "s/^project('harbor', 'c',//;s/.*version: '\([^']*\)'.*/\1/p" \
            "$ROOT/meson.build" | head -1)}"
VER="${VER:-0.1.0}"
WORK="$ROOT/.pkg-appimage"
BLD="$WORK/build"
APPDIR="$WORK/AppDir"
OUT="$WORK/out"
SYSLIB=/usr/lib/x86_64-linux-gnu
SYSLIBDIR="$SYSROOT/usr/lib/x86_64-linux-gnu"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA="df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5"
GTKPLUGIN_URL="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/7a3fbc31a9e5075073ff8790f26effbac5f84453/linuxdeploy-plugin-gtk.sh"
GTKPLUGIN_SHA="b0f4cbc684a0103a9651f0955b635eaea0096b3a66c0f5a2c2aa337960375171"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA="e8c1169c4af66c3184d7ee110713d4be04befe671fa358e2c1591ee840553b03"

FORBIDDEN=(
    HARBOR_DEV_ACTIONS HARBOR_DEV_ACCEPT_HOSTKEY
    HARBOR_DEV_CANCEL_PROMPT HARBOR_RELAY_DROP
    HARBOR_SERVER_FRAME_DUMP on_dev_ sync-auth
)

fetch_tool() {  # name url sha
    local name="$1" url="$2" sha="$3" dst="$TOOLS/$1"
    mkdir -p "$TOOLS"
    if [ -f "$dst" ] && [ "$(sha256sum "$dst" | cut -d' ' -f1)" = "$sha" ]; then
        echo "    cached $name"
    else
        echo "    fetch $name"
        curl -sfL --retry 3 -o "$dst" "$url"
        [ "$(sha256sum "$dst" | cut -d' ' -f1)" = "$sha" ] || {
            echo "sha256 mismatch for $name" >&2; exit 1; }
    fi
    chmod +x "$dst"
}

echo "==> harbor $VER AppImage 打包（fresh release builddir: $BLD）"
rm -rf "$WORK"; mkdir -p "$APPDIR" "$OUT" "$TOOLS/bin"

echo "==> 下载/校验工具（$TOOLS）"
fetch_tool linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA"
fetch_tool linuxdeploy-plugin-gtk.sh "$GTKPLUGIN_URL" "$GTKPLUGIN_SHA"
fetch_tool appimagetool-x86_64.AppImage "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA"
ln -sf "$TOOLS/linuxdeploy-plugin-gtk.sh" "$TOOLS/bin/linuxdeploy-plugin-gtk"
ln -sf "$TOOLS/appimagetool-x86_64.AppImage" "$TOOLS/bin/appimagetool"
# gtk 插件在 ubuntu 上强要 dpkg-architecture；本机没有，给个回答
# DEB_HOST_MULTIARCH 的 shim（只用于 get_triplet_path）。
cat > "$TOOLS/bin/dpkg-architecture" <<'EOF'
#!/bin/sh
[ "$1" = "-qDEB_HOST_MULTIARCH" ] && echo x86_64-linux-gnu || echo x86_64-linux-gnu
EOF
chmod +x "$TOOLS/bin/dpkg-architecture"

# shellcheck disable=SC1091
source "$ROOT/scripts/env.sh"

LDFLAGS="${LDFLAGS:-} -Wl,--strip-all" \
    meson setup "$BLD" "$ROOT" \
        -Dbuildtype=release -Db_ndebug=true --prefix=/usr \
        -Dserver=true
ninja -C "$BLD"
DESTDIR="$APPDIR" ninja -C "$BLD" install

# server 不进客户端 AppImage
rm -f "$APPDIR/usr/bin/harbor-server"
rm -rf "$APPDIR/usr/share/doc/harbor-server"

echo "==> release check"
fail=0
for pat in "${FORBIDDEN[@]}"; do
    if grep -qa "$pat" "$APPDIR/usr/bin/harbor"; then
        echo "    !! harbor contains dev hook '$pat'" >&2
        fail=1
    fi
done
[ "$fail" = 0 ] || { echo "release check failed" >&2; exit 1; }
echo "    clean"

echo "==> linuxdeploy + gtk 插件"
export APPIMAGE_EXTRACT_AND_RUN=1   # 本机无 fuse3，AppImage 工具自解运行
export ARCH=x86_64
export DEPLOY_GTK_VERSION=4
export LD_GTK_LIBRARY_PATH="$SYSLIBDIR"
export LINUXDEPLOY="$TOOLS/linuxdeploy-x86_64.AppImage"
export PATH="$TOOLS/bin:$PATH"
export NO_APPSTREAM=1               # appstreamcli 校验已单独做过

"$TOOLS/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/harbor" \
    --desktop-file "$APPDIR/usr/share/applications/dev.harbor.Harbor.desktop" \
    --icon-file "$ROOT/linux/data/icons/hicolor/scalable/apps/dev.harbor.Harbor.svg" \
    --library "$SYSLIB/librsvg-2.so.2" \
    --plugin gtk

# --- sysroot/本机差异的收尾修正 ------------------------------------------
echo "==> AppDir 收尾"
# 1) glib schemas：插件按 sysroot 绝对路径拷进了 AppDir/home/...，
#    归位到 usr/share/glib-2.0/schemas 并改写 hook 里的引用。
hook="$APPDIR/apprun-hooks/linuxdeploy-plugin-gtk.sh"
if [ -d "$APPDIR/home" ]; then
    sys_share="$APPDIR$SYSROOT/usr/share"
    mkdir -p "$APPDIR/usr/share"
    cp -a "$sys_share/." "$APPDIR/usr/share/" 2>/dev/null || true
    rm -rf "$APPDIR/home"
fi
# hook 行整行重写，避免依赖插件生成路径的具体形态
if [ -f "$hook" ]; then
    sed -i \
        -e 's|^export GSETTINGS_SCHEMA_DIR=.*|export GSETTINGS_SCHEMA_DIR="$APPDIR/usr/share/glib-2.0/schemas"|' \
        -e 's|^export GI_TYPELIB_PATH=.*|export GI_TYPELIB_PATH="$APPDIR/usr/lib/girepository-1.0"|' \
        -e 's|^export GTK_EXE_PREFIX=.*|export GTK_EXE_PREFIX="$APPDIR/usr"|' \
        -e 's|^export GTK_PATH=.*|export GTK_PATH="$APPDIR/usr/lib/gtk-4.0"|' \
        -e 's|^export GDK_PIXBUF_MODULE_FILE=.*|export GDK_PIXBUF_MODULE_FILE="$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"|' \
        "$hook"
fi

# 2) gdk-pixbuf svg loader + 重建 loaders.cache（含 svg）
pb_dir="$(dirname "$(find "$APPDIR/usr/lib" -name 'libpixbufloader-png.so' -o -name 'libpixbufloader-ani.so' | head -1)")"
if [ -n "$pb_dir" ]; then
    cp "$SYSLIB/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so" "$pb_dir/" 2>/dev/null || true
    cache="$(dirname "$pb_dir")/loaders.cache"
    GDK_PIXBUF_MODULEDIR="$pb_dir" \
        "$SYSLIB/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders" > "$cache"
    sed -i 's|"[^"]*/loaders/|"|g' "$cache"
fi

# 3) 插件 hook 里强制 GDK_BACKEND=x11 我们保留（与本仓库 headless 测试一致）。

echo "==> appimagetool"
( cd "$OUT" && "$TOOLS/bin/appimagetool" "$APPDIR" "Harbor-$VER-x86_64.AppImage" )

ls -la "$OUT"
