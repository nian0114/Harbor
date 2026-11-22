# Harbor macOS 构建环境（在 Mac 上 source）。
# 用法:  source scripts/env-macos.sh
# sysroot 由 scripts/bootstrap-macos.sh 生成。
HARBOR_MACOS_SYSROOT="${HARBOR_MACOS_SYSROOT:-$HOME/.local/harbor-macos}"
export HARBOR_MACOS_SYSROOT

_tools="$HARBOR_MACOS_SYSROOT/tools"
case ":$PATH:" in
    *":$_tools/venv/bin:"*) ;;
    *) PATH="$_tools/venv/bin:$_tools/CMake.app/Contents/bin:$_tools/bin:$PATH" ;;
esac
export PATH

# pkgconf 只看本架构前缀（native x86_64；交叉走 macos/meson/*.ini）
export PKG_CONFIG_LIBDIR="$HARBOR_MACOS_SYSROOT/x86_64/lib/pkgconfig"
unset PKG_CONFIG_PATH 2>/dev/null || true

export MACOSX_DEPLOYMENT_TARGET=13.0
