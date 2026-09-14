# -*- coding: utf-8 -*-
"""Paired-identity audit for the TinecmaTool rebrand.

A half-rename is worse than no rename: the producer and the consumer of an
identity string must agree, so we check every *pair* (side A vs side B) rather
than just "is the new string present somewhere".

Run from the repo root:  python tools/check_branding_pairs.py
Exit code 0 = every pair consistent.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN_DIRS = ['renderdoc', 'renderdoccmd', 'renderdocshim', 'qrenderdoc']
EXCLUDE_DIRS = {'.git', '.vs', '.workbuddy', '3rdparty', 'docs', 'build', 'x64', 'Win32',
                'agent-chats', 'backup_pre_fix', 'batch_fbx_exporter_ExtraUV', 'util',
                'official', 'android', 'pyside', 'qt', 'python'}
EXT = {'.h', '.hpp', '.cpp', '.c', '.inl', '.pri', '.pro', '.rc', '.json', '.vcxproj',
       '.txt', '.py', '.bat', '.cmd', '.sh', '.cmake', '.filters', '.ui', '.qrc', '.md'}
ROOT_FILES = ['CMakeLists.txt', 'build_and_deploy.cmd', 'renderdoc.sln']

TEXT = {}


def load(path):
    with open(path, 'rb') as f:
        raw = f.read()
    if raw.startswith(b'\xff\xfe'):
        return raw[2:].decode('utf-16-le', 'replace')
    if raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8', 'replace')
    if len(raw) > 8 and raw.count(b'\x00') > len(raw) // 8:
        return raw.decode('utf-16-le', 'replace')
    return raw.decode('utf-8', 'replace')


for d in SCAN_DIRS:
    for dp, dns, fns in os.walk(os.path.join(ROOT, d)):
        dns[:] = [x for x in dns if x not in EXCLUDE_DIRS]
        for fn in fns:
            if os.path.splitext(fn)[1].lower() in EXT:
                p = os.path.join(dp, fn)
                try:
                    TEXT[os.path.normpath(p)] = load(p)
                except Exception:
                    pass
for fn in ROOT_FILES:
    p = os.path.join(ROOT, fn)
    if os.path.isfile(p):
        TEXT[os.path.normpath(p)] = load(p)


def rel(p):
    return os.path.relpath(p, ROOT).replace(os.sep, '/')


# Files whose brand strings are legitimately still the upstream ones.
PLATFORM = ('renderdoccmd/renderdoccmd_android.cpp', 'renderdoccmd/renderdoccmd_apple.cpp',
            'renderdoccmd/renderdoccmd_linux.cpp', 'renderdoc/android/',
            'renderdoc/os/apple/', 'renderdoc/driver/metal/', 'qrenderdoc/share/',
            'renderdoccmd/android/', 'renderdoc/os/posix/',
            'renderdoc/api/app/renderdoc_app.h',      # public C API
            'qrenderdoc/Code/pyrenderdoc/',           # python module names
            'fix_vulkan_layer_registration.py',       # talks to the OFFICIAL install
            'renderdoc/rdocself.version',
            'backup_pre_fix/')

# Lines that mention an upstream name only in prose / an error message. These are
# not identities, they are documentation, and they SHOULD say RenderDoc.
PROSE = re.compile(r'(^\s*(//|rem\s|/\*|\*|#))|(echo\s)|(\[ERROR\])')


def find(needle, extra_exclude=()):
    out = []
    for p, t in TEXT.items():
        r = rel(p)
        if any(s in r for s in PLATFORM) or any(s in r for s in extra_exclude):
            continue
        for i, line in enumerate(t.split('\n'), 1):
            if needle in line:
                out.append((r, i, line.strip(), bool(PROSE.match(line.strip()))))
    return out


FAILED = []


def report(title, needle, *, want='present', extra_exclude=(), prose_ok=False, show=4):
    hits = find(needle, extra_exclude)
    if prose_ok:
        real = [h for h in hits if not h[3]]
    else:
        real = hits
    if want == 0:
        good = len(real) == 0
    elif want == 'present':
        good = len(real) > 0
    elif isinstance(want, int):
        good = len(real) == want
    else:
        good = True
    if not good:
        FAILED.append('%s  (found %d, want %s)' % (needle, len(real), want))
    print('  [%s] %-44s real=%d wanted=%s' % ('OK' if good else 'FAIL', needle, len(real), want))
    for r, i, line, _ in (real or hits)[:show]:
        print('        %s:%d  %s' % (r, i, line[:118]))
    rest = len(real or hits) - show
    if rest > 0:
        print('        ... %d more' % rest)


def head(t):
    print('\n' + '=' * 78)
    print(t)
    print('=' * 78)


head('PAIR 1  replay marker   (exported symbol  <->  LibraryHooks::Detect)')
report('new marker export', 'TinecmaTool__replay__marker', extra_exclude=('build_and_deploy.cmd',))
report('old marker gone', 'renderdoc__replay__marker', want=0)
report('RDOC_BASE_NAME define', '#define RDOC_BASE_NAME TinecmaTool', extra_exclude=('build_and_deploy.cmd',))
report('CMake RDOC_BASE_NAME', 'set(RDOC_BASE_NAME "TinecmaTool")')
print('  consumers build the name at runtime: STRINGIZE(RDOC_BASE_NAME) "__replay__marker"')

head('PAIR 2  global-hook shim file mapping  (shim creates  <->  injector opens)')
report('mapping name 64', 'TinecmaToolGlobalHookData64', want=2)
report('mapping name 32', 'TinecmaToolGlobalHookData32', want=2)
report('old mapping names gone', 'RenderDocGlobalHookData', want=0)

head('PAIR 3  crash-server ready event  (renderdoccmd creates  <->  crash_handler waits)')
report('TINECMATOOL_CRASHHANDLE literal', '"TINECMATOOL_CRASHHANDLE"', want=2,
       extra_exclude=('build_and_deploy.cmd',))
report('old event name gone', '"RENDERDOC_CRASHHANDLE"', want=0)
report('old breakpad pipe gone', 'RenderDocBreakpadServer', want=0)

head('PAIR 4  helper exe names  (injection blacklist must match the renamed exes)')
report('shim dll literal', 'TinecmaToolshim64.dll', extra_exclude=('build_and_deploy.cmd',))
report('old shim dll gone', 'renderdocshim64.dll', want=0, extra_exclude=('build_and_deploy.cmd',))
report('blacklist cmd exe', 'tinecmatoolcmd.exe', want=2)
report('blacklist ui exe', 'qtinecmatool.exe', want=2)
# build_and_deploy.cmd deliberately names the stale pre-rebrand binaries so it can
# refuse to ship a build next to them -> it is the one file allowed to say these.
STALE = ('build_and_deploy.cmd',)
report('old blacklist entries gone', 'qrenderdoc.exe', want=0, extra_exclude=STALE)
report('old blacklist entries gone', 'renderdoccmd.exe', want=0, extra_exclude=STALE)

head('PAIR 5  names read OUT OF the captured process (Vulkan / GL tool name)')
report('vk tool-name compare', '== "TinecmaTool"')
report('gl debug tool name', 'GL_DEBUG_TOOL_NAME_EXT')

head('PAIR 6  Vulkan layer identity  (loader manifest  <->  exported entrypoints)')
report('VK_LAYER_TINECMATOOL_Capture', 'VK_LAYER_TINECMATOOL_Capture')
report('upstream layer name only in prose', 'VK_LAYER_RENDERDOC_Capture', want=0, prose_ok=True)

head('PAIR 7  on-disk paths')
report('appdata source-tree includes are unrelated (informational)',
       'renderdoc\\3rdparty', want='any', show=0)
report('old symbol cache', '\\renderdoc\\symbols', want=0)
report('old dump folder', 'RenderDoc\\dumps', want=0)
report('old capture folder prefix', 'RenderDoc\\%ls_', want=0)
report('old remote-server log path', '/RenderDoc/RemoteServer_', want=0)

head('PAIR 8  thread-hijack injection wiring (win32_process.cpp)')
w = TEXT[os.path.normpath(os.path.join(ROOT, 'renderdoc', 'os', 'win32', 'win32_process.cpp'))]
for tok, want in (('#ifndef TINECMATOOL_USE_THREADHIJACK_INJECT', 1),
                  ('#define TINECMATOOL_USE_THREADHIJACK_INJECT 1', 1),
                  ('#if TINECMATOOL_USE_THREADHIJACK_INJECT', 2),
                  ('static bool InjectDLL_ThreadHijack', 1),
                  ('static bool InjectFunctionCall_ThreadHijack', 1),
                  ('if(InjectDLL_ThreadHijack(', 1),
                  ('if(InjectFunctionCall_ThreadHijack(', 1)):
    got = w.count(tok)
    good = got == want
    if not good:
        FAILED.append('win32_process.cpp %s = %d (want %d)' % (tok, got, want))
    print('  [%s] %-56s %d (want %d)' % ('OK' if good else 'FAIL', tok, got, want))
print('  fallback RDCWARN count = %d' % w.count('falling back to CreateRemoteThread'))

head('PAIR 9  case-sensitivity guard (needles vs strlower()\'d haystacks)')
print('  sys_win32_hooks.cpp lowercases `app` / `cmd` before contains(), so the')
print('  needle there MUST be lowercase or the blacklist silently never fires.')
report('lowercase cmd needle', 'contains("tinecmatoolcmd.exe")', want=2)
report('lowercase ui needle', 'contains("qtinecmatool.exe")', want=2)
report('PascalCase needle must NOT be here', 'app.contains("TinecmaToolcmd.exe")', want=0)
report('PascalCase needle must NOT be here', 'app.contains("qTinecmaTool.exe")', want=0)

print('\n' + '=' * 78)
if FAILED:
    print('RESULT: %d INCONSISTENC(Y|IES)' % len(FAILED))
    for f in FAILED:
        print('  - %s' % f)
else:
    print('RESULT: ALL PAIRS CONSISTENT')
print('=' * 78)
sys.exit(1 if FAILED else 0)
