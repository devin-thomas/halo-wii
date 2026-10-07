"""Actual owner declarations and untouched packet catalogs, isolated by C unit.

Only the measurement tables/accessors are authored. Opaque catalog types are
deliberately separate from evidence about typed sender/receiver declarations.
"""
import hashlib
from pathlib import Path
import re


OWNERS = (
    ("client_message_handler", "source/networking/network_client_message_handler.c"),
    ("server_message_handler", "source/networking/network_server_message_handler.c"),
    ("client_manager", "source/networking/network_client_manager.c"),
    ("server_manager", "source/networking/network_server_manager.c"),
    ("key_agreement", "source/bungie_net/common/key_agreement.c"),
    ("players", "source/game/players.h"),
)
PREAMBLE = '#include "native_packet_abi.h"\n#include "packet_shim.h"\n'


def _sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _mask(text):
    """Keep positions/newlines while hiding braces in C comments and literals."""
    token = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return token.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text)


def _block(text, pattern, label):
    masked = _mask(text)
    matches = list(re.finditer(pattern, masked, re.M))
    if len(matches) != 1:
        raise ValueError(f"Expected one {label}, found {len(matches)}")
    start = matches[0].start()
    opening = masked.find('{', matches[0].start(), matches[0].end())
    if opening < 0:
        opening = masked.find('{', matches[0].end())
    depth = 1
    cursor = opening + 1
    while cursor < len(masked) and depth:
        depth += (masked[cursor] == '{') - (masked[cursor] == '}')
        cursor += 1
    if depth:
        raise ValueError("Unclosed declaration: " + label)
    ending = re.match(r'\s*;', masked[cursor:])
    if ending is None:
        raise ValueError("Declaration terminator changed: " + label)
    return text[start:cursor + ending.end()]


def _declaration(text, kind, name):
    return _block(text, rf'^{kind}\s+{re.escape(name)}\s*\{{', kind + ' ' + name)


def _first_enum(text):
    match = re.search(r'(?m)^enum\s*\{', _mask(text))
    if match is None:
        raise ValueError("Missing first anonymous enum")
    return _first_block(text, match.start())


def _first_block(text, start):
    # Anchor the extraction so later anonymous enums are not candidates.
    return _block(text[start:], r'\Aenum\s*\{', 'first anonymous enum')


def _macro(text, name):
    match = re.search(rf'(?m)^#define {re.escape(name)}(?:\(|[ \t]|$)', text)
    if match is None:
        raise ValueError("Missing macro " + name)
    end = text.find('\n', match.start())
    while end >= 0 and text[match.start():end].rstrip().endswith('\\'):
        end = text.find('\n', end + 1)
    return text[match.start():end if end >= 0 else len(text)]


def _members(declaration):
    body = _mask(declaration).split('{', 1)[1].rsplit('}', 1)[0]
    names = []
    for part in body.split(';'):
        part = part.strip()
        if not part:
            continue
        # These supported declarations have one named scalar/aggregate/array per line.
        match = re.fullmatch(r'(?:[A-Za-z_]\w*\s+)+([A-Za-z_]\w*)\s*(?:\[[^\[\]]+\])?', part)
        if match is None:
            raise ValueError("Review unsupported member declaration: " + part)
        names.append(match.group(1))
    if not names:
        raise ValueError("Empty measured declaration")
    return names


