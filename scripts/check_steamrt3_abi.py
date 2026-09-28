"""Reject Linux artifacts requiring symbols newer than Steam Runtime 3."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LIMITS = {"GLIBC": (2, 31), "GLIBCXX": (3, 4, 28), "CXXABI": (1, 3, 12)}


def main() -> None:
  paths = [
    ROOT / "build-linux/cs2glaz/linux-x86_64/cs2glaz.so",
    ROOT / "build-linux/cs2glaz_baker/linux-x86_64/cs2glaz_baker",
    ROOT / "build-linux/cs2glaz_tests/linux-x86_64/cs2glaz_tests",
  ]
  failures: list[str] = []
  for path in paths:
    if not path.is_file():
      continue
    with path.open("rb") as stream:
      magic = stream.read(4)
    if magic != b"\x7fELF":
      continue
    versions = subprocess.check_output(
      ["readelf", "--version-info", str(path)], text=True
    )
    summary = []
    for family, limit in LIMITS.items():
      found = max((
        tuple(map(int, value.split(".")))
        for value in re.findall(rf"\b{family}_(\d+(?:\.\d+)+)\b", versions)
      ), default=())
      if found:
        name = f"{family}_{'.'.join(map(str, found))}"
        summary.append(name)
        if found > limit:
          failures.append(f"{path}: {name}")
    print(path.relative_to(ROOT), " ".join(summary))
  if failures:
    raise SystemExit("SteamRT3 ABI exceeded:\n" + "\n".join(failures))


if __name__ == "__main__":
  main()
