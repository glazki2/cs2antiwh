"""Build verified release ZIPs from already-built binaries and optional map bakes.

Inputs stay inside this checkout; outputs go to packages/. Unsafe/missing ZIP
entries, broken map/report pairs, modes, integrity, or checksums stop packaging.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parent
PACKAGES = ROOT / "packages"
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
RELEASE_MANIFEST = ROOT / "release" / f"v{VERSION}-manifest.json"
ARCHIVE_NAMES = {
  "windows-x86_64": f"cs2glaz-{VERSION}-windows-x86_64.zip",
  "linux-x86_64": f"cs2glaz-{VERSION}-linux-x86_64.zip",
  "official-maps": f"cs2glaz-{VERSION}-official-maps.zip",
}
LICENSE_FILES = {
  ROOT / "third_party" / "cgltf.LICENSE": "cgltf.LICENSE",
  ROOT / "third_party" / "miniz.LICENSE": "miniz.LICENSE",
  ROOT / "third_party" / "picosha2.LICENSE": "picosha2.LICENSE",
  ROOT / "third_party" / "zstd" / "LICENSE": "zstd.LICENSE",
  ROOT / "third_party" / "ValveResourceFormat.LICENSE": "ValveResourceFormat.LICENSE",
}


def copy_file(source: Path, target: Path) -> None:
  target.parent.mkdir(parents=True, exist_ok=True)
  shutil.copy2(source, target)


def write_text(path: Path, text: str) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="\n") as stream:
    stream.write(text)


def set_zip_modes(zip_path: Path, modes: dict[str, int]) -> None:
  tmp = zip_path.with_name(zip_path.name + ".tmp")
  with zipfile.ZipFile(zip_path, "r") as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
      data = zin.read(info.filename)
      if info.filename in modes:
        info.create_system = 3
        info.external_attr = modes[info.filename] << 16
      zout.writestr(info, data)
  tmp.replace(zip_path)


def make_zip(directory: Path, modes: dict[str, int] | None = None) -> Path:
  archive = Path(str(directory) + ".zip")
  if archive.exists():
    archive.unlink()
  shutil.make_archive(str(directory), "zip", directory)
  if modes:
    set_zip_modes(archive, modes)
  return archive


def verify_zip(archive: Path, required: set[str], executables: set[str] | None = None) -> None:
  with zipfile.ZipFile(archive, "r") as package:
    names = [info.filename for info in package.infolist()]
    if len(names) != len(set(names)) or package.testzip() is not None:
      raise RuntimeError(f"invalid or duplicate zip entries in {archive.name}")
    for name in names:
      path = PurePosixPath(name)
      if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise RuntimeError(f"unsafe zip entry in {archive.name}: {name}")
    missing = required - set(names)
    if missing:
      raise RuntimeError(f"missing entries in {archive.name}: {', '.join(sorted(missing))}")
    for name in executables or set():
      mode = package.getinfo(name).external_attr >> 16
      if mode & 0o111 == 0:
        raise RuntimeError(f"Linux executable bit missing in {archive.name}: {name}")


def package_root(name: str) -> Path:
  path = PACKAGES / name
  if path.exists():
    shutil.rmtree(path)
  zip_path = Path(str(path) + ".zip")
  if zip_path.exists():
    zip_path.unlink()
  return path


def copy_common_files(out: Path) -> None:
  copy_file(ROOT / "LICENSE", out / "LICENSE")
  copy_file(ROOT / "THIRD_PARTY_NOTICES", out / "THIRD_PARTY_NOTICES")
  copy_file(ROOT / "CHANGELOG.md", out / "CHANGELOG.md")
  copy_file(ROOT / "README.md", out / "README.md")
  copy_file(ROOT / "cfg" / "cs2glaz.cfg", out / "cfg" / "cs2glaz.cfg")
  copy_file(ROOT / "gamedata" / "cs2glaz.games.txt", out / "addons" / "cs2glaz" / "gamedata" / "cs2glaz.games.txt")
  copy_file(ROOT / "gamedata" / "cs2glaz.signatures.txt", out / "addons" / "cs2glaz" / "gamedata" / "cs2glaz.signatures.txt")
  for source, name in LICENSE_FILES.items():
    copy_file(source, out / "licenses" / name)
  write_text(out / "addons" / "cs2glaz" / "data" / "maps" / ".gitkeep", "")


def build_core_package(platform: str, plugin_name: str, baker_name: str) -> Path:
  out = package_root(f"cs2glaz-{VERSION}-{platform}")
  copy_common_files(out)
  copy_file(ROOT / plugin_name, out / "addons" / "cs2glaz" / "bin" / Path(plugin_name).name)
  copy_file(ROOT / baker_name, out / "tools" / Path(baker_name).name)
  write_text(
    out / "addons" / "metamod" / "cs2glaz.vdf",
    '"Metamod Plugin"\n{\n  "alias"  "cs2glaz"\n  "file"   "addons/cs2glaz/bin/cs2glaz"\n}\n',
  )

  modes: dict[str, int] = {}
  if platform.startswith("linux"):
    (out / "tools" / "cs2glaz_baker").chmod(0o755)
    modes = {
      "tools/cs2glaz_baker": 0o755,
    }
  archive = make_zip(out, modes)
  required = {
    "addons/cs2glaz/bin/" + Path(plugin_name).name,
    "addons/cs2glaz/gamedata/cs2glaz.games.txt",
    "addons/cs2glaz/gamedata/cs2glaz.signatures.txt",
    "addons/metamod/cs2glaz.vdf",
    "cfg/cs2glaz.cfg",
    "tools/" + Path(baker_name).name,
  } | {f"licenses/{name}" for name in LICENSE_FILES.values()}
  verify_zip(archive, required, set(modes))
  return archive


def find_baker() -> Path:
  candidates = [
    ROOT / "build" / "cs2glaz_baker" / "windows-x86_64" / "cs2glaz_baker.exe",
    ROOT / "build-linux" / "cs2glaz_baker" / "linux-x86_64" / "cs2glaz_baker",
  ]
  if os.name != "nt":
    candidates.reverse()
  for candidate in candidates:
    if candidate.is_file():
      return candidate
  raise RuntimeError("built cs2glaz_baker is required to validate official maps")


def inspect_bvh8(path: Path) -> dict[str, object]:
  try:
    result = subprocess.run(
      [str(find_baker()), "--inspect-bvh8", str(path)],
      capture_output=True,
      text=True,
      timeout=60,
      check=False,
    )
  except (OSError, subprocess.TimeoutExpired) as error:
    raise RuntimeError(f"BVH8 inspection failed for {path.name}: {error}") from error
  if result.returncode != 0:
    detail = (result.stderr or result.stdout).strip()
    raise RuntimeError(f"BVH8 inspection failed for {path.name}: {detail or f'exit {result.returncode}'}")
  try:
    value = json.loads(result.stdout)
  except json.JSONDecodeError as error:
    raise RuntimeError(f"BVH8 inspector returned invalid JSON for {path.name}: {error}") from error
  if not isinstance(value, dict):
    raise RuntimeError(f"BVH8 inspector returned invalid data for {path.name}")
  return value


def validate_map_pair(name: str, bvh8_path: Path, report_path: Path) -> None:
  try:
    report = json.loads(report_path.read_text(encoding="utf-8"))
  except (OSError, UnicodeError, json.JSONDecodeError) as error:
    raise RuntimeError(f"invalid bake report for {name}: {error}") from error
  if not isinstance(report, dict):
    raise RuntimeError(f"invalid bake report for {name}")
  inspected = inspect_bvh8(bvh8_path)
  text_fields = ("map", "source_kind", "source_crc32")
  int_pairs = (("source_size", "source_size"), ("baked_triangles", "triangles"),
               ("nodes", "nodes"), ("packets", "packets"), ("max_depth", "max_depth"))
  if report.get("map") != name or inspected.get("map") != name:
    raise RuntimeError(f"map name does not match bake/report pair for {name}")
  if report.get("source_kind") not in {"world_physics", "nested_map_vpk"}:
    raise RuntimeError(f"invalid source kind in bake report for {name}")
  for field in text_fields:
    if not isinstance(report.get(field), str) or report[field] != inspected.get(field):
      raise RuntimeError(f"{field} does not match bake/report pair for {name}")
  if not re.fullmatch(r"0x[0-9a-f]{8}", report["source_crc32"]):
    raise RuntimeError(f"invalid source CRC in bake report for {name}")
  for report_field, inspected_field in int_pairs:
    left = report.get(report_field)
    right = inspected.get(inspected_field)
    if type(left) is not int or type(right) is not int or left < 0 or left != right:
      raise RuntimeError(f"{report_field} does not match bake/report pair for {name}")


def build_official_maps_package() -> Path:
  manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
  map_names = manifest.get("official_maps", [])
  if manifest.get("version") != VERSION or len(map_names) != 23 or len(set(map_names)) != len(map_names):
    raise RuntimeError("release manifest version or official map list is invalid")
  if any(not name.startswith(("ar_", "cs_", "de_")) or not name.replace("_", "").isascii()
      or not name.replace("_", "").isalnum() for name in map_names):
    raise RuntimeError("release manifest contains an unsafe official map name")
  maps = [ROOT / "data" / "maps" / f"{name}{suffix}" for name in map_names for suffix in (".bvh8", ".json")]
  missing = [path.name for path in maps if not path.is_file()]
  if missing:
    raise RuntimeError(f"official map bakes or reports are missing: {', '.join(missing)}")
  for name in map_names:
    validate_map_pair(name, ROOT / "data" / "maps" / f"{name}.bvh8", ROOT / "data" / "maps" / f"{name}.json")

  out = package_root(f"cs2glaz-{VERSION}-official-maps")
  copy_file(ROOT / "DATA_NOTICE", out / "DATA_NOTICE")
  for path in maps:
    copy_file(path, out / "addons" / "cs2glaz" / "data" / "maps" / path.name)
  archive = make_zip(out)
  verify_zip(archive, {"DATA_NOTICE"} | {f"addons/cs2glaz/data/maps/{path.name}" for path in maps})
  return archive


def sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
      digest.update(chunk)
  return digest.hexdigest()


def write_checksums(archives: list[Path], require_complete: bool = False) -> None:
  names = {path.name for path in archives}
  expected = set(ARCHIVE_NAMES.values())
  if require_complete and names != expected:
    missing = ", ".join(sorted(expected - names))
    extra = ", ".join(sorted(names - expected))
    raise RuntimeError(f"final package set is incomplete (missing: {missing or 'none'}; extra: {extra or 'none'})")
  lines = [f"{sha256(path)}  {path.name}" for path in sorted(archives)]
  write_text(PACKAGES / "SHA256SUMS.txt", "\n".join(lines) + "\n")
  if (PACKAGES / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines() != lines:
    raise RuntimeError("checksum file verification failed")


def current_archives() -> list[Path]:
  return sorted(PACKAGES.glob(f"cs2glaz-{VERSION}-*.zip"))


def main() -> None:
  PACKAGES.mkdir(exist_ok=True)
  targets = sys.argv[1:] or ["windows-x86_64", "linux-x86_64", "official-maps"]
  unknown = set(targets) - set(ARCHIVE_NAMES)
  if unknown:
    raise SystemExit(f"unknown package targets: {' '.join(sorted(unknown))}")
  archives: list[Path] = []
  if "windows-x86_64" in targets:
    archives.append(build_core_package(
      "windows-x86_64",
      "build/cs2glaz/windows-x86_64/cs2glaz.dll",
      "build/cs2glaz_baker/windows-x86_64/cs2glaz_baker.exe",
    ))
  if "linux-x86_64" in targets:
    archives.append(build_core_package(
      "linux-x86_64",
      "build-linux/cs2glaz/linux-x86_64/cs2glaz.so",
      "build-linux/cs2glaz_baker/linux-x86_64/cs2glaz_baker",
    ))
  if "official-maps" in targets:
    archives.append(build_official_maps_package())
  if not archives:
    raise SystemExit(f"no packages built for targets: {' '.join(targets)}")
  write_checksums(current_archives(), set(targets) == set(ARCHIVE_NAMES))


if __name__ == "__main__":
  main()
