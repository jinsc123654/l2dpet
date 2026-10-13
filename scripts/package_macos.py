#!/usr/bin/env python3
"""Create a relocatable, ad-hoc signed macOS app and DMG (not hardware tested)."""
import pathlib, plistlib, platform, shutil, subprocess, sys
root, work = map(pathlib.Path, sys.argv[1:])
version = (root / 'version.txt').read_text().strip().strip('{}').strip()
arch = platform.machine()
out = root / 'dist' / ('macos-' + arch)
out.mkdir(parents=True, exist_ok=True)
app = out / 'l2dpet.app'
if app.exists(): shutil.rmtree(app)
contents = app / 'Contents'
mac = contents / 'MacOS'
libs = contents / 'Frameworks'
resources = contents / 'Resources'
for p in (mac, libs, resources): p.mkdir(parents=True)
for name, build in [('l2dtest', 'build-host'), ('l2dpet-ui', 'build-panel')]:
    shutil.copy2(work / build / name, mac / name)
shutil.copytree(root / 'live2d/bundled-models', resources / 'models')
shutil.copytree(work / 'build-panel/assets', resources / 'assets')
for name in ('l2dpet.png', 'l2dpet-logo.png'):
    shutil.copy2(work / 'build-panel' / name, resources / name)
# Existing runtime resolves assets beside the executable; links keep Resources canonical.
for name in ('models', 'assets', 'l2dpet.png', 'l2dpet-logo.png'):
    (mac / name).symlink_to('../Resources/' + name)
with (contents / 'Info.plist').open('wb') as f:
    plistlib.dump(dict(CFBundleExecutable='l2dtest', CFBundleIdentifier='com.l2dpet.desktop',
        CFBundleName='l2dpet', CFBundlePackageType='APPL', CFBundleShortVersionString=version,
        CFBundleVersion=version, NSHighResolutionCapable=True, LSUIElement=True), f)
note = ('macOS 测试构建 / macOS test build\n版本 / Version: ' + version + '\n架构 / Architecture: ' + arch +
    '\n未经 macOS 实机验证。CI 仅验证编译与 headless 渲染，不代表桌宠窗口、托盘、权限或开机启动已验证。\n'
    'Not verified on real macOS hardware. CI checks compilation and headless rendering only.\n'
    '本构建仅使用临时 ad-hoc 签名，未进行 Apple 公证，Gatekeeper 可能阻止打开。\n'
    'Ad-hoc signed only; not Apple notarized. Gatekeeper may block opening.\n'
    '将 l2dpet.app 拖到 Applications 后打开。 / Drag l2dpet.app to Applications.\n')
(resources / 'TEST-BUILD.txt').write_text(note)

def run(*args): return subprocess.check_output(args, text=True)
def deps(p):
    return [line.strip().split(' (', 1)[0] for line in run('otool', '-L', str(p)).splitlines()[1:]]
def rpaths(p):
    lines = run('otool', '-l', str(p)).splitlines()
    return [lines[i+2].strip().split('path ',1)[1].split(' (offset',1)[0]
            for i,line in enumerate(lines) if line.strip() == 'cmd LC_RPATH']
copied = {}
def relocate(target, original, executable):
    for dep in deps(original):
        if dep.startswith(('/usr/lib/', '/System/Library/')): continue
        if original.suffix == '.dylib' and dep == run('otool','-D',str(original)).splitlines()[-1].strip(): continue
        def expand(s):
            return s.replace('@loader_path', str(original.parent)).replace('@executable_path', str(executable.parent))
        candidates = ([pathlib.Path(expand(p)) / dep[len('@rpath/'):] for p in rpaths(original) + rpaths(executable)]
                      if dep.startswith('@rpath/') else [pathlib.Path(expand(dep))])
        source = next((p.resolve() for p in candidates if p.exists()), None)
        if source is None: raise RuntimeError(f'Unresolved dependency: {original}: {dep}')
        dest = libs / source.name
        if dest.name in copied and copied[dest.name] != source:
            raise RuntimeError(f'Dylib name collision: {source}')
        if dest.name not in copied:
            copied[dest.name] = source
            shutil.copy2(source, dest)
            dest.chmod(0o755)
            subprocess.run(['install_name_tool','-id','@rpath/'+dest.name,str(dest)],check=True)
            relocate(dest, source, executable)
        replacement = ('@loader_path/../Frameworks/' if target.parent == mac else '@loader_path/') + dest.name
        subprocess.run(['install_name_tool','-change',dep,replacement,str(target)],check=True)
for name, build in [('l2dtest','build-host'), ('l2dpet-ui','build-panel')]:
    original = work / build / name
    relocate(mac / name, original, original)
for p in list(libs.iterdir()) + [mac/'l2dtest',mac/'l2dpet-ui']:
    for dep in deps(p):
        if not dep.startswith(('/usr/lib/','/System/Library/','@loader_path/','@rpath/')):
            raise RuntimeError(f'Non-portable dependency: {p}: {dep}')
    # Signing the main executable signs its enclosing bundle; do that last.
    if p != mac / 'l2dtest':
        subprocess.run(['codesign','--force','--sign','-',str(p)],check=True)
subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
name = f'l2dpet-{version}-macos-{arch}-untested'
# ditto preserves macOS bundle symlinks and executable permissions.
subprocess.run(['ditto','-c','-k','--sequesterRsrc','--keepParent',str(app),str(out/(name+'.zip'))],check=True)
with __import__('tempfile').TemporaryDirectory() as stage:
    stage = pathlib.Path(stage)
    shutil.copytree(app,stage/app.name)
    (stage/'TEST-BUILD.txt').write_text(note)
    (stage/'Applications').symlink_to('/Applications')
    dmg = out/(name+'.dmg')
    if dmg.exists(): dmg.unlink()
    subprocess.run(['hdiutil','create','-volname','l2dpet (untested)','-srcfolder',str(stage),
                    '-format','UDZO',str(dmg)],check=True)
print(out)
