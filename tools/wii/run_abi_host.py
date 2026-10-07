"""Build/run authored C fixtures with a native GCC, without modifying global PATH."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(".local/wii-abi-host"))
    parser.add_argument("--compare", type=Path, help="compare canonical/arithmetic lines from a PPC report")
    args = parser.parse_args()
    compiler = args.cc.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    record_path = args.output / "host-info.json"
    # Invalidate old success evidence before replacing any executable or report.
    record_path.write_text(json.dumps({"scope": "synthetic_abi_only", "state": "incomplete"}) + "\n", encoding="utf-8")
    executable = (args.output / ("fixture.exe" if os.name == "nt" else "fixture")).resolve()
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", "-DWII_ABI_HOST"]
    sources = ["port/wii/abi/boundary.c", "port/wii/abi/fixture.c"]
    subprocess.run([str(compiler), *flags, *sources, "-o", str(executable)], env=environment, check=True)
    result = subprocess.run([str(executable)], env=environment, text=True, capture_output=True, check=True)
    print(result.stdout, end="")
    (args.output / "report.txt").write_text(result.stdout, encoding="utf-8")
    record = {
        "scope": "synthetic_abi_only", "state": "pass", "compiler": subprocess.check_output([str(compiler), "--version"], text=True, env=environment).splitlines()[0],
        "target": subprocess.check_output([str(compiler), "-dumpmachine"], text=True, env=environment).strip(),
        "flags": flags, "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "report": result.stdout.splitlines(),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "inputs_sha256": {name: hashlib.sha256(Path(name).read_bytes()).hexdigest()
                          for name in (*sources, "port/wii/abi/boundary.h", "port/wii/abi/fixture.h", "tools/wii/run_abi_host.py")},
    }
    if args.compare:
        prefixes = ("ABI CANONICAL ", "ABI ARITHMETIC ")
        actual = [line for line in args.compare.read_text(encoding="utf-8").splitlines() if line.startswith(prefixes)]
        expected = [line for line in result.stdout.splitlines() if line.startswith(prefixes)]
        if len(expected) != 2 or actual != expected:
            record["state"] = "comparison_failed"
            record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            parser.error("PPC report must contain exactly one matching canonical and arithmetic record")
        record["ppc_comparison"] = "matching synthetic canonical/arithmetic records"
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
