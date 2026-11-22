# Harbor — agent notes

- Spec: `docs/PRD.md`; UI reference: `prototype/` (open `prototype/index.html`). Architecture: `docs/dev/architecture.md`; object fields: `docs/dev/data-model.md`.
- Build env (no root on dev box): `source scripts/env.sh` first (sysroot from `scripts/bootstrap-sysroot.sh`, see `docs/dev/build-env.md`).
- Build: `meson setup build && ninja -C build`; tests: `meson test -C build --print-errorlogs`.
- Headless GUI run: `Xvfb :99 -screen 0 1400x900x24 &` then `DISPLAY=:99 GDK_BACKEND=x11 ./build/linux/harbor`.
- Commits: `<type> <scope>: <subject>` (type ∈ chore|docs|feat|fix|refactor|style|test), one logical change per commit, body explains why; bug fixes in their own commit.
- Dev servers (rootless): `scripts/dev-sshd.sh` (SSH 127.0.0.1:2222), `scripts/dev-desktop.sh` (VNC :5901 / RDP :13389, pw `pw123`), `build/server/harbor-server --listen 127.0.0.1:8787 --data DIR` (sync/relay). Two app instances: set distinct `HARBOR_DATA_DIR` (runs non-unique). Debug-only D-Bus dev actions need `HARBOR_DEV_ACTIONS=1`.
- Protocols are specified in `docs/api/` (sync, share, backup); implementations must follow them byte-for-byte.
- Release: `-Dbuildtype=release` requires `-Db_ndebug=true` (meson errors otherwise); packages via `scripts/package-deb.sh`, `scripts/package-appimage.sh`.
- i18n: Chinese source strings wrapped in `_()`/`N_()` (linux) or `HB_TR()` (core); English in `po/en.po`; `meson test` runs `check-i18n` which fails on unwrapped CJK literals.