def generate_native(output: Path, extract):
    """Return generated C paths and an audit catalog, matching other subset generators."""
    del extract  # This gate extracts declarations/initializers, not function bodies.
    output.mkdir(parents=True, exist_ok=True)
    sources, copied, generated, unsupported, owners = [], [], {}, [], []
    cache = {}

    def read(filename):
        if filename not in cache:
            cache[filename] = Path(filename).read_text(encoding="utf-8")
        return cache[filename]

    def record(filename, label, snippet):
        original = read(filename)
        if original.count(snippet) != 1:
            raise ValueError("Copied snippet is not unique: " + filename + ':' + label)
        start = original.index(snippet)
        row = {"source": filename, "label": label, "sha256": _sha(snippet),
               "start_line": original.count('\n', 0, start) + 1,
               "end_line": original.count('\n', 0, start + len(snippet)) + 1}
        copied.append(row)
        return snippet

    def emit(name, parts, short_wchar=True):
        path = output / name
        path.write_text('\n\n'.join(parts) + '\n', encoding="utf-8", newline='\n')
        sources.append(path)
        generated[name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                           "short_wchar": short_wchar}

    players_source = 'source/game/players.h'
    cseries_source = 'source/cseries/cseries.h'
    math_source = 'source/math/real_math.h'
    key_header = 'source/bungie_net/common/public_key_crypt.h'
    real_match = re.search(r'(?m)^typedef float real;', read(cseries_source))
    if real_match is None:
        raise ValueError("Actual real typedef changed")
    shared = [record(cseries_source, 'real', real_match.group()),
              record(cseries_source, 'time constants', _first_enum(read(cseries_source))),
              record(players_source, 'player constants', _first_enum(read(players_source)))]
    for name in ('real_euler_angles2d', 'real_vector2d'):
        declaration = _declaration(read(math_source), 'union', name)
        typedef = re.search(rf'(?m)^typedef union {name} {name};', read(math_source))
        if typedef is None:
            raise ValueError("Actual real union typedef changed: " + name)
        shared.append(record(math_source, name, declaration))
        shared.append(record(math_source, name + ' typedef', typedef.group()))
    player_declarations = {name: record(players_source, name, _declaration(read(players_source), 'struct', name))
                           for name in ('player_action', 'network_player')}
    shared.extend(player_declarations.values())
    for name in ('player_action_size_assert', 'player_action_desired_facing_yaw_offset_assert',
                 'player_action_desired_facing_pitch_offset_assert'):
        assertion = re.search(rf'(?ms)^typedef char {name}\[.*?\];', read(players_source))
        if assertion is None:
            raise ValueError("Actual nested player layout assertion changed: " + name)
        shared.append(record(players_source, name, assertion.group()))
    record(key_header, 'public_key', _declaration(read(key_header), 'struct', 'public_key'))

    for index, (label, filename) in enumerate(OWNERS):
        original = read(filename)
        parts = [PREAMBLE]
        if label == 'key_agreement':
            parts.append('#include "bungie_net/common/public_key_crypt.h"')
            declarations = {'public_key': _declaration(read(key_header), 'struct', 'public_key')}
        else:
            parts.append('#include "port/linux/include/halo_port_limits.h"')
            parts.extend(shared)
            declarations = dict(player_declarations) if label == 'players' else {}
            if label != 'players':
                parts.append(record(filename, 'owner constants', _first_enum(original)))
        for match in re.finditer(r'(?m)^struct\s+(message_\w+)\s*\{', _mask(original)):
            name = match.group(1)
            declaration = _declaration(original, 'struct', name)
            if name == 'message_server_game_advertise':
                unsupported.append({"owner": filename, "name": name, "declaration_sha256": _sha(declaration),
                                    "reason": "XDK/transport and network_game_map dependencies not qualified",
                                    "start_line": original.count('\n', 0, match.start()) + 1})
                continue
            declarations[name] = record(filename, name, declaration)
            parts.append(declaration)
        layouts = []
        for number, (name, declaration) in enumerate(declarations.items()):
            members = _members(declaration)
            member_rows = ',\n'.join(f' {{ "{member}", offsetof(struct {name}, {member}), sizeof(((struct {name} *)0)->{member}) }}'
                                    for member in members)
            parts.append(f'static const struct native_abi_member members_{number}[] = {{\n{member_rows}\n}};')
            layouts.append(f' {{ "{filename}", "{name}", sizeof(struct {name}), _Alignof(struct {name}), '
                           f'sizeof(members_{number}) / sizeof(members_{number}[0]), members_{number} }}')
        parts.append('static const struct native_abi_layout layouts[] = {\n' + ',\n'.join(layouts) + '\n};')
        parts.append(f'const struct native_abi_layout *native_abi_owner_{index}(size_t *count) {{\n'
                     ' if (count) *count = sizeof(layouts) / sizeof(layouts[0]);\n return layouts;\n}')
        emit('native_' + label + '.c', parts)
        owners.append({"index": index, "owner": filename, "layouts": [
            {"name": name, "members": _members(declaration), "declaration_sha256": _sha(declaration)}
            for name, declaration in declarations.items()]})

    dispatcher = ['#include "native_packet_abi.h"']
    dispatcher.extend(f'const struct native_abi_layout *native_abi_owner_{i}(size_t *);' for i in range(len(OWNERS)))
    dispatcher.append(f'size_t native_abi_owner_count(void) {{ return {len(OWNERS)}; }}')
    dispatcher.append('size_t native_abi_selected_wchar_width(void) { return sizeof(wchar_t); }')
    dispatcher.append('const struct native_abi_layout *native_abi_owner_layouts(size_t owner, size_t *count) {\n'
                      ' switch (owner) {\n' + ''.join(f' case {i}: return native_abi_owner_{i}(count);\n' for i in range(len(OWNERS))) +
                      ' default: if (count) *count = 0; return NULL;\n }\n}')
    emit('native_abi_dispatch.c', dispatcher)
    emit('native_abi_default_widths.c', ['#include "native_packet_abi.h"',
         'size_t native_abi_default_wchar_width(void) { return sizeof(wchar_t); }'], short_wchar=False)

    network_source = 'source/networking/network_messages.c'
    original = read(network_source)
    parts = [PREAMBLE, '#include "port/linux/include/halo_port_limits.h"\n#include "networking/network_messages.h"']
    parts.extend(record(network_source, name, _macro(original, name))
                 for name in ('DATA_PACKET_FIELD', 'DATA_PACKET_FIELD_END', 'NETWORK_GAME_MESSAGE_DEFINITION'))
    catalog_declaration = _declaration(original, 'struct', 'network_game_message_packet_definitions')
    parts.append(record(network_source, 'catalog declaration', catalog_declaration))
    opaque_start = original.index('#define DEFINE_NETWORK_GAME_MESSAGE(')
    opaque_end = original.index('#undef DEFINE_NETWORK_GAME_MESSAGE', opaque_start) + len('#undef DEFINE_NETWORK_GAME_MESSAGE')
    parts.append(record(network_source, 'opaque catalog declarations', original[opaque_start:opaque_end]))
    initializer = _block(original, r'^static struct network_game_message_packet_definitions data_0030aa68\s*=\s*\{', 'network catalog initializer')
    parts.append(record(network_source, 'network catalog initializer', initializer))
    bounds = dict(re.findall(r'struct data_packet_field (\w+)_fields\[(\d+)\];', catalog_declaration))
    names = dict(re.findall(r'NETWORK_GAME_MESSAGE_DEFINITION\(\s*(\w+),\s*"([^"]+)"', initializer))
    entries = re.findall(r'\{\s*(\d+),\s*(\d+),\s*&data_0030aa68\.(\w+)\s*\}', initializer)
    if len(entries) != 35 or len(bounds) != 35 or len(names) != 35:
        raise ValueError("Network catalog shape changed; review actual declarations/order")
    network_rows = []
    for number, (packet_class, flags, member) in enumerate(entries):
        network_rows.append({"type": number, "packet_class": int(packet_class), "flags": int(flags),
                             "member": member, "name": names[member], "field_bound": int(bounds[member])})
    parts.append('static const struct native_abi_catalog catalog[] = {\n' + ',\n'.join(
        f' {{ "{row["name"]}", {row["type"]}, {row["packet_class"]}, &data_0030aa68.{row["member"]}, '
        f'sizeof(data_0030aa68.{row["member"]}_fields) / sizeof(data_0030aa68.{row["member"]}_fields[0]) }}'
        for row in network_rows) + '\n};')
    enum_source = 'source/networking/network_messages.h'
    enum_declaration = _declaration(read(enum_source), 'enum', 'network_game_message_type')
    record(enum_source, 'network type enum included from actual header', enum_declaration)
    enum_names = re.findall(r'\b(_message_\w+)\b', _mask(enum_declaration))
    if len(enum_names) != len(network_rows):
        raise ValueError("Network enum shape changed")
    parts.append('static const struct native_abi_enum enum_values[] = {\n' + ',\n'.join(
        f' {{ "{name}", {name} }}' for name in enum_names) + '\n};')
    parts.append('const struct native_abi_enum *native_abi_network_enum(size_t *count) {\n'
                 ' if (count) *count = sizeof(enum_values) / sizeof(enum_values[0]);\n return enum_values;\n}')
    parts.append('const struct native_abi_catalog *native_abi_network_catalog(size_t *count) {\n'
                 ' if (count) *count = sizeof(catalog) / sizeof(catalog[0]);\n return catalog;\n}')
    parts.append('const struct data_packet_group_definition *native_abi_network_group(void) { return &data_0030aa68.group; }')
    emit('native_network_catalog.c', parts)

    key_source = OWNERS[4][1]
    original = read(key_source)
    parts = [PREAMBLE, '#include "bungie_net/common/public_key_crypt.h"',
             record(key_source, 'key constants', _first_enum(original)),
             record(key_source, 'key packet enum', _declaration(original, 'enum', 'key_agreement_packet_type'))]
    parts.extend(record(key_source, name, _macro(original, name)) for name in ('DATA_PACKET_FIELD', 'DATA_PACKET_FIELD_END'))
    for name in ('message_initiate_key_agreement', 'message_finalize_key_agreement'):
        parts.append(_declaration(original, 'struct', name))
    key_statics = ('message_initiate_key_agreement_packet_fields', 'message_initiate_key_agreement_packet',
                   'message_finalize_key_agreement_packet_fields', 'message_finalize_key_agreement_packet',
                   'key_agreement_packets_group_packets', 'key_agreement_packets_group')
    snippets = {}
    for name in key_statics:
        snippet = _block(original, rf'^static struct \w+ {name}(?:\[[^\]]+\])?\s*=\s*\{{', name)
        snippets[name] = snippet
        parts.append(record(key_source, name, snippet))
    key_entries = re.findall(r'\{\s*(\d+),\s*(\d+),\s*&(\w+)\s*\}', snippets['key_agreement_packets_group_packets'])
    if len(key_entries) != 2:
        raise ValueError("Key catalog shape changed")
    key_rows = []
    for number, (packet_class, flags, member) in enumerate(key_entries):
        name = re.search(r'"([^"]+)"', snippets[member]).group(1)
        field_bound = int(re.search(r'\[(\d+)\]', snippets[member + '_fields']).group(1))
        key_rows.append({"type": number, "packet_class": int(packet_class), "flags": int(flags),
                         "member": member, "name": name, "field_bound": field_bound})
    parts.append('static const struct native_abi_catalog catalog[] = {\n' + ',\n'.join(
        f' {{ "{row["name"]}", {row["type"]}, {row["packet_class"]}, &{row["member"]}, '
        f'sizeof({row["member"]}_fields) / sizeof({row["member"]}_fields[0]) }}' for row in key_rows) + '\n};')
    parts.append('const struct native_abi_catalog *native_abi_key_catalog(size_t *count) {\n'
                 ' if (count) *count = sizeof(catalog) / sizeof(catalog[0]);\n return catalog;\n}')
    parts.append('const struct data_packet_group_definition *native_abi_key_group(void) { return &key_agreement_packets_group; }')
    emit('native_key_catalog.c', parts)
    # Include headers are source evidence too, including capacity formulas.
    for filename in (key_header, 'port/linux/include/halo_port_limits.h', 'port/linux/include/halo_port_capacity.h'):
        read(filename)
    return sources, {"owners": owners, "unsupported": unsupported,
                     "network_catalog": network_rows, "key_catalog": key_rows,
                     "network_enum": [{"name": name, "ordinal": i} for i, name in enumerate(enum_names)],
                     "copied": copied, "generated": generated,
                     "sources": {filename: {"source_sha256": hashlib.sha256(Path(filename).read_bytes()).hexdigest(),
                                            "normalized_sha256": _sha(text)} for filename, text in cache.items()},
                     "copied_hash_normalization": "UTF-8 source read with universal newlines, snippets unchanged",
                     "default_width_unit": "native_abi_default_widths.c",
                     "selected_width_flag": "-fshort-wchar for new selected layout/catalog units only",
                     "opaque_catalog_is_typed_owner_proof": False, "production_integrated": False}
