"""Generate source-faithful packet diagnostic units; no production edits."""
import hashlib
from pathlib import Path

PACKET_FUNCTIONS = ("_data_packet_verify", "_data_packet_encode", "_data_packet_decode",
                    "data_packet_verify", "data_packet_encode", "data_packet_decode")
STRING_FUNCTIONS = {"source/cseries/cseries.c": ("strnlen", "csstrncpy", "csstrcpy"),
                    "source/memory/data_encoding.c": ("data_encode_string", "data_decode_string")}


def generate_packets(output: Path, extract):
    functions = {}
    packet_text = Path("source/memory/data_packets.c").read_text(encoding="utf-8")
    bodies = []
    for name in PACKET_FUNCTIONS:
        body = extract(packet_text, name)
        functions[name] = {"source": "source/memory/data_packets.c",
                           "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
        bodies.append(body)
    # Original function bodies are identical in both units. Preprocessor aliases
    # select scalar services, never change packet branches or native layouts.
    units = []
    for label in ("reference", "candidate"):
        prefix = '#include "packet_shim.h"\n'
        aliases = []
        if label == "candidate":
            aliases += [f'#define {name} candidate_packet_{name.removeprefix("data_packet_")}'
                        for name in PACKET_FUNCTIONS if not name.startswith("_")]
            aliases += [f'#define data_{name} candidate_{name}' for name in
                        ("encode_memory", "encode_integer", "decode_memory", "decode_integer", "decode_byte")]
        # Diagnostic error formatting is authored, but the original format and
        # arguments are still evaluated. Buffers are local to each generated unit.
        prefix += '\n'.join(aliases) + '\nstatic char temporary[256];\n'
        prefix += '#define csprintf(...) packet_format(__VA_ARGS__)\n'
        prefix += ('static char *packet_format(char *buffer, const char *format, ...) {\n'
                   ' va_list args; va_start(args, format); vsnprintf(buffer, 256, format, args);\n'
                   ' va_end(args); return buffer;\n}\n')
        wrapper = (f'\nvoid packet_{label}_dispatch_decode(struct data_packet_definition *definition, '
                   'struct data_encoding_state *state, short version, void *destination) {\n'
                   ' _data_packet_decode(definition, state, version, destination, NULL, definition->fields, NULL);\n}\n')
        path = output / f"packet_{label}.c"
        path.write_text(prefix + '\n\n'.join(bodies) + wrapper, encoding="utf-8")
        units.append(path)
    service_parts = ['#include "packet_shim.h"\n#define strnlen packet_strnlen\n']
    for filename, names in STRING_FUNCTIONS.items():
        text = Path(filename).read_text(encoding="utf-8")
        for name in names:
            body = extract(text, name)
            functions[name] = {"source": filename, "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
            service_parts.append(body)
    services = output / "packet_strings.c"
    services.write_text('\n\n'.join(service_parts) + '\n', encoding="utf-8")
    units.append(services)
    return units, {"functions": functions,
                   "generated_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in units},
                   "production_integrated": False,
                   "limits": "authored schemas and assertion/memory/format services; actual packet branches/native count casts retained"}
