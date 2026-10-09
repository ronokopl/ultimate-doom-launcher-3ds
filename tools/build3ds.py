"""Build entry point: export -> gen_make -> PREFLIGHT -> make (devkitPro msys2) -> error summary.

  py tools/build3ds.py [--dev] [make targets/args...]
  --dev : compile even if preflight still reports unfinished rows (development iterations only).
"""
import subprocess, sys, os, re, collections
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MSYS_BASH = r'C:\devkitPro\msys2\usr\bin\bash.exe'

def run(script):
    return subprocess.run([sys.executable, os.path.join(HERE, script)], cwd=HERE,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

def main():
    args = sys.argv[1:]
    dev = '--dev' in args
    args = [a for a in args if a != '--dev']
    for s in ('export.py', 'gen_c.py', 'gen_make.py', 'gen_icon.py', 'gen_banner.py', 'gen_banner3d.py', 'gen_cia.py', 'gen_romfs.py'):
        r = run(s)
        if r.returncode:
            print(r.stdout); sys.exit('failed: ' + s)
    pf = run('preflight.py')
    m = re.search(r'"errors": (\d+)', pf.stdout)
    errors = int(m.group(1)) if m else -1
    print('preflight errors:', errors)
    if errors and not dev:
        print(pf.stdout[-3000:]); sys.exit('preflight not clean: fix the sheets first (or --dev for iteration builds)')
    log = os.path.join(ROOT, 'build', 'make.log')
    cmd = ('export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM; cd "%s" && make -f build/Makefile -k -j24 %s'
           % (ROOT.replace('\\', '/').replace('E:', '/e'), ' '.join(args)))
    with open(log, 'w') as f:
        rc = subprocess.run([MSYS_BASH, '-lc', cmd], stdout=f, stderr=subprocess.STDOUT).returncode
    text = open(log, errors='replace').read()
    errs = re.findall(r'^([^\s:][^:\n]*):\d+:\d+: (?:fatal )?error: (.*)$', text, re.M)
    print('make rc=%d, errors=%d' % (rc, len(errs)))
    for f, n in collections.Counter(e[0] for e in errs).most_common(25):
        print('  %4d %s' % (n, f))
    if rc == 0:	# set row statuses from objective checks, then re-run preflight on the result
        for s in ('verify_build.py', 'export.py', 'preflight.py'):
            r = run(s)
            if s != 'export.py':
                print(r.stdout.strip()[-1500:])
    sys.exit(rc)

if __name__ == '__main__':
    main()
