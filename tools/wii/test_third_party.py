"""Provenance checks for the vendored decoders in port/wii/third_party (HWI-008D).

The pinned SHA-256 values in port/wii/third_party/README.md must match the
files, and the README must name each file's licence. The patched pl_mpeg must
differ from upstream only by its documented patch.
"""
import hashlib
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2] / "port" / "wii" / "third_party"
FILES = ("pl_mpeg/pl_mpeg.h", "pl_mpeg/pl_mpeg_wii.h", "pl_mpeg/pl_mpeg_wii.patch", "pl_mpeg/LICENSE",
         "stb/stb_image.h")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ThirdPartyTests(unittest.TestCase):
    def setUp(self):
        self.readme = (ROOT / "README.md").read_text(encoding="utf-8")

    def test_pinned_hashes_match(self):
        for name in FILES:
            row = next((line for line in self.readme.splitlines() if "`%s`" % name in line and "|" in line), None)
            self.assertIsNotNone(row, name)
            pinned = re.findall(r"`([0-9a-f]{64})`", row)
            self.assertEqual(pinned, [sha256(ROOT / name)], name)

    def test_stored_without_line_ending_conversion(self):
        self.assertIn("-text", (ROOT / ".gitattributes").read_text(encoding="ascii"))
        for name in FILES:
            self.assertNotIn(b"\r\n", (ROOT / name).read_bytes(), name)

    def test_licences_present(self):
        self.assertIn(b"SPDX-License-Identifier: MIT", (ROOT / "pl_mpeg/pl_mpeg.h").read_bytes()[:200])
        self.assertIn(b"Permission is hereby granted", (ROOT / "pl_mpeg/LICENSE").read_bytes())
        tail = (ROOT / "stb/stb_image.h").read_bytes()[-4000:]
        self.assertIn(b"ALTERNATIVE A - MIT License", tail)
        self.assertIn(b"ALTERNATIVE B - Public Domain", tail)

    def test_patch_is_the_only_difference(self):
        upstream = (ROOT / "pl_mpeg/pl_mpeg.h").read_text(encoding="utf-8").splitlines()
        patched = (ROOT / "pl_mpeg/pl_mpeg_wii.h").read_text(encoding="utf-8").splitlines()
        patch = (ROOT / "pl_mpeg/pl_mpeg_wii.patch").read_text(encoding="utf-8").splitlines()
        removed = [line[1:] for line in patch if line.startswith("-") and not line.startswith("---")]
        added = [line[1:] for line in patch if line.startswith("+") and not line.startswith("+++")]
        # Apply the hunks in order: every hunk's context and removals must match upstream.
        result, position = [], 0
        hunk = None
        for line in patch:
            header = re.match(r"^@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@", line)
            if header:
                start = int(header.group(1)) - 1
                result += upstream[position:start]
                position = start
                hunk = True
            elif hunk and line.startswith(" "):
                self.assertEqual(upstream[position], line[1:])
                result.append(line[1:])
                position += 1
            elif hunk and line.startswith("-"):
                self.assertEqual(upstream[position], line[1:])
                position += 1
            elif hunk and line.startswith("+"):
                result.append(line[1:])
        result += upstream[position:]
        self.assertEqual(result, patched)
        self.assertTrue(removed and added)
        self.assertIn("HWI-008D", "\n".join(added))


if __name__ == "__main__":
    unittest.main()
