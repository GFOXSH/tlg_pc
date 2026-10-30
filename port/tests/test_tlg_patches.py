from paths import ROOT
import struct
import tempfile
import unittest
from pathlib import Path
import tlg_patches as t


def text_with(names, expected=True):
    """A text segment with each patch's expected bytes (or other bytes) at its offset."""
    patch_ends = [address - t.GHIDRA_BASE + len(original)
                  for patches in t.PATCHES.values()
                  for address, original, _new, _what in patches]
    patch_ends.append(t.GARLIC_SIZE - t.GHIDRA_BASE + len(t.GARLIC_SIZE_EXPECTED))
    text = bytearray(max([t.TEXT_SIZE] + patch_ends))
    for name in names:
        for address, original, _, _ in t.PATCHES[name]:
            offset = address - t.GHIDRA_BASE
            text[offset:offset + len(original)] = original if expected else b'\xcc' * len(original)
    offset = t.GARLIC_SIZE - t.GHIDRA_BASE
    text[offset:offset + len(t.GARLIC_SIZE_EXPECTED)] = (t.GARLIC_SIZE_EXPECTED if expected else
                                                        b'\\xcc' * len(t.GARLIC_SIZE_EXPECTED))
    return text


def plain_elf(text, segment_offset=None):
    segment_offset = t.TEXT_FILE_OFFSET if segment_offset is None else segment_offset
    data = bytearray(segment_offset) + text
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 0,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, segment_offset, 0, 0, len(text), len(text), 0x4000)
    return data


def self_dump(text):
    """The same ELF in a SELF: one plaintext segment, stored at another file offset."""
    data = bytearray(0x200) + text
    data[:4] = t.SELF_MAGIC
    struct.pack_into('<H', data, 24, 1)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, len(text), len(text))
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 64,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 128, 1, 5, t.TEXT_FILE_OFFSET, 0, 0, len(text), len(text), 0x4000)
    return data


class FrameRatePatchTests(unittest.TestCase):
    def prepare(self, eboot, mode, resolution='1080p'):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'eboot.bin').write_bytes(eboot)
            applied, lines = t.prepare(tmp, tmp, mode, resolution)
            return applied, lines, Path(tmp, 'patches.bin').read_bytes()

    def test_self_dump_of_103_gets_the_patches(self):
        # #40: a SELF dump of 1.03 has another sha256 than the plain dump, but the same code.
        applied, lines, blob = self.prepare(self_dump(text_with(t.MODES['60'])), '60')
        self.assertEqual(applied, '60')
        self.assertIn('Frame rate: 60 FPS mode, 5 patch writes', lines)
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 6))

    def test_self_dump_of_another_build_stays_at_30(self):
        applied, lines, blob = self.prepare(self_dump(text_with(t.MODES['unlocked'], expected=False)), 'unlocked')
        self.assertEqual(applied, '30')
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 0))

    def test_unreadable_self_stays_at_30(self):
        applied, lines, _ = self.prepare(t.SELF_MAGIC + bytes(60), '60')
        self.assertEqual(applied, '30')
        self.assertTrue(any(line.startswith('Patches: cannot read the SELF') for line in lines))

    def test_plain_elf_with_another_hash_stays_at_30(self):
        # Unchanged: a plain ELF is only patched when it is the checked 1.03 dump.
        applied, lines, _ = self.prepare(plain_elf(text_with(t.MODES['60'])), '60')
        self.assertEqual(applied, '30')
        self.assertTrue(any('is not v1.03' in line for line in lines))

    def test_gpu_heap_applies_even_at_30_and_is_checked(self):
        applied, lines, blob = self.prepare(plain_elf(text_with([])), '30')
        self.assertEqual(applied, '30')
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 1))
        offset, size = struct.unpack_from('<QQ', blob, 24)
        self.assertEqual(offset, t.GARLIC_SIZE - t.GHIDRA_BASE)
        self.assertEqual(blob[40:40 + size],
                         bytes.fromhex('41bf') + struct.pack('<I', (2816 + t.GARLIC_EXTRA_MB) << 20))

    def test_gpu_heap_uses_the_elf_load_segment_file_offset(self):
        # The image-relative address is unchanged, but this PT_LOAD stores its bytes elsewhere.
        eboot = plain_elf(text_with(t.MODES['60']), segment_offset=0x8000)
        patch_offset = t.GARLIC_SIZE - t.GHIDRA_BASE
        file_pos = t.file_offset(eboot, patch_offset, len(t.GARLIC_SIZE_EXPECTED))
        self.assertEqual(file_pos, 0x8000 + patch_offset)
        self.assertEqual(eboot[file_pos:file_pos + len(t.GARLIC_SIZE_EXPECTED)],
                         t.GARLIC_SIZE_EXPECTED)
        (offset, data, _), = t.garlic_writes(eboot)
        self.assertEqual(offset, patch_offset)
        self.assertEqual(data[:2], bytes.fromhex('41bf'))
        self.assertEqual(len(t.mode_writes(eboot, t.MODES['60'])),
                         sum(len(t.PATCHES[name]) for name in t.MODES['60']))

    def test_unmapped_gpu_heap_address_is_rejected(self):
        eboot = bytearray(plain_elf(text_with([])))
        # PT_LOAD p_filesz is too short to contain the Garlic instruction.
        struct.pack_into('<Q', eboot, 64 + 32, t.GARLIC_SIZE - t.GHIDRA_BASE)
        with self.assertRaises(ValueError):
            t.garlic_writes(eboot)

    def test_30_without_a_matching_heap_instruction_stays_unpatched(self):
        applied, lines, blob = self.prepare(plain_elf(text_with([], expected=False)), '30')
        self.assertEqual(applied, '30')
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 0))
        self.assertTrue(any(line.startswith('GPU heap:') for line in lines))

    def test_30_fps_applies_the_cloth_guard(self):
        applied, lines, blob = self.prepare(self_dump(text_with(t.MODES['30'])), '30')
        self.assertEqual(applied, '30')
        self.assertIn('Frame rate: 30 FPS mode, 1 patch writes', lines)
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 2))

    def test_720p_resolution_applies_checked_code_and_table_patches(self):
        names = t.MODES['30'] + t.RESOLUTIONS['720p']
        applied, lines, blob = self.prepare(self_dump(text_with(names)), '30', '720p')
        self.assertEqual(applied, '30')
        self.assertIn('Frame rate: 30 FPS mode, 1 patch writes', lines)
        self.assertIn('Resolution: 720p, 2 patch writes', lines)
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 4))
        offset, _length = struct.unpack_from('<QQ', blob, 24)
        self.assertEqual(offset, t.PATCHES['cloth_null_guard'][0][0] - t.GHIDRA_BASE)

    def test_resolution_patch_requires_the_supported_eboot(self):
        names = t.MODES['30'] + t.RESOLUTIONS['720p']
        applied, lines, blob = self.prepare(plain_elf(text_with(names)), '30', '720p')
        self.assertEqual(applied, '30')
        self.assertIn('Resolution: 1080p, 0 patch writes', lines)
        self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 1))

    def test_30_writes_an_empty_patch_file_for_an_unreadable_eboot(self):
        applied, lines, blob = self.prepare(b'', '30')
        self.assertEqual((applied, struct.unpack_from('<8sQQ', blob)), ('30', (b'G3PATCH2', 0, 0)))


if __name__ == '__main__':
    unittest.main()
