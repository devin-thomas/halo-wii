"""Source-faithful header functions and explicit caller union-operation excerpts."""
import hashlib
from pathlib import Path
import re

HEADER_FUNCTIONS = ("build_message_header", "byte_swap_message_header", "create_message")


def generate_callers(output: Path, extract):
    source = Path("source/bungie_net/common/message_header.c")
    text = source.read_text(encoding="utf-8")
    limits = Path("port/linux/include/halo_port_limits.h").read_text(encoding="utf-8")
    network_capacity = re.search(r"(?m)^#define HALO_PORT_NETWORK_PACKET_SIZE[ \t]+(0x[0-9A-Fa-f]+|[0-9]+)$", limits)
    if network_capacity is None:
        raise ValueError("Network encoded capacity declaration changed")
    parts = ['#include "caller_reference.h"\n', network_capacity.group(),
             '#define debug_malloc caller_reference_allocate\n']
    functions = {}
    for name in HEADER_FUNCTIONS:
        body = extract(text, name)
        functions[name] = {"source": source.as_posix(),
                           "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
        parts.append(body)
    excerpts = {}
    for label, filename, union, buffer, capacity in (
        ("network", "source/networking/network_messages.c", "network_game_message_size",
         "encoded_message", "HALO_PORT_NETWORK_PACKET_SIZE"),
        ("key", "source/bungie_net/common/key_agreement.c", "key_agreement_packet_value",
         "encoded_packet", "KEY_AGREEMENT_ENCODED_PACKET_SIZE")):
        original = Path(filename).read_text(encoding="utf-8")
        declaration = re.search(rf"(?ms)^union {union}\s*\{{[^{{}}]*\}};", original)
        assignment = f"encoded_{'message' if label == 'network' else 'packet'}_size.value = sizeof({buffer});"
        if declaration is None or original.count(assignment) != 1:
            raise ValueError("Caller union declaration/initialization changed; review extraction: " + filename)
        if label == "key":
            constant = re.search(r"\bKEY_AGREEMENT_ENCODED_PACKET_SIZE\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)\s*,", original)
            if constant is None:
                raise ValueError("Key encoded capacity declaration changed")
            parts.append('#define KEY_AGREEMENT_ENCODED_PACKET_SIZE ' + constant.group(1))
        parts.append(declaration.group())
        variable = "encoded_message_size" if label == "network" else "encoded_packet_size"
        wrapper = (f'void packet_caller_reference_{label}_union(short written, '
                   'struct packet_caller_union_observation *out) {\n'
                   f' byte {buffer}[{capacity}]; union {union} {variable};\n'
                   f' {assignment}\n'
                   f' out->initialized_value = {variable}.value; out->initial_encoded = {variable}.encoded;\n'
                   f' {variable}.encoded = written;\n'
                   f' out->after_write_value = {variable}.value; out->after_write_encoded = {variable}.encoded;\n'
                   f' memcpy(out->bytes, &{variable}, sizeof(out->bytes));\n}}\n')
        parts.append(wrapper)
        excerpts[label] = {
            "source": filename,
            "union_declaration_sha256": hashlib.sha256(declaration.group().encode()).hexdigest(),
            "original_initialization_sha256": hashlib.sha256(assignment.encode()).hexdigest(),
            "diagnostic_wrapper_sha256": hashlib.sha256(wrapper.encode()).hexdigest(),
            "short_output_write": "injected_numeric_short_stands_in_for_group_encoder_output",
            "whole_caller_body_executed": False,
        }
    generated = output / "caller_reference.c"
    generated.write_text('\n\n'.join(parts) + '\n', encoding="utf-8")
    return [generated], {
        "functions": functions, "union_excerpts": excerpts,
        "network_capacity_macro_sha256": hashlib.sha256(network_capacity.group().encode()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "services": "authored_initialized_allocator_and_memcpy_not_engine_heap_or_cseries_pointer_assertions",
        "production_integrated": False,
    }
