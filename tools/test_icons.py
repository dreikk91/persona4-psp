import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import psp_icons  # noqa: E402


def pbp(sections):
    """A PBP with the eight sections given in order."""
    head, at = bytearray(b'\x00PBP\x00\x00\x01\x00'), 0x28
    for part in sections:
        head += struct.pack('<I', at)
        at += len(part)
    return bytes(head) + b''.join(sections)


class XmbPictures(unittest.TestCase):
    def setUp(self):
        from PIL import Image
        self.Image = Image
        self.folder = Path(tempfile.mkdtemp())

    def picture(self, name, size, mode='RGB'):
        path = self.folder / name
        self.Image.new(mode, size, (200, 30, 30, 128) if mode == 'RGBA' else (200, 30, 30)).save(path)
        return path

    def test_exact_size_is_kept_without_a_note(self):
        png, note = psp_icons.user_image(self.picture('i.png', (144, 80)), 'ICON0')
        self.assertEqual(note, '')
        self.assertEqual(self.Image.open(io.BytesIO(png)).size, (144, 80))

    def test_other_shapes_fill_the_slot_and_say_what_was_cut(self):
        png, note = psp_icons.user_image(self.picture('b.jpg', (1000, 1000)), 'PIC1')
        out = self.Image.open(io.BytesIO(png))
        self.assertEqual((out.size, out.mode), ((480, 272), 'RGB'))
        self.assertIn('cut at the edges', note)
        png, note = psp_icons.user_image(self.picture('w.png', (960, 544)), 'PIC1')  # same shape, twice the size
        self.assertNotIn('cut', note)

    def test_icon_keeps_transparency_background_does_not(self):
        icon, _ = psp_icons.user_image(self.picture('t.png', (288, 160), 'RGBA'), 'ICON0')
        self.assertEqual(self.Image.open(io.BytesIO(icon)).mode, 'RGBA')
        back, _ = psp_icons.user_image(self.picture('t2.png', (480, 272), 'RGBA'), 'PIC1')
        self.assertEqual(self.Image.open(io.BytesIO(back)).mode, 'RGB')

    def test_not_a_picture_is_refused(self):
        bad = self.folder / 'x.png'
        bad.write_bytes(b'not a picture')
        with self.assertRaises(ValueError):
            psp_icons.user_image(bad, 'ICON0')

    def test_repack_swaps_only_the_named_images(self):
        sections = [b'SFO', b'old icon', b'', b'', b'old background', b'', b'PRX' * 100, b'']
        out = psp_icons.repack_pbp(pbp(sections), {'ICON0': b'new icon!', 'PIC1': b'bg'})
        offsets = list(struct.unpack_from('<8I', out, 8)) + [len(out)]
        got = [out[offsets[i]:offsets[i + 1]] for i in range(8)]
        self.assertEqual(got, [b'SFO', b'new icon!', b'', b'', b'bg', b'', b'PRX' * 100, b''])
        self.assertEqual(psp_icons.repack_pbp(pbp(sections), {}), pbp(sections))
        with self.assertRaises(ValueError):
            psp_icons.repack_pbp(b'ELF' + bytes(64), {'ICON0': b'x'})


if __name__ == '__main__':
    unittest.main()
