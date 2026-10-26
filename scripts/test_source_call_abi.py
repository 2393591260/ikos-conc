#!/usr/bin/env python3
"""Real LLVM value-ABI admission controls; run separately for GNU32 and GNU64."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile

REASON = "source call ABI attributes not covered"
EXTENSIONS = {"signext", "zeroext"}
HEAP = ("pthread_initialization_contract", "constant-heap-runtime-byte-frame-v1")
READONLY = ("readonly_heap_reads_contract", None)
STDIO = ("runtime_stdio_contract", None)


def parameter_span(line, symbol):
    """Find this symbol's real argument list, including nested function types."""
    marker = "@" + symbol + "("
    start = line.find(marker)
    if start < 0:
        return None
    start += len(marker)
    depth = 1
    for end in range(start, len(line)):
        if line[end] == "(":
            depth += 1
        elif line[end] == ")":
            depth -= 1
            if not depth:
                return start, end
    raise AssertionError(("unterminated symbol arguments", symbol, line))


def split_parameters(body):
    parts, start, stack = [], 0, []
    pairs = {")": "(", "]": "[", "}": "{", ">": "<"}
    for pos, char in enumerate(body):
        if char in "([{<":
            stack.append(char)
        elif char in ")]}>":
            assert stack and stack.pop() == pairs[char], body
        elif char == "," and not stack:
            parts.append(body[start:pos])
            start = pos + 1
    parts.append(body[start:])
    return parts


def line_role(line):
    stripped = line.lstrip()
    if stripped.startswith(("define ", "declare ")):
        return "declaration"
    if re.search(r"\bcall\b", line):
        return "call"
    return None


def mutate_parameter(ir, symbol, position, attribute, sides):
    """Preserve actual type/value; change only one ABI value attribute."""
    lines, counts = ir.splitlines(), {"call": 0, "declaration": 0}
    for row, line in enumerate(lines):
        role = line_role(line)
        span = parameter_span(line, symbol) if role in sides else None
        if span is None:
            continue
        start, end = span
        args = split_parameters(line[start:end])
        assert position < len(args), (symbol, position, line)
        parameter = args[position].strip()
        # These controls mutate simple integer/pointer leaves, never aggregates
        # or a function-pointer type whose spelling contains embedded spaces.
        match = re.match(r"((?:i\d+|%[-a-zA-Z$._0-9]+|ptr)\**)(?=\s|$)(.*)", parameter)
        assert match, (symbol, position, parameter)
        tail = match.group(2)
        if attribute in EXTENSIONS:
            tail = re.sub(r"(?<!\S)(?:signext|zeroext)(?!\S)", "", tail)
        assert not re.search(r"(?<!\S)" + re.escape(attribute) + r"(?!\S)", tail), parameter
        args[position] = match.group(1) + " " + attribute + tail
        lines[row] = line[:start] + ",".join(args) + line[end:]
        counts[role] += 1
    for side in sides:
        assert counts[side] > 0, (symbol, side, counts)
    if "declaration" in sides:
        assert counts["declaration"] == 1, (symbol, counts)
    result = "\n".join(lines) + "\n"
    assert result != ir
    return result


def mutate_return(ir, symbol, attribute, sides):
    lines, counts = ir.splitlines(), {"call": 0, "declaration": 0}
    for row, line in enumerate(lines):
        role = line_role(line)
        span = parameter_span(line, symbol) if role in sides else None
        if span is None:
            continue
        symbol_at = line.index("@" + symbol + "(")
        prefix = line[:symbol_at]
        # Attribute goes immediately before the real return type; linkage,
        # tail-call markers and unrelated return attributes stay unchanged.
        match = re.search(r"(i\d+\**|%[-a-zA-Z$._0-9]+\*+|ptr)\s*$", prefix)
        assert match, (symbol, line)
        before = prefix[:match.start()]
        if attribute in EXTENSIONS:
            before = re.sub(r"(?<!\S)(?:signext|zeroext)(?!\S)", "", before)
        assert not re.search(r"(?<!\S)" + re.escape(attribute) + r"(?!\S)", before), line
        lines[row] = before + attribute + " " + prefix[match.start():] + line[symbol_at:]
        counts[role] += 1
    for side in sides:
        assert counts[side] > 0, (symbol, side, counts)
    if "declaration" in sides:
        assert counts["declaration"] == 1, (symbol, counts)
    result = "\n".join(lines) + "\n"
    assert result != ir
    return result


