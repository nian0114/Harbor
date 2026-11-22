soup_dep = dependency('libsoup-3.0')
jsonglib_dep = dependency('json-glib-1.0')
gio_dep = dependency('gio-2.0')
glib_dep = dependency('glib-2.0')
srv_sqlite_dep = dependency('sqlite3')
srv_sodium_dep = dependency('libsodium')

harbor_server = executable('harbor-server',
    'src/main.c',
    'src/db.c',
    'src/http.c',
    'src/relay.c',
    'src/util.c',
    dependencies: [soup_dep, jsonglib_dep, gio_dep, glib_dep,
                   srv_sqlite_dep, srv_sodium_dep],
)

if get_option('tests')
    subdir('tests')
endif
