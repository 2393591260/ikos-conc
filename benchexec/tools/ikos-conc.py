"""BenchExec tool-info module for the IKOS concurrency (data-race) analysis.

Registers IKOS as an SV-COMP C.no-data-race tool:

  - cmdline(): fixes the analysis to `--analyses=race --concurrency=auto`,
    disables the (potentially multi-MB) per-check report in favour of the
    one-line summary verdict. The SV-COMP `--propertyfile` is deliberately
    NOT forwarded — the property is always no-data-race.
  - determine_result(): maps the rendered verdict line back to the SV-COMP
    three-way result. Sound mapping under the FN=0 red line: only an
    explicit `The program is SAFE` yields TRUE; a `potentially UNSAFE`
    warning (possible unsoundness) and anything unparsed yield UNKNOWN.

Runnable standalone for local P1 validation:

    python3 benchexec/tools/ikos-conc.py <file.c|.i> [--property p.prp]

which runs IKOS once and prints TRUE / FALSE(no-data-race) / UNKNOWN.
"""

import re
import subprocess
import sys
from pathlib import Path

# --- BenchExec interface (guarded so the module also runs standalone) ---
try:
    from benchexec import result
    from benchexec.tools.template import BaseTool2
    _TRUE, _FALSE, _UNKNOWN, _TIMEOUT = (
        result.RESULT_TRUE_PROP,
        result.RESULT_FALSE_PROP,
        result.RESULT_UNKNOWN,
        result.RESULT_TIMEOUT,
    )
except ImportError:  # pragma: no cover - standalone / offline test
    class BaseTool2:
        pass

    _TRUE, _FALSE, _UNKNOWN, _TIMEOUT = "true", "false", "unknown", "timeout"


RE_DEF_UNSAFE = re.compile(r"\bThe program is definitely UNSAFE\b")
RE_UNKNOWN = re.compile(r"\bThe program is UNKNOWN\b")
RE_POT_UNSAFE = re.compile(r"\bThe program is potentially UNSAFE\b")
RE_SAFE = re.compile(r"\bThe program is SAFE\b")


def verdict_from_text(output):
    """Map rendered IKOS output to an SV-COMP result ('true'/'false'/'unknown').

    NOTE: "Total number of checks: 0" is the NORMAL summary for a race-free
    program — the checker only inserts a check when it finds a conflicting
    pair (or an unknown), so 0 checks + SAFE is correct, not a collapse.
    """
    # Priority mirrors report.py print_summary: definite-unsafe -> unknown ->
    # warning -> safe. "potentially UNSAFE" is a non-race warning (e.g. an
    # ignored side effect that "might be unsound"), so under FN=0 it must not
    # be TRUE.
    if RE_DEF_UNSAFE.search(output):
        return _FALSE
    if RE_UNKNOWN.search(output):
        return _UNKNOWN
    if RE_POT_UNSAFE.search(output):
        return _UNKNOWN
    if RE_SAFE.search(output):
        return _TRUE
    return _UNKNOWN


class Tool(BaseTool2):
    def name(self):
        return "ikos-conc"

    def executable(self, tool_locator):
        return tool_locator.find_executable("ikos")

    def version(self, executable):
        return self._version_from_tool(executable)

    def cmdline(self, tool, executable, options, sourcefiles, propertyfile=None,
                rlimits=None):
        return (
            [executable,
             "--analyses=race",
             "--concurrency=auto",
             "--format=no",
             "--display-times=no"]
            + options
            + sourcefiles
        )

    def determine_result(self, run):
        if run.was_timeout:
            return _TIMEOUT
        if run.exit_code != 0:
            # IKOS exits 0 on every verdict; non-zero means compile/analyzer
            # crash -> UNKNOWN under SV-COMP.
            return _UNKNOWN
        output = run.output
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        return verdict_from_text(output)


# --- standalone P1 wrapper (python3 .../ikos-conc.py <file> [--property ...]) ---
if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--property", default=None, help="ignored (property fixed to no-data-race)")
    ap.add_argument("--ikos", default=None)
    ap.add_argument("--timeout", type=int, default=90)
    args = ap.parse_args()

    ikos = args.ikos or "ikos"
    try:
        proc = subprocess.run(
            [ikos, "--analyses=race", "--concurrency=auto",
             "--format=no", "--display-times=no", args.source],
            capture_output=True, text=True, timeout=args.timeout)
        out = proc.stdout + proc.stderr
        rc = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or "") + (exc.stderr or "")
        rc = -1
        timed_out = True

    if timed_out or rc != 0:
        print("UNKNOWN")
    else:
        v = verdict_from_text(out)
        print({"true": "TRUE", "false": "FALSE(no-data-race)", "unknown": "UNKNOWN"}[v])
