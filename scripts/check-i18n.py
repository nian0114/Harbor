#!/usr/bin/env python3
"""i18n coverage check: every C string literal containing CJK characters
in linux/src and core/src must be wrapped in _() / N_() / HB_TR() /
HB_N_() / ngettext(), or be explicitly allowlisted below.

Allowlist entries are (file, substring) pairs for *data* strings —
compared, stored, or protocol text — each with a reason. UI text has no
business here: wrap it instead. Run: scripts/check-i18n.py [srcdir]."""
import re, sys, pathlib

CJK = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff01-\uff5e]')

# (file-glob-free exact basename, literal substring, reason)
ALLOWLIST = [
    ('core/src/session.c', '"密码"',
     'secret-prompt detection keyword, matched against terminal text'),
    ('core/src/session.c', '"口令"',
     'secret-prompt detection keyword, matched against terminal text'),
    ('linux/src/hb-settings-view.c', '"中文"',
     'language autonym, shown untranslated like "English"'),
    ('core/src/ssh.c', '"密码"',
     'kbi password-prompt detection keyword, matched against server text'),
    ('macos/Sources/HarborKit/PaletteRank.swift', '"全局"',
     'snippet scope value written by the vault, matched as data not UI'),
    ('macos/Tests/HarborTests/PaletteRankTests.swift', '"全局"',
     'test data mirroring the vault snippet scope value'),
]

def scan(src):
    """Yield (start, end) spans of string literals outside comments."""
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i+1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '/' and i + 1 < n and src[i+1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            s = i
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == '\\' else 1
            yield (s, i + 1)
            i += 1
        elif c == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == '\\' else 1
            i += 1
        else:
            i += 1

WRAP_MACROS = ('_', 'N_', 'HB_TR', 'HB_N_', 'ngettext',
               'Q_', 'C_', 'g_dgettext', 'dgettext', 'gettext',
               'L',)  # L() = macOS Swift NSLocalizedString 封装

def wrapped(src, s):
    """True if chain start s sits inside a wrap-macro call: walk back to
    the '(' that opens the enclosing call at paren depth 0 and check the
    identifier before it."""
    j, depth = s - 1, 0
    while j >= 0:
        c = src[j]
        if c in ')]':
            depth += 1
        elif c in '([':
            if depth == 0:
                k = j - 1
                while k >= 0 and src[k] in ' \t\n':
                    k -= 1
                e = k + 1
                while k >= 0 and (src[k].isalnum() or src[k] == '_'):
                    k -= 1
                return src[k+1:e] in WRAP_MACROS
            depth -= 1
        j -= 1
    return False

def chains(src, spans):
    out, cur = [], []
    for s, e in spans:
        if cur and src[cur[-1][1]:s].strip() == '':
            cur.append((s, e))
        else:
            if cur:
                out.append(cur)
            cur = [(s, e)]
    if cur:
        out.append(cur)
    return out

def lineno(src, pos):
    return src.count('\n', 0, pos) + 1

def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')
    bad = []
    for sub in ('linux/src', 'core/src'):
        for p in sorted((root / sub).glob('*.c')):
            rel = str(p.relative_to(root))
            src = p.read_text()
            for ch in chains(src, list(scan(src))):
                s0, e0 = ch[0][0], ch[-1][1]
                text = src[s0:e0]
                if not CJK.search(text):
                    continue
                if wrapped(src, s0):
                    continue
                if any(rel == f and sub_ in text for f, sub_, _ in ALLOWLIST):
                    continue
                bad.append((rel, lineno(src, s0), text))
    for p in sorted((root / 'macos').rglob('*.swift')):
        rel = str(p.relative_to(root))
        src = p.read_text()
        for ch in chains(src, list(scan(src))):
            s0, e0 = ch[0][0], ch[-1][1]
            text = src[s0:e0]
            if not CJK.search(text):
                continue
            if wrapped(src, s0):
                continue
            if any(rel == f and sub_ in text for f, sub_, _ in ALLOWLIST):
                continue
            bad.append((rel, lineno(src, s0), text))
    for rel, ln, text in bad:
        print(f'{rel}:{ln}: unwrapped CJK literal {text[:80]}')
    if bad:
        print(f'{len(bad)} unwrapped CJK literal(s) — wrap with _()/N_()/'
              'HB_TR()/ngettext() or add to ALLOWLIST with a reason',
              file=sys.stderr)
        return 1
    print('i18n coverage: all CJK literals wrapped or allowlisted')
    return 0

if __name__ == '__main__':
    sys.exit(main())
