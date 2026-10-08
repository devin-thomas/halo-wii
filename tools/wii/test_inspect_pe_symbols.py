"""Authored PE bytes only; no emulator binaries or PDB dependencies."""
import hashlib
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import uuid

from inspect_pe_symbols import InspectionError, inspect_bytes, inspect_file

GUID = '10203040-5060-7080-90A0-B0C0D0E0F001'


def authored_image(pe64=True, age=1, path=b'C:\\authored\\fixture.pdb'):
    data = bytearray(1536)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    coff, optional = 132, 152
    optional_size = 240 if pe64 else 224
    struct.pack_into('<HH', data, coff, 0x8664 if pe64 else 0x14c, 1)
    struct.pack_into('<H', data, coff + 16, optional_size)
    struct.pack_into('<H', data, optional, 0x20b if pe64 else 0x10b)
    struct.pack_into('<I', data, optional + 16, 0x1000)
    struct.pack_into('<Q' if pe64 else '<I', data, optional + (24 if pe64 else 28),
                     0x140000000 if pe64 else 0x400000)
    struct.pack_into('<II', data, optional + 56, 0x3000, 512)
    directory = 112 if pe64 else 96
    struct.pack_into('<I', data, optional + directory - 4, 16)
    struct.pack_into('<II', data, optional + directory + 6 * 8, 0x1000, 28)
    section = optional + optional_size
    data[section:section + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, section + 8, 1024, 0x1000, 1024, 512)
    record = b'RSDS' + uuid.UUID(GUID).bytes_le + struct.pack('<I', age) + path + b'\0'
    struct.pack_into('<IIII', data, 512 + 12, 2, len(record), 0x1100, 768)
    data[768:768 + len(record)] = record
    return data


