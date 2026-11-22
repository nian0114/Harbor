project('harbor', 'c',
    version: '0.1.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
        'werror=false',
    ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

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
endif

if get_option('server')
    subdir('server')
endif
