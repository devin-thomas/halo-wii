"""Script value cells on a big-endian target (HWI-016C): a static audit of the engine's script code.

A script value is a long cell holding a boolean in its first byte and a short in its first two bytes
(source/hs/hs.h HS_CELL_*). On a little-endian target a cast of the long reads the same value; on the
Wii it reads the other end. These tests read the engine's own source (no game data) and check that every
place a script value crosses between a cell and a C value agrees with the value's width:

- each script function's evaluator (source/hs/hs.c HS_EVALUATE_* instantiations): its arguments' views
  (an argument struct's fields, or the macro's union members or longs) and its result's width, against
  the function's definition's parameter and return types;
- hs_runtime.c reads no short or boolean cell with a cast of the long.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HS = (ROOT / "source/hs/hs.c").read_text(encoding="utf-8", errors="replace")
RUNTIME = (ROOT / "source/hs/hs_runtime.c").read_text(encoding="utf-8", errors="replace")
HS_H = (ROOT / "source/hs/hs.h").read_text(encoding="utf-8", errors="replace")


def type_names():
    body = HS_H[HS_H.index("enum hs_type"):HS_H.index("NUMBER_OF_HS_TYPES")]
    return [line.strip().rstrip(",") for line in body.splitlines() if line.strip().startswith("_hs")]


def short_cell_types():
    """the types hs_global_reconcile_read reads and writes as shorts, and the six object names"""
    names = type_names()
    read = RUNTIME[RUNTIME.rindex("static void hs_global_reconcile_read(\n"):
                   RUNTIME.rindex("static void hs_global_reconcile_write(\n")]
    shorts = {case.split(":")[0].strip() for case in read.split("case ")[1:]
              if "*(short *)&global->value" in case.split("break;")[0]}
    shorts |= {name for name in names if name.endswith("_name") and name.startswith("_hs_type_")}
    return shorts


SHORTS = short_cell_types()


def width(hs_type):
    if hs_type in SHORTS:
        return "short"
    if hs_type == "_hs_type_boolean":
        return "boolean"
    if hs_type == "_hs_type_real":
        return "real"
    if hs_type == "_hs_type_void":
        return "void"
    return "long"


def definitions():
    """evaluator -> [(script name, return type, parameter types)]"""
    result = {}
    for chunk in HS.split("static struct hs_function_definition")[1:]:
        body = chunk[:chunk.find("};")]
        match = re.search(r'(_hs_type_\w+),\s*[^,]+,\s*"([^"]+)",\s*(\w+),\s*(\w+),', body)
        if not match:
            continue
        after = body[match.end():]
        count = re.search(r"\n\s*(\d+),", after)
        parameters = re.findall(r"_hs_type_\w+", after[count.end():]) if count else []
        result.setdefault(match.group(4), []).append((match.group(2), match.group(1), parameters))
    return result


def argument_structs():
    """struct hs_arguments_* -> [(offset, view)] in 4-byte cells"""
    views = {"short": "short", "word": "short", "boolean": "boolean", "byte": "boolean", "real": "real",
             "long": "long", "unsigned long": "long"}
    structs = {}
    for match in re.finditer(r"^struct (hs_arguments_\w+)\s*\{(.*?)\};", HS, re.M | re.S):
        fields, offset = [], 0
        for line in match.group(2).splitlines():
            line = line.strip().rstrip(";")
            if not line or line.startswith("/*"):
                continue
            field = re.match(r"(.+?)\s*(\**)\s*(\w+)(\[(\d+)\])?$", line)
            ctype = field.group(1).replace("const", "").strip()
            pointer = bool(field.group(2))
            count = int(field.group(5)) if field.group(5) else 1
            size = 4 if pointer or ctype in ("long", "real", "unsigned long") else 2 if ctype in ("short", "word") else 1
            offset = (offset + size - 1) // size * size
            for index in range(count):
                if not field.group(3).startswith("pad"):
                    fields.append((offset + index * size, "long" if pointer else views.get(ctype, ctype)))
            offset += size * count
        structs[match.group(1)] = fields
    return structs


FIXED_VIEWS = {
    "HS_EVALUATE_VOID_BOOLEAN": ["boolean"], "HS_EVALUATE_VOID_UNSIGNED_SHORT": ["short"],
    "HS_EVALUATE_VOID_LONG_BOOLEAN": ["long", "boolean"], "HS_EVALUATE_VOID_SHORT_SHORT": ["short", "short"],
    "HS_EVALUATE_VOID_LONG_UNSIGNED_SHORT": ["long", "short"], "HS_EVALUATE_VOID_SHORT_BOOLEAN": ["short", "boolean"],
    "HS_EVALUATE_SHORT_FROM_UNSIGNED_SHORT": ["short"], "HS_EVALUATE_REAL_FROM_UNSIGNED_SHORT": ["short"],
    "HS_EVALUATE_VOID_STRING": ["long"], "HS_EVALUATE_VOID_LONG_STRING": ["long", "long"],
    "HS_EVALUATE_VOID_LONG_LONG_STRING": ["long", "long", "long"], "HS_EVALUATE_VOID_LONG": ["long"],
    "HS_EVALUATE_VOID_LONG_LONG": ["long", "long"], "HS_EVALUATE_SHORT_FROM_LONG": ["long"],
    "HS_EVALUATE_REAL_FROM_LONG": ["long"], "HS_EVALUATE_LONG_FROM_LONG": ["long"],
}
RESULTS = {
    "HS_EVALUATE_NO_ARGUMENTS": ("long", "void"), "HS_EVALUATE_RETURN_LONG": ("long", "void"),
    "HS_EVALUATE_LONG_FROM_LONG": ("long", "void"),
    "HS_EVALUATE_RETURN_SHORT": ("short",), "HS_EVALUATE_SHORT_FROM_LONG": ("short",),
    "HS_EVALUATE_SHORT_FROM_UNSIGNED_SHORT": ("short",), "HS_EVALUATE_RETURN_SHORT_FROM_ARGUMENTS": ("short",),
    "HS_EVALUATE_RETURN_BOOLEAN": ("boolean",), "HS_EVALUATE_RETURN_BOOLEAN_NO_ARGUMENTS": ("boolean",),
    "HS_EVALUATE_REAL_FROM_LONG": ("real",), "HS_EVALUATE_REAL_FROM_UNSIGNED_SHORT": ("real",),
}
STRUCT_KINDS = ("HS_EVALUATE_VOID_FROM_ARGUMENTS", "HS_EVALUATE_VOID_FROM_ARGUMENTS_WITH_REAL",
                "HS_EVALUATE_RETURN_BOOLEAN", "HS_EVALUATE_RETURN_SHORT_FROM_ARGUMENTS")


class EvaluatorCellTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.definitions = definitions()
        cls.structs = argument_structs()
        cls.instances = re.findall(r"^(HS_EVALUATE_\w+)\((\w+),\s*(?:struct\s+)?(\w+)?", HS, re.M)

    def test_the_audit_sees_the_evaluators(self):
        self.assertGreater(len(self.instances), 300)
        self.assertGreater(len(self.definitions), 300)
        self.assertIn("_hs_type_trigger_volume", SHORTS)
        self.assertNotIn("_hs_type_ai", SHORTS)

    def test_struct_views_match_the_parameters(self):
        checked = 0
        for kind, evaluator, struct in self.instances:
            if kind not in STRUCT_KINDS:
                continue
            fields = self.structs.get(struct)
            self.assertIsNotNone(fields, f"{evaluator}: {struct}")
            for name, _, parameters in self.definitions.get(evaluator, []):
                for index, hs_type in enumerate(parameters):
                    views = [view for offset, view in fields if offset == 4 * index]
                    if not views and kind.endswith("WITH_REAL") and width(hs_type) == "real":
                        continue
                    checked += 1
                    self.assertEqual(views[:1], [width(hs_type)], f"{name} argument {index} ({struct})")
        self.assertGreater(checked, 80)

    def test_fixed_views_match_the_parameters(self):
        checked = 0
        for kind, evaluator, _ in self.instances:
            if kind not in FIXED_VIEWS:
                continue
            for name, _, parameters in self.definitions.get(evaluator, []):
                self.assertEqual(len(parameters), len(FIXED_VIEWS[kind]), name)
                for index, hs_type in enumerate(parameters):
                    checked += 1
                    self.assertEqual(width(hs_type), FIXED_VIEWS[kind][index], f"{name} argument {index}")
        self.assertGreater(checked, 200)

    def test_results_match_the_return_types(self):
        checked = 0
        for kind, evaluator, _ in self.instances:
            for name, return_type, _ in self.definitions.get(evaluator, []):
                allowed = RESULTS.get(kind, ("void",))
                if kind == "HS_EVALUATE_NO_OP":
                    continue
                checked += 1
                self.assertIn(width(return_type), allowed, f"{name} ({kind})")
        self.assertGreater(checked, 300)


class RuntimeCastTest(unittest.TestCase):
    def test_no_short_or_boolean_cell_read_by_a_cast(self):
        """hs_runtime.c: a cast of a long to short or boolean is either of a C long that holds the value
        whole (a node's long-written global or element index, a thread's index) or not there at all"""
        allowed = ("(short)variable->data", "(short)expression->data", "(short)thread_index",
                   "(short)thread->script_index", "(short)size", "(short)((random", "(short)hs_object_type_masks",
                   "(short)*(real *)&r", "(short)l, l)")
        for number, line in enumerate(RUNTIME.splitlines(), 1):
            for match in re.finditer(r"\((short|boolean)\)\s*\*?\w+", line):
                text = line[match.start():]
                if not any(text.startswith(prefix) for prefix in allowed):
                    self.fail(f"hs_runtime.c:{number}: {line.strip()}")

    def test_sleep_until_reads_each_argument_as_its_parser_types_it(self):
        """(sleep_until <boolean> [<short> [<long>]]): hs_parse_sleep_until's types, read as such by the
        runtime (a timeout read as a short cell went unnoticed until a10's script timed out on the host only)"""
        compile_text = (ROOT / "source/hs/hs_compile.c").read_text(encoding="utf-8", errors="replace")
        parser = compile_text[compile_text.index("boolean hs_parse_sleep_until("):]
        parser = parser[:parser.index("\n}\n")]
        self.assertEqual(re.findall(r"hs_parse\(\w+, (_hs_type_\w+)\)", parser),
                         ["_hs_type_boolean", "_hs_type_short_integer", "_hs_type_long_integer"])
        body = RUNTIME[RUNTIME.rindex("void hs_evaluate_sleep_until("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("HS_CELL_BOOLEAN(*condition)", body)
        self.assertIn("HS_CELL_SHORT(*ticks)", body)
        self.assertNotRegex(body, r"HS_CELL_\w+\(\*timeout\)")


if __name__ == "__main__":
    unittest.main()
