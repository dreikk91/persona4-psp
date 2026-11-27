#!/usr/bin/env python
"""The EBOOT's XMB images made from the player's own ISO: ICON0.PNG (144x80) and PIC1.PNG (480x272).

The repository and the release package carry no game artwork. Both images come from the title screen's textures in
DATA.CVM/TITLE/TITLE.BIN (8-bit TMX): PIC1 is the school corridor behind the title, darkened so the XMB's white title
text stays readable; ICON0 is the P4 logo on a darker crop of the same corridor. tools/psp_link.py pbp_icons packs
them from port/icons; without them the EBOOT has no images.

The player's own pictures can replace either one (tools/psp_prepare.py --icon0/--pic1, or the utility's fields):
user_image fits them to the XMB's sizes and repack_pbp swaps them into a built EBOOT.PBP, so the cached build stays.

python tools/psp_icons.py --iso <ISO>              # writes port/icons/ICON0.PNG and PIC1.PNG
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from psp_disc import walk, root, ISO, SECTOR, CVM_BASE  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TITLE = 'DATA.CVM/TITLE/TITLE.BIN'
# 8-bit TMX images in TITLE.BIN, in file order (USA disc): 3 is the 256x256 white P4 logo (the ATLUS mark under it from
# row LOGO_ROWS on), 8 the 512x512 corridor
LOGO, CORRIDOR, LOGO_ROWS = 3, 8, 222


def read_title(iso):
    with open(iso, 'rb') as f:
        lba, size = root(f, 0)
        for path, start, _ in list(walk(f, 0, lba, size)):
            if path.upper() != 'DATA.CVM':
                continue
            base = start * SECTOR + CVM_BASE
            rl, rs = root(f, base)
            for name, at, n in walk(f, base, rl, rs):
                if (path + '/' + name).upper() == TITLE:
                    f.seek(base + at * SECTOR)
                    return f.read(n)
    raise SystemExit(f'{TITLE} not found in {iso}')


def tmx_rgba(d):
    """Every 8-bit TMX image in d as a Pillow RGBA image, with the runtime's palette order and alpha (rwsky.c)."""
    from PIL import Image
    found, o = [], 0
    while (o := d.find(b'TMX0', o)) >= 0:
        s, o = o - 8, o + 4
        if s < 0 or d[s] != 2 or d[s + 0x16] not in (19, 27) or not d[s + 0x10]:
            continue
        w, h = struct.unpack_from('<HH', d, s + 0x12)
        raw = struct.unpack_from('<256I', d, s + 0x40)
        pal = []
        for i in range(256):  # PS2 CSM1 order: the middle two groups of every 32 swapped (func_003fd520)
            c = raw[i + 8 if i & 24 == 8 else i - 8 if i & 24 == 16 else i]
            a = c >> 24
            pal.append(bytes((c & 255, c >> 8 & 255, c >> 16 & 255, 255 if a >= 0x80 else a * 2)))
        px = s + 0x40 + 1024 * d[s + 0x10]
        found.append(Image.frombytes('RGBA', (w, h), b''.join(pal[i] for i in d[px:px + w * h])))
    return found


def fit(img, size, centre_y=0.5):
    """img scaled to cover size, cropped around centre_y."""
    from PIL import Image
    w, h = size
    scale = max(w / img.width, h / img.height)
    big = img.resize((round(img.width * scale), round(img.height * scale)), Image.Resampling.LANCZOS)
    top = min(max(0, round(big.height * centre_y - h / 2)), big.height - h)
    left = (big.width - w) // 2
    return big.crop((left, top, left + w, top + h))


def darken(img, factor):
    from PIL import ImageEnhance
    return ImageEnhance.Brightness(img).enhance(factor)


