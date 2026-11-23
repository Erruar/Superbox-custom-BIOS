import sys
from pathlib import Path

img = Path(sys.argv[1]).read_bytes()


def hx(o, n=128):
    b = img[o:o + n]
    print(f'@{o:#010x}')
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)
        print(f'{o + i:#010x}  ' + ' '.join(f'{c:02x}' for c in row) + '  |' + asc + '|')


for spec in sys.argv[2:]:
    if not spec.lower().startswith(('0x',)):
        spec = '0x' + spec.split(':', 1)[0] + (':' + spec.split(':', 1)[1] if ':' in spec else '')
    if ':' in spec:
        o, n = spec.split(':')
        hx(int(o, 0), int(n))
    else:
        hx(int(spec, 0))
