"""Exercise spring configuration through the public compiler and game loader."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent
BONES = ["ManeRoot", "ManeMid", "ManeTip"]


def chain(**overrides):
    return {"name": "Mane", "bones": BONES, **overrides}


def block(kind, values):
    return f"{kind}[JSON] (\n{json.dumps(values, ensure_ascii=True)}\n)\n"


def model(configuration, extra=""):
    return f'''zsharp = type.script:model
noticed model Mane[] (
 parent: "Chain.fbx":
 body: static:
 collider: none:
 {configuration}
 {extra}
)
'''


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("binary", type=Path)
    options = arguments.parse_args()
    binary = options.binary.resolve(strict=True)
    checks = 0
    with tempfile.TemporaryDirectory(prefix="zsharp-spring-parser-") as temporary:
        directory = Path(temporary)
        project = directory / "project with spaces"
        # Copy the owned FBX and public project rather than testing another
        # implementation of the parser or mocking imported skeletons.
        shutil.copytree(FIXTURE, project,
                        ignore=shutil.ignore_patterns("parser-tests.py", "__pycache__"))
        environment = os.environ.copy()
        for key in tuple(environment):
            if key.startswith("ZSHARP_MAVEN_") or key == "ZSHARP_JVM_CLASSPATH":
                environment.pop(key)
        for key, folder in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                            ("XDG_DATA_HOME", "data")):
            environment[key] = str(directory / folder)
        settings = (project / "project.zsettings").read_text(encoding="utf-8")
        main_source = '''zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  number Camera = Main.cameraX:
 )
)
'''
        (project / "Main.zsharp").write_text(main_source, encoding="utf-8")
        original_scene = (project / "Main.zscene").read_text(encoding="utf-8")

        def check(name, configuration, expected=None, *, extra="", version=None,
                  scene=None, additional=None):
            nonlocal checks
            (project / "Mane.zmodel").write_text(model(configuration, extra), encoding="utf-8")
            (project / "project.zsettings").write_text(
                settings if version is None else settings.replace(
                    "ZSharp: [1.4.1.0]:", f"ZSharp: [{version}]:"),
                encoding="utf-8")
            (project / "Main.zscene").write_text(
                original_scene if scene is None else scene, encoding="utf-8")
            if additional:
                for filename, content in additional.items():
                    (project / filename).write_text(content, encoding="utf-8")
            try:
                result = subprocess.run([str(binary), "check", str(project / "Main.zsharp")],
                                        cwd=project, env=environment, text=True,
                                        capture_output=True, timeout=30)
            finally:
                if additional:
                    for filename in additional:
                        (project / filename).unlink()
            output = result.stdout + result.stderr
            if result.returncode < 0:
                raise AssertionError(f"{name}: compiler crashed ({result.returncode})\n{output}")
            if expected is None:
                if result.returncode != 0:
                    raise AssertionError(f"{name}: valid project rejected\n{output}")
            elif result.returncode == 0 or (expected and expected not in output):
                raise AssertionError(f"{name}: expected rejection containing {expected!r}\n{output}")
            checks += 1

        valid = block("springBones", [chain()])
        check("defaults", valid)
        check("disabled model", valid, extra="springBonesEnabled: dead:")
        check("all parameters and both collider shapes",
              block("springBones", [chain(stiffness=30, damping=5,
                    gravity={"z": 0, "x": 1, "y": -9.8}, radius=0.03, maxAngle=75)])
              + block("springColliders", [
                  {"name": "Sphere", "bone": "ManeRoot"},
                  {"name": "Capsule", "bone": "ManeMid",
                   "offset": {"x": 0, "y": 0, "z": 0},
                   "tail": {"z": 0, "y": 0.4, "x": 0}, "radius": 0.1},
              ]))
        check("JSON escaped names", 'springBones[JSON] ([{"name":"Ma\\u006ee",'
              '"bones":["Mane\\u0052oot","ManeMid","ManeTip"]}])')
        check("supported numeric boundaries",
              block("springBones", [chain(stiffness=10000, damping=1000,
                    gravity={"x": -10000, "y": 10000, "z": 0},
                    radius=0, maxAngle=180)]))
        check("old language without opt-in", "", version="1.4.0.0")
        check("feature version gate", valid, "1.4.1.0", version="1.4.0.0")
        check("enabled field version gate", "", "1.4.1.0",
              extra="springBonesEnabled: alive:", version="1.4.0.0")
        check("duplicate enabled field", valid, "duplicate",
              extra="springBonesEnabled: alive:\nspringBonesEnabled: dead:")
        check("invalid enabled status", valid, "alive or dead",
              extra="springBonesEnabled: true:")
        check("duplicate springBones block", valid + valid, "duplicate")
        colliders = block("springColliders", [{"name": "Head", "bone": "ManeRoot"}])
        check("duplicate springColliders block", valid + colliders + colliders, "duplicate")
        check("colliders without chains", colliders, "require springBones")
        check("empty chains", block("springBones", []), "must not be empty")
        check("empty colliders", valid + block("springColliders", []), "must not be empty")
        check("unknown chain field", block("springBones", [chain(misspelled=1)]), "unknown")
        check("duplicate chain field",
              'springBones[JSON] ([{"name":"Mane","bones":["ManeRoot","ManeMid"],'
              '"stiffness":20,"stiffness":30}])', "duplicate")
        check("missing chain name", block("springBones", [{"bones": BONES}]), "require name")
        check("missing chain bones", block("springBones", [{"name": "Mane"}]), "require name")
        check("duplicate chain names", block("springBones", [
              {"name": "Same", "bones": ["ManeRoot", "ManeMid"]},
              {"name": "Same", "bones": ["OtherRoot", "OtherTip"]}]), "duplicate")
        check("repeated chain bone", block("springBones", [chain(bones=["ManeRoot", "ManeRoot"])]), "unique")
        check("overlapping chains", block("springBones", [
              chain(bones=["ManeRoot", "ManeMid"]),
              {"name": "Other", "bones": ["ManeMid", "ManeTip"]}]), "unique")
        check("single bone", block("springBones", [chain(bones=["ManeRoot"])]), "at least two")
        check("chain bone cap", block("springBones", [chain(bones=[f"B{i}" for i in range(65)])]), "limit")
        check("chain cap", block("springBones", [
              {"name": f"C{i}", "bones": [f"R{i}", f"T{i}"]} for i in range(33)]), "too many")
        check("total bone cap", block("springBones", [
              {"name": f"C{i}", "bones": [f"C{i}B{j}" for j in range(55)]}
              for i in range(5)]), "limit")
        check("oversized name", block("springBones", [chain(name="x" * 256)]), "255")
        check("empty name", block("springBones", [chain(name="")]), "255")
        check("escaped NUL", block("springBones", [chain(name="bad\x00name")]), "control")
        check("negative stiffness", block("springBones", [chain(stiffness=-1)]), "range")
        check("excess damping", block("springBones", [chain(damping=1001)]), "range")
        check("excess radius", block("springBones", [chain(radius=1001)]), "range")
        check("excess angle", block("springBones", [chain(maxAngle=181)]), "range")
        check("nonnumeric scalar", block("springBones", [chain(stiffness="20")]), "requires a number")
        check("nonfinite scalar", block("springBones", [chain(stiffness=float("nan"))]), "requires a number")
        check("nonfinite gravity", block("springBones", [chain(
              gravity={"x": 0, "y": float("inf"), "z": 0})]), "requires a number")
        check("gravity bound", block("springBones", [chain(
              gravity={"x": 0, "y": -10001, "z": 0})]), "range")
        check("missing vector axis", block("springBones", [chain(gravity={"y": -9.8})]), "require x, y, z")
        check("unknown vector axis", block("springBones", [chain(gravity={"x": 0, "y": 0, "w": 0})]), "unknown")
        check("duplicate vector axis",
              'springBones[JSON] ([{"name":"Mane","bones":["ManeRoot","ManeMid"],'
              '"gravity":{"x":0,"y":0,"z":0,"x":1}}])', "duplicate")
        check("unknown collider field", valid + block("springColliders", [
              {"name": "Head", "bone": "ManeRoot", "mass": 1}]), "unknown")
        check("duplicate collider name", valid + block("springColliders", [
              {"name": "Head", "bone": "ManeRoot"}, {"name": "Head", "bone": "ManeMid"}]), "duplicate")
        check("missing collider bone", valid + block("springColliders", [{"name": "Head"}]), "require name, bone")
        check("nonpositive collider radius", valid + block("springColliders", [
              {"name": "Head", "bone": "ManeRoot", "radius": 0}]), "positive radius")
        check("collider cap", valid + block("springColliders", [
              {"name": f"C{i}", "bone": "ManeRoot"} for i in range(65)]), "too many")
        check("collider offset cap", valid + block("springColliders", [
              {"name": "Head", "bone": "ManeRoot",
               "offset": {"x": 1000001, "y": 0, "z": 0}}]), "range")
        check("missing FBX bone", block("springBones", [chain(bones=["Missing", "ManeTip"])]), "missing")
        check("non-adjacent chain", block("springBones", [chain(bones=["ManeRoot", "ManeTip"])]), "direct parent/child")
        check("missing collider FBX bone", valid + block("springColliders", [
              {"name": "Head", "bone": "Missing"}]), "missing")
        unplaced_scene = '''zsharp = type.scene
noticed scene Main[] (
 scene Main ( cameraZ: 8: )
 objects[JSON] ( [] )
)
'''
        check("unplaced invalid definition still validated",
              block("springBones", [chain(bones=["Missing", "ManeTip"])]),
              "missing", scene=unplaced_scene)
        check("AI inherits model springs", valid, additional={
              "Companion.zai": '''zsharp = type.ai
noticed ai Companion[] ( model: "Mane.zmodel": )
'''}, scene='''zsharp = type.scene
noticed scene Main[] (
 scene Main ( cameraZ: 8: )
 objects[JSON] ( [{"id":"Companion","name":"Companion","location":{"x":0,"y":0,"z":0}}] )
)
''')
        check("AI cannot declare spring blocks", valid, "expected ':'", additional={
              "Companion.zai": '''zsharp = type.ai
noticed ai Companion[] (
 model: "Mane.zmodel":
 springBones[JSON] ([{"name":"Other","bones":["ManeRoot","ManeMid"]}])
)
'''})
        check("AI model cannot reference itself", valid, "loaded .zmodel", additional={
              "Companion.zai": '''zsharp = type.ai
noticed ai Companion[] ( model: "Companion.zmodel": )
'''})
    print(f"Spring parser and public-loader regressions passed ({checks} checks).")


if __name__ == "__main__":
    main()
