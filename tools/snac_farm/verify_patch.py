#!/usr/bin/env python3
"""Prove tools/patch_sys.py did what it claims to a cloned core.

patch_sys.py edits at regex anchors rather than replacing files, so it can
succeed on a core whose sys/ is an old framework resync and still leave the
build without a working pad - a missing anchor is reported, but a partially
applied edit would not be. This checks the result instead of trusting the exit
code, and it also checks that nothing outside sys/ was touched, because a patch
that drifted into rtl/ would be a per-core source change and the whole claim of
this project is that there are none.

    verify_patch.py <core-tree> <psx_snac_pad.sv>
"""

import hashlib
import os
import re
import subprocess
import sys

# Each check is (label, regex, expected count) against sys/sys_top.v. Counts are
# exact, so the patterns must be ones the framework does not already contain -
# `ifndef MISTER_DUAL_SDRAM and .clk(FPGA_CLK2_50) both appear a dozen times in
# a stock sys_top.v, so the else-branches of our four guards are matched
# instead, which only our patch introduces.
SYS_TOP_CHECKS = [
    ("reader instantiated",        r"\bpsx_snac_pad\s+snac_pad\b", 1),
    ("UIO 0x45 status reply",      r"io_din\[7:0\] == 'h45", 1),
    ("UIO 0x45 command handler",   r"cmd == 'h45", 1),
    ("clk_sys snapshot process",   r"snac_s_btn1\s*<=\s*snac_btn1", 1),
    ("dual-SDRAM fallback for enable", r"wire snac_en = 1'b0;", 1),
    ("dual-SDRAM fallback for mux",    r"wire \[6:0\] user_drv = user_out;", 1),
    ("user port muxed",            r"wire \[6:0\] user_drv = snac_en \?", 1),
    ("USER_IO driven from mux",    r"assign USER_IO\[\d\] = [^;]*user_drv\[\d\]", 7),
    ("core sees idle inputs",      r"assign user_in\[\d\] =\s*snac_en \|", 7),
    ("old driver gone",            r"assign USER_IO\[\d\] = [^;]*user_out\[\d\]", 0),
]

INSTANCE_RE = re.compile(r"psx_snac_pad\s+snac_pad\s*\((?P<ports>[^;]*?)\)\s*;", re.S)

# Ports the reader must actually be wired to. The pinout is the whole point of
# the feature, so a patch that instantiated the module but left the user port
# unconnected would pass every count above.
REQUIRED_PORTS = [".clk(FPGA_CLK2_50)", ".enable(snac_en)",
                  ".dat_pad(USER_IO[4])", ".ack_pad(USER_IO[3])"]


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def git(tree, *args):
    return subprocess.run(["git", "-C", tree, *args],
                          capture_output=True, text=True).stdout


def main():
    tree, module = sys.argv[1], sys.argv[2]
    name = os.path.basename(os.path.abspath(tree))
    fails = []

    def check(ok, label, detail=""):
        print(f"  [{'ok' if ok else 'FAIL'}] {label}{'  ' + detail if detail else ''}")
        if not ok:
            fails.append(label)

    print(f"== {name}")

    sv = os.path.join(tree, "sys", "psx_snac_pad.sv")
    check(os.path.exists(sv), "sys/psx_snac_pad.sv exists")
    if os.path.exists(sv):
        check(sha(sv) == sha(module), "module is byte-identical to the framework",
              sha(sv)[:12])

    qip = os.path.join(tree, "sys", "sys.qip")
    qtxt = open(qip).read() if os.path.exists(qip) else ""
    hits = re.findall(r"^set_global_assignment -name SYSTEMVERILOG_FILE .*"
                      r"psx_snac_pad\.sv.*$", qtxt, re.M)
    check(len(hits) == 1, "sys.qip lists the module exactly once",
          hits[0].strip() if hits else "")

    top = os.path.join(tree, "sys", "sys_top.v")
    ttxt = open(top).read() if os.path.exists(top) else ""
    for label, rx, want in SYS_TOP_CHECKS:
        got = len(re.findall(rx, ttxt))
        check(got == want, f"sys_top.v: {label}", f"{got} of {want}")

    inst = INSTANCE_RE.search(ttxt)
    ports = inst.group("ports") if inst else ""
    for p in REQUIRED_PORTS:
        check(p in ports, f"reader port {p} connected")

    # The project file Quartus will actually compile, chosen the way the
    # container script chooses it, so a mismatch shows up here and not an hour
    # into a build. Its revision name is also the name Quartus gives the .rbf,
    # which is the name the installed core has to keep.
    qpfs = sorted(f for f in os.listdir(tree) if f.endswith(".qpf")
                  and "Q13" not in f)
    check(bool(qpfs), "a non-Q13 .qpf exists", ", ".join(qpfs) or "none")
    if qpfs:
        rev = qpfs[0][:-4]
        check(os.path.exists(os.path.join(tree, rev + ".qsf")),
              f"matching {rev}.qsf exists")
        print(f"  quartus revision: {rev}  -> output_files/{rev}.rbf")

    if os.path.isdir(os.path.join(tree, ".git")):
        changed = [l[3:] for l in git(tree, "status", "--porcelain").splitlines()]
        outside = [f for f in changed if not f.startswith("sys/")]
        check(not outside, "nothing changed outside sys/",
              ", ".join(outside) or "clean")
        print("  changed files: " + ", ".join(changed))

    print(f"== {name}: {'PASS' if not fails else 'FAIL (' + ', '.join(fails) + ')'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
