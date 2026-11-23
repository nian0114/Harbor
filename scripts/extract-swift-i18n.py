#!/usr/bin/env python3
"""Extract L("…") msgids from macos/Sources into a .pot fragment on
stdout, so they can be merged into po/harbor.pot / po/en.po by hand
(msgid "" header line is omitted — paste after xgettext output or use
as-is for diffing). L() arguments are required to be plain literals;
interpolation/non-literals are reported on stderr and exit non-zero.

  scripts/extract-swift-i18n.py [repo-root] > /tmp/swift-msgids.pot
"""
import re, sys, pathlib

L_CALL = re.compile(r'\bL\(')
L_LIT = re.compile(r'\bL\(\s*"((?:[^"\\]|\\.)*)"\s*\)')

def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')
    errs, found = [], {}  # msgid -> first location
    for p in sorted((root / 'macos' / 'Sources').rglob('*.swift')):
        rel = str(p.relative_to(root))
        src = p.read_text()
        for m in L_CALL.finditer(src):
            if not src[m.end():].lstrip().startswith('"'):
                errs.append(f'{rel}:{src.count(chr(10),0,m.start())+1}: '
                            'L() argument is not a string literal')
        for m in L_LIT.finditer(src):
            lit = m.group(1)
            ln = src.count('\n', 0, m.start()) + 1
            if '\\(' in lit:
                errs.append(f'{rel}:{ln}: L() contains interpolation')
                continue
            found.setdefault(lit, f'{rel}:{ln}')
    for e in errs:
        print(e, file=sys.stderr)
    if errs:
        return 1
    for lit, loc in sorted(found.items(), key=lambda kv: kv[1]):
        print(f'#: {loc}')
        print(f'msgid "{lit}"')
        print('msgstr ""\n')
    return 0

if __name__ == '__main__':
    sys.exit(main())