class PEBounds(unittest.TestCase):
    def reject(self, data, message):
        with self.assertRaisesRegex(InspectionError, message):
            inspect_bytes(data)

    def test_pe32_and_pe64_guid_endianness_and_privacy(self):
        for pe64 in (False, True):
            data = authored_image(pe64)
            before = bytes(data)
            result = inspect_bytes(data)
            self.assertEqual(result['codeview']['guid'], GUID)
            self.assertEqual(result['codeview']['age'], 1)
            self.assertEqual(result['codeview']['symbol_key'], GUID.replace('-', '') + '1')
            self.assertEqual(result['codeview']['pdb_basename'], 'fixture.pdb')
            self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(result['class'], 'PE32+' if pe64 else 'PE32')
            self.assertEqual(bytes(data), before)
            self.assertNotIn('authored', str(result))

    def test_symbol_age_hex_and_forward_slash(self):
        result = inspect_bytes(authored_image(age=16, path=b'/authored/fixture.pdb'))
        self.assertEqual(result['codeview']['symbol_key'], GUID.replace('-', '') + '10')

    def test_truncations(self):
        data = authored_image()
        for size in (0, 1, 61, 63, 130, 151, 200, 512, 1000):
            with self.subTest(size=size):
                self.reject(data[:size], 'outside|invalid|bounded')

    def test_dos_and_pe_signatures(self):
        for offset in (0, 128):
            data = authored_image()
            data[offset] = 0
            self.reject(data, 'DOS/PE|signature')

    def test_pe_offset_wrap_or_inside_dos(self):
        for value in (0, 63, 0xffffffff):
            data = authored_image()
            struct.pack_into('<I', data, 60, value)
            self.reject(data, 'signature')

    def test_optional_magic_and_counts(self):
        changes = ((152, '<H', 0x999, 'magic'),
                   (132 + 2, '<H', 0, 'section count'),
                   (132 + 2, '<H', 97, 'section count'),
                   (132 + 16, '<H', 112, 'directory'),
                   (152 + 108, '<I', 6, 'directory count'),
                   (152 + 108, '<I', 17, 'directory count'))
        for offset, format, value, message in changes:
            data = authored_image()
            struct.pack_into(format, data, offset, value)
            self.reject(data, message)

    def test_header_and_virtual_section_extents(self):
        changes = ((152 + 56, 0), (152 + 60, 400), (152 + 60, 2000),
                   (392 + 12, 0), (392 + 12, 0x2fff), (392 + 8, 0xffffffff))
        for offset, value in changes:
            data = authored_image()
            struct.pack_into('<I', data, offset, value)
            self.reject(data, 'extent')

    def test_raw_section_extent_or_header_overlap(self):
        for offset in (0, 511, 513, 0xffffffff):
            data = authored_image()
            struct.pack_into('<I', data, 392 + 20, offset)
            self.reject(data, 'overlap|outside')

    def test_multiple_sections_overlap(self):
        for raw_offset, rva, message in ((512, 0x2000, 'raw sections'), (1024, 0x1000, 'virtual sections')):
            data = authored_image()
            struct.pack_into('<H', data, 134, 2)
            data[432:440] = b'.second\0'
            struct.pack_into('<4I', data, 440, 512, rva, 512, raw_offset)
            self.reject(data, message)

    def test_virtual_tail_not_file_data(self):
        data = authored_image()
        struct.pack_into('<I', data, 392 + 16, 512)
        struct.pack_into('<II', data, 152 + 112 + 6 * 8, 0x1200, 28)
        self.reject(data, 'not wholly backed')

    def test_debug_extent_rules(self):
        for rva, size in ((0, 28), (0x1000, 0), (0x1000, 27), (0x1000, 1048600),
                          (0x13ff, 28), (0xffffffff, 28)):
            data = authored_image()
            struct.pack_into('<II', data, 152 + 112 + 6 * 8, rva, size)
            self.reject(data, 'directory|outside|backed')

    def test_codeview_record_bounds_and_mapping(self):
        for size, rva, offset in ((24, 0x1100, 768), (1048577, 0x1100, 768),
                                  (50, 0x1101, 768), (50, 0, 0xffffffff)):
            data = authored_image()
            struct.pack_into('<III', data, 512 + 16, size, rva, offset)
            self.reject(data, 'size|disagreement|outside')

    def test_codeview_format_name_and_age(self):
        data = authored_image()
        data[768:772] = b'NB10'
        self.reject(data, 'RSDS')
        self.reject(authored_image(age=0), 'age')
        self.reject(authored_image(path=b''), 'name')
        self.reject(authored_image(path=b'/authored/'), 'basename')
        self.reject(authored_image(path=b'\xff'), 'UTF-8')
        data = authored_image()
        size = struct.unpack_from('<I', data, 528)[0]
        data[768 + size - 1] = 65
        self.reject(data, 'bounded')

    def test_missing_or_multiple_rsds_rejected(self):
        data = authored_image()
        struct.pack_into('<I', data, 524, 0)
        self.reject(data, 'exactly one')
        data = authored_image()
        struct.pack_into('<I', data, 152 + 112 + 6 * 8 + 4, 56)
        data[540:568] = data[512:540]
        self.reject(data, 'exactly one')

    def test_file_sha_required_and_no_source_mutation(self):
        data = bytes(authored_image())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'authored.exe'
            path.write_bytes(data)
            self.assertEqual(inspect_file(path, hashlib.sha256(data).hexdigest())['codeview']['guid'], GUID)
            with self.assertRaisesRegex(InspectionError, 'SHA-256'):
                inspect_file(path, '0' * 64)
            self.assertEqual(path.read_bytes(), data)

    def test_descriptor_and_path_timestamp_apis_can_differ(self):
        data = bytes(authored_image())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'authored.exe'
            path.write_bytes(data)
            actual = path.stat()
            descriptor = SimpleNamespace(st_mode=actual.st_mode, st_dev=actual.st_dev,
                st_ino=actual.st_ino, st_size=actual.st_size,
                st_mtime_ns=actual.st_mtime_ns, st_ctime_ns=actual.st_ctime_ns + 1000)
            with mock.patch('inspect_pe_symbols.os.fstat', return_value=descriptor):
                self.assertEqual(inspect_file(path)['codeview']['guid'], GUID)

    def test_changed_descriptor_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'authored.exe'
            path.write_bytes(authored_image())
            actual = path.stat()
            before = SimpleNamespace(st_mode=actual.st_mode, st_dev=actual.st_dev,
                st_ino=actual.st_ino, st_size=actual.st_size,
                st_mtime_ns=actual.st_mtime_ns, st_ctime_ns=actual.st_ctime_ns)
            after = SimpleNamespace(**vars(before))
            after.st_mtime_ns += 1
            with mock.patch('inspect_pe_symbols.os.fstat', side_effect=[before, after]):
                with self.assertRaisesRegex(InspectionError, 'changed'):
                    inspect_file(path)


if __name__ == '__main__':
    unittest.main()
