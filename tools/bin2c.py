#!/usr/bin/env python3
"""bin2c.py IN OUT NAME - writes a C file holding IN's bytes as
`const unsigned char NAME[]` and `const unsigned int NAME_size`.
The build uses it to put the Game Boy Advance music player into the browser."""
import sys
src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('/* made by tools/bin2c.py from %s; do not edit */\n' % src.split('/')[-1])
    f.write('const unsigned char %s[%d] __attribute__((aligned(32))) = {\n' % (name, len(data)))
    for i in range(0, len(data), 20):
        f.write('    ' + ','.join(str(b) for b in data[i:i + 20]) + ',\n')
    f.write('};\nconst unsigned int %s_size = %d;\n' % (name, len(data)))
print('%s: %d bytes' % (dst, len(data)))
