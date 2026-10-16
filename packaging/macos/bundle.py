"""Monta o ``clivcut.app`` autocontido a partir do build do cross-compile.

Faz no Linux o que o ``macdeployqt`` faria num Mac::

    clivcut.app/Contents/
        MacOS/clivcut, MacOS/clivcut-render
        Frameworks/      Qt*.framework, FFmpeg, SDL2, ONNX Runtime (.dylib)
        PlugIns/         plugins do Qt (cocoa, formatos de imagem, TLS...)
        Resources/qml/   módulos QML que o app importa (qmlimportscanner)
        Resources/qt.conf

Tudo que é universal (o Qt oficial) é afinado para a arquitetura do pacote;
cada binário fica com um único rpath, ``@executable_path/../Frameworks``, e o
pacote é assinado ad hoc com o rcodesign (o arm64 não roda sem assinatura).

Uso (pelo build.sh): bundle.py --arch x86_64 --build <build> --qt <qt macos>
                     --qt-host <qt linux> --deps <deps> --out <clivcut.app>
"""
import argparse
import json
import os
import plistlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

MACHO_64 = b'\xcf\xfa\xed\xfe'
MACHO_FAT = b'\xca\xfe\xba\xbe'
LC_RPATH = 0x8000001C
LC_DYLIB_DEPS = {0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023}  # load, weak, reexport, lazy, upward
RPATH = '@executable_path/../Frameworks'

# Plugins do Qt que o app usa: janela (cocoa), imagens/ícones (SVG, JPEG...),
# HTTPS dos downloads de modelos (tls) e estado da rede.
QT_PLUGINS = {
    'platforms': ['libqcocoa.dylib', 'libqoffscreen.dylib'],  # offscreen: clivcut-render
    'imageformats': None,      # None = a pasta inteira
    'iconengines': None,
    'tls': None,
    'networkinformation': None,
}
# Importados em tempo de execução, invisíveis para o scanner: o estilo Basic
# (QQuickStyle::setStyle no main.cpp) e os diálogos nativos.
EXTRA_QML_IMPORTS = ['QtQuick.Controls.Basic', 'QtQuick.Dialogs', 'QtQuick.Layouts']


def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with open(path, 'rb') as f:
        return f.read(4) in (MACHO_64, MACHO_FAT)


def is_fat(path: Path) -> bool:
    with open(path, 'rb') as f:
        return f.read(4) == MACHO_FAT


def load_commands(path: Path):
    """(dylibs carregadas, rpaths) de um Mach-O de 64 bits já afinado."""
    data = path.read_bytes()
    if data[:4] != MACHO_64:
        raise ValueError(f'{path}: não é um Mach-O de 64 bits afinado')
    ncmds, = struct.unpack_from('<I', data, 16)
    offset, deps, rpaths = 32, [], []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd in LC_DYLIB_DEPS or cmd == LC_RPATH:
            name_at, = struct.unpack_from('<I', data, offset + 8)
            raw = data[offset + name_at:offset + size].split(b'\0', 1)[0].decode()
            (rpaths if cmd == LC_RPATH else deps).append(raw)
        offset += size
    return deps, rpaths


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


