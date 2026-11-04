"""Summarize an enhanced in-level native-render draw-state investigation."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re


def summarize(run):
    run = run.resolve()
    root = Path(__file__).resolve().parents[1]
    if run == root or root in run.parents:
        raise ValueError("Keep game logs and traces outside the source checkout")
    result = json.loads((run / "script_result.json").read_text())
    if result.get("result") != "PASS":
        raise ValueError("Enhanced level route did not pass")
    env = dict(line.split("=", 1) for line in (run / "env.txt").read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    for key, expected in {"PS2X_VU1_AOT": "1", "PS2X_VU1_AOT_STATIC": "1", "PS2X_HWGS_DIRECT": "1",
                          "PS2X_HIDDEN_WINDOW": "0", "PS2X_TF_NATIVE60": "3"}.items():
        if env.get(key) != expected:
            raise ValueError(f"Required enhanced test setting is missing: {key}")
    log = (run / "stderr.log").read_text(errors="replace")
    begin = re.search(r"\[script\] mark perf_begin vs=(\d+)", log)
    end = re.search(r"\[script\] mark perf_end vs=(\d+)", log)
    final = re.search(r"\[hwgs:draw-trace\] fields=(\d+) range=(\d+):(\d+) rows=(\d+) capped=(\d+)", log)
    if not begin or not end or not final or final[5] != "0":
        raise ValueError("Missing route boundaries or complete trace summary")
    first, last, total = map(int, final.group(2, 3, 4))
    if not int(begin[1]) < first <= last < int(end[1]) or int(final[1]) <= last:
        raise ValueError("Trace does not lie within the completed level measurement interval")
    rows = list(csv.DictReader((run / "gpu-draws.csv").open(newline="")))
    if len(rows) != total or not rows:
        raise ValueError("Trace row count differs from completed producer summary")
    fields = Counter()
    plain = []
    previous = first
    for number, row in enumerate(rows, 1):
        field = int(row["field"])
        if int(row["draw"]) != number or not first <= field <= last or field < previous:
            raise ValueError("Draw trace has a gap, duplicate, or unordered row")
        previous = field
        fields[field] += 1
        if int(row["topology"]) == 2 and int(row["vs"]) & 0x70 == 0 and row["line_expand"] == "0":
            plain.append(row)
    if sorted(fields) != list(range(first, last + 1)):
        raise ValueError("A field has no observed draws; investigate before using this trace")
    features = ["depth", "palette", "feedback", "one_barrier", "full_barrier", "software_blend", "hardware_blend",
                "alpha_second_pass", "blend_multi_pass", "automatic_lod", "manual_lod", "fog", "date"]
    groups = Counter((row["vs"], row["ps_lo"], row["ps_hi"], row["depth_key"], row["color_mask"]) for row in plain)
    return {"fields": len(fields), "draws": total, "draws_per_field": total / len(fields),
            "ordinary_triangle_draws": len(plain), "ordinary_triangle_indices": sum(int(row["indices"]) for row in plain),
            "ordinary_triangle_textured": sum(int(row["tex_width"]) != 0 for row in plain),
            "ordinary_triangle_features": {key: sum(int(row[key]) != 0 for row in plain) for key in features},
            "ordinary_triangle_shader_groups": len(groups),
            "top_groups": [{"vs": key[0], "ps_lo": key[1], "ps_hi": key[2], "depth": key[3], "color_mask": key[4], "draws": count}
                           for key, count in groups.most_common(10)],
            "scope": "Read-only enhanced level draw metadata; no VU-family attribution, live offload, or FPS claim"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.run_directory), indent=2))
