cc = meson.get_compiler('c')

core_inc = include_directories('include')
cjson_inc = include_directories('third_party/cjson')
core_priv_inc = include_directories('src')

# macOS 上所有依赖都是 bootstrap 出的静态 .a —— static:true 让
# pkgconf --static 带上 Requires.private/Libs.private（链接闭包）。
dep_static = host_machine.system() == 'darwin'

# sqlite3/zlib 在 macOS 上是 SDK .tbd（动态），不走 static。
sqlite3_dep = dependency('sqlite3')
sodium_dep = dependency('libsodium', static: dep_static)
vterm_dep = dependency('vterm', static: dep_static)
ssh2_dep = dependency('libssh2', static: dep_static)
crypto_dep = dependency('libcrypto', static: dep_static)
thread_dep = dependency('threads')
util_dep = cc.find_library('util', required: false)

# NLS：dgettext 在 glibc/libintl 可用时启用 HB_TR() 翻译（msgid 是中文）。
core_c_args = []
if cc.has_function('dgettext', prefix: '#include <libintl.h>')
    core_c_args += '-DENABLE_NLS'
endif

# FR-07 远程桌面后端（第三方头按 system include，头内 deprecated 成员不算我们告警）
vncclient_dep = dependency('libvncclient', include_type: 'system',
                           static: dep_static)
freerdp_deps = [dependency('freerdp-client3', include_type: 'system',
                           static: dep_static),
                dependency('winpr3', include_type: 'system',
                           static: dep_static)]

# vendored cJSON：第三方代码，单独 target 关闭警告
cjson_lib = static_library('harbor-cjson',
    'third_party/cjson/cJSON.c',
    c_args: ['-w'],
    include_directories: cjson_inc,
)

harbor_core_lib = static_library('harbor-core',
    'src/common.c',
    'src/obj.c',
    'src/vault.c',
    'src/vt.c',
    'src/pty.c',
    'src/session.c',
    'src/ssh.c',
    'src/sshcfg.c',
    'src/hostio.c',
    'src/keys.c',
    'src/watch.c',
    'src/knownhosts.c',
    'src/snippet.c',
    'src/sftp.c',
    'src/pf.c',
    'src/rd.c',
    'src/rd_vnc.c',
    'src/rd_rdp.c',
    'src/sync.c',
    'src/backup.c',
    'src/share.c',
    'src/bip39_words.c',
    include_directories: [core_inc, cjson_inc, core_priv_inc],
    c_args: core_c_args,
    link_whole: cjson_lib,
    dependencies: [sqlite3_dep, sodium_dep, vterm_dep, ssh2_dep, crypto_dep,
                   thread_dep, util_dep, vncclient_dep] + freerdp_deps,
)

harbor_core_dep = declare_dependency(
    link_with: harbor_core_lib,
    include_directories: core_inc,
    dependencies: [sqlite3_dep, sodium_dep, vterm_dep, ssh2_dep, crypto_dep,
                   thread_dep, util_dep, vncclient_dep] + freerdp_deps,
)

if get_option('tests')
    subdir('tests')
endif
