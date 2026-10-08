#!/usr/bin/env python3
"""Read a bounded PE CodeView identity without loading or executing the image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import uuid

MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_DEBUG_BYTES = 1024 * 1024


class InspectionError(ValueError):
    pass


def inspect_bytes(data):
    def span(offset, size, label):
        if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
            raise InspectionError(label + ' outside file bytes')
        return data[offset:offset + size]

    def word(offset, format, label):
        return struct.unpack(format, span(offset, struct.calcsize(format), label))[0]

    if len(data) > MAX_FILE_BYTES or span(0, 2, 'DOS magic') != b'MZ':
        raise InspectionError('bounded DOS/PE image required')
    pe = word(60, '<I', 'PE offset')
    if pe < 64 or span(pe, 4, 'PE signature') != b'PE\0\0':
        raise InspectionError('invalid PE signature offset')
    coff = pe + 4
    span(coff, 20, 'COFF header')
    machine = word(coff, '<H', 'machine')
    count = word(coff + 2, '<H', 'section count')
    optional_bytes = word(coff + 16, '<H', 'optional header size')
    optional = coff + 20
    span(optional, optional_bytes, 'optional header')
    magic = word(optional, '<H', 'optional magic')
    if magic == 0x20b:
        minimum, directory, image_base = 112, 112, word(optional + 24, '<Q', 'image base')
        image_class = 'PE32+'
    elif magic == 0x10b:
        minimum, directory, image_base = 96, 96, word(optional + 28, '<I', 'image base')
        image_class = 'PE32'
    else:
        raise InspectionError('unsupported optional header magic')
    if optional_bytes < minimum + 7 * 8 or not 1 <= count <= 96:
        raise InspectionError('optional directory or section count invalid')
    directory_count = word(optional + minimum - 4, '<I', 'directory count')
    if directory_count < 7 or directory_count > (optional_bytes - minimum) // 8:
        raise InspectionError('data directory count invalid')
    image_size = word(optional + 56, '<I', 'image size')
    header_size = word(optional + 60, '<I', 'header size')
    table = optional + optional_bytes
    span(table, count * 40, 'section table')
    if not image_size or header_size < table + count * 40 or header_size > len(data) or header_size > image_size:
        raise InspectionError('image/header extent invalid')
    sections = []
    for index in range(count):
        at = table + index * 40
        raw_name = span(at, 8, 'section name').split(b'\0', 1)[0]
        virtual_size, rva, raw_size, raw_offset = struct.unpack('<4I', span(at + 8, 16, 'section extents'))
        extent = max(virtual_size, raw_size)
        if rva < header_size or rva > image_size or extent > image_size - rva:
            raise InspectionError('section virtual extent invalid')
        if raw_size:
            if raw_offset < header_size:
                raise InspectionError('section raw bytes overlap headers')
            span(raw_offset, raw_size, 'section raw bytes')
        for previous in sections:
            if extent and previous['extent'] and rva < previous['rva'] + previous['extent'] and previous['rva'] < rva + extent:
                raise InspectionError('overlapping virtual sections')
            if raw_size and previous['raw_size'] and raw_offset < previous['raw_offset'] + previous['raw_size'] and previous['raw_offset'] < raw_offset + raw_size:
                raise InspectionError('overlapping raw sections')
        sections.append({'name': raw_name.decode('ascii', errors='backslashreplace'), 'rva': rva,
                         'extent': extent, 'raw_size': raw_size, 'raw_offset': raw_offset})

    def file_offset(rva, size, label):
        if rva > image_size or size > image_size - rva:
            raise InspectionError(label + ' outside image')
        if rva < header_size and size <= header_size - rva:
            span(rva, size, label)
            return rva
        for section in sections:
            if rva >= section['rva']:
                delta = rva - section['rva']
                if delta <= section['raw_size'] and size <= section['raw_size'] - delta:
                    offset = section['raw_offset'] + delta
                    span(offset, size, label)
                    return offset
        raise InspectionError(label + ' not wholly backed by one file section')

    debug_rva, debug_bytes = struct.unpack('<2I', span(optional + directory + 6 * 8, 8, 'debug directory'))
    if not debug_rva or not debug_bytes or debug_bytes > MAX_DEBUG_BYTES or debug_bytes % 28:
        raise InspectionError('bounded nonempty debug directory required')
    debug_offset = file_offset(debug_rva, debug_bytes, 'debug directory')
    identities = []
    for index in range(debug_bytes // 28):
        at = debug_offset + index * 28
        kind, size, rva, offset = struct.unpack('<4I', span(at + 12, 16, 'debug entry'))
        if kind != 2:
            continue
        if not 25 <= size <= MAX_DEBUG_BYTES:
            raise InspectionError('CodeView size invalid')
        if rva and file_offset(rva, size, 'CodeView data') != offset:
            raise InspectionError('CodeView RVA/raw offset disagreement')
        value = span(offset, size, 'CodeView data')
        if value[:4] != b'RSDS':
            raise InspectionError('CodeView requires RSDS GUID/age format')
        terminator = value.find(b'\0', 24)
        if terminator < 25:
            raise InspectionError('CodeView PDB name must be bounded and nonempty')
        try:
            name = value[24:terminator].decode('utf-8')
        except UnicodeDecodeError as error:
            raise InspectionError('CodeView PDB name is not UTF-8') from error
        basename = name.replace('\\', '/').rsplit('/', 1)[-1]
        if not basename:
            raise InspectionError('CodeView PDB basename empty')
        guid = str(uuid.UUID(bytes_le=bytes(value[4:20]))).upper()
        age = struct.unpack_from('<I', value, 20)[0]
        if not age:
            raise InspectionError('CodeView age must be nonzero')
        identities.append({'guid': guid, 'age': age, 'symbol_key': guid.replace('-', '') + format(age, 'X'),
                           'pdb_basename': basename, 'debug_entry': index, 'record_bytes': size,
                           'record_sha256': hashlib.sha256(value).hexdigest()})
    if len(identities) != 1:
        raise InspectionError('exactly one RSDS identity required')
    return {'scope': 'offline_PE_CodeView_identity_no_execution_or_symbol_attribution',
            'sha256': hashlib.sha256(data).hexdigest(), 'file_bytes': len(data), 'class': image_class,
            'machine': machine, 'timestamp': word(coff + 4, '<I', 'COFF timestamp'),
            'image_base': image_base, 'image_size': image_size,
            'entry_rva': word(optional + 16, '<I', 'entry RVA'), 'sections': sections,
            'codeview': identities[0]}


def inspect_file(path, expected_sha256=None):
    def signature(value):
        return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 64 <= before.st_size <= MAX_FILE_BYTES:
            raise InspectionError('bounded regular PE file required')
        pathname = path.stat()
        if (pathname.st_dev, pathname.st_ino, pathname.st_size) != (before.st_dev, before.st_ino, before.st_size):
            raise InspectionError('PE path/descriptor identity disagreement')
        data = stream.read(MAX_FILE_BYTES + 1)
        if len(data) != before.st_size or signature(os.fstat(stream.fileno())) != signature(before) or signature(path.stat()) != signature(pathname):
            raise InspectionError('PE input changed during bounded snapshot')
    if expected_sha256 is not None and hashlib.sha256(data).hexdigest() != expected_sha256:
        raise InspectionError('PE SHA-256 does not match required identity')
    return inspect_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--expected-sha256')
    args = parser.parse_args()
    if args.expected_sha256 and not re.fullmatch('[0-9a-f]{64}', args.expected_sha256):
        parser.error('--expected-sha256 requires 64 lowercase hexadecimal digits')
    try:
        result = inspect_file(args.image.resolve(strict=True), args.expected_sha256)
    except (OSError, InspectionError) as error:
        print(json.dumps({'scope': 'offline_PE_CodeView_identity', 'error': str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
