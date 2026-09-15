#!/usr/bin/env python3
"""Summarise a mjpeg-soak.sh run directory into a short table.

Usage: soak-report.py <soak-dir>

Reports the things a long run is actually for: whether frame rate held, what
it cost in CPU and heat, whether memory trended, and whether the kernel said
anything new.
"""
import re
import sys
import pathlib


def main(d: pathlib.Path) -> int:
    def read(name):
        p = d / name
        return p.read_text(errors="replace") if p.exists() else ""

    print(f"=== {d.name} ===")
    print(read("meta.txt").strip())

    cap = read("capture.log")
    done = [l for l in cap.splitlines() if "DONE" in l]
    mins = [l for l in cap.splitlines() if re.match(r"^\[\s*\d+s\]", l)]
    print(f"\n--- capture ({len(mins)} per-minute samples) ---")
    for l in mins[:2] + (["  ..."] if len(mins) > 4 else []) + mins[-2:]:
        print(" ", l.strip())
    if done:
        print("\n ", done[-1].strip())

    rows = [l.split("\t") for l in read("samples.tsv").splitlines()[1:] if l.strip()]
    if rows:
        def col(i, f=float):
            out = []
            for r in rows:
                try:
                    out.append(f(r[i]))
                except (ValueError, IndexError):
                    pass
            return out

        cpu, mem, slab = col(1), col(2), col(3)
        kbps = col(6)
        temps = []
        for r in rows:
            if len(r) > 7:
                vals = [int(x) for x in r[7].split() if x.isdigit()]
                if vals:
                    temps.append(max(vals) / 1000.0)

        def line(name, v, unit="", fmt="{:.1f}"):
            if not v:
                return
            s = (fmt + " / " + fmt + " / " + fmt).format(min(v), sum(v) / len(v), max(v))
            print(f"  {name:<22} {s} {unit}   (min/avg/max)")

        print(f"\n--- system ({len(rows)} samples) ---")
        line("CPU", cpu, "%")
        line("temp (hottest zone)", temps, "C")
        line("tx", kbps, "kbit/s", "{:.0f}")
        if mem:
            drift = (mem[-1] - mem[0]) / 1024.0
            print(f"  {'MemAvailable':<22} {mem[0]/1024:.0f} -> {mem[-1]/1024:.0f} MB   "
                  f"drift {drift:+.0f} MB")
        if slab:
            print(f"  {'Slab':<22} {slab[0]/1024:.0f} -> {slab[-1]/1024:.0f} MB   "
                  f"drift {(slab[-1]-slab[0])/1024:+.1f} MB")

    dd = read("client-dd.txt")
    m = re.findall(r"(\d+) bytes.*?copied, ([\d.]+) s", dd, re.S)
    if m:
        tot = sum(int(b) for b, _ in m)
        secs = sum(float(s) for _, s in m)
        print(f"\n--- encoded stream ---")
        print(f"  delivered {tot/1e6:.1f} MB in {secs:.0f}s = {tot*8/secs/1e6:.2f} Mbit/s")

    delta = read("dmesg-delta.txt")
    new = [l[2:] for l in delta.splitlines() if l.startswith("> ")]
    print(f"\n--- dmesg delta: {len(new)} new lines ---")
    bad = [l for l in new if re.search(r"Oops|WARNING|BUG:|Call trace|error|fail", l, re.I)]
    for l in (bad[:10] if bad else new[:5]):
        print("  ", l.strip()[:120])
    if not new:
        print("   (kernel said nothing new)")
    elif not bad:
        print("   (nothing matching Oops/WARNING/BUG/error/fail)")

    ff = [l for l in read("ffmpeg.log").splitlines() if l.strip()]
    if ff:
        print(f"\n--- ffmpeg said ({len(ff)} lines) ---")
        for l in ff[:5]:
            print("  ", l.strip()[:120])
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(pathlib.Path(sys.argv[1])))
