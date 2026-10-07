"""Compile actual engine scalar/packet functions in an isolated authored harness.

Function bodies/actual memory headers are retained; the generated translation
unit replaces only MSVC ui64 literal suffixes with C ULL. No game code is edited.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from build import verify_dol, verify_elf
from check_toolchain import inspect_toolchain, toolchain_inputs


SELECTED = {
    "source/memory/byte_swapping.c": ["byte_swap_memory"],
    "source/memory/data_encoding.c": [
        "data_encode_new", "data_encode_memory", "data_encode_integer",
        "data_decode_new", "data_decode_memory", "data_decode_byte",
        "data_decode_short", "data_decode_long", "data_decode_int64",
        "data_decode_integer",
    ],
}
HARNESS = Path("port/wii/abi/memory_subset")


def extract(text, name):
    pattern = (rf"(?m)^(?:static[ \t]+)?(?:void|boolean|byte|short|long|__int64|char(?:[ \t]+const)?)"
               rf"[ \t]+(?:\*[ \t]*)?{re.escape(name)}\s*\([^;{{}}]*\)\s*(?={{)")
    starts = list(re.finditer(pattern, text))
    if len(starts) != 1:
        raise ValueError(f"Expected one definition of {name}, found {len(starts)}")
    start = starts[0].start()
    brace = text.index("{", starts[0].end())
    # Ignore braces inside comments and quoted literals when finding body end.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         text[brace:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:brace + token.end()]
    raise ValueError(f"Unterminated definition of {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(".local/wii-memory-subset"))
    parser.add_argument("--candidate", action="store_true",
                        help="compare the preserved reference with a diagnostic endian/alignment adapter")
    parser.add_argument("--packets", action="store_true",
                        help="exercise actual packet dispatch with reference and candidate scalar services")
    parser.add_argument("--packet-policy", action="store_true",
                        help="add a third diagnostic decoder for legacy excluded-field placeholders")
    parser.add_argument("--packet-verifier", action="store_true",
                        help="add bounded flat-schema verifier policy and retain all earlier diagnostics")
    parser.add_argument("--packet-arrays", action="store_true",
                        help="add bounded recursive schema and paired array codec diagnostic")
    parser.add_argument("--packet-groups", action="store_true",
                        help="add actual group boundary reference and bounded paired-codec wrapper")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--compile-only", action="store_true", help="compile objects without execution")
    mode.add_argument("--wii-devkitpro", type=Path, help="link an asset-free Wii ELF/DOL using this official SDK")
    args = parser.parse_args()
    if args.packet_groups:
        args.packet_arrays = True
    if args.packet_arrays:
        args.packet_verifier = True
    if args.packet_verifier:
        args.packet_policy = True
    if args.packet_policy:
        args.packets = True
    if args.packets:
        args.candidate = True
    compiler = args.cc.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    record_path = args.output / "subset-info.json"
    record_path.write_text('{"scope":"actual_scalar_subset","state":"incomplete"}\n', encoding="utf-8")
    environment = os.environ.copy()
    overrides = ("GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "CPATH",
                 "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                 "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES")
    for name in overrides:
        environment.pop(name, None)
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output([str(compiler), "-dumpmachine"], env=environment, text=True).strip()
    if target == "powerpc-eabi" and not (args.compile_only or args.wii_devkitpro):
        parser.error("powerpc-eabi requires --compile-only or --wii-devkitpro; target binaries cannot run on the host")
    if args.wii_devkitpro and target != "powerpc-eabi":
        parser.error("--wii-devkitpro requires a powerpc-eabi compiler")
    inputs = [*SELECTED, "source/memory/byte_swapping.h", "source/memory/data_encoding.h",
              "source/cseries/cseries.h", str(HARNESS / "shim.h"), str(HARNESS / "fixture.c"),
              str(HARNESS / "fixture.h"), Path(__file__).resolve().relative_to(Path.cwd()).as_posix()]
    if args.candidate:
        inputs += [str(HARNESS / name) for name in ("candidate.h", "candidate.c", "candidate_edges.c")]
    if args.packets:
        inputs += ["source/memory/data_packets.c", "source/memory/data_packets.h",
                   "source/memory/data_packet_groups.h", "source/cseries/cseries.c",
                   "tools/wii/packet_subset.py"]
        inputs += [str(HARNESS / name) for name in
                   ("packet_fixture.c", "packet_fixture.h", "packet_shim.h", "cseries.h")]
    if args.packet_policy:
        inputs += [str(HARNESS / name) for name in
                   ("packet_version_policy.c", "packet_version_edges.c", "packet_version_edges.h")]
    if args.packet_verifier:
        inputs += [str(HARNESS / name) for name in
                   ("packet_verifier_policy.c", "packet_verifier_policy.h",
                    "packet_verifier_fixture.c", "packet_verifier_fixture.h")]
    if args.packet_arrays:
        inputs += [str(HARNESS / name) for name in
                   ("packet_array_policy.c", "packet_array_policy.h",
                    "packet_array_fixture.c", "packet_array_fixture.h")]
    if args.packet_groups:
        inputs += ["source/memory/data_packet_groups.c", "tools/wii/group_subset.py"]
        inputs += [str(HARNESS / name) for name in
                   ("packet_group_policy.c", "packet_group_policy.h", "packet_group_services.c",
                    "packet_group_fixture.c", "packet_group_fixture.h")]
    sdk = None
    inventory = None
    sdk_identity = {}
    if args.wii_devkitpro:
        sdk = args.wii_devkitpro.resolve(strict=True)
        inventory = inspect_toolchain(sdk)
        if inventory["errors"]:
            raise ValueError("Wii preflight failed: " + "; ".join(inventory["errors"]))
        if compiler != Path(inventory["tools"]["powerpc-eabi-gcc"]["path"]).resolve():
            parser.error("Selected compiler does not belong to --wii-devkitpro")
        inputs += [str(HARNESS / "wii_main.c"), "tools/wii/build.py", "tools/wii/check_toolchain.py"]
        sdk_files = [sdk / "devkitPPC/wii_rules", compiler,
                     Path(inventory["tools"]["elf2dol"]["path"]),
                     sdk / "libogc/lib/wii/libfat.a", sdk / "libogc/lib/wii/libogc.a"]
        sdk_identity = {p.relative_to(sdk).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sdk_files}
        fingerprint = hashlib.sha256()
        for path in toolchain_inputs(sdk):
            fingerprint.update(path.relative_to(sdk).as_posix().encode() + b"\0")
            if path.is_file():
                fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
        sdk_identity["toolchain_tree_sha256"] = fingerprint.hexdigest()
    input_hashes = {Path(name).as_posix(): hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in inputs}
    source_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    source_dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", *map(str, inputs)], text=True))
    flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-multichar",
             "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-I", str(HARNESS), "-I", "source"]
    if target == "powerpc-eabi":
        flags += ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
    elif target == "x86_64-w64-mingw32":
        # LLP64 size_t is 64 bits while long remains 32. Retain/report GCC's
        # warnings for the engine's -sizeof selectors rather than rewriting it.
        flags += ["-Wno-error=overflow"]
    pieces = ['#include "shim.h"\n']
    functions = {}
    macros = {}
    for name, selected in SELECTED.items():
        text = Path(name).read_text(encoding="utf-8")
        if name == "source/memory/byte_swapping.c":
            match = re.search(r"(?ms)^#define SWAP8\(q\).*?(?=\n\n)", text)
            if match is None:
                raise ValueError("Actual byte swapping source lost its SWAP8 macro")
            macro = match.group()
            macros["SWAP8"] = hashlib.sha256(macro.encode()).hexdigest()
            pieces.append(re.sub(r"\b(0x[0-9a-fA-F]+|[0-9]+)ui64\b", r"\1ULL", macro))
        for function in selected:
            body = extract(text, function)
            functions[function] = {"source": name, "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
            pieces.append(re.sub(r"\b(0x[0-9a-fA-F]+|[0-9]+)ui64\b", r"\1ULL", body))
    generated = args.output / "engine_scalars.c"
    generated.write_text("\n\n".join(pieces) + "\n", encoding="utf-8")
    sources = [generated, HARNESS / "fixture.c"]
    candidate_generated = None
    driver = None
    packet_record = None
    if args.candidate:
        # Recompile the unchanged fixture through opt-in adapter names; the
        # original fixture and extracted functions stay linked independently.
        candidate_generated = args.output / "candidate_fixture.c"
        remap = {name: "candidate_" + name.removeprefix("data_")
                 for name in SELECTED["source/memory/data_encoding.c"]
                 if name not in ("data_encode_new", "data_decode_new")}
        candidate_generated.write_text('#include "candidate.h"\n' +
            '\n'.join(f'#define {name} {adapter}' for name, adapter in remap.items()) +
            '\n#define wii_memory_subset wii_memory_candidate_subset\n'
            '#define main wii_memory_candidate_unused_main\n#include "fixture.c"\n', encoding="utf-8")
        sources += [HARNESS / "candidate.c", HARNESS / "candidate_edges.c", candidate_generated]
        flags += ["-DWII_MEMORY_CANDIDATE"]
        if sdk is None and not args.compile_only:
            flags += ["-DWII_MEMORY_PPC"]  # Disable only the fixture's native main.
            driver = args.output / "candidate_driver.c"
            driver.write_text('#include "candidate.h"\n#include "fixture.h"\n'
                'int main(void) {\n'
                ' puts("REFERENCE BEGIN"); int reference = wii_memory_subset(stdout, 1);\n'
                ' printf("REFERENCE END result=%d\\n", reference);\n'
                ' puts("CANDIDATE BEGIN"); int candidate = wii_memory_candidate_subset(stdout, 1);\n'
                ' int edges = wii_memory_candidate_edges(stdout, 1);\n'
                ' printf("CANDIDATE END result=%d\\n", candidate || edges);\n'
                ' return reference || candidate || edges;\n}\n', encoding="utf-8")
            sources.append(driver)
    if args.packets:
        from packet_subset import generate_packets
        packet_sources, packet_record = generate_packets(args.output, extract, args.packet_policy)
        sources += [*packet_sources, HARNESS / "packet_fixture.c"]
        flags += ["-DWII_MEMORY_PACKETS"]
        if driver is not None:
            driver.write_text('#include "packet_fixture.h"\n'
                              'int main(void) { return wii_packet_compare(stdout, 1); }\n', encoding="utf-8")
        if args.packet_policy:
            sources += [HARNESS / "packet_version_policy.c", HARNESS / "packet_version_edges.c"]
            flags += ["-DWII_MEMORY_PACKET_POLICY"]
            if driver is not None:
                driver.write_text('#include "packet_fixture.h"\n#include "packet_version_edges.h"\n'
                    'int main(void) {\n'
                    ' int original = wii_packet_compare(stdout, 1);\n'
                    ' int policy = wii_packet_policy_subset(stdout, 1);\n'
                    ' int edges = wii_packet_version_edges(stdout, 1);\n'
                    ' return original || policy || edges;\n}\n', encoding="utf-8")
        if args.packet_verifier:
            sources += [HARNESS / "packet_verifier_policy.c", HARNESS / "packet_verifier_fixture.c"]
            flags += ["-DWII_MEMORY_PACKET_VERIFIER"]
            if driver is not None:
                array_include = '#include "packet_array_fixture.h"\n' if args.packet_arrays else ''
                array_call = ' int arrays = wii_packet_array_fixture(stdout, 1);\n' if args.packet_arrays else ''
                array_result = ' || arrays' if args.packet_arrays else ''
                if args.packet_groups:
                    array_include += '#include "packet_group_fixture.h"\n'
                    array_call += ' int groups = wii_packet_group_fixture(stdout, 1);\n'
                    array_result += ' || groups'
                driver.write_text(array_include + '#include "candidate.h"\n#include "fixture.h"\n'
                    '#include "packet_fixture.h"\n#include "packet_version_edges.h"\n'
                    '#include "packet_verifier_fixture.h"\n'
                    'int main(void) {\n'
                    ' puts("SCALAR REFERENCE BEGIN");\n'
                    ' int scalar_reference = wii_memory_subset(stdout, 1);\n'
                    ' printf("SCALAR REFERENCE END result=%d\\n", scalar_reference);\n'
                    ' puts("SCALAR CANDIDATE BEGIN");\n'
                    ' int scalar_candidate = wii_memory_candidate_subset(stdout, 1);\n'
                    ' int scalar_edges = wii_memory_candidate_edges(stdout, 1);\n'
                    ' printf("SCALAR CANDIDATE END result=%d\\n", scalar_candidate || scalar_edges);\n'
                    ' int original = wii_packet_compare(stdout, 1);\n'
                    ' int policy = wii_packet_policy_subset(stdout, 1);\n'
                    ' int edges = wii_packet_version_edges(stdout, 1);\n'
                    ' int verifier = wii_packet_verifier_fixture(stdout, 1);\n'
                    + array_call +
                    ' return scalar_reference || scalar_candidate || scalar_edges || original || policy || edges || verifier' + array_result + ';\n'
                    '}\n', encoding="utf-8")
        if args.packet_arrays:
            sources += [HARNESS / "packet_array_policy.c", HARNESS / "packet_array_fixture.c"]
            flags += ["-DWII_MEMORY_PACKET_ARRAYS"]
        if args.packet_groups:
            from group_subset import generate_groups
            group_sources, group_record = generate_groups(args.output, extract)
            sources += [*group_sources, HARNESS / "packet_group_policy.c",
                        HARNESS / "packet_group_services.c", HARNESS / "packet_group_fixture.c"]
            flags += ["-DWII_MEMORY_PACKET_GROUPS"]
    build_id = None
    if sdk is not None:
        build_id = hashlib.sha256(json.dumps({"source_commit": source_commit, "source_dirty": source_dirty,
                                               "inputs": input_hashes, "sdk": sdk_identity, "flags": flags},
                                              sort_keys=True).encode()).hexdigest()[:16]
        (args.output / "subset_build_id.h").write_text(f'#define WII_MEMORY_BUILD_ID "{build_id}"\n', encoding="utf-8")
        flags += ["-DWII_MEMORY_PPC", "-I", str(sdk / "libogc/include"), "-I", str(args.output)]
        sources.append(HARNESS / "wii_main.c")
    artifacts = {}
    report = []
    diagnostics = []

    def compile_checked(arguments):
        result = subprocess.run(arguments, env=environment, capture_output=True, text=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
            diagnostics.extend(result.stderr.splitlines())
        result.check_returncode()

    elf_record = None
    link_flags = []
    executable = None
    flag_exceptions = ({"packet_reference.c": ["-Wno-error=maybe-uninitialized"],
                        "packet_candidate.c": ["-Wno-error=maybe-uninitialized"],
                        "packet_strings.c": ["-Wno-error=type-limits", "-Wno-error=pointer-sign"]}
                       if args.packets else {})
    if args.packet_policy:
        flag_exceptions["packet_policy.c"] = ["-Wno-error=maybe-uninitialized"]
    if args.packet_groups:
        flag_exceptions["group_reference.c"] = ["-Wno-error=sign-compare", "-Wno-error=char-subscripts"]
        if target == "powerpc-eabi":
            flag_exceptions["group_reference.c"].append("-Wno-error=type-limits")
    if args.compile_only or sdk is not None or args.packets:
        objects = []
        for source in sources:
            obj = args.output / (source.stem + ".o")
            compile_checked([str(compiler), *flags, *flag_exceptions.get(source.name, []),
                             "-c", str(source), "-o", str(obj)])
            data = obj.read_bytes()
            if target == "powerpc-eabi" and (data[:6] != b"\x7fELF\x01\x02" or
                                             int.from_bytes(data[18:20], "big") != 20):
                raise ValueError(f"Expected ELF32 big-endian PowerPC object: {obj}")
            artifacts[obj.name] = hashlib.sha256(data).hexdigest()
            objects.append(str(obj))
        if sdk is not None:
            elf = args.output / "memory_subset.elf"
            dol = args.output / "memory_subset.dol"
            map_path = args.output / "memory_subset.map"
            link_flags = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float",
                          "-Wl,-Map," + str(map_path), "-L", str(sdk / "libogc/lib/wii"),
                          "-lfat", "-logc", "-lm"]
            compile_checked([str(compiler), *objects, *link_flags, "-o", str(elf)])
            elf_record = verify_elf(elf)
            compile_checked([inventory["tools"]["elf2dol"]["path"], str(elf), str(dol)])
            verify_dol(dol)
            for artifact in (elf, dol, map_path):
                artifacts[artifact.name] = hashlib.sha256(artifact.read_bytes()).hexdigest()
        elif not args.compile_only:
            executable = (args.output / ("fixture.exe" if os.name == "nt" else "fixture")).resolve()
            compile_checked([str(compiler), *objects, "-o", str(executable)])
    else:
        executable = (args.output / ("fixture.exe" if os.name == "nt" else "fixture")).resolve()
        compile_checked([str(compiler), *flags, *map(str, sources), "-o", str(executable)])
    if executable is not None:
        artifacts[executable.name] = hashlib.sha256(executable.read_bytes()).hexdigest()
        result = subprocess.run([str(executable)], env=environment, text=True, capture_output=True)
        report = (result.stdout + result.stderr).splitlines()
        print("\n".join(report))
        (args.output / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
        if result.returncode != 0 and not args.packets:
            record_path.write_text(json.dumps({"scope": "actual_scalar_subset", "state": "execution_failed",
                                               "exit_code": result.returncode, "report": report}) + "\n", encoding="utf-8")
            raise SystemExit(result.returncode)
    record = {
        "scope": "actual_scalar_subset", "state": "compile_pass" if (args.compile_only or sdk) else "execution_pass",
        "source_commit": source_commit, "source_dirty": source_dirty,
        "compiler": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).splitlines()[0],
        "target": target, "flags": flags, "functions": functions, "macros": macros,
        "cleared_child_environment_overrides": list(overrides),
        "inputs_sha256": input_hashes,
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "object_format": "ELF32 big-endian PowerPC" if target == "powerpc-eabi" else "native host",
        "artifacts_sha256": artifacts, "report": report, "compile_diagnostics": diagnostics,
        "limits": "scalar subset with authored assertion/memory services; no engine integration or alignment safety proof",
    }
    if args.candidate:
        record["candidate"] = {"scope": "diagnostic_adapter_only",
                               "fixture_sha256": hashlib.sha256(candidate_generated.read_bytes()).hexdigest(),
                               "reference_fixture_unchanged": True,
                               "production_integrated": False,
                               "aggregate_result_scope": "candidate_and_edges" if sdk else "reference_candidate_and_edges",
                               "limits": "truthful buffer extents and nonoverlapping encode input/output; structure/packet integration unqualified"}
        if driver is not None:
            record["candidate"]["driver_sha256"] = hashlib.sha256(driver.read_bytes()).hexdigest()
    if args.packets:
        record["scope"] = "actual_packet_dispatch_comparison"
        record["limits"] = ("authored schemas and assertion/memory/format services; "
                            "aligned native count prefixes; original scalar typed wire access retained; "
                            "no production integration or physical Wii qualification")
        record["packets"] = packet_record
        record["packets"]["compile_flag_exceptions"] = flag_exceptions
        record["compiler_sha256"] = hashlib.sha256(compiler.read_bytes()).hexdigest()
        record["candidate"]["aggregate_result_scope"] = "packet_reference_and_candidate"
        if sdk is None and not args.compile_only:
            record["exit_code"] = result.returncode
            record["state"] = "execution_pass" if result.returncode == 0 else "execution_failed"
        if args.packet_policy:
            record["scope"] = "actual_packet_excluded_decode_policy_comparison"
            record["candidate"]["aggregate_result_scope"] = "packet_reference_candidate_policy_and_policy_edges"
        if args.packet_verifier:
            record["scope"] = "bounded_packet_verifier_policy_comparison"
            record["candidate"]["aggregate_result_scope"] = "scalar_packet_placeholder_and_bounded_verifier_sections"
            record["verifier_policy"] = {
                "scope": "standalone_bounded_flat_native_metadata_only", "production_integrated": False,
                "arrays": "rejected_without_child_traversal",
                "own_version_excluded_field_size": 0, "revalidate_cached_definitions": True,
                "metadata_commit": "only_after_complete_validation", "original_verifier_preserved": True,
                "limits": "truthful schema bound and no concurrent mutation; runtime-version native layout unqualified"}
        if args.packet_arrays:
            record["scope"] = "bounded_packet_array_policy_comparison"
            record["candidate"]["aggregate_result_scope"] = "all_prior_sections_and_paired_array_codec"
            record["array_policy"] = {
                "scope": "isolated_snapshot_schema_and_paired_codec", "production_integrated": False,
                "native_capacity": "stable_all_version_latent_reserves_checked_before_io",
                "schema_bound": "truthful_accessible_1_to_32767_fields",
                "stack": "explicit_heap_frames_no_c_recursion",
                "visit_budget": "non_end_fields_at_most_full_native_extent_32767",
                "array_reserve": "2_plus_maximum_count_times_full_child_stride",
                "runtime_version": "0_to_definition_version_only_none_encodes_own",
                "wire_bound": "nonnegative_signed_short_capacity_or_length",
                "excluded_arrays": "raw_count_placeholder_then_zero_reserve_and_skip_child_schema",
                "invalid_counts": "reject_before_current_native_field_write_retain_prior_wire_mutation",
                "trailing_bytes": "accepted_and_unconsumed",
                "original_metadata_and_bodies_preserved": True,
                "limits": "candidate compatibility policy; no group integration or physical Wii qualification"}
        if args.packet_groups:
            record["scope"] = "actual_group_boundary_and_bounded_wrapper_comparison"
            record["candidate"]["aggregate_result_scope"] = "all_prior_sections_and_group_boundaries"
            record["groups"] = group_record
            record["group_policy"] = {
                "scope": "isolated_borrowed_array_plans_with_actual_capacity_checks",
                "production_integrated": False, "portable_type_count": "1_to_128",
                "schema_bound": "truthful_accessible_entry_table_and_live_immutable_compiled_plans",
                "maximum_sizes": "explicit_nonnegative_signed_short_bounds_no_narrowing",
                "append": "payload_plus_trailer_strictly_below_configured_max_and_within_actual_capacity",
                "decode_length": "valid_type_class_remove_trailer_before_payload_even_on_failure",
                "consumed_size": "separate_from_supplied_payload_length_trailing_bytes_accepted",
                "null_definition": "type_only_decode_success_encode_reject",
                "error": "per_call_result_original_shared_error_preserved_separately",
                "limits": "original safe byte schemas; real table shapes authored; no production callers or peer sessions"}
    if sdk is not None:
        # Native SDK paths stay local; publishable records use root-relative paths.
        record["flags"] = [flag.replace(str(sdk), "<DEVKITPRO>") for flag in flags]
        record["link_flags"] = [flag.replace(str(sdk), "<DEVKITPRO>") for flag in link_flags]
        record.update({"build_id": build_id, "sdk_inputs_sha256": sdk_identity,
                       "elf": elf_record, "runtime_verified": False, "collect_failures": True})
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    if args.packets and sdk is None and not args.compile_only:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
