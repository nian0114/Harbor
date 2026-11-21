server_bin = '-DSERVER_BIN="' + harbor_server.full_path() + '"'

test_server = executable('test_server', 'test_server.c',
    c_args: server_bin,
    dependencies: [soup_dep, jsonglib_dep, gio_dep, glib_dep,
                   srv_sqlite_dep, srv_sodium_dep],
)
test('server', test_server, timeout: 300)

test_server_e2e = executable('test_server_e2e', 'test_server_e2e.c',
    c_args: server_bin,
    dependencies: [harbor_core_dep, soup_dep, jsonglib_dep, gio_dep,
                   glib_dep],
)
test('server-e2e', test_server_e2e, timeout: 120)
