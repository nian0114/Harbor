test_vault = executable('test_vault', 'test_vault.c',
    include_directories: [include_directories('../src'), include_directories('../third_party/cjson')],
    dependencies: harbor_core_dep,
)
test('vault', test_vault)
