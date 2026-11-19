# Harbor 构建环境（无 root 用户态 sysroot）。
# 用法:  source scripts/env.sh
# sysroot 由 scripts/bootstrap-sysroot.sh 生成：apt-get download + dpkg -x。
HARBOR_SYSROOT="${HARBOR_SYSROOT:-$HOME/.local/harbor-sysroot}"
export HARBOR_SYSROOT

# tool-bin：注入 --sysroot 的 cc/gcc/g++ 包装器（编译/链接都走 sysroot）
# usr/bin ：pkg-config、meson、ninja、make、binutils、glib-compile-resources 等
case ":$PATH:" in
    *":$HARBOR_SYSROOT/tool-bin:"*) ;;
    *) PATH="$HARBOR_SYSROOT/tool-bin:$HARBOR_SYSROOT/usr/bin:$PATH" ;;
esac
export PATH

# runtime-libs：只含"系统缺失"的库符号链接（libssh2、libvterm、libvncclient、
# libwinpr-tools3、libbfd 等几十个叶子库）。刻意不包含 sysroot 的
# libc.so.6 / ld-linux —— 避免遮蔽系统 C 库。
case ":${LD_LIBRARY_PATH:-}:" in
    *":$HARBOR_SYSROOT/runtime-libs:"*) ;;
    *) LD_LIBRARY_PATH="$HARBOR_SYSROOT/runtime-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
export LD_LIBRARY_PATH

# pkg-config：让 .pc 文件中的 /usr 前缀映射到 sysroot 内
export PKG_CONFIG_SYSROOT_DIR="$HARBOR_SYSROOT"
export PKG_CONFIG_LIBDIR="$HARBOR_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$HARBOR_SYSROOT/usr/lib/pkgconfig:$HARBOR_SYSROOT/usr/share/pkgconfig"

# meson 的 python 模块在 sysroot 的 dist-packages 里（shebang 用系统 python3 即可）
case ":${PYTHONPATH:-}:" in
    *":$HARBOR_SYSROOT/usr/lib/python3/dist-packages:"*) ;;
    *) PYTHONPATH="$HARBOR_SYSROOT/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}" ;;
esac
export PYTHONPATH
