import json
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import package


SUMMARY = {
    "map": "de_test",
    "source_kind": "world_physics",
    "source_crc32": "0x1234abcd",
    "source_size": 123,
    "triangles": 456,
    "nodes": 12,
    "packets": 34,
    "max_depth": 5,
}
REPORT = {
    "map": "de_test",
    "source_kind": "world_physics",
    "source_crc32": "0x1234abcd",
    "source_size": 123,
    "baked_triangles": 456,
    "nodes": 12,
    "packets": 34,
    "max_depth": 5,
}


class PackageTests(unittest.TestCase):
    def test_readme_does_not_link_release_binaries(self):
        urls = re.findall(r'https?://[^\s)"<>]+', (package.ROOT / "README.md").read_text(encoding="utf-8"))
        self.assertFalse([url for url in urls if "/releases/" in url or re.search(r"\.(?:zip|exe|dll|so)(?:$|[?#])", url)])

    def test_map_pair_metadata_must_match_inspected_bake(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bvh8 = root / "de_test.bvh8"
            report = root / "de_test.json"
            bvh8.write_bytes(b"test")
            report.write_text(json.dumps(REPORT), encoding="utf-8")
            with patch.object(package, "inspect_bvh8", return_value=SUMMARY):
                package.validate_map_pair("de_test", bvh8, report)
                for field in ("source_crc32", "source_size", "baked_triangles", "nodes", "packets", "max_depth"):
                    bad = dict(REPORT)
                    bad[field] = "0x00000000" if field == "source_crc32" else bad[field] + 1
                    report.write_text(json.dumps(bad), encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, field):
                        package.validate_map_pair("de_test", bvh8, report)

    def test_malformed_and_swapped_reports_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bvh8 = root / "de_test.bvh8"
            report = root / "de_test.json"
            bvh8.write_bytes(b"test")
            report.write_text("not json", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "invalid bake report"):
                package.validate_map_pair("de_test", bvh8, report)
            swapped = dict(REPORT, map="de_other")
            report.write_text(json.dumps(swapped), encoding="utf-8")
            with patch.object(package, "inspect_bvh8", return_value=SUMMARY), self.assertRaisesRegex(RuntimeError, "map name"):
                package.validate_map_pair("de_test", bvh8, report)

    def test_inspector_failure_rejects_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "de_test.json"
            report.write_text(json.dumps(REPORT), encoding="utf-8")
            with patch.object(package, "inspect_bvh8", side_effect=RuntimeError("bad BVH8")), self.assertRaisesRegex(RuntimeError, "bad BVH8"):
                package.validate_map_pair("de_test", root / "de_test.bvh8", report)

    def test_checksums_keep_existing_targets_and_final_requires_all_three(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(package, "PACKAGES", Path(directory)):
            first_two = list(package.ARCHIVE_NAMES.values())[:2]
            for name in first_two:
                (package.PACKAGES / name).write_bytes(name.encode())
            package.write_checksums(package.current_archives())
            checksums = (package.PACKAGES / "SHA256SUMS.txt").read_text(encoding="utf-8")
            self.assertTrue(all(name in checksums for name in first_two))
            with self.assertRaisesRegex(RuntimeError, "incomplete"):
                package.write_checksums(package.current_archives(), require_complete=True)
            last = list(package.ARCHIVE_NAMES.values())[2]
            (package.PACKAGES / last).write_bytes(last.encode())
            package.write_checksums(package.current_archives(), require_complete=True)

    def test_core_packages_require_every_license_source(self):
        self.assertIn("cgltf.LICENSE", package.LICENSE_FILES.values())
        self.assertIn("miniz.LICENSE", package.LICENSE_FILES.values())
        self.assertIn("picosha2.LICENSE", package.LICENSE_FILES.values())
        self.assertIn("zstd.LICENSE", package.LICENSE_FILES.values())
        self.assertIn("ValveResourceFormat.LICENSE", package.LICENSE_FILES.values())
        self.assertTrue(all(path.is_file() for path in package.LICENSE_FILES))
        # The baker reads map physics natively; no .NET/VRF tools are shipped.
        self.assertFalse(hasattr(package, "VRF_FILES"))

    def test_build_inputs_are_pinned_and_ci_uses_shared_scripts(self):
        manifest = json.loads((package.ROOT / "build-dependencies.json").read_text(encoding="utf-8"))
        for dependency in ("ambuild", "metamod", "hl2sdk_manifests", "hl2sdk"):
            self.assertRegex(manifest[dependency]["commit"], r"^[0-9a-f]{40}$")
        self.assertNotIn("vrf", manifest)
        self.assertRegex(manifest["steamrt3_image"], r"@sha256:[0-9a-f]{64}$")

        github = (package.ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        gitlab = (package.ROOT / ".gitlab-ci.yml").read_text(encoding="utf-8")
        self.assertIn("scripts/build-windows.ps1", github)
        self.assertIn("scripts/build-linux.sh", github)
        self.assertIn("scripts/build-linux.sh", gitlab)
        self.assertNotIn("check_studio", gitlab)
        for action in re.findall(r"uses:\s*[^@\s]+@([^\s]+)", github):
            self.assertRegex(action, r"^[0-9a-f]{40}$")
        self.assertIn(manifest["steamrt3_image"], github)
        self.assertIn(manifest["steamrt3_image"], gitlab)
        self.assertIn(
            "DEPENDENCIES.mkdir(parents=True, exist_ok=True)",
            (package.ROOT / "scripts/bootstrap.py").read_text(encoding="utf-8"),
        )

    def test_config_transaction_marker_is_last(self):
        lines = [
            line.strip()
            for line in (package.ROOT / "cfg/cs2glaz.cfg").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("//")
        ]
        self.assertEqual(lines[-1], "cs2glaz_config_loaded")
        self.assertEqual(lines.count("cs2glaz_config_loaded"), 1)
        self.assertIn("cs2glaz_auto_update 0", lines)


if __name__ == "__main__":
    unittest.main()
