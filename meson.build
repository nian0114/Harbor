project('harbor', 'c',
    version: '0.1.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
        'werror=false',
    ],
)

if host_machine.system() != 'darwin'
    add_project_arguments('-D_GNU_SOURCE', language: 'c')
endif

# Release builds must compile the dev hooks out: warn loudly if a
# release/plain buildtype still has asserts on (NDEBUG off means the
# HARBOR_DEV_ACTIONS / frame-dump / relay-drop hooks ship).
if get_option('buildtype') in ['release', 'plain'] and \
   get_option('b_ndebug') != 'true'
    error('release builds must set -Db_ndebug=true ' +
          '(dev hooks are only compiled out under NDEBUG)')
endif
if get_option('b_sanitize') != 'none' and get_option('buildtype') == 'release'
    warning('release build with a sanitizer enabled')
endif


subdir('core')

if get_option('linux')
    subdir('linux')
    subdir('po')
endif

# FR-15 i18n coverage gate: any CJK string literal in core/src or
# linux/src that is not wrapped for gettext fails this test.
python3 = find_program('python3', required: false)
if python3.found()
    test('i18n-coverage', python3,
         args: [files('scripts/check-i18n.py'), meson.project_source_root()],
         suite: 'lint')
endif

if get_option('server')
    subdir('server')
endif
