#!/usr/bin/env python3
"""Turns tools/jsprelude.js into source/jsprelude.c (a C string the browser runs
before each page's scripts). Run it after changing jsprelude.js."""
import os
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'jsprelude.js'), encoding='utf-8').read()
lines = ['/* jsprelude.c - made by tools/make_jsprelude.py from tools/jsprelude.js: do not edit by hand. */',
         '#include <stddef.h>', '', 'const char js_prelude[] =']
for line in src.split('\n'):
    esc = line.replace('\\', '\\\\').replace('"', '\\"')
    lines.append('    "' + esc + '\\n"')
lines[-1] += ';'
lines.append('const size_t js_prelude_len = sizeof(js_prelude) - 1;')
open(os.path.join(here, '..', 'source', 'jsprelude.c'), 'w').write('\n'.join(lines) + '\n')
print('wrote source/jsprelude.c')
