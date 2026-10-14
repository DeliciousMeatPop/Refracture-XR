#!/usr/bin/env python3
# Extracts source files + cflags of Digitalis modules (android_x86_64_static variant) from Soong ninja.
import json, re, sys

MODS = set(sys.argv[1].split(','))
OUT = sys.argv[2]
NINJA = '/root/aosp/out/soong/build.sdk_phone64_x86_64_digitalis.incremental.ninja'
pre = 'out/soong/.intermediates/frameworks/libs/binary_translation/'
res = {}
modvars = {}
want_var = re.compile(r'^m\.(' + '|'.join(re.escape(m) for m in MODS) + r')_android_x86_64_static\.(\w+) = (.*)$')
with open(NINJA, errors='replace') as fh:
    lines = iter(fh)
    for line in lines:
        mv = want_var.match(line)
        if mv:
            modvars.setdefault(mv.group(1), {})[mv.group(2)] = mv.group(3).rstrip('\n')
            continue
        if line != 'build $\n':
            continue
        out = next(lines).strip()
        if not out.startswith(pre) or '/android_x86_64_static/obj/' not in out or not out.endswith('.o: $'):
            continue
        mod = out[len(pre):].split('/android_x86_64_static')[0]
        name = mod.split('/')[-1]
        if name not in MODS:
            continue
        stmt = ''
        for l in lines:
            stmt += ' ' + l.strip().rstrip('$')
            if not l.rstrip().endswith('$'):
                break
        srcs = re.findall(r'(frameworks/\S+\.(?:cc|cpp|S|c))', stmt)
        ent = {'obj': out[:-3], 'src': srcs[0] if srcs else stmt, 'vars': {}}
        for l in lines:
            if not l.startswith('    '):
                break
            k, _, v = l.strip().partition(' = ')
            ent['vars'][k] = v
        res.setdefault(name, []).append(ent)
json.dump({'objs': res, 'vars': modvars}, open(OUT, 'w'), indent=1)
for m, objs in sorted(res.items()):
    print(m, len(objs))
