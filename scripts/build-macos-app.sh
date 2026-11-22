#!/bin/bash
# Harbor macOS app 构建（在 Mac 上运行）：
#   scripts/build-macos-app.sh            # debug，native arch
#   scripts/build-macos-app.sh --release  # release，universal（arm64+x86_64）
# 产物：~/harbor-build/Harbor.app（ad-hoc 签名）
set -euo pipefail

SRC="${SRC_DIR:-$HOME/harbor}"
BUILD="${BUILD_DIR:-$HOME/harbor-build}"
SYSROOT="${HARBOR_MACOS_SYSROOT:-$HOME/.local/harbor-macos}"
PKG="$SRC/macos"
APP="$BUILD/Harbor.app"

source "$SRC/scripts/env-macos.sh"

# 1. universal core 保持最新
bash "$SRC/scripts/build-macos-core.sh" "$SRC" "$BUILD" >/dev/null

LINKTXT="$BUILD/link-line.txt"
LINK=$(tr '\n' ' ' < "$LINKTXT" | tr -d '\\' | tr ' ' '\n' | grep -v '^-pthread$' | tr '\n' ' ')

# 2. swift build
cd "$PKG"
if [ "${1:-}" = "--test" ]; then
    # swift test 也要链接 core：复用同一 link line。
    LINKF=$(for t in $LINK; do printf -- "-Xlinker %s " "$t"; done)
    swift test $LINKF
    exit $?
fi
if [ "${1:-}" = "--release" ]; then
    swift build -c release --arch x86_64 --arch arm64 \
        $(for t in $LINK; do printf -- "-Xlinker %s " "$t"; done)
    BIN="$(swift build -c release --arch x86_64 --arch arm64 \
        --show-bin-path)/Harbor"
else
    swift build \
        $(for t in $LINK; do printf -- "-Xlinker %s " "$t"; done)
    BIN="$(swift build --show-bin-path)/Harbor"
fi

# 3. 组装 .app
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Harbor"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>dev.harbor.Harbor</string>
    <key>CFBundleName</key><string>Harbor</string>
    <key>CFBundleExecutable</key><string>Harbor</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleVersion</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleDevelopmentRegion</key><string>zh_CN</string>
</dict>
</plist>
PLIST

# 4. ad-hoc 签名
codesign --force --sign - "$APP" >/dev/null 2>&1 || \
    codesign --force --sign - "$APP"
echo "built: $BIN"
echo "app:   $APP"
