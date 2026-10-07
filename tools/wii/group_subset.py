"""Preserve the complete actual group translation unit for isolated diagnostics."""
import hashlib
from pathlib import Path

GROUP_FUNCTIONS = ("data_packet_group_initialize", "data_packet_groups_get_error",
                   "data_packet_group_append_packet_header", "data_packet_group_encode_packet",
                   "data_packet_group_decode_packet")


def generate_groups(output: Path, extract):
    source = Path("source/memory/data_packet_groups.c")
    text = source.read_text(encoding="utf-8")
    generated = output / "group_reference.c"
    observer = ('\nconst char *packet_group_reference_peek_error(void) {\n'
                ' return global_data_packet_groups_error_string;\n}\n')
    generated.write_text('#include "packet_shim.h"\n' + text + observer, encoding="utf-8")
    return [generated], {
        "source": source.as_posix(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "functions": {name: {"body_sha256": hashlib.sha256(extract(text, name).encode()).hexdigest()}
                      for name in GROUP_FUNCTIONS},
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "transformation": "diagnostic_include_and_read_only_error_observer_entire_source_preserved",
        "error_observer_sha256": hashlib.sha256(observer.encode()).hexdigest(),
        "byte_swap_service": "authored_one_byte_group_descriptor_only",
        "production_integrated": False,
    }
