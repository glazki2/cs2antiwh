"""Verify that Windows outputs are x64 release binaries without debug CRT imports."""

from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
  paths = (
    ROOT / "build/cs2glaz/windows-x86_64/cs2glaz.dll",
    ROOT / "build/cs2glaz_baker/windows-x86_64/cs2glaz_baker.exe",
    ROOT / "build/cs2glaz_tests/windows-x86_64/cs2glaz_tests.exe",
  )
  forbidden = ("VCRUNTIME140D", "MSVCP140D", "ucrtbased")
  for path in paths:
    if not path.is_file():
      raise SystemExit(f"missing Windows artifact: {path}")
    output = subprocess.check_output(
      ["dumpbin", "/headers", "/dependents", str(path)],
      text=True, errors="replace"
    )
    if "machine (x64)" not in output:
      raise SystemExit(f"not an x64 Windows artifact: {path}")
    imported = [name for name in forbidden if name.lower() in output.lower()]
    if imported:
      raise SystemExit(f"debug runtime import in {path}: {', '.join(imported)}")
    print(path.relative_to(ROOT), "x64 release imports verified")


if __name__ == "__main__":
  main()