class Bundler:
    def __init__(self, args):
        self.arch = args.arch
        self.build = Path(args.build)
        self.qt = Path(args.qt)
        self.qt_host = Path(args.qt_host)
        self.app = Path(args.out)
        self.contents = self.app / 'Contents'
        self.frameworks = self.contents / 'Frameworks'
        # Onde procurar as @rpath/*.dylib: dependências, ONNX Runtime, Qt.
        self.lib_dirs = [Path(args.deps) / 'lib', *self.build.glob('_deps/onnxruntime_pkg-src/lib'),
                         self.qt / 'lib']

    def make(self):
        shutil.rmtree(self.app, ignore_errors=True)
        shutil.copytree(self.build / 'src/app/clivcut.app', self.app, symlinks=True)
        shutil.copy2(self.build / 'src/cli/clivcut-render', self.contents / 'MacOS')
        self.frameworks.mkdir(exist_ok=True)
        self.copy_plugins()
        self.copy_qml()
        (self.contents / 'Resources/qt.conf').write_text(
            '[Paths]\nPlugins = PlugIns\nImports = Resources/qml\nQmlImports = Resources/qml\n')
        self.patch_info_plist()
        self.resolve_dependencies()
        self.sign()

    # -- conteúdo do Qt ---------------------------------------------------

    def copy_plugins(self):
        for group, names in QT_PLUGINS.items():
            src = self.qt / 'plugins' / group
            if not src.is_dir():
                continue
            dst = self.contents / 'PlugIns' / group
            dst.mkdir(parents=True, exist_ok=True)
            for lib in sorted(src.glob('*.dylib')):
                if (names is None or lib.name in names) and not lib.stem.endswith('_debug'):
                    shutil.copy2(lib, dst)

    def copy_qml(self):
        """Os módulos QML que o app importa, sem os submódulos não usados."""
        with tempfile.TemporaryDirectory() as extra:
            Path(extra, 'Extra.qml').write_text(
                ''.join(f'import {m}\n' for m in EXTRA_QML_IMPORTS) + 'Item {}\n')
            out = subprocess.run(
                [self.qt_host / 'libexec/qmlimportscanner', '-rootPath', '/src/src/app/qml',
                 '-rootPath', extra, '-importPath', self.qt / 'qml'],
                check=True, capture_output=True, text=True).stdout
        qml_root = (self.qt / 'qml').resolve()
        dst_root = self.contents / 'Resources/qml'
        for entry in json.loads(out):
            path = Path(entry.get('path', ''))
            if entry.get('type') != 'module' or not path.is_dir() or qml_root not in path.resolve().parents:
                continue
            self._copy_module(path, dst_root / path.resolve().relative_to(qml_root))

    def _copy_module(self, src: Path, dst: Path):
        dst.mkdir(parents=True, exist_ok=True)
        for item in src.iterdir():
            if item.is_dir():
                # Subpasta com qmldir é outro módulo: entra só se o scanner pedir.
                if not (item / 'qmldir').exists() and not item.name.endswith('.dSYM'):
                    shutil.copytree(item, dst / item.name, symlinks=True, dirs_exist_ok=True)
            elif not item.stem.endswith('_debug'):
                shutil.copy2(item, dst / item.name)

    def patch_info_plist(self):
        plist = self.contents / 'Info.plist'
        info = plistlib.loads(plist.read_bytes())
        info.update({
            'CFBundleDisplayName': 'clivcut',
            'LSMinimumSystemVersion': os.environ.get('MACOS_MIN', '13.0'),
            'LSArchitecturePriority': [self.arch],
            'NSHighResolutionCapable': True,
            'NSSupportsAutomaticGraphicsSwitching': True,
            'NSRequiresAquaSystemAppearance': False,
            'LSApplicationCategoryType': 'public.app-category.video',
            # Sem esta chave o macOS encerra o app ao abrir o microfone (narração).
            'NSMicrophoneUsageDescription': 'clivcut records voiceovers from the microphone.',
        })
        plist.write_bytes(plistlib.dumps(info))

    # -- dependências e rpaths --------------------------------------------

    def machos(self):
        return [p for p in self.app.rglob('*') if is_macho(p)]

    def resolve_dependencies(self):
        done = set()
        while True:
            pending = [p for p in self.machos() if p not in done]
            if not pending:
                break
            for path in pending:
                done.add(path)
                if is_fat(path):
                    run('llvm-lipo', path, '-thin', self.arch, '-output', path)
                deps, rpaths = load_commands(path)
                for dep in deps:
                    self.bring(dep, path)
                if rpaths != [RPATH]:
                    if rpaths:
                        run('llvm-install-name-tool', '-delete_all_rpaths', path)
                    run('llvm-install-name-tool', '-add_rpath', RPATH, path)

    def bring(self, dep: str, user: Path):
        """Copia para Contents/Frameworks o alvo de uma dependência @rpath."""
        if dep.startswith(('/usr/lib/', '/System/', '@loader_path/', '@executable_path/')):
            return
        if not dep.startswith('@rpath/'):
            raise SystemExit(f'{user}: dependência fora do pacote: {dep}')
        first = dep.split('/')[1]
        if first.endswith('.framework'):
            dst = self.frameworks / first
            if not dst.exists():
                shutil.copytree(self.qt / 'lib' / first, dst, symlinks=True,
                                ignore=shutil.ignore_patterns('Headers', '*.prl', '*_debug'))
            return
        dst = self.frameworks / first
        if dst.exists():
            return
        for d in self.lib_dirs:
            if (d / first).exists():
                shutil.copy2((d / first).resolve(), dst)
                return
        raise SystemExit(f'{user}: {dep} não encontrada em {[str(d) for d in self.lib_dirs]}')

    def sign(self):
        """Ad hoc: cada binário, depois o pacote (que sela os recursos)."""
        for path in self.machos():
            run('rcodesign', 'sign', path)
        run('rcodesign', 'sign', self.app)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--arch', required=True, choices=['x86_64', 'arm64'])
    ap.add_argument('--build', required=True)
    ap.add_argument('--qt', required=True)
    ap.add_argument('--qt-host', required=True)
    ap.add_argument('--deps', required=True)
    ap.add_argument('--out', required=True)
    Bundler(ap.parse_args()).make()


if __name__ == '__main__':
    main()
