#!/usr/bin/env python3
"""Add the framework PSX SNAC pad reader to a core's sys folder.

Cores carry their own copy of sys/ at whatever framework version they last
resynced, so this inserts the change at stable anchors instead of replacing
files wholesale. Idempotent: running it twice is a no-op.
"""

import re
import shutil
import sys
from pathlib import Path

SNAPSHOT = """
// SNAC pad reader control + clk_sys snapshots of its 50MHz-domain state.
// The user port doesn't exist on dual-SDRAM builds, so the reader is left
// out there and snac_en folds to a constant 0.
`ifndef MISTER_DUAL_SDRAM
reg         snac_en = 0;
reg         snac_s_conn1 = 0, snac_s_conn2 = 0;
reg   [7:0] snac_s_id1 = 8'hFF, snac_s_id2 = 8'hFF;
reg  [15:0] snac_s_btn1 = 0, snac_s_btn2 = 0;
reg   [7:0] snac_s_lx1 = 8'h80, snac_s_ly1 = 8'h80, snac_s_rx1 = 8'h80, snac_s_ry1 = 8'h80;
reg   [7:0] snac_s_lx2 = 8'h80, snac_s_ly2 = 8'h80, snac_s_rx2 = 8'h80, snac_s_ry2 = 8'h80;

always @(posedge clk_sys) begin
\treg [2:0] upd_sr;
\tupd_sr <= {upd_sr[1:0], snac_upd};
\tif (upd_sr[2] ^ upd_sr[1]) begin
\t\tsnac_s_conn1 <= snac_conn1;
\t\tsnac_s_id1   <= snac_id1;
\t\tsnac_s_btn1  <= snac_btn1;
\t\tsnac_s_lx1   <= snac_lx1;
\t\tsnac_s_ly1   <= snac_ly1;
\t\tsnac_s_rx1   <= snac_rx1;
\t\tsnac_s_ry1   <= snac_ry1;
\t\tsnac_s_conn2 <= snac_conn2;
\t\tsnac_s_id2   <= snac_id2;
\t\tsnac_s_btn2  <= snac_btn2;
\t\tsnac_s_lx2   <= snac_lx2;
\t\tsnac_s_ly2   <= snac_ly2;
\t\tsnac_s_rx2   <= snac_rx2;
\t\tsnac_s_ry2   <= snac_ry2;
\tend
end
`else
wire snac_en = 1'b0;
`endif
"""

STATUS_LINE = """`ifndef MISTER_DUAL_SDRAM
\t\t\tif(io_din[7:0] == 'h45) io_dout_sys <= {8'h4A, 6'd0, snac_s_conn2, snac_s_conn1};
`endif
"""

CMD_BLOCK = """`ifndef MISTER_DUAL_SDRAM
\t\t\tif(cmd == 'h45) begin
\t\t\t\tif(cnt[3:0] == 0) snac_en <= io_din[0];
\t\t\t\tcase(cnt[3:0])
\t\t\t\t\t0: io_dout_sys <= {snac_s_conn1, 7'd0, snac_s_id1};
\t\t\t\t\t1: io_dout_sys <= snac_s_btn1;
\t\t\t\t\t2: io_dout_sys <= {snac_s_ly1, snac_s_lx1};
\t\t\t\t\t3: io_dout_sys <= {snac_s_ry1, snac_s_rx1};
\t\t\t\t\t4: io_dout_sys <= {snac_s_conn2, 7'd0, snac_s_id2};
\t\t\t\t\t5: io_dout_sys <= snac_s_btn2;
\t\t\t\t\t6: io_dout_sys <= {snac_s_ly2, snac_s_lx2};
\t\t\t\t\t7: io_dout_sys <= {snac_s_ry2, snac_s_rx2};
\t\t\t\tendcase
\t\t\tend
`endif
"""

