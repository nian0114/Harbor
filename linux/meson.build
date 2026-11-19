gtk4_dep = dependency('gtk4', version: '>=4.10')
adw_dep = dependency('libadwaita-1')
secret_dep = dependency('libsecret-1')

gnome = import('gnome')
hb_res = gnome.compile_resources('hb-resources',
    'data/harbor.gresource.xml',
    source_dir: 'data',
    c_name: 'hb_resources')

harbor_exe = executable('harbor',
    'src/main.c',
    'src/hb-application.c',
    'src/hb-window.c',
    'src/hb-icons.c',
    'src/hb-store.c',
    'src/hb-vault-page.c',
    'src/hb-hosts-view.c',
    'src/hb-notify.c',
    hb_res,
    include_directories: 'src',
    dependencies: [gtk4_dep, adw_dep, secret_dep, harbor_core_dep],
    install: true,
)

install_data('data/dev.harbor.Harbor.desktop',
    install_dir: get_option('datadir') / 'applications')
