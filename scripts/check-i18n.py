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
    ('macos/Sources/Harbor/AppearancePanel.swift', '"简体中文"',
     'language autonym, shown untranslated like "English"'),
    ('core/src/ssh.c', '"密码"',
     'kbi password-prompt detection keyword, matched against server text'),
    ('macos/Sources/HarborKit/PaletteRank.swift', '"全局"',
     'snippet scope value written by the vault, matched as data not UI'),
    ('macos/Tests/HarborTests/PaletteRankTests.swift', '"全局"',
     'test data mirroring the vault snippet scope value'),
    ('macos/Tests/HarborTests/M2bVaultTests.swift', '"管理员"',
     'CSV test data verifying Chinese UTF-8 round-trips through core'),
    ('macos/Tests/HarborTests/FwdKitTests.swift', '服务器 0.0.0.0:40001',
     'expected localized output of FwdKit.bindText (remote)'),
    ('macos/Tests/HarborTests/FwdKitTests.swift', '2 连接',
     'expected localized output of FwdKit.trafficText'),
    ('macos/Tests/HarborTests/SyncKitTests.swift', '"ＡＢＣ"',
     'fullwidth input data for the NFKC normalisation test'),
    ('macos/Tests/HarborTests/LangTests.swift', '"主机"',
     'msgid passthrough assertion compares output to the raw msgid'),
    ('macos/Tests/HarborTests/LangTests.swift', '"中文"',
     'msgid passthrough assertion compares output to the raw msgid'),
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
    return check_macos_catalog(root)

# --- macOS catalog gate -------------------------------------------------
# L() is the macOS translation entry point: its argument MUST be a plain
# string literal (no \(...) interpolation — format through String(format:)
# instead) and every msgid must have a non-empty msgstr in po/en.po, the
# single catalog consumed by both the Linux .mo build and the macOS
# en.strings generation in build-macos-app.sh.

L_CALL = re.compile(r'\bL\(')
L_LIT  = re.compile(r'\bL\(\s*"((?:[^"\\]|\\.)*)"\s*\)')

def enpo_msgstrs(path):
    """Parse po/en.po → {msgid: msgstr} honouring continuation lines."""
    out = {}
    cur_key = cur_val = None
    field = None
    def unq(t):
        return re.sub(r'\\(.)', lambda m: {'n':'\n','t':'\t','"':'"','\\':'\\'}.get(m.group(1), m.group(1)), t[1:-1])
    for line in path.read_text().splitlines():
        if line.startswith('msgid_plural'):
            field = 'plural'
        elif line.startswith('msgid '):
            if cur_key is not None:
                out[cur_key] = cur_val
            cur_key, cur_val = unq(line[6:].strip()), None
            field = 'id'
        elif line.startswith('msgstr['):
            if cur_val is None:
                cur_val = unq(line[line.index(']') + 1:].strip())
            field = 'str'
        elif line.startswith('msgstr '):
            cur_val = unq(line[7:].strip()); field = 'str'
        elif line.startswith('"') and field == 'str':
            cur_val = (cur_val or '') + unq(line.strip())
        elif line.startswith('"') and field == 'id':
            cur_key += unq(line.strip())
        elif not line.startswith('#') and line.strip():
            field = None
    if cur_key is not None:
        out[cur_key] = cur_val
    return out

def check_macos_catalog(root):
    bad = []
    for p in sorted((root / 'macos' / 'Sources').rglob('*.swift')):
        rel = str(p.relative_to(root))
        src = p.read_text()
        for m in L_CALL.finditer(src):
            if src[max(0, m.start() - 5):m.start()] == 'func ':
                continue  # the func L( declaration itself
            line_start = src.rfind('\n', 0, m.start()) + 1
            prev_nl = src.rfind('\n', 0, max(0, line_start - 1)) + 1
            line_end = src.find('\n', m.end())
            whole = src[prev_nl:line_end if line_end > 0 else len(src)]
            rest = src[m.end():].lstrip()
            if not rest.startswith('"') and 'i18n-dynamic' not in whole:
                bad.append((rel, lineno(src, m.start()),
                            'L() argument is not a string literal'))
    msgids = set()
    for p in sorted((root / 'macos' / 'Sources').rglob('*.swift')):
        rel = str(p.relative_to(root))
        src = p.read_text()
        for m in L_LIT.finditer(src):
            lit = m.group(1)
            if '\\(' in lit:
                bad.append((rel, lineno(src, m.start()),
                            'L() argument contains \\(...) interpolation — '
                            'use String(format: L("…"), …)'))
                continue
            msgids.add((rel, lineno(src, m.start()),
                        lit.replace('\\"', '"').replace('\\\\', '\\')
                           .replace('\\n', '\n')))
    en = enpo_msgstrs(root / 'po' / 'en.po')
    seen = set()
    for rel, ln, mid in sorted(msgids):
        if mid in seen:
            continue
        seen.add(mid)
        if mid not in en or not en[mid]:
            bad.append((rel, ln,
                        f'macOS msgid untranslated in po/en.po: {mid!r}'))
    for rel, ln, text in bad:
        print(f'{rel}:{ln}: {text}')
    if bad:
        print(f'{len(bad)} macOS i18n problem(s)', file=sys.stderr)
        return 1
    print(f'macOS catalog: {len(seen)} L() msgids all translated in en.po')
    return 0

if __name__ == '__main__':
    sys.exit(main())
