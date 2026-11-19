cc = meson.get_compiler('c')

core_inc = include_directories('include')
cjson_inc = include_directories('third_party/cjson')
core_priv_inc = include_directories('src')

sqlite3_dep = dependency('sqlite3')
sodium_dep = dependency('libsodium')
vterm_dep = dependency('vterm')
ssh2_dep = dependency('libssh2')
crypto_dep = dependency('libcrypto')
thread_dep = dependency('threads')
util_dep = cc.find_library('util', required: false)

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
    include_directories: [core_inc, cjson_inc, core_priv_inc],
    link_whole: cjson_lib,
    dependencies: [sqlite3_dep, sodium_dep, vterm_dep, ssh2_dep, crypto_dep,
                   thread_dep, util_dep],
)

harbor_core_dep = declare_dependency(
    link_with: harbor_core_lib,
    include_directories: core_inc,
    dependencies: [sqlite3_dep, sodium_dep, vterm_dep, ssh2_dep, crypto_dep,
                   thread_dep, util_dep],
)

if get_option('tests')
    subdir('tests')
endif
