#//puncho - SB0: helper of build_phonon.cmd (WP-SB0 R-SB0-1, R-SB0-4, R-SB0-5).
#
#   phonon_build.py treehash <dir>
#       one SHA-256 over a tree: files in sorted relative path order ('/' separators); for each file the
#       path (UTF-8), a NUL, the size in decimal, a NUL, then the bytes. Prints the hex digest.
#   phonon_build.py ippcheck <variant dir>
#       the R-SB0-4 checks on <variant dir>\core and <variant dir>\build; prints one line per check and
#       exits 1 on the first failure. Needs dumpbin on PATH (vcvars64).
#   phonon_build.py source <pristine core> <patch dir> <variant dir> <out SOURCE.txt>
#   phonon_build.py thirdparty <core THIRDPARTY.md> <license dir> <out THIRDPARTY.md>
#
# Writes nothing that depends on time, host or path order beyond what each line names.

import hashlib
import json
import os
import re
import subprocess
import sys


def tree_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace('\\', '/')
            out.append((rel, full))
    out.sort(key=lambda x: x[0].encode('utf-8'))
    return out


def tree_hash(root):
    h = hashlib.sha256()
    for rel, full in tree_files(root):
        with open(full, 'rb') as f:
            data = f.read()
        h.update(rel.encode('utf-8'))
        h.update(b'\0')
        h.update(str(len(data)).encode('ascii'))
        h.update(b'\0')
        h.update(data)
    return h.hexdigest()


