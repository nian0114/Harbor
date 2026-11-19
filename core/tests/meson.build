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
