i18n = import('i18n')

# msgid 即中文原文；HB_TR/HB_N_ 是 core 侧的包装宏。
i18n.gettext('harbor',
    preset: 'glib',
    args: [
        '--from-code=UTF-8',
        '--keyword=HB_TR',
        '--keyword=HB_N_',
    ],
    install: true,
    install_dir: get_option('localedir'),
)
