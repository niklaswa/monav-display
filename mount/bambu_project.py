"""Turns the geometry-only 3MFs from make_3mf.py into Bambu Studio projects for the A1, with the
print settings below baked in. Run make_3mf.py first; needs Bambu Studio in /Applications."""
import json, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).parent
STUDIO = Path("/Applications/BambuStudio.app/Contents")
PROFILES = STUDIO / "Resources/profiles/BBL"
PRINTER = "Bambu Lab A1 0.4 nozzle"
PROCESS = "0.20mm Standard @BBL A1"
FILAMENT = "eSUN PLA+ @BBL A1"
PLATE = "Textured PEI Plate"  # the A1's stock plate; the CLI would write Cool Plate (35 °C bed)
CHANGES = {
    # travel round the holes instead of across them, otherwise the knob's bore fills with strings
    "reduce_crossing_wall": "1",
    # solid socket fingers
    "wall_loops": "3",
}


def resolve(kind, name):
    """A system profile with its inherits chain merged in. The CLI doesn't follow 'inherits' and
    takes built-in defaults (200 °C nozzle, no wipe) for everything a parent profile sets."""
    out = {}
    while name:
        profile = json.loads((PROFILES / kind / f"{name}.json").read_text())
        for k, v in profile.items():
            out.setdefault(k, v)
        name = profile.get("inherits")
    out.pop("inherits", None)
    return out


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    machine, process, filament = tmp / "machine.json", tmp / "process.json", tmp / "filament.json"
    machine.write_text(json.dumps(resolve("machine", PRINTER)))
    process.write_text(json.dumps({**resolve("process", PROCESS), **CHANGES}))
    filament.write_text(json.dumps(resolve("filament", FILAMENT)))
    for plate in ("monav-mount.3mf", "monav-test.3mf", "monav-socket.3mf"):
        out = tmp / plate
        run = subprocess.run([STUDIO / "MacOS/BambuStudio", "--arrange", "0", "--orient", "0",
                              "--load-settings", f"{machine};{process}", "--load-filaments", filament,
                              "--export-3mf", out, HERE / plate], cwd=tmp, capture_output=True, text=True)
        if run.returncode:
            sys.exit(f"{plate}: Bambu Studio exited with {run.returncode}\n" + "\n".join(
                l for l in run.stdout.splitlines() if "[error]" in l))
        # mark the changes against the system process, so Studio shows the preset as modified
        with zipfile.ZipFile(out) as zin, zipfile.ZipFile(HERE / plate, "w", zipfile.ZIP_DEFLATED) as zout:
            for item in zin.infolist():
                data = zin.read(item)
                if item.filename == "Metadata/project_settings.config":
                    config = json.loads(data)
                    config["different_settings_to_system"][0] = ";".join(sorted(CHANGES))
                    config["curr_bed_type"] = PLATE
                    data = json.dumps(config, indent=4).encode()
                zout.writestr(item, data)
        print(plate, (HERE / plate).stat().st_size // 1024, "KB")