def make(iso, out):
    images = tmx_rgba(read_title(iso))
    if len(images) <= max(LOGO, CORRIDOR) or images[LOGO].size != (256, 256) or images[CORRIDOR].size != (512, 512):
        raise SystemExit(f'{TITLE}: unexpected images {[i.size for i in images]}')
    from PIL import Image, ImageFilter
    # the title shows the 512x512 texture over 640x448: that shape first, then cut to the PSP's 480x272
    corridor = images[CORRIDOR].convert('RGB').resize((640, 448), Image.Resampling.LANCZOS)
    out.mkdir(parents=True, exist_ok=True)
    darken(fit(corridor, (480, 272), 0.42), 0.6).save(out / 'PIC1.PNG', optimize=True)
    icon = darken(fit(corridor, (144, 80), 0.42), 0.4).convert('RGBA')
    logo = images[LOGO].crop((0, 0, 256, LOGO_ROWS))
    logo = logo.crop(logo.getbbox())
    logo.thumbnail((132, 72), Image.Resampling.LANCZOS)
    at = ((144 - logo.width) // 2, (80 - logo.height) // 2)
    shadow = Image.new('RGBA', logo.size, (0, 0, 0, 0))
    shadow.putalpha(logo.getchannel('A').filter(ImageFilter.GaussianBlur(2)))
    icon.alpha_composite(shadow, (at[0] + 1, at[1] + 1))
    icon.alpha_composite(logo, at)
    icon.convert('RGB').save(out / 'ICON0.PNG', optimize=True)


# XMB slots: ICON0 is the 144x80 icon in the game list, PIC1 the 480x272 background behind it (the whole screen)
SIZES = {'ICON0': (144, 80), 'PIC1': (480, 272)}
GUIDE = ("Optional pictures for the PSP menu (XMB). Leave a field empty to use the one made from the game's title screen.\n"
         "• Icon: 144x80 pixels, shown in the game list. A transparent PNG is fine.\n"
         "• Background: 480x272 pixels, fills the screen behind the icon. Keep it dark: the game's title is written "
         "over it in white.\n"
         "PNG, JPG, BMP or WEBP. Another size is scaled to fill the space and the edges are trimmed evenly; "
         "the same shape (a larger 144x80 or 480x272) loses nothing.")


def user_image(path, slot):
    """The player's picture for an XMB slot as PNG bytes: scaled to cover the slot's size and centre-cropped; ICON0
    keeps transparency, PIC1 is opaque. Returns (png bytes, note about scaling or cropping, '' when it fit)."""
    import io
    from PIL import Image, UnidentifiedImageError
    w, h = SIZES[slot]
    try:
        with Image.open(path) as img:
            img.load()
            src = img.copy()
    except (OSError, UnidentifiedImageError) as error:
        raise ValueError(f'{slot}: cannot read {path} as a picture ({error})') from error
    note = ''
    if src.size != (w, h):
        lost = 1 - min(src.width / src.height / (w / h), (w / h) / (src.width / src.height))
        note = f'{src.width}x{src.height} scaled to {w}x{h}' + (f', {lost:.0%} cut at the edges' if lost > 0.005 else '')
    alpha = slot == 'ICON0' and (src.mode in ('RGBA', 'LA', 'PA') or 'transparency' in src.info)
    out = fit(src.convert('RGBA' if alpha else 'RGB'), (w, h))
    buffer = io.BytesIO()
    out.save(buffer, 'PNG', optimize=True)
    return buffer.getvalue(), note


PBP_SLOTS = ('PARAM.SFO', 'ICON0', 'ICON1', 'PIC0', 'PIC1', 'SND0', 'DATA.PSP', 'DATA.PSAR')


def repack_pbp(pbp, images):
    """EBOOT.PBP bytes with the named image sections ({'ICON0': png, 'PIC1': png}) replaced; the rest, the PRX
    included, byte for byte as they were."""
    if pbp[:4] != b'\x00PBP' or len(pbp) < 0x28:
        raise ValueError('not an EBOOT.PBP')
    offsets = list(struct.unpack_from('<8I', pbp, 8)) + [len(pbp)]
    if offsets != sorted(offsets) or offsets[0] != 0x28:
        raise ValueError('EBOOT.PBP sections out of order')
    parts = [images.get(name, pbp[offsets[i]:offsets[i + 1]]) for i, name in enumerate(PBP_SLOTS)]
    head, at = bytearray(pbp[:8]), 0x28
    for part in parts:
        head += struct.pack('<I', at)
        at += len(part)
    return bytes(head) + b''.join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--iso', type=Path, default=ISO)
    ap.add_argument('--out', type=Path, default=ROOT / 'port/icons')
    args = ap.parse_args()
    try:
        make(args.iso, args.out)
    except (OSError, ValueError, struct.error, SystemExit) as error:  # optional: the EBOOT then has no images
        print(f'XMB images not made ({error}); the EBOOT gets none', flush=True)
        return
    print(f'ICON0.PNG and PIC1.PNG from {TITLE} in {args.out}', flush=True)


if __name__ == '__main__':
    main()
