# Harbor — agent notes

- Spec: `docs/PRD.md`; UI reference: `prototype/` (open `prototype/index.html`). Architecture: `docs/dev/architecture.md`; object fields: `docs/dev/data-model.md`.
- Build env (no root on dev box): `source scripts/env.sh` first (sysroot from `scripts/bootstrap-sysroot.sh`, see `docs/dev/build-env.md`).
- Build: `meson setup build && ninja -C build`; tests: `meson test -C build --print-errorlogs`.
- Headless GUI run: `Xvfb :99 -screen 0 1400x900x24 &` then `DISPLAY=:99 GDK_BACKEND=x11 ./build/linux/harbor`.
- Commits: `<type> <scope>: <subject>` (type ∈ chore|docs|feat|fix|refactor|style|test), one logical change per commit, body explains why; bug fixes in their own commit.
