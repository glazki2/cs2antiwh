# Third-party notices

cs2glaz is derived from an MIT-licensed plugin; that
project's copyright and permission notice is kept in `LICENSE`.

cs2glaz uses the following third-party software. Each project
remains under its own license and copyright. Packaged license texts are placed
under `addons/cs2glaz/licenses`.

- [Metamod:Source](https://github.com/alliedmodders/metamod-source) and its
  [KHook](https://github.com/Kenzzer/KHook) headers, consumed at build time at the
  commit pinned in `build-dependencies.json`.
- [Source 2 SDK (HL2SDK, cs2 branch)](https://github.com/alliedmodders/hl2sdk), consumed at
  build time; individual SDK files retain their Valve and contributor notices.
  `third_party/generated/network_connection.pb.h` is generated from its protocol
  definitions.
- [AMBuild](https://github.com/alliedmodders/ambuild), the build tool, BSD 3-Clause.
- [cgltf](https://github.com/jkuhlmann/cgltf) 1.15, MIT. Used by the baker's optional
  `--compare-glb` parity check and tests.
- [MaskedOcclusionCulling](https://github.com/GameTechDev/MaskedOcclusionCulling), Apache-2.0.
- [miniz](https://github.com/richgel999/miniz) 3.1.2, MIT. Unpacks verified update packages.
- [PicoSHA2](https://github.com/okdshin/PicoSHA2), MIT. Verifies update package digests.
- [Zstandard](https://github.com/facebook/zstd) 1.5.7 single-file decompressor, BSD 3-Clause
  (dual-licensed with GPLv2 upstream). Reads Zstandard-compressed map physics.
- [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat), MIT. Format
  reference for the native KV3/physics reader; source of the surface-name table
  (`src/baker/surface_names.inc`) and of the test resources in `tests/fixtures`. No
  ValveResourceFormat binaries are built, invoked or shipped.
- [Funchook](https://github.com/kubo/funchook) with [diStorm](https://github.com/gdabah/distorm),
  prebuilt libraries under `vendor/funchook` that only the optional CMake build links;
  the AMBuild release build does not use them.

The CheckTransmit integration pattern is adapted from CS2KZ and CS2Fixes.

Generated `.bvh8` map data is derived from Counter-Strike 2 game data and is not
licensed under the project's MIT license; see `DATA_NOTICE`.
