gtk4_dep = dependency('gtk4', version: '>=4.10')
adw_dep = dependency('libadwaita-1')
secret_dep = dependency('libsecret-1')
m_dep = cc.find_library('m', required: true)

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
    'src/hb-keychain-view.c',
    'src/hb-notify.c',
    'src/hb-watch-ui.c',
    'src/hb-term-theme.c',
    'src/hb-terminal.c',
    'src/hb-term-page.c',
    'src/hb-ssh-page.c',
    'src/hb-conn.c',
    'src/hb-find-bar.c',
    'src/hb-shortcuts.c',
    'src/hb-palette.c',
    'src/hb-session-page.c',
    'src/hb-workspace.c',
    'src/hb-snippet-send.c',
    'src/hb-snippets-view.c',
    'src/hb-known-view.c',
    'src/hb-logs-view.c',
    'src/hb-settings-view.c',
    'src/hb-dialogs.c',
    'src/hb-host-form.c',
    'src/hb-host-drawer.c',
    'src/hb-host-actions.c',
    hb_res,
    include_directories: 'src',
    dependencies: [gtk4_dep, adw_dep, secret_dep, m_dep, harbor_core_dep],
    install: true,
)

install_data('data/dev.harbor.Harbor.desktop',
    install_dir: get_option('datadir') / 'applications')
