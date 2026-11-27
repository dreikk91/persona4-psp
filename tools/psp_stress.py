#!/usr/bin/env python
"""Field-reload stress run in PPSSPP with freed heap blocks poisoned (b529, issue #24).

Models freed at a field change and new ones landing at the same addresses is where port tables that keep game
pointers go stale (#24: the lazy-frame table read a dead hierarchy). This loads Heaven (tools/routes/f63.txt), turns
on p4_poison_check (port/main.c: every freed block filled with 0xFFEEFEEF), then warps between fields through
p4_scene_req (port/scene_menu.c) every --every seconds. A stale read then faults at once instead of on the tester's
PSP. Look for CRASH in the log and "Invalid address" / "Jump to invalid address" (other than 0) in ppsspp.log.

  PSPDEV=... PPSSPP_DIR=... python tools/psp_stress.py build/psplink_b529 --warps 12
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x08804000
FIELDS = ((63, 1, 0), (9, 4, 0), (28, 2, 0))  # dungeon corridor (181 models), Junes, Heaven's field


def symbol(elf, name):
    nm = Path(os.environ["PSPDEV"]) / "bin" / "psp-nm"
    for line in subprocess.run([str(nm), str(elf)], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return BASE + int(parts[0], 16)
    raise SystemExit(f"{name} not in {elf} (a profiling build is needed)")


def presses(req, poison, warps, every, start=35.0):
    out = [f"25:{poison:x}=1"]
    t = start
    for k in range(warps):
        field, room, entry = FIELDS[k % len(FIELDS)]
        out += [f"{t:g}:{req + 4:x}={field}", f"{t + 0.5:g}:{req + 8:x}={room}", f"{t + 1:g}:{req + 12:x}={entry}",
                f"{t + 2:g}:{req:x}=1"]  # the kind word last: the request starts when it is set
        t += every
    return out, t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path, help="profiling build folder (psp_hw_debug.py --out), its p4.elf")
    ap.add_argument("--warps", type=int, default=12)
    ap.add_argument("--every", type=float, default=20.0, help="seconds between field requests")
    args = ap.parse_args()
    elf = args.build / "p4.elf"
    for name in ("EBOOT.PBP", "p4data.hwdbg.bin", "p4str.bin"):
        shutil.copy2(args.build / name, ROOT / "build" / "psp_dist" / "PSP" / "GAME" / "P4" / name)
    seq, end = presses(symbol(elf, "p4_scene_req"), symbol(elf, "p4_poison_check"), args.warps, args.every)
    env = dict(os.environ, P4_ROUTE=str(ROOT / "tools" / "routes" / "f63.txt"), P4_PRESS=" ".join(seq))
    print(f"{args.warps} warps, {end + 15:g} s", flush=True)
    sys.exit(subprocess.run([sys.executable, str(ROOT / "tools" / "psp_run.py"), f"{end + 15:g}"], env=env).returncode)


if __name__ == "__main__":
    main()
