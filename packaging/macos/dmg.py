"""Instalador ``.dmg`` do clivcut — montado no Linux, sem ``hdiutil``.

A janela que abre ao montar segue o padrão "arraste para Aplicativos": fundo
claro, o ícone do app à esquerda, o atalho de Aplicativos à direita e, entre
eles, "drag and drop" com uma seta curva. Só o app e o atalho aparecem; o
fundo, o ``.DS_Store`` e o ícone do volume ficam ocultos.

O volume segue o caminho que a Mozilla usa no Firefox: ``mkfs.hfsplus`` cria
o HFS+, o ``hfsplus`` (libdmg-hfsplus) o preenche sem montar nada e o ``dmg``
comprime (UDZO). Janela, fundo e posição dos ícones vão no ``.DS_Store``,
escrito com ``ds_store`` + ``mac_alias``.

Uso:
    dmg.py --app clivcut.app --work <tmp> --out dist/clivcut.dmg
    dmg.py --icns packaging/macos/clivcut.icns   # ícone do app (CMake)
    dmg.py --preview <pasta>                     # confere a arte em PNG
"""
import argparse
import datetime
import os
import shutil
import struct
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ICON_PNG = HERE / 'clivcut-1024.png'
VOLUME_NAME = 'clivcut'
FONT = '/opt/fonts/Poppins-SemiBold.ttf'

BACKGROUND = (244, 248, 252)
NAVY = (8, 28, 66)
BLUE = (29, 99, 237)

# Janela em PONTOS (o Finder mede assim; o fundo sai em 1x e 2x). ``icons`` é o
# CENTRO de cada ícone, como o Finder grava no .DS_Store.
LAYOUT = {
    'window': (720, 400),
    'icon_size': 128,
    'text_size': 13,
    'icons': {'app': (165, 180), 'applications': (555, 180)},
}

# HFS+ (TN1150): cabeçalho do volume a 1024 bytes do início (e cópia no fim).
_HEADER_OFFSET = 1024
_CREATE_DATE = 16          # UInt32, segundos desde 1904
_NEXT_CATALOG_ID = 64      # UInt32, próximo CNID livre
_FINDER_INFO = 80          # 8 x UInt32; [2] = pasta aberta ao montar
_ROOT_CNID = 2
_MAC_EPOCH = datetime.datetime(1904, 1, 1, tzinfo=datetime.timezone.utc)
_MACHO_64 = b'\xcf\xfa\xed\xfe'


# -- arte ----------------------------------------------------------------------

def _font(size):
    try:
        return ImageFont.truetype(FONT, size)
    except OSError:
        return ImageFont.load_default(size)


def _bezier(p0, p1, p2, t):
    u = 1 - t
    return (u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
            u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1])


def _gradient_text(img, center, text, size, k):
    """Texto centrado com degradê horizontal (marinho -> azul)."""
    font = _font(size * k)
    mask = Image.new('L', img.size, 0)
    ImageDraw.Draw(mask).text((center[0] * k, center[1] * k), text, font=font, fill=255, anchor='mm')
    left, _, right, _ = mask.getbbox()
    ramp = Image.linear_gradient('L').rotate(90).resize((right - left, img.height))
    fill = Image.new('RGBA', img.size, NAVY + (255,))
    colored = Image.merge('RGB', [ramp.point(lambda v, a=a, b=b: round(a + (b - a) * v / 255))
                                  for a, b in zip(NAVY, BLUE)]).convert('RGBA')
    fill.paste(colored, (left, 0))
    img.paste(fill, (0, 0), mask)


def _arrow(draw, k, start, control, end):
    """Seta curva de traço uniforme e ponta aberta (chevron)."""
    width = round(5.5 * k)
    pts = [tuple(c * k for c in _bezier(start, control, end, i / 200)) for i in range(201)]
    draw.line(pts, fill=NAVY, width=width, joint='curve')
    for x, y in (pts[0], pts[-1]):
        r = width / 2
        draw.ellipse((x - r, y - r, x + r, y + r), fill=NAVY)
    # Ponta na direção da tangente final da curva.
    dx, dy = end[0] - control[0], end[1] - control[1]
    norm = (dx * dx + dy * dy) ** 0.5
    ux, uy = dx / norm, dy / norm
    tip = (end[0] * k, end[1] * k)
    for sign in (1, -1):
        # Gira a direção contrária em +-38 graus.
        c, s = 0.788, 0.616 * sign
        bx, by = -ux * c - (-uy) * s, -ux * s + -uy * c
        tail = (tip[0] + bx * 20 * k, tip[1] + by * 20 * k)
        draw.line([tip, tail], fill=NAVY, width=width)
        draw.ellipse((tail[0] - width / 2, tail[1] - width / 2, tail[0] + width / 2, tail[1] + width / 2),
                     fill=NAVY)
    r = width / 2
    draw.ellipse((tip[0] - r, tip[1] - r, tip[0] + r, tip[1] + r), fill=NAVY)


