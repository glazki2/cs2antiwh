"""Configure, build, test, verify, and package one CS2GLAZ platform."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def run(*args: str, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
  subprocess.run(args, cwd=cwd, env=env, check=True)


def main() -> None:
  parser = argparse.ArgumentParser()
  parser.add_argument("--platform", choices=("windows", "linux"), required=True)
  # Accepted for older callers; the Visibility Studio was removed with VRF.
  parser.add_argument("--skip-studio", action="store_true")
  args = parser.parse_args()

  python = sys.executable
  dependencies = ROOT / ".build-deps" / args.platform
  run(python, str(ROOT / "scripts/bootstrap.py"), "--platform", args.platform)
  build = ROOT / ("build" if args.platform == "windows" else "build-linux")
  build.mkdir(exist_ok=True)
  env = os.environ.copy()
  env["PYTHONPATH"] = str(dependencies / "ambuild")
  if args.platform == "linux":
    env.update(CC="gcc", CXX="g++")
  run(
    python, str(ROOT / "configure.py"),
    "--hl2sdk-root", str(dependencies),
    "--hl2sdk-manifests", str(dependencies / "hl2sdk-manifests"),
    "--mms-path", str(dependencies / "metamod-source"),
    cwd=build, env=env,
  )
  run(python, "-c", "from ambuild2.run import cli_run; cli_run()", cwd=build, env=env)

  target = "windows-x86_64" if args.platform == "windows" else "linux-x86_64"
  executable = build / "cs2glaz_tests" / target / (
    "cs2glaz_tests.exe" if args.platform == "windows" else "cs2glaz_tests"
  )
  run(str(executable))
  run(python, "-m", "unittest", "discover", "-v", "tests")
  checker = "check_windows_imports.py" if args.platform == "windows" \
    else "check_steamrt3_abi.py"
  run(python, str(ROOT / "scripts" / checker))
  run(python, str(ROOT / "package.py"), target)

  archive = ROOT / "packages" / f"cs2glaz-{(ROOT / 'VERSION').read_text().strip()}-{target}.zip"
  if not archive.is_file() or archive.stat().st_size == 0:
    raise SystemExit(f"package was not produced: {archive}")
  print(f"Verified package: {archive}")


if __name__ == "__main__":
  main()
