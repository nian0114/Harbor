#!/bin/bash
# Harbor macOS 打包（在 Mac 上运行）：
#   scripts/package-macos.sh
# 产物：$BUILD/Harbor-<ver>.dmg（release universal + ad-hoc 签名 +
# /Applications 链接）。不做 hardened runtime / 公证（个人分发）。
set -euo pipefail

SRC="${SRC_DIR:-$HOME/harbor}"
BUILD="${BUILD_DIR:-$HOME/harbor-build}"
APP="$BUILD/Harbor.app"
BIN="$APP/Contents/MacOS/Harbor"

# 1. release universal 构建（含 .app 组装 + ad-hoc 签名 + 图标）
bash "$SRC/scripts/build-macos-app.sh" --release

# 2. release 门禁（对齐 linux package-*.sh）：dev 钩子不得出现在包里；
#    NDEBUG 由 dev-hook 字符串是否编译消失间接证明 + assert 文本检查。
DEVSTRINGS="dev.sock HARBOR_DEV_ACTIONS DevServer HB_SNDBUF \
HB_SFTP_DBG sftpdbg HARBOR_RELAY_DROP_DELTA_PCT \
HARBOR_SERVER_FRAME_DUMP HARBOR_PFDBG \
pal-open pal-type pal-items ws-state ws-focus-pane \
sysnotifs notif-jump watch-test share-start share-stop \
share-input share-regen rd-type rd-click rd-fps sleep-sim \
assert-state tray-menu tray-count login-status devRuns"
fail=0
for s in $DEVSTRINGS; do
    if strings -a "$BIN" | grep -qF "$s"; then
        echo "GUARD FAIL: dev string in binary: $s"
        fail=1
    fi
done
# core 必须 NDEBUG：release 下 assert() 编译消失，无 "Assertion failed"
if strings -a "$BIN" | grep -q "Assertion failed"; then
    echo "GUARD FAIL: assert strings present (NDEBUG missing?)"
    fail=1
fi
[ "$fail" = 0 ] || { echo "release guard failed"; exit 1; }
echo "guard: no dev strings, NDEBUG ok"

# 3. universal 架构 + Info.plist 完整性
ARCHS=$(lipo -archs "$BIN" 2>/dev/null || file "$BIN")
echo "archs: $ARCHS"
for k in CFBundleShortVersionString CFBundleVersion \
         NSHumanReadableCopyright LSApplicationCategoryType \
         LSMinimumSystemVersion CFBundleIconFile; do
    /usr/libexec/PlistBuddy -c "Print :$k" "$APP/Contents/Info.plist" \
        >/dev/null 2>&1 || { echo "GUARD FAIL: plist missing $k"; exit 1; }
done
echo "guard: plist complete"

# 4. ad-hoc 重签（deep，无 hardened runtime）+ 严格验证
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict "$APP"
echo "guard: codesign verify ok"

# 5. DMG（UDZO，/Applications 链接）
VER=$(/usr/libexec/PlistBuddy -c \
    "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")
DMG="$BUILD/Harbor-$VER.dmg"
STAGE=$(mktemp -d "$BUILD/dmg-stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname Harbor -srcfolder "$STAGE" -ov \
    -format UDZO "$DMG" >/dev/null
SZ=$(du -k "$DMG" | cut -f1)
echo "dmg:  $DMG"
echo "size: $((SZ/1024)) MB ($SZ KB) —— 目标 ≤40 MB"
if [ "$SZ" -gt 40960 ]; then
    echo "over target; largest contents:"
    du -sk "$APP"/Contents/MacOS/* "$APP"/Contents/Resources/* \
        2>/dev/null | sort -rn | head -8
fi
