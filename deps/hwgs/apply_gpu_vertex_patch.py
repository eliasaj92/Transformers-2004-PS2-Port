"""Apply the opt-in native vertex interface to the pinned private PCSX2 checkout."""
from pathlib import Path
import argparse
import json
import subprocess


def apply(source: Path):
    here = Path(__file__).resolve().parent
    expected = json.loads((here / "dependencies.json").read_text())["pcsx2_revision"]
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != expected:
        raise ValueError("The GPU vertex patch requires the pinned PCSX2 revision")
    patch = here / "pcsx2-gpu-vertices.patch"
    command = ["git", "-C", str(source), "apply"]
    already = subprocess.run(command + ["--reverse", "--check", str(patch)], capture_output=True)
    if already.returncode == 0:
        print("Native GPU vertex patch already applied")
        return
    subprocess.run(command + ["--check", str(patch)], check=True)
    subprocess.run(command + [str(patch)], check=True)
    print("Applied native GPU vertex patch")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    apply(parser.parse_args().source.resolve())
