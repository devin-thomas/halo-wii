"""Generate source-faithful packet diagnostic units; no production edits."""
import hashlib
from pathlib import Path

PACKET_FUNCTIONS = ("_data_packet_verify", "_data_packet_encode", "_data_packet_decode",
                    "data_packet_verify", "data_packet_encode", "data_packet_decode")
STRING_FUNCTIONS = {"source/cseries/cseries.c": ("strnlen", "csstrncpy", "csstrcpy"),
                    "source/memory/data_encoding.c": ("data_encode_string", "data_decode_string")}


def generate_packets(output: Path, extract, policy=False):
    functions = {}
    packet_text = Path("source/memory/data_packets.c").read_text(encoding="utf-8")
    bodies = []
    for name in PACKET_FUNCTIONS:
        body = extract(packet_text, name)
        functions[name] = {"source": "source/memory/data_packets.c",
                           "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
        bodies.append(body)
    # The two preserved units differ only by scalar-service aliases. A third
    # opt-in policy unit below records its single excluded-decode transformation.
    units = []
    policy_record = None
    for label in (("reference", "candidate", "policy") if policy else ("reference", "candidate")):
        prefix = '#include "packet_shim.h"\n'
        aliases = []
        if label != "reference":
            aliases += [f'#define {name} {label}_packet_{name.removeprefix("data_packet_")}'
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
        selected_bodies = list(bodies)
        if label == "policy":
            # Only the third diagnostic's excluded-decode arm is changed.
            # The reference/candidate and encoder bodies remain byte-identical.
            index = PACKET_FUNCTIONS.index("_data_packet_decode")
            original = selected_bodies[index]
            old = '\t\telse\n\t\t\tcsmemset(decoded_data, 0, field->size);'
            new = ('\t\telse\n\t\t{\n'
                   '\t\t\tif (!packet_policy_excluded(state, field, decoded_data))\n'
                   '\t\t\t\tbreak;\n\t\t}')
            if original.count(old) != 1:
                raise ValueError("Actual excluded-decode arm changed; review policy transformation")
            selected_bodies[index] = original.replace(old, new)
            policy_record = {"scope": "diagnostic_excluded_decode_only",
                             "original_body_sha256": hashlib.sha256(original.encode()).hexdigest(),
                             "transformed_body_sha256": hashlib.sha256(selected_bodies[index].encode()).hexdigest(),
                             "replacement_count": 1, "nonzero_placeholders": "accepted_without_interpreting_contents",
                             "excluded_arrays": "unsupported_overflow_and_stop_not_qualified",
                             "production_integrated": False}
        path.write_text(prefix + '\n\n'.join(selected_bodies) + wrapper, encoding="utf-8")
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
    if policy:
        wrapper = output / "packet_policy_fixture.c"
        wrapper.write_text('#include "packet_shim.h"\n'
            '#define wii_packet_compare packet_policy_unused_compare\n'
            '#include "packet_fixture.c"\n'
            'int wii_packet_policy_subset(FILE *report, int collect) {\n'
            ' const struct packet_ops ops = {"POLICY", policy_packet_verify, policy_packet_encode,\n'
            '  policy_packet_decode, packet_policy_dispatch_decode};\n'
            ' return packet_section(report, collect, &ops);\n}\n', encoding="utf-8")
        units.append(wrapper)
    record = {"functions": functions,
                   "generated_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in units},
                   "production_integrated": False,
                   "limits": "authored schemas and assertion/memory/format services; actual packet branches/native count casts retained"}
    if policy:
        record["policy"] = policy_record
    return units, record
