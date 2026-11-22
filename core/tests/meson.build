test_vault = executable('test_vault', 'test_vault.c',
    include_directories: [include_directories('../src'), include_directories('../third_party/cjson')],
    dependencies: harbor_core_dep,
)
test('vault', test_vault)

test_vt = executable('test_vt', 'test_vt.c',
    dependencies: harbor_core_dep,
)
test('vt', test_vt)

bench_vt = executable('bench_vt', 'bench_vt.c',
    dependencies: harbor_core_dep,
)
benchmark('vt', bench_vt)

test_pty = executable('test_pty', 'test_pty.c',
    dependencies: harbor_core_dep,
)
test('pty', test_pty)

test_session = executable('test_session', 'test_session.c',
    dependencies: harbor_core_dep,
)
test('session', test_session)

test_ssh = executable('test_ssh', 'test_ssh.c',
    dependencies: harbor_core_dep,
)
test('ssh', test_ssh, timeout: 60)

test_hostio = executable('test_hostio', 'test_hostio.c',
    dependencies: harbor_core_dep,
)
test('hostio', test_hostio)

test_keys = executable('test_keys', 'test_keys.c',
    dependencies: harbor_core_dep,
)
test('keys', test_keys, timeout: 120)

test_watch = executable('test_watch', 'test_watch.c',
    c_args: '-DWATCH_SCEN_FILE="' + meson.current_source_dir() / 'data' / 'watch_scenarios.txt' + '"',
    dependencies: harbor_core_dep,
)
test('watch', test_watch, timeout: 60)

test_knownhosts = executable('test_knownhosts', 'test_knownhosts.c',
    dependencies: harbor_core_dep,
)
test('knownhosts', test_knownhosts)

test_snippet = executable('test_snippet', 'test_snippet.c',
    dependencies: harbor_core_dep,
)
test('snippet', test_snippet)

test_sftp = executable('test_sftp', 'test_sftp.c', 'sshd_fixture.c',
    include_directories: include_directories('../src'),
    dependencies: harbor_core_dep,
)
test('sftp', test_sftp, timeout: 120)

test_pf = executable('test_pf', 'test_pf.c', 'sshd_fixture.c',
    include_directories: include_directories('../src'),
    dependencies: harbor_core_dep,
)
test('pf', test_pf, timeout: 180)

test_rd = executable('test_rd', 'test_rd.c', 'sshd_fixture.c',
    include_directories: include_directories('../src'),
    dependencies: [harbor_core_dep, dependency('libvncserver')],
)
test('rd', test_rd, timeout: 240)

test_sync = executable('test_sync', 'test_sync.c',
    include_directories: [include_directories('../src'), include_directories('../third_party/cjson')],
    dependencies: harbor_core_dep,
)
test('sync', test_sync, timeout: 300)

test_share = executable('test_share', 'test_share.c',
    include_directories: [include_directories('../src'), include_directories('../third_party/cjson')],
    dependencies: harbor_core_dep,
)
test('share', test_share, timeout: 180)
