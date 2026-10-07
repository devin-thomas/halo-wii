"""Read-only, bounded XDVDFS inventory and Xbox cache-header identity.

The on-disc layout follows port/linux/src/xiso.c. This tool traverses every
reachable directory entry and rejects malformed/unsupported inputs instead of
silently skipping them. It never extracts file contents. Header recognition is
separate from decompression, tag validation, asset conversion and gameplay.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

SECTOR_SIZE = 2048
DESCRIPTOR_OFFSET = 0x10000
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MAX_DIRECTORY_BYTES = 4 << 20
MAX_ENTRIES = 100000
MAX_DIRECTORY_DEPTH = 128
MAX_PATH_BYTES = 4096
MAX_TOTAL_PATH_BYTES = 16 << 20
MAX_TOTAL_DIRECTORY_BYTES = 64 << 20
BUILD_REGIONS = {'01.01.14.2342': 'PAL', '01.10.12.2276': 'NTSC', '01.08.15.1749': 'NTSC'}


class InventoryError(ValueError):
    """The reachable filesystem cannot be completely inventoried safely."""


class Reader:
    def __init__(self, stream, size):
        if size < 0:
            raise InventoryError('Negative image size')
        self.stream = stream
        self.size = size

    def span(self, offset, size, context):
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise InventoryError(f'{context}: span {offset}+{size} exceeds image size {self.size}')

    def read(self, offset, size, context):
        self.span(offset, size, context)
        self.stream.seek(offset)
        data = self.stream.read(size)
        if len(data) != size:
            raise InventoryError(f'{context}: short read at {offset}, expected {size}, got {len(data)}')
        return data


def _nonoverlap(spans, context):
    end = -1
    previous = ''
    for start, stop, name in sorted(spans):
        if start < end:
            raise InventoryError(f'{context}: overlapping {previous!r} and {name!r}')
        end, previous = stop, name


def _directory(table, path):
    if not table or all(value == 255 for value in table):
        return []
    stack, visited, spans, rows = [0], set(), [], []
    names = set()
    while stack:
        offset = stack.pop()
        if offset in visited:
            raise InventoryError(f'{path or "/"}: repeated directory node or tree cycle at {offset}')
        visited.add(offset)
        if len(visited) > MAX_ENTRIES:
            raise InventoryError(f'{path or "/"}: directory node limit exceeded')
        if offset + 14 > len(table):
            raise InventoryError(f'{path or "/"}: truncated directory entry at {offset}')
        left, right, sector, size, attributes, length = struct.unpack_from('<HHIIBB', table, offset)
        if left == 65535:
            raise InventoryError(f'{path or "/"}: padding sentinel reached in a nonempty tree')
        stop = (offset + 14 + length + 3) & ~3
        if not length or stop > len(table):
            raise InventoryError(f'{path or "/"}: invalid or truncated entry name at {offset}')
        raw = table[offset + 14:offset + 14 + length]
        if any(value < 32 or value > 126 for value in raw):
            raise InventoryError(f'{path or "/"}: entry name encoding/control bytes unsupported')
        name = raw.decode('ascii')
        if name in ('.', '..') or any(value in name for value in '/\\:'):
            raise InventoryError(f'{path or "/"}: unsafe single-component entry name {name!r}')
        key = name.casefold()
        if key in names:
            raise InventoryError(f'{path or "/"}: duplicate case-insensitive entry {name!r}')
        names.add(key)
        spans.append((offset, stop, name))
        rows.append((name, sector, size, bool(attributes & 0x10)))
        # Zero is the absent-child sentinel; root is the only implicit node0.
        if right:
            stack.append(right * 4)
        if left:
            stack.append(left * 4)
    _nonoverlap(spans, path or '/')
    return rows


def _map_header(reader, row):
    result = {'path': row['path'], 'stored_file_size': row['size'], 'issues': [],
              'header_identity_supported': False, 'body_validation': 'not_performed'}
    if row['size'] < 0x800:
        result['issues'].append('File is shorter than the 2048-byte cache header')
        return result
    data = reader.read(row['offset'], 0x800, row['path'] + ' header')
    result['header_sha256'] = hashlib.sha256(data).hexdigest()
    version, length, reserved, tag_offset, tag_size = struct.unpack_from('<iiIii', data, 4)
    result.update(version=version, declared_file_length=length, tag_data_offset=tag_offset,
                  tag_data_size=tag_size, reserved_0x0c=reserved,
                  scenario_type_observed=struct.unpack_from('<h', data, 0x60)[0],
                  checksum_observed=struct.unpack_from('<I', data, 0x64)[0],
                  stored_length_matches_declared=row['size'] == length,
                  compression_validation='not_performed', header_strings_encoding='latin1_lossless_presentation')
    if data[:4] != b'daeh' or data[0x7fc:] != b'toof':
        result['issues'].append('Header/footer signature differs from little-endian Xbox head/foot values')
    if version != 5:
        result['issues'].append(f'Cache version {version} is not Xbox version5')
    if not 0x800 <= length <= 0x11600000:
        result['issues'].append('Declared length is outside the Xbox header limits')
    if tag_offset < 0 or not 0 <= tag_size <= 0x01600000 or tag_offset > length - tag_size:
        result['issues'].append('Tag extent is outside the declared decompressed length or tag-cache limit')
    for key, start in [('name', 0x20), ('build', 0x40)]:
        field = data[start:start + 32]
        if b'\0' not in field:
            result['issues'].append(f'{key} is not terminated within32 bytes')
        else:
            result[key] = field.split(b'\0', 1)[0].decode('latin1')
    result['build_region_observed'] = BUILD_REGIONS.get(result.get('build'), 'unknown')
    result['header_identity_supported'] = not result['issues']
    return result


def inspect(stream, size):
    """Inventory the complete reachable tree without writing or loading assets."""
    reader = Reader(stream, size)
    volumes = []
    for partition in PARTITION_OFFSETS:
        offset = partition + DESCRIPTOR_OFFSET
        if offset + SECTOR_SIZE > size:
            continue
        data = reader.read(offset, SECTOR_SIZE, 'volume descriptor candidate')
        if data[:20] == MAGIC and data[0x7ec:] == MAGIC:
            sector, length = struct.unpack_from('<II', data, 20)
            volumes.append({'partition_offset': partition, 'descriptor_offset': offset,
                            'root_sector': sector, 'root_size': length,
                            'descriptor_sha256': hashlib.sha256(data).hexdigest()})
    if len(volumes) != 1:
        raise InventoryError(f'Expected one complete XDVDFS volume descriptor; found {len(volumes)}')
    volume = volumes[0]
    partition = volume['partition_offset']
    pending = [('', volume['root_sector'], volume['root_size'], 0)]
    visited_directories = set()
    spans = [(volume['descriptor_offset'], volume['descriptor_offset'] + SECTOR_SIZE, 'volume descriptor')]
    entries = []
    directory_bytes = 0
    path_bytes = 0
    while pending:
        path, sector, length, depth = pending.pop()
        if depth > MAX_DIRECTORY_DEPTH:
            raise InventoryError(f'{path}: directory depth limit exceeded')
        if length > MAX_DIRECTORY_BYTES:
            raise InventoryError(f'{path or "/"}: directory table exceeds byte limit')
        directory_bytes += length
        if directory_bytes > MAX_TOTAL_DIRECTORY_BYTES:
            raise InventoryError('Total directory-table byte limit exceeded')
        offset = partition + sector * SECTOR_SIZE
        reader.span(offset, length, path or '/ root directory')
        if length:
            if offset in visited_directories:
                raise InventoryError(f'{path or "/"}: directory cycle or reused table')
            visited_directories.add(offset)
            spans.append((offset, offset + length, path or '/ root directory'))
        table = reader.read(offset, length, path or '/ root directory')
        for name, child_sector, child_size, directory in _directory(table, path):
            relative = path + '/' + name if path else name
            if len(relative) > MAX_PATH_BYTES:
                raise InventoryError(f'{relative[:80]}: path byte limit exceeded')
            path_bytes += len(relative)
            if path_bytes > MAX_TOTAL_PATH_BYTES:
                raise InventoryError('Total path byte limit exceeded')
            if len(entries) >= MAX_ENTRIES:
                raise InventoryError('Total entry count limit exceeded')
            child_offset = partition + child_sector * SECTOR_SIZE
            reader.span(child_offset, child_size, relative)
            row = {'path': relative, 'kind': 'directory' if directory else 'file',
                   'sector': child_sector, 'size': child_size, 'offset': child_offset}
            entries.append(row)
            if directory:
                pending.append((relative, child_sector, child_size, depth + 1))
            elif child_size:
                spans.append((child_offset, child_offset + child_size, relative))
    _nonoverlap(spans, 'filesystem allocation')
    entries.sort(key=lambda row: (row['path'].casefold(), row['path']))
    files = [row for row in entries if row['kind'] == 'file']
    maps = [_map_header(reader, row) for row in files if row['path'].lower().endswith('.map')]
    movies = [row for row in files if row['path'].lower().endswith(('.bik', '.wmv', '.xmv'))]
    return {'schema_version': 1, 'scope': 'read_only_reachable_xdvdfs_and_xbox_cache_headers',
            'filesystem_inventory': 'pass', 'volume': volume, 'entries': entries, 'maps': maps,
            'counts': {'files': len(files), 'directories': len(entries) - len(files),
                       'map_files': len(maps), 'movies': len(movies),
                       'file_bytes': sum(row['size'] for row in files)},
            'map_header_identity': 'pass' if maps and all(row['header_identity_supported'] for row in maps) else 'blocked',
            'ui_map_present': any(row['path'].casefold() == 'maps/ui.map' for row in files),
            'asset_bytes_extracted': 0, 'production_integrated': False,
            'limits': ['reachable_entries_only_not_disc_provenance_or_unallocated_sectors',
                       'printable_ascii_single_component_xdvdfs_names_only',
                       'allocation_aliases_rejected_no_silent_entry_skips',
                       'source_image_must_be_immutable_during_read',
                       'headers_only_no_body_inflate_checksum_tag_geometry_script_endian_or_gameplay_acceptance']}


def inventory(image):
    image = Path(image)
    with image.open('rb') as stream:
        before = os.fstat(stream.fileno())
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(4 << 20), b''):
            digest.update(chunk)
        result = inspect(stream, before.st_size)
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise InventoryError('Image size or modification time changed during inspection')
    result.update(image_name=image.name, image_size=before.st_size, image_sha256=digest.hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, help='Create a new JSON report; existing paths are refused')
    args = parser.parse_args()
    try:
        result = inventory(args.image)
        text = json.dumps(result, indent=2, ensure_ascii=True) + '\n'
        if args.output:
            with args.output.open('x', encoding='utf-8', newline='\n') as report:
                report.write(text)
        else:
            sys.stdout.write(text)
    except (InventoryError, OSError) as error:
        print(f'XISO inventory failed: {error}', file=sys.stderr)
        return 1
    return 0 if result['map_header_identity'] == 'pass' else 2


if __name__ == '__main__':
    raise SystemExit(main())
