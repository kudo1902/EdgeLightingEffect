#!/usr/bin/env python3
"""Rewrite the data behind docs/neon-resolution-scale-comparison.html.

    update-page.py --after head-1.json [head-2.json ...] --before old-1.json [old-2.json ...]
                   [--page docs/neon-resolution-scale-comparison.html]

Each --after / --before file is one `neon-scale-check generate` run. Several
runs of the same build are merged: every metric must agree exactly between
them (renders are deterministic), and each timing takes the minimum, which is
how the page's "min over interleaved runs" figures are made. Interleave the
runs of the two builds when timing.

Only the page's `const DATA = ...;` line is replaced. Scene titles, blurbs,
crop rects and cross-section columns are kept from the page as it is. The
per-scene NOTES, the findings and the prose quote numbers and are written by
hand: re-read them against the new data.
"""
import argparse
import json
import re
import sys

TAGS = ["s1000", "s750", "s500", "s350", "s250", "s125"]


def merge(paths, label):
    runs = [json.load(open(p)) for p in paths]
    base = runs[0]
    gpus = {r.get("gpu", "") for r in runs}
    if len(gpus) > 1:
        sys.exit(f"{label}: runs come from different GPUs: {sorted(gpus)}")
    for r in runs[1:]:
        for sid, sc in base["scenarios"].items():
            if r["scenarios"][sid]["metrics"] != sc["metrics"]:
                sys.exit(f"{label}: metrics for {sid} differ between runs - renders should be deterministic")
    for sid, sc in base["scenarios"].items():
        for t in TAGS:
            times = [r["scenarios"][sid]["ms"][t] for r in runs if r["scenarios"][sid]["ms"][t] >= 0]
            sc["ms"][t] = min(times) if times else -1
    return base


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--page", default="docs/neon-resolution-scale-comparison.html")
    ap.add_argument("--after", nargs="+", required=True, help="runs of the build the page describes")
    ap.add_argument("--before", nargs="+", required=True, help="runs of the build it is compared against")
    args = ap.parse_args()

    after, before = merge(args.after, "--after"), merge(args.before, "--before")
    if after.get("gpu") != before.get("gpu"):
        sys.exit("--after and --before were timed on different GPUs; the page compares them directly")

    html = open(args.page).read()
    m = re.search(r"const DATA = (\{.*?\});\n", html, re.S)
    if not m:
        sys.exit(f"no `const DATA = ...;` line in {args.page}")
    old = json.loads(m.group(1))

    gpu = after.get("gpu", old.get("gpu", "")).replace(" OpenGL Engine", "")
    data = {"width": old["width"], "height": old["height"], "gpu": gpu, "scales": old["scales"],
            "scenarios": [], "sweep": {}, "sweepBefore": {}}
    for o in old["scenarios"]:
        sid = o["id"]
        a, b = after["scenarios"][sid], before["scenarios"][sid]
        data["scenarios"].append({
            "id": sid, "title": o["title"], "blurb": o["blurb"], "crop": o["crop"],
            "profile": {"x": o["profile"]["x"], "y0": o["profile"]["y0"], "edgeY": o["profile"]["edgeY"], "rows": a["profile"]},
            "metrics": a["metrics"],
            "ms": {t: round(a["ms"][t], 4) for t in TAGS},
            "before": {"metrics": {t: {"p99": b["metrics"][t]["p99"], "maxDelta": b["metrics"][t]["maxDelta"]} for t in TAGS[1:]},
                       "ms": {t: round(b["ms"][t], 4) for t in TAGS}},
        })
    data["sweep"] = {"step": after["sweep"]["step"], "steps": after["sweep"]["steps"], "series": after["sweep"]["series"]}
    for sc in after["sweep"]["series"]:
        series = before["sweep"]["series"][sc]
        data["sweepBefore"][sc] = {t: {"wobble": round(max(abs(v) for v in series[t]["centroidErr"]), 4),
                                       "width": round(sum(series[t]["fwhm"]) / len(series[t]["fwhm"]), 3)} for t in TAGS}

    line = "const DATA = " + json.dumps(data, separators=(",", ":")) + ";\n"
    open(args.page, "w").write(html[:m.start()] + line + html[m.end():])

    print(f"updated {args.page} ({gpu})")
    for sc in data["scenarios"]:
        worst = max(sc["metrics"][t]["maxDelta"] for t in TAGS[1:])
        print(f"  {sc['id']:14s} worst max {worst:3d}   0.25: {sc['ms']['s1000'] / sc['ms']['s250']:.1f}x faster than 1.0"
              f" (before {sc['before']['ms']['s1000'] / sc['before']['ms']['s250']:.1f}x)")
    print("Now re-read the page's NOTES, findings and prose: they quote these numbers by hand.")


if __name__ == "__main__":
    main()