def file_hash(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def fail(text):
    print('CHECK FAILED: ' + text)
    sys.exit(1)


def ok(text):
    print('check ok: ' + text)


# Windows system DLLs a static-CRT phonon.dll may import. Anything else (an ipp*.dll, a CRT DLL, OpenCL) stops.
SYSTEM_DLLS = {'kernel32.dll', 'user32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll',
               'gdi32.dll', 'ws2_32.dll', 'bcrypt.dll', 'ntdll.dll', 'winmm.dll', 'shlwapi.dll', 'version.dll'}

IPP_NAME = re.compile(rb'(?<![A-Za-z0-9_])_?ipp(s|core|i|vm|m|ch|cc|dc|cv)[A-Z_][A-Za-z0-9_]{2,}')


def ipp_check(vdir):
    core = os.path.join(vdir, 'core')
    build = os.path.join(vdir, 'build')

    # 1. no IPP package in the dependency folder
    if os.path.exists(os.path.join(core, 'deps', 'ipp')):
        fail('core/deps/ipp exists')
    ok('no core/deps/ipp')

    # 2. the cache: IPP off. FFT_LIBRARY is a plain (non-cache) variable in core/CMakeLists.txt:217-236,
    # so the cache cannot hold it; the PFFFT package found is the cache's evidence, the vcxproj's source list is the proof (3).
    cache = open(os.path.join(build, 'CMakeCache.txt'), 'r', encoding='utf-8', errors='replace').read()
    if not re.search(r'^STEAMAUDIO_ENABLE_IPP:BOOL=OFF$', cache, re.M):
        fail('CMakeCache.txt lacks STEAMAUDIO_ENABLE_IPP:BOOL=OFF')
    ok('CMakeCache.txt STEAMAUDIO_ENABLE_IPP:BOOL=OFF')
    for m in re.finditer(r'^IPP_[A-Za-z0-9_]*:[A-Z]+=(.*)$', cache, re.M):
        if m.group(1).strip() and 'NOTFOUND' not in m.group(1):
            fail('CMakeCache.txt holds a found IPP path: ' + m.group(0).strip())
    m = re.search(r'^PFFFT_[A-Za-z_]*:[A-Z]+=(.+)$', cache, re.M)
    if not m or 'NOTFOUND' in m.group(1):
        fail('CMakeCache.txt has no PFFFT path')
    ok('CMakeCache.txt PFFFT found: ' + m.group(0).strip())

    # 3. no project defines IPL_ENABLE_IPP or links an IPP library; core compiles pffft_fft.cpp, not ipp_fft.cpp
    core_proj = None
    for dirpath, dirnames, filenames in os.walk(build):
        for name in filenames:
            if not name.endswith('.vcxproj'):
                continue
            path = os.path.join(dirpath, name)
            text = open(path, 'r', encoding='utf-8', errors='replace').read()
            if 'IPL_ENABLE_IPP' in text:
                fail(path + ' defines IPL_ENABLE_IPP')
            if re.search(r'[\\/;"]ipp[a-z0-9_]*\.lib', text, re.I) or re.search(r'[\\/]ipp[\\/]', text, re.I):
                fail(path + ' names an IPP library')
            if name == 'core.vcxproj':
                core_proj = text
    if core_proj is None:
        fail('no core.vcxproj')
    if 'pffft_fft.cpp' not in core_proj or 'ipp_fft.cpp' in core_proj or 'ipp_array_math.cpp' in core_proj:
        fail('core.vcxproj does not compile pffft_fft.cpp alone (FFT_LIBRARY is not PFFFT)')
    ok('no vcxproj has IPL_ENABLE_IPP or an ipp library; core.vcxproj compiles pffft_fft.cpp, not ipp_fft.cpp')

    # 4. phonon.dll depends on Windows system DLLs only
    dll = os.path.join(build, 'src', 'core', 'Release', 'phonon.dll')
    out = subprocess.run(['dumpbin', '/nologo', '/dependents', dll], capture_output=True, text=True)
    if out.returncode != 0:
        fail('dumpbin /dependents failed: ' + out.stdout + out.stderr)
    deps = re.findall(r'^\s+(\S+\.dll)\s*$', out.stdout, re.M | re.I)
    if not deps:
        fail('dumpbin /dependents listed nothing')
    for d in deps:
        if d.lower() not in SYSTEM_DLLS:
            fail('phonon.dll depends on ' + d)
    ok('phonon.dll dependents: ' + ', '.join(deps))

    # 5. no IPP name was linked. core is an OBJECT library (src/core/CMakeLists.txt:186), so there is no
    # core.lib, and /GL objects carry no COFF symbol table; the linked names are in phonon.pdb and the DLL.
    pdb = os.path.join(build, 'src', 'core', 'Release', 'phonon.pdb')
    for path in (dll, pdb):
        data = open(path, 'rb').read()
        m = IPP_NAME.search(data)
        if m:
            fail(path + ' holds the IPP name ' + m.group(0).decode('ascii', 'replace'))
    out = subprocess.run(['dumpbin', '/nologo', '/exports', dll], capture_output=True, text=True)
    if re.search(r'\bipp', out.stdout):
        fail('phonon.dll exports an ipp name')
    ok('no ipps/ippcore/ippvm name in phonon.dll, phonon.pdb or the exports')


def write_source(pristine_core, patch_dir, vdir, out_path):
    lines = []
    lines.append('Steam Audio 4.8.1 core, built by build_phonon.cmd without Intel IPP (WP-SB0).')
    lines.append('pristine core/ tree SHA-256 (phonon_build.py treehash): ' + tree_hash(pristine_core))
    deps = json.load(open(os.path.join(pristine_core, 'build', 'dependencies.json'), 'r'))
    for name in ('flatbuffers', 'pffft', 'zlib', 'mysofa', 'catch'):
        fetch = deps[name]['fetch']
        lines.append('dependency %s %s %s' % (name, fetch['git'], fetch['tag']))
    for name in ('0001-repeatable-bake.patch', '0002-probe-stats.patch'):
        path = os.path.join(patch_dir, name)
        lines.append('patch %s SHA-256 %s' % (name, file_hash(path)))
    # the compiler's version, never the machine's install path
    version = 'unknown'
    files = os.path.join(vdir, 'build', 'CMakeFiles')
    for name in sorted(os.listdir(files)):
        path = os.path.join(files, name, 'CMakeCXXCompiler.cmake')
        if os.path.exists(path):
            m = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([0-9.]+)"\)', open(path, 'r').read())
            if m:
                version = m.group(1)
                break
    lines.append('compiler MSVC cl ' + version)
    with open(out_path, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def write_thirdparty(core_md, license_dir, out_path):
    text = open(core_md, 'r', encoding='utf-8').read().rstrip('\n') + '\n'
    for title, name in (('FlatBuffers', 'flatbuffers-LICENSE'), ('zlib', 'zlib-LICENSE')):
        lic = open(os.path.join(license_dir, name), 'r', encoding='utf-8', errors='replace').read().rstrip('\n')
        text += '\n## ' + title + '\n\n```\n' + lic + '\n```\n'
    with open(out_path, 'w', newline='\n', encoding='utf-8') as f:
        f.write(text)


def main():
    cmd = sys.argv[1]
    if cmd == 'treehash':
        print(tree_hash(sys.argv[2]))
    elif cmd == 'ippcheck':
        ipp_check(sys.argv[2])
    elif cmd == 'source':
        write_source(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif cmd == 'thirdparty':
        write_thirdparty(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print('unknown command ' + cmd)
        sys.exit(2)


main()