def background(scale=1) -> Image.Image:
    """Fundo da janela em ``scale`` (1 = pontos, 2 = Retina)."""
    ss = 4                                   # supersampling: bordas lisas
    k = scale * ss
    w, h = LAYOUT['window']
    img = Image.new('RGBA', (w * k, h * k), BACKGROUND + (255,))
    draw = ImageDraw.Draw(img)
    # Faixa clara em curva no rodapé, como a da referência.
    draw.ellipse((-w * 0.4 * k, (h - 46) * k, w * 1.4 * k, (h + 260) * k), fill=(255, 255, 255, 255))

    (ax, ay), (bx, by) = LAYOUT['icons']['app'], LAYOUT['icons']['applications']
    _gradient_text(img, ((ax + bx) / 2, ay - 4), 'drag and drop', 30, k)
    _arrow(draw, k, start=(ax + 68, ay + 46), control=((ax + bx) / 2, ay + 150), end=(bx - 76, ay + 44))
    return img.resize((w * scale, h * scale), Image.Resampling.LANCZOS).convert('RGB')


def write_background(path: Path) -> Path:
    """TIFF com as duas resoluções (72 e 144 dpi): o Finder escolhe a da tela."""
    path.parent.mkdir(parents=True, exist_ok=True)
    background(1).save(path, format='TIFF', dpi=(72, 72), compression='tiff_lzw',
                       save_all=True, append_images=[background(2)])
    _patch_tiff_resolution(path, [72, 144])   # save_all grava o dpi só na 1a página
    return path


def _patch_tiff_resolution(path: Path, dpis):
    """Reescreve XResolution/YResolution (tags 282/283) de cada página."""
    data = bytearray(path.read_bytes())
    endian = '<' if data[:2] == b'II' else '>'
    (offset,) = struct.unpack_from(endian + 'I', data, 4)
    for dpi in dpis:
        if not offset:
            break
        (count,) = struct.unpack_from(endian + 'H', data, offset)
        for i in range(count):
            entry = offset + 2 + i * 12
            tag, typ, _n, value = struct.unpack_from(endian + 'HHII', data, entry)
            if tag in (282, 283) and typ == 5:          # RATIONAL -> [num, den]
                struct.pack_into(endian + 'II', data, value, dpi, 1)
        (offset,) = struct.unpack_from(endian + 'I', data, offset + 2 + count * 12)
    path.write_bytes(bytes(data))


def write_icns(path: Path) -> Path:
    icon = Image.open(ICON_PNG).convert('RGBA')
    path.parent.mkdir(parents=True, exist_ok=True)
    icon.save(path, format='ICNS', append_images=[
        icon.resize((n, n), Image.Resampling.LANCZOS) for n in (16, 32, 64, 128, 256, 512)])
    return path


# -- volume HFS+ ----------------------------------------------------------------

def _read_u32(image: Path, offset: int) -> int:
    with open(image, 'rb') as f:
        f.seek(_HEADER_OFFSET + offset)
        return struct.unpack('>I', f.read(4))[0]


def _write_u32(image: Path, offset: int, value: int):
    """Grava no cabeçalho principal e na cópia do fim do volume."""
    size = image.stat().st_size
    with open(image, 'r+b') as f:
        for base in (_HEADER_OFFSET, size - _HEADER_OFFSET):
            f.seek(base + offset)
            f.write(struct.pack('>I', value))


def _hfs(image: Path, *args):
    subprocess.run(['hfsplus', '--symlinks', 'clone_link', '--special-modes', 'no',
                    str(image), *[str(a) for a in args]], check=True, stdout=subprocess.DEVNULL)


def _executables(root: Path):
    """Executáveis Mach-O (MH_EXECUTE) e scripts: o hfsplus cria tudo como 0644."""
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            path = Path(dirpath) / name
            if path.is_symlink():
                continue
            with open(path, 'rb') as f:
                head = f.read(16)
            if head[:2] == b'#!' or (head[:4] == _MACHO_64 and struct.unpack('<I', head[12:16])[0] == 2):
                yield path


