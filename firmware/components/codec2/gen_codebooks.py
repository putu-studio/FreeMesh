# Makes codec2's generated codebook .c files from its codebook/*.txt tables (same output as its generate_codebook.c)
import re, sys, os, math
src = sys.argv[1]; out = sys.argv[2]
cm = open(os.path.join(src, 'CMakeLists.txt')).read()
sets = dict(re.findall(r'set\((CODEBOOKS\w*)\s+([^)]*)\)', cm))
jobs = re.findall(r'OUTPUT \$\{CMAKE_CURRENT_BINARY_DIR\}/(\w+)\.c\s+COMMAND generate_codebook (\w+) \$\{(\w+)\}', cm)
num = re.compile(r'[-.]?\d[\d.]*(?:[eE][-+]?\d+)?|-\.\d+(?:[eE][-+]?\d+)?')
def load(path):
    vals = [float(x) for x in num.findall(open(path).read())]
    k, m = int(vals[0]), int(vals[1])
    return k, m, vals[2:2 + k * m]
def g(f):  # C printf %g
    return '%g' % f
for fname, arr, setname in jobs:
    files = [f.replace('${D}', os.path.join(src, 'codebook')) for f in sets[setname].split()]
    o = ["/* THIS IS A GENERATED FILE. Edit generate_codebook.c and its input */\n\n/*\n * This intermediary file and the files that used to create it are under \n * The LGPL. See the file COPYING.\n */\n\n#include \"defines.h\"\n\n"]
    cbs = []
    for i, f in enumerate(files):
        k, m, cb = load(f); cbs.append((k, m))
        assert len(cb) == k * m, f
        o.append("  /* %s */\n#ifdef __EMBEDDED__\nstatic const float codes%d[] = {\n#else\nstatic float codes%d[] = {\n#endif\n" % (os.path.basename(f), i, i))
        for j, v in enumerate(cb):
            o.append("  " + g(v) + ("," if j < k * m - 1 else "") + ("\n" if (j + 1) % k == 0 else ""))
        o.append("};\n")
    o.append("\nconst struct lsp_codebook %s[] = {\n" % arr)
    for i, (k, m) in enumerate(cbs):
        o.append("  /* %s */\n  {\n    %d,\n    %d,\n    %d,\n    codes%d\n  },\n" % (os.path.basename(files[i]), k, int(round(math.log(m) / math.log(2))), m, i))
    o.append("  { 0, 0, 0, 0 }\n};\n")
    open(os.path.join(out, fname + '.c'), 'w').write(''.join(o))
    print(fname, len(files), 'tables')
