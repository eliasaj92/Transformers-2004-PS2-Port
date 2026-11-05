# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a saved in-process native mesh test without launching anything."""
import argparse
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image

from prepare_native_mesh import digest, require
from verify_native_mesh import verify


def verify_host(asset, work):
    output = work / "host-probe"
    log = (work / "stderr.log").read_text(errors="replace")
    route = json.loads((work / "script_result.json").read_text())
    run = json.loads((work / "host-run.json").read_text())
    host = json.loads((output / "host.json").read_text())
    probe = json.loads((output / "native/probe.json").read_text())
    begin = re.findall(r"\[script\] mark native_probe_begin vs=(\d+)", log)
    end = re.findall(r"\[script\] mark native_probe_end vs=(\d+)", log)
    require(route["result"] == "PASS" and run["exit_code"] == 0 and run["run_limit"] == 1, "Route did not pass")
    require(len(begin) == len(end) == 1 and int(begin[0]) < int(end[0]), "Missing playable-level interval")
    require(log.count("[native-host-probe] begin ") == 1 and log.count("[native-host-probe] resumed ") == 1,
            "Missing or repeated host native pass")
    # Script marks count EE fields; GSBackendHW::ticks counts delivered renderer
    # vsync calls. They are not the same clock and the GS worker may lag the EE.
    # Arming occurs after the script mark; verify that causal ordering directly.
    require(log.index("[script] mark native_probe_begin ") < log.index("[native-host-probe] begin ") <
            log.index("[native-host-probe] resumed ") < log.index("[script] mark native_probe_end "),
            "Native pass did not occur between the in-level route markers")
    require(host["shared_device"] and host["shared_queue"] and probe["host_device"] and probe["debug_layer"],
            "Native test did not use the validated host graphics device")
    require(host["resumed_tick"] - host["first_tick"] >= 60, "Gameplay did not continue after native rendering")
    require(run["window_verified"] and run["armed_after_player_ready"] and run["protected_inputs_unchanged"],
            "Window, level readiness or input preservation failed")
    before = (output / "host-before.rgba").read_bytes()
    after = (output / "host-after.rgba").read_bytes()
    require(before == after and before, "Native pass changed the completed game image")
    for name in ("host-before", "host-after", "host-resumed"):
        meta = json.loads((output / (name + ".json")).read_text())
        data = (output / (name + ".rgba")).read_bytes()
        require(len(meta["frames"]) == 1 and len(data) == meta["width"] * meta["height"] * 4, "Incomplete host image")
        rgba = np.frombuffer(data, dtype=np.uint8).reshape(meta["height"], meta["width"], 4)
        require(float(rgba[:, :, :3].std()) > 10, "Host image is blank")
        Image.fromarray(rgba).save(output / (name + ".png"))
    verify(asset, output / "native")
    record = {"pass": True, "gameplay_begin_ee_vs": int(begin[0]), "native_present_tick": host["first_tick"],
              "gameplay_resumed_present_tick": host["resumed_tick"], "gameplay_end_ee_vs": int(end[0]),
              "host_pixels_preserved": True, "host_image_sha256": digest(before),
              "host_image_bytes": len(before), "native_frames": 4, "native_draws": 5,
              "scope": "Native pass on the enhanced game's device/queue; not a world-mesh replacement or benchmark"}
    (work / "host-verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    verify_host(args.asset.resolve(), args.work.resolve())