def require_extension_pair(ir, symbol, extension):
    seen = {"call": 0, "declaration": 0}
    for line in ir.splitlines():
        role = line_role(line)
        span = parameter_span(line, symbol) if role else None
        if span is None:
            continue
        start, end = span
        args = split_parameters(line[start:end])
        assert len(args) == 1
        assert extension in args[0].split(), (symbol, line)
        assert extension in line[:line.index("@" + symbol + "(")].split(), (symbol, line)
        seen[role] += 1
    assert seen["call"] and seen["declaration"] == 1, (symbol, seen)


def actual_symbol(ir, choices):
    found = [name for name in choices if "@" + name + "(" in ir]
    assert len(found) == 1, ("expected exactly one real libc symbol", found)
    return found[0]



def indirect_call(ir, bits, symbol="malloc", return_type="i8*", argument_type=None):
    """Keep an actual indirect operand through a volatile function-pointer load."""
    lines, changed = ir.splitlines(), 0
    result = []
    argument_type = argument_type or "i" + str(bits)
    pointer_type = return_type + " (" + argument_type + ")*"
    global_name = "@abi_source_target_" + symbol
    assert global_name not in ir
    declaration = [line for line in lines if line_role(line) == "declaration"
                   and "@" + symbol + "(" in line]
    assert len(declaration) == 1 and return_type + " @" + symbol + "(" + argument_type in declaration[0], declaration
    for line in lines:
        span = parameter_span(line, symbol) if line_role(line) == "call" else None
        if span is not None:
            assert re.search(re.escape(return_type) + r"\s+@" + re.escape(symbol) + r"\(", line), line
            name = "%abi_source_callee_" + str(changed)
            result.append("  " + name + " = load volatile " + pointer_type + ", " +
                          pointer_type + "* " + global_name + ", align " + str(bits // 8))
            line = line.replace("@" + symbol + "(", name + "(", 1)
            changed += 1
        result.append(line)
    assert changed, ("expected a real call before indirect mutation", symbol)
    first_function = next(i for i, line in enumerate(result)
                          if line.lstrip().startswith(("define ", "declare ")))
    # External initialization prevents a pre-import global-constant fold;
    # the volatile load also keeps the actual source access explicit.
    result.insert(first_function, global_name + " = externally_initialized global " +
                  pointer_type + " @" + symbol)
    converted = "\n".join(result) + "\n"
    assert re.search(r"\bcall\b[^\n]*%abi_source_callee_\d+\(", converted)
    return converted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ikos", type=Path)
    parser.add_argument("--bits", choices=(32, 64), type=int, default=32)
    parser.add_argument("--group", choices=("heap", "readonly", "narrow", "stdio"),
                        action="append", help="Repeat to select independent control groups")
    args = parser.parse_args()
    root = args.source_root.resolve()
    entry = args.ikos or root / "install/bin/ikos"
    sys.path.insert(0, str(root / "scripts"))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in {"PYTHONPATH", "PYTHONHOME"}}
    compiler = json.loads(subprocess.check_output(command[:-1] + ["-c",
        "import json; from ikos import settings, analyzer; "
        "print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))"],
        text=True, env=env, timeout=30))
    fixture_root = root / "analyzer/test/regression/bmc-values"
    heap = (fixture_root / "heap-pthread-initialization.c").read_text()
    groups = set(args.group or ("heap", "readonly", "narrow", "stdio"))
    total = 0
    with tempfile.TemporaryDirectory(prefix="ikos-source-call-abi-") as temp:
        work = Path(temp)

        def compile_ir(name, body):
            source, ir = work / (name + ".c"), work / (name + ".ll")
            source.write_text(body)
            subprocess.run([compiler[0], *compiler[1], "-S", "-emit-llvm",
                "-m" + str(args.bits), str(source), "-o", str(ir)],
                check=True, capture_output=True, env=env, timeout=30)
            return ir.read_text()

        def control(name, ir, contract, accepted, budget="off"):
            nonlocal total
            source = work / (name + "-" + budget + ".ll")
            source.write_text(ir)
            dbpath = work / (name + "-" + budget + ".db")
            launch = command + ["--analyses=race", "--concurrency=auto",
                "--partitioning=guards", "--demote-race-to-unknown",
                "-m" + str(args.bits), "-o", str(dbpath), str(source)]
            run_env = dict(env)
            if budget == "off":
                launch.insert(len(command), "--bmc-deadline-ms=0")
            else:
                assert budget == "default"
                # Exercise the actual configured default; do not invent or
                # enlarge a solver/structural budget for a refusal control.
                run_env["IKOS_BMC_RECOVER"] = "1"
            proc = subprocess.run(launch, env=run_env, capture_output=True,
                                  text=True, timeout=45)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, budget, output)
            with sqlite3.connect(dbpath) as db:
                rows = [json.loads(row[0]) for row in db.execute("SELECT info FROM checks")]
                result = json.loads(db.execute(
                    "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
            key, expected_value = contract
            proofs = [row for row in rows if row.get(key) and
                      (expected_value is None or row.get(key) == expected_value)]
            if accepted is not None:
                assert bool(proofs) == accepted, (name, budget, proofs, result, output)
            debts = [item for item in result["obligations"]
                     if item.get("source") == "source_abi" and item.get("reason") == REASON]
            if accepted is None:
                # Canonical indirect calls are outside this source-only byte
                # frame, but must not acquire an ABI-attribute debt merely for
                # being indirect. No positive frame/SAFE claim is made here.
                assert not debts and result["verdict"] != "FALSE", (name, result, output)
            elif accepted:
                assert not debts, (name, budget, result, output)
                assert result["verdict"] == "SAFE" and result["coverage"] == "Checked", (
                    name, budget, result, output)
            else:
                assert result["verdict"] == "UNKNOWN" and result["coverage"] == "Blocked", (
                    name, budget, result, output)
                assert result["unresolved_count"] > 0 and debts, (name, budget, result, output)
                assert not result["search_performed"], (name, budget, result)
                assert all(not item.get("contract") and not item.get("safety_only") for item in debts), debts
            total += 1
            print(f"source-abi={name}/bits={args.bits}/bmc={budget}: "
                  f"{result['verdict']}/{result['coverage']} proof={bool(proofs)} "
                  f"ir={hashlib.sha256(ir.encode()).hexdigest()}", flush=True)

        if "heap" in groups:
            original = compile_ir("heap-native", heap)
            control("heap-native", original, HEAP, True)
            # The target address is explicit in a global initializer; the
            # actual source call remains indirect in this LLVM input. An AI
            # resolver finding malloc must not erase the call-site inreg gate.
            indirect = indirect_call(original, args.bits)
            control("indirect-malloc-native", indirect, HEAP, None)
            attributed = mutate_parameter(original, "malloc", 0, "inreg", {"call"})
            attributed = indirect_call(attributed, args.bits)
            control("indirect-malloc-inreg", attributed, HEAP, False)
            control("indirect-malloc-inreg", attributed, HEAP, False, "default")
            both = {"call", "declaration"}
            for symbol, short in (("malloc", "malloc"), ("pthread_mutex_init", "mutex-init"),
                                  ("pthread_mutex_lock", "mutex-lock"), ("pthread_create", "create")):
                for sides, suffix in ((both, "both"), ({"call"}, "call"), ({"declaration"}, "decl")):
                    changed = mutate_parameter(original, symbol, 0, "inreg", sides)
                    name = short + "-param-inreg-" + suffix
                    control(name, changed, HEAP, False)
                    if suffix == "both" and short in {"malloc", "mutex-lock", "create"}:
                        control(name, changed, HEAP, False, "default")
            for symbol, short in (("malloc", "malloc"), ("pthread_mutex_lock", "mutex-lock")):
                changed = mutate_return(original, symbol, "inreg", both)
                control(short + "-return-inreg", changed, HEAP, False)
            for kind, mutation in (("param", lambda: mutate_parameter(
                    original, "worker", 0, "inreg", {"declaration"})),
                    ("return", lambda: mutate_return(
                    original, "worker", "inreg", {"declaration"}))):
                changed = mutation()
                control("callback-" + kind + "-inreg", changed, HEAP, False)
                if kind == "param":
                    control("callback-" + kind + "-inreg", changed, HEAP, False, "default")

        if "readonly" in groups:
            readonly = (fixture_root / "readonly-heap-reads.c").read_text()
            original = compile_ir("readonly-native", readonly)
            control("readonly-native", original, READONLY, True)
            changed = mutate_parameter(original, "malloc", 0, "inreg", {"call", "declaration"})
            control("readonly-malloc-inreg", changed, READONLY, False)

        if "narrow" in groups:
            # Readonly construction supports finite scalar helpers, including
            # bool comparisons. Calls and definitions must survive in LLVM.
            readonly = (fixture_root / "readonly-heap-reads.c").read_text()
            for name, ctype, extension in (
                    ("bool", "_Bool", "zeroext"),
                    ("signed-char", "signed char", "signext"),
                    ("unsigned-char", "unsigned char", "zeroext"),
                    ("signed-short", "short", "signext"),
                    ("unsigned-short", "unsigned short", "zeroext")):
                symbol = "abi_narrow"
                declaration = ("__attribute__((noinline)) static " + ctype +
                    " " + symbol + "(" + ctype + " value) { return value; }\n")
                needle = "static datum *make_vector(int length) {"
                value = "    data[position] = arbitrary();"
                assert readonly.count(needle) == 1 and readonly.count(value) == 1
                body = readonly.replace(needle, declaration + needle).replace(
                    value, "    data[position] = (datum)abi_narrow((" + ctype + ")arbitrary());")
                original = compile_ir("narrow-" + name, body)
                require_extension_pair(original, symbol, extension)
                control("narrow-" + name + "-native", original, READONLY, True)
                other = "signext" if extension == "zeroext" else "zeroext"
                for sides, suffix in (({"call"}, "call"), ({"declaration"}, "definition")):
                    for kind, changed in (
                        ("param", mutate_parameter(original, symbol, 0, other, sides)),
                        ("return", mutate_return(original, symbol, other, sides))):
                        label = "narrow-" + name + "-" + suffix + "-" + kind + "-mismatch"
                        control(label, changed, READONLY, False)
                        if name == "bool" and suffix == "call" and kind == "param":
                            control(label, changed, READONLY, False, "default")
                if name == "bool":
                    # Even paired signext is outside the supported C bool ABI.
                    changed = mutate_parameter(original, symbol, 0, "signext", {"call", "declaration"})
                    control("bool-paired-signext", changed, READONLY, False)
                    changed = mutate_parameter(original, symbol, 0, "signext", {"call"})
                    changed = indirect_call(changed, args.bits, symbol, "i1", "i1")
                    control("indirect-bool-extension-mismatch", changed, READONLY, False)
                    control("indirect-bool-extension-mismatch", changed, READONLY, False, "default")

        if "stdio" in groups:
            stdio = (fixture_root / "runtime-stdio.c").read_text()
            original = compile_ir("stdio-native", stdio)
            control("stdio-native", original, STDIO, True)
            scanf = actual_symbol(original, ("sscanf", "__isoc99_sscanf"))
            for symbol, short in ((scanf, "scan"), ("fprintf", "fprintf")):
                changed = mutate_parameter(original, symbol, 0, "inreg", {"call", "declaration"})
                control("stdio-" + short + "-fixed-inreg", changed, STDIO, False)
                if short == "fprintf":
                    control("stdio-" + short + "-fixed-inreg", changed, STDIO, False, "default")
            changed = mutate_parameter(original, "fprintf", 2, "inreg", {"call"})
            control("stdio-vararg-inreg", changed, STDIO, False)
    verify_runtime(identity)
    print(f"PASS {total} real LLVM source-call ABI controls, bits={args.bits}", flush=True)


if __name__ == "__main__":
    main()
