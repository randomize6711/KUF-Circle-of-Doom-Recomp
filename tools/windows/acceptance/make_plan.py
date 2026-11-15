#!/usr/bin/env python3
"""The game scenarios of the Windows acceptance test (M17, docs/WINDOWS_TEST.md): plan.json for Run-Acceptance.ps1.

  make_plan.py OUT_DIR SCENARIO.toml ...

Each scenario becomes the game options tools/menu_test.py gives it on Linux (menu_test.game_args: the same
script, timed exit, storage answer, captures) plus what run_sandboxed.sh adds (headless prompts, audio pacing, the
1280x720 output), so the Windows run is checked on Linux by the same rules: menu_test.py --verify-only. Written to
OUT_DIR: plan.json, scripts/ (script files), fixtures/saves/<name>/ (saves a scenario starts with: copies of
build/m6-saves/..., the player's own test saves: the kit is private, never published), fixtures/pc_settings/.

A scenario's optional [windows] table (tests/windows/*.toml): display = "native" (the real monitor: no simulated
output size, the window as the settings say), env = {NAME = value}, alt_tab = [{after = "<log text>",
away_seconds = N}] (the harness presses Alt+Tab when the text appears in the log and again after N seconds).
Scenarios that need Linux helpers (copy_in) or damaged-save preparation are refused: not part of this test.
"""
import json
import os
import shutil
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import menu_test  # noqa: E402

PREFIX = "win"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out = os.path.abspath(sys.argv[1])
    os.makedirs(os.path.join(out, "scripts"), exist_ok=True)
    plan = []
    for path in sys.argv[2:]:
        with open(path, "rb") as f:
            doc = tomllib.load(f)
        sc, win = doc["scenario"], doc.get("windows", {})
        name = os.path.splitext(os.path.basename(path))[0]
        for unsupported in ("copy_in", "damage", "gdb"):
            if unsupported in sc:
                sys.exit(f"{path}: '{unsupported}' is not supported on Windows")
        entry = {"name": name, "run_id": f"{PREFIX}-{name}", "scenario": os.path.relpath(path, ROOT),
                 "description": sc["description"], "seconds": sc["seconds"], "timeout": sc["seconds"] + 90,
                 "display": win.get("display", "regression"), "env": win.get("env", {}),
                 "alt_tab": win.get("alt_tab", []), "expect_exit": sc.get("expect_exit", 0)}
        if "script_file" in sc:
            src = os.path.join(os.path.dirname(os.path.abspath(path)), sc["script_file"])
            items = menu_test.script_items(menu_test.file_script_text(open(src).read()))
            shutil.copyfile(src, os.path.join(out, "scripts", f"{name}.kufscript"))
            entry["script_file"] = f"scripts/{name}.kufscript"
        else:
            items = menu_test.script_items(sc["script"])
            entry["script_file"] = None
        entry["args"] = menu_test.game_args(sc, items, [])
        # run_sandboxed.sh's own options (the Windows harness adds the folders): prompts answered with their
        # defaults; audio paced (K14) unless the scenario is about audio; the simulated 1280x720 output in a
        # 1280x720 window for the regression scenarios (their images are checked as on Linux).
        extra = ["--headless=true"]
        if "SDL_AUDIO_DRIVER" not in entry["env"]:
            extra.append("--kuf_audio_pace=true")
        if entry["display"] != "native" and not any(a.startswith("--kuf_display_size=") for a in entry["args"]):
            extra += ["--kuf_display_size=1280x720", "--fullscreen=false"]
        entry["args"] = extra + entry["args"]
        if "saves_from_run" in sc:
            entry["saves"] = {"from_run": f"{PREFIX}-{sc['saves_from_run']}"}
        elif "saves_from_dir" in sc:
            src = os.path.realpath(os.path.join(ROOT, sc["saves_from_dir"]))
            if not src.startswith(os.path.join(ROOT, "build") + os.sep):
                sys.exit(f"{path}: saves_from_dir must be under build/")
            fx = os.path.basename(src)
            dest = os.path.join(out, "fixtures", "saves", fx)
            if not os.path.isdir(dest):
                shutil.copytree(src, dest)
                for d, _, fs in os.walk(dest):  # writable copies (the source may be read-only): the game writes saves
                    os.chmod(d, 0o755)
                    for f in fs:
                        os.chmod(os.path.join(d, f), 0o644)
            entry["saves"] = {"fixture": f"fixtures/saves/{fx}"}
        else:
            entry["saves"] = None
        if "pc_settings_from_run" in sc:
            entry["pc_settings"] = {"from_run": f"{PREFIX}-{sc['pc_settings_from_run']}"}
        elif "pc_settings_file" in sc:
            os.makedirs(os.path.join(out, "fixtures", "pc_settings"), exist_ok=True)
            fx = os.path.basename(sc["pc_settings_file"])
            shutil.copyfile(os.path.join(ROOT, sc["pc_settings_file"]), os.path.join(out, "fixtures", "pc_settings", fx))
            entry["pc_settings"] = {"file": f"fixtures/pc_settings/{fx}"}
        else:
            entry["pc_settings"] = None
        plan.append(entry)
    names = {e["run_id"] for e in plan}
    for e in plan:  # a chained scenario after the one it starts from
        for k in ("saves", "pc_settings"):
            dep = (e[k] or {}).get("from_run")
            if dep and dep not in names:
                sys.exit(f"{e['scenario']}: needs {dep} in the same plan")
    with open(os.path.join(out, "plan.json"), "w", encoding="utf-8") as f:
        json.dump({"prefix": PREFIX, "scenarios": plan}, f, indent=1, ensure_ascii=False)
    total = sum(e["seconds"] for e in plan)
    print(f"{len(plan)} scenarios, {total // 60} min of play: {os.path.join(out, 'plan.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
