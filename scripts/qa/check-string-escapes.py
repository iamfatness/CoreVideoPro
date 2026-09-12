"""Find invalid C++ escape sequences in string literals.

MSVC accepts (with a warning) escapes that GCC and Clang reject outright, so a
Windows-only build cannot catch them. That is how "C:\\media\\clip.mp4" written
with single backslashes reached main and broke the Linux and macOS CI jobs.
"""
import re
import sys
import pathlib

VALID = set("'\"?\\abfnrtv01234567xuUeo\n")
roots = [pathlib.Path('native/tests'), pathlib.Path('native/src'), pathlib.Path('native/zoom-engine')]
bad = []

string_re = re.compile(r'"(?:[^"\\\n]|\\.)*"')

for root in roots:
    for path in root.rglob('*'):
        if path.suffix not in ('.cpp', '.h', '.mm'):
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith('//') or stripped.startswith('*'):
                continue
            if 'R"' in line:      # raw string literals: backslashes are literal
                continue
            for literal in string_re.findall(line):
                i = 0
                while i < len(literal) - 1:
                    if literal[i] == '\\':
                        nxt = literal[i + 1]
                        if nxt not in VALID:
                            bad.append(f'{path}:{lineno}: invalid escape \\{nxt} in {literal[:70]}')
                        i += 2
                        continue
                    i += 1

if bad:
    print(f'{len(bad)} invalid escape sequence(s):')
    for b in bad:
        print('  ' + b)
    sys.exit(1)
print('no invalid escape sequences found')