def _write_ds_store(target: Path, volume_created: datetime.datetime, background_cnid: int, app_name: str):
    """A janela do volume: sem barras, do tamanho do fundo, ícones no lugar."""
    from ds_store import DSStore
    from mac_alias import ALIAS_FIXED_DISK, ALIAS_KIND_FILE, Alias, TargetInfo, VolumeInfo

    width, height = LAYOUT['window']
    # Alias do fundo montado à mão (o Alias.for_file só roda num Mac com o
    # volume montado). O Finder o resolve pelo nome do volume + caminho.
    alias = Alias(
        volume=VolumeInfo(VOLUME_NAME.encode(), volume_created, b'H+', ALIAS_FIXED_DISK, 0,
                          b'\0\0', posix_path=f'/Volumes/{VOLUME_NAME}'.encode()),
        target=TargetInfo(ALIAS_KIND_FILE, b'.background.tiff', _ROOT_CNID, background_cnid,
                          volume_created, b'\0\0\0\0', b'\0\0\0\0',
                          folder_name=VOLUME_NAME.encode(), cnid_path=[],
                          carbon_path=f'{VOLUME_NAME}:.background.tiff'.encode(),
                          posix_path=b'/.background.tiff'),
    )
    with DSStore.open(str(target), 'w+') as d:
        d['.']['vSrn'] = ('long', 1)
        d['.']['bwsp'] = {
            'ShowStatusBar': False,
            'WindowBounds': f'{{{{200, 120}}, {{{width}, {height}}}}}',
            'ContainerShowSidebar': False,
            'PreviewPaneVisibility': False,
            'SidebarWidth': 0,
            'ShowTabView': False,
            'ShowToolbar': False,
            'ShowPathbar': False,
            'ShowSidebar': False,
        }
        d['.']['icvp'] = {
            'viewOptionsVersion': 1,
            'backgroundType': 2,
            'backgroundImageAlias': alias.to_bytes(),
            'backgroundColorRed': 1.0,
            'backgroundColorGreen': 1.0,
            'backgroundColorBlue': 1.0,
            'gridOffsetX': 0.0,
            'gridOffsetY': 0.0,
            'gridSpacing': 100.0,
            'arrangeBy': 'none',
            'showIconPreview': False,
            'showItemInfo': False,
            'labelOnBottom': True,
            'textSize': float(LAYOUT['text_size']),
            'iconSize': float(LAYOUT['icon_size']),
            'scrollPositionX': 0.0,
            'scrollPositionY': 0.0,
        }
        d['.']['icvl'] = (b'type', b'icnv')
        d[app_name]['Iloc'] = LAYOUT['icons']['app']
        d['Applications']['Iloc'] = LAYOUT['icons']['applications']


def build_dmg(app: Path, out_dmg: Path, work: Path) -> Path:
    """``out_dmg`` com o ``app`` e o atalho de Aplicativos, na janela desenhada."""
    shutil.rmtree(work, ignore_errors=True)
    stage = work / 'stage'
    stage.mkdir(parents=True)
    shutil.move(str(app), stage / app.name)
    write_icns(stage / '.VolumeIcon.icns')
    bg = write_background(work / 'background.tiff')

    # Volume com folga de 20% + 32 MB: o catálogo e os extents ocupam espaço.
    payload = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file() and not p.is_symlink())
    size = int(payload * 1.2) + 32 * 1024 * 1024
    size -= size % 4096
    image = work / 'volume.hfs'
    with open(image, 'wb') as f:
        f.truncate(size)
    subprocess.run(['mkfs.hfsplus', '-v', VOLUME_NAME, str(image)], check=True, stdout=subprocess.DEVNULL)

    created = _MAC_EPOCH + datetime.timedelta(seconds=_read_u32(image, _CREATE_DATE))
    # O fundo entra PRIMEIRO: o CNID dele é o próximo livre do volume recém
    # criado, e o alias no .DS_Store precisa dele.
    background_cnid = _read_u32(image, _NEXT_CATALOG_ID)
    _hfs(image, 'add', bg, '/.background.tiff')
    assert _read_u32(image, _NEXT_CATALOG_ID) == background_cnid + 1, 'unexpected CNID allocation'

    ds_store = work / 'DS_Store'
    _write_ds_store(ds_store, created, background_cnid, app.name)
    _hfs(image, 'add', ds_store, '/.DS_Store')
    _hfs(image, 'symlink', '/Applications', '/Applications')
    _hfs(image, 'addall', stage, '/')
    for path in _executables(stage):
        _hfs(image, 'chmod', '755', '/' + path.relative_to(stage).as_posix())
    # C = kHasCustomIcon na raiz: o Finder usa o .VolumeIcon.icns no volume.
    _hfs(image, 'attr', '/', 'C')
    # finderInfo[2] = pasta que o Finder abre ao montar (o "bless --openfolder").
    _write_u32(image, _FINDER_INFO + 8, _ROOT_CNID)

    out_dmg.parent.mkdir(parents=True, exist_ok=True)
    out_dmg.unlink(missing_ok=True)
    subprocess.run(['dmg', 'build', str(image), str(out_dmg)], check=True, stdout=subprocess.DEVNULL)
    shutil.rmtree(work)
    return out_dmg


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--app', type=Path)
    ap.add_argument('--work', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--icns', type=Path, help='só grava o ícone do app (.icns)')
    ap.add_argument('--preview', type=Path, help='grava a arte em PNG/TIFF nesta pasta')
    a = ap.parse_args()
    if a.icns:
        write_icns(a.icns)
    elif a.preview:
        write_icns(a.preview / 'clivcut.icns')
        write_background(a.preview / 'background.tiff')
        background(1).save(a.preview / 'background-1x.png')
        background(2).save(a.preview / 'background-2x.png')
        print('ok ->', a.preview)
    elif a.app and a.work and a.out:
        print('ok ->', build_dmg(a.app, a.out, a.work))
    else:
        ap.error('use --app/--work/--out, --icns ou --preview')


if __name__ == '__main__':
    main()