INSTANCE = """
`ifndef MISTER_DUAL_SDRAM
// Framework PSX SNAC pad reader. When enabled by the HPS (UIO 0x45) it owns
// the user port with the PSX SNAC adapter pinout (ATT1/ATT2/CMD/CLK out,
// DAT/ACK in) and the core sees idle lines. Pad state is served back to the
// HPS which exposes the pads as regular input devices (menu included).
wire snac_att1_n, snac_att2_n, snac_clk, snac_cmd, snac_upd;
wire        snac_conn1, snac_conn2;
wire  [7:0] snac_id1, snac_id2;
wire [15:0] snac_btn1, snac_btn2;
wire  [7:0] snac_lx1, snac_ly1, snac_rx1, snac_ry1;
wire  [7:0] snac_lx2, snac_ly2, snac_rx2, snac_ry2;

psx_snac_pad snac_pad
(
\t.clk(FPGA_CLK2_50),
\t.enable(snac_en),

\t.att1_n(snac_att1_n),
\t.att2_n(snac_att2_n),
\t.clk_pad(snac_clk),
\t.cmd_pad(snac_cmd),
\t.dat_pad(USER_IO[4]),
\t.ack_pad(USER_IO[3]),

\t.upd(snac_upd),

\t.pad1_connected(snac_conn1),
\t.pad1_id(snac_id1),
\t.pad1_buttons(snac_btn1),
\t.pad1_lx(snac_lx1),
\t.pad1_ly(snac_ly1),
\t.pad1_rx(snac_rx1),
\t.pad1_ry(snac_ry1),

\t.pad2_connected(snac_conn2),
\t.pad2_id(snac_id2),
\t.pad2_buttons(snac_btn2),
\t.pad2_lx(snac_lx2),
\t.pad2_ly(snac_ly2),
\t.pad2_rx(snac_rx2),
\t.pad2_ry(snac_ry2)
);

//                               [6]   [5]       [4]DAT [3]ACK [2]       [1]          [0]
wire [6:0] user_drv = snac_en ? {1'b1, snac_clk, 1'b1,  1'b1,  snac_cmd, snac_att1_n, snac_att2_n} : user_out;
`else
wire [6:0] user_drv = user_out;
`endif
"""


class PatchError(Exception):
    pass


def patch_sys_top(path: Path) -> str:
    src = path.read_text()

    if "psx_snac_pad" in src:
        return "already patched"

    # 1. snapshot registers, right after the io_dout_sys declaration
    m = re.search(r"^reg \[15:0\] io_dout_sys;\s*$", src, re.M)
    if not m:
        raise PatchError("no io_dout_sys declaration")
    src = src[: m.end()] + "\n" + SNAPSHOT + src[m.end():]

    # 2. status byte for the probe, alongside the other single-word replies
    anchors = [
        r"^\t*if\(io_din\[7:0\] == 'h42\) io_dout_sys <= \{1'b1, frame_cnt\};\s*$",
        r"^\t*if\(io_din\[7:0\] == 'h2F\) io_dout_sys <= 1;\s*$",
        r"^\t*if\(io_din\[7:0\] == 'h20\) io_dout_sys <= 'b11;\s*$",
    ]
    for a in anchors:
        m = re.search(a, src, re.M)
        if m:
            src = src[: m.end()] + "\n" + STATUS_LINE.rstrip("\n") + src[m.end():]
            break
    else:
        raise PatchError("no anchor for the status reply")

    # 3. the 0x45 command handler, among the other command handlers
    m = re.search(r"^\t*if\(cmd == 'h25\).*$", src, re.M)
    if not m:
        raise PatchError("no anchor for the command handler")
    src = src[: m.end()] + "\n" + CMD_BLOCK.rstrip("\n") + src[m.end():]

    # 4. user port: insert the reader and mux it in front of the core
    m = re.search(r"^assign USER_IO\[0\] =.*$", src, re.M)
    if not m:
        raise PatchError("no USER_IO assignments")
    src = src[: m.start()] + INSTANCE.lstrip("\n") + "\n" + src[m.start():]

    src, n = re.subn(r"(assign USER_IO\[\d\] = [^;]*?)user_out(\[\d\])", r"\1user_drv\2", src)
    if n != 7:
        raise PatchError(f"expected 7 USER_IO drivers, rewrote {n}")

    # core must not see the pad traffic while the reader owns the port
    src, n = re.subn(r"^(assign user_in\[(\d)\] =\s*)(.*USER_IO\[\2\];)$",
                     r"\1snac_en | \3", src, flags=re.M)
    if n != 7:
        raise PatchError(f"expected 7 user_in assignments, rewrote {n}")

    path.write_text(src)
    return "patched"


def patch_qip(path: Path) -> str:
    if not path.exists():
        raise PatchError("no sys.qip")
    txt = path.read_text()
    if "psx_snac_pad.sv" in txt:
        return "already listed"
    line = "set_global_assignment -name SYSTEMVERILOG_FILE [file join $::quartus(qip_path) psx_snac_pad.sv ]\n"
    if not txt.endswith("\n"):
        txt += "\n"
    path.write_text(txt + line)
    return "listed"


def main() -> int:
    core = Path(sys.argv[1])
    module = Path(sys.argv[2])

    sys_dir = core / "sys"
    if not sys_dir.is_dir():
        print(f"FAIL {core.name}: no sys/ folder")
        return 1

    try:
        shutil.copy(module, sys_dir / "psx_snac_pad.sv")
        a = patch_sys_top(sys_dir / "sys_top.v")
        b = patch_qip(sys_dir / "sys.qip")
    except PatchError as e:
        print(f"FAIL {core.name}: {e}")
        return 1

    print(f"OK {core.name}: sys_top {a}, qip {b}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
