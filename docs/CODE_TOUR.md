# CS2FOW code tour

This guide follows CS2FOW in the same order that a person experiences it:

**Load map -> bake walls -> collect player capsules -> test silhouettes -> decide visibility -> withhold hidden entities**

It explains the intent of the code. The engine and file-format details are still in the source files beside the checks they protect.

## Plain-language glossary

**Recipient:** the player whose next network snapshot CS2 is building.

**Target:** another player whom the recipient might or might not be able to see.

**Visual group:** the explicitly known networked entities CS2FOW treats as the visible body of one target: pawn, carried weapons, wearables, and a currently carried hostage prop. Unknown gameplay entities are not inferred from generic links.

**Bake:** the `.bvh8` file made from a map's static collision triangles. Baking moves expensive map preparation out of normal play.

**Bounding volume hierarchy with eight children per node (BVH8):** a tree of boxes that quickly skips most triangles when a ray crosses the map.

**Hitbox capsule:** one of Valve's rounded three-dimensional player hit volumes. Nineteen animated capsules cover the runtime target body.

**Axis-aligned bounding box (AABB):** the player's copied collision box. Runtime checks its eight corners after padding the box 32 units sideways and 8 units upward.

**Valve package (VPK):** the archive format containing CS2 maps and resources.

**Snapshot:** copied player numbers given to the background worker. It is not a CS2 network snapshot.

**Visibility matrix:** a recipient-by-target table of visible/hidden decisions published by the worker.

**CheckTransmit:** the CS2 server step that decides which entity bits are present in one recipient's outgoing snapshot.

**Primary and `dont_transmit` lists:** paired entity-bit lists. Hiding an entity means marking it `dont_transmit` before removing it from the primary send list.

**Full update:** a refresh chosen by CS2 that sends a recipient complete entity state. CS2FOW recognizes it but never requests it.

**Quarantine:** a short record of a previously hidden visual group. It prevents known old group members from escaping during an uncertain group change.

**Handle and edict:** a handle identifies one particular lifetime of an entity; an edict is its network-list index. Checking both lifetime and index helps avoid acting on a recycled entity.

**CRC:** a checksum used to catch changed or damaged VPK entries and bake payloads.

**Fail open:** show/send the target normally when any required fact is missing, invalid, changed, or stale.

## Folder map

| Path | Job |
| --- | --- |
| `src/plugin/plugin.cpp` | Load/unload the plugin, react to maps and frames, load valid bakes, and coordinate the other parts. |
| `src/plugin/settings.*` | Own every CS2FOW ConVar, transactional config load, committed settings snapshot, and administrator commands. |
| `src/plugin/runtime_compatibility.*` | Parse gamedata and classify strict binary, AVX/OS, schema, layout, private-function, and optional capability checks. |
| `src/plugin/game_state.cpp` | Read live CS2 players and visual groups on the game thread, then make copied worker snapshots. |
| `src/plugin/visibility_worker.*` | Own the background thread, replace pending work with the newest snapshot, evaluate capsule visibility, and publish results. |
| `src/plugin/transmit.cpp` | Apply lifecycle rules and visibility results to the paired primary/`dont_transmit` lists; keep quarantine and debug evidence state. |
| `src/plugin/automatic_baker.*` | Run and monitor the external baker without blocking the game thread. |
| `src/plugin/updater.*` | Verify compatible GitHub release assets, stage complete platform packages off the game loop, and install them only during the next server start. |
| `src/core/bvh8.cpp` | Traverse an in-memory BVH8 and answer whether a line segment hits a triangle. |
| `src/core/bvh8_format.cpp` | Validate, read, verify, and safely replace BVH8 version 3 files. |
| `src/core/builder.*` | Turn accepted triangles into BVH8 nodes and triangle packets. |
| `src/core/visibility_sampling.*` | Define Valve capsule bindings and build ping-scaled recipient origins and the held-weapon muzzle point. |
| `src/core/capsule_visibility.*` | Compare animated capsule silhouettes against a target-fitted CPU map depth buffer and copied live smoke. |
| `src/core/vpk.*` | Parse VPK versions 1/2, list entries, extract them, and verify their CRCs. |
| `src/core/map_source.*` | Find direct or nested map physics sources and validate safe map subpaths. |
| `src/core/lifecycle_guard.h` | Fixed-size rules for player lifetimes, pair warmup, visual-group identity, and quarantine. |
| `src/core/transmit_masks.h` | Parse gamedata numbers, read the private full-update flag, and perform the paired withhold operation. |
| `src/core/transmit_debug.h` | Aggregate entity bits actually hidden by CS2FOW without allocating in `CheckTransmit`. |
| `src/core/subprocess.*` | Start external tools with argument lists, timeouts, cancellation, and captured output. |
| `src/baker/` | Command-line bake sequence, the native binary-KV3 and map-physics reader (`kv3.*`, `physics_import.*`), the shared bake recipe (`physics_recipe.*`), and the optional GLB parity reader. |
| `tests/` | Small assert-based tests grouped into map/BVH and visibility/transmit responsibilities. |
| `cfg/`, `gamedata/`, `data/` | Shipped settings, platform offsets, and optional map bakes. |

## Bake flow

1. `src/baker/main.cpp` checks the command arguments and safe map name. `--list-maps --vpk` stops after validated, sorted VPK discovery.
2. `find_map_source` opens the outer VPK. A direct `maps/<map>/world_physics.vmdl_c` wins. If it is absent, `maps/<map>.vpk` is the fallback.
3. `src/core/vpk.cpp` checks the VPK header, tree bounds, entry terminators, preload data, embedded/numbered archive ranges, and CRC before trusting extracted bytes. Version 2 embedded entries must stay inside its declared file-data section even when footer bytes follow it.
4. For a nested map, the C++ baker extracts the nested VPK into a temporary directory. Python and the web service do not understand or patch VPK/BVH details.
5. The baker extracts the chosen `world_physics.vmdl_c` and `physics_import.cpp` reads it natively: the resource block table, the binary KV3 physics data (versions 0-5, uncompressed, LZ4 or Zstandard), and every sphere, capsule, hull and mesh. Shapes are grouped by collision attribute and surface property exactly like ValveResourceFormat 19.2's physics GLB export, and `physics_group_accepted` keeps the surfaces accepted by the bake recipe. `--compare-glb <physics.glb>` optionally checks the result against a GLB exported by another tool.
6. `builder.cpp` packs the accepted triangles into eight-wide packets and builds the BVH8 tree.
7. `bvh8_format.cpp` writes a version 3 file beside the destination. It reloads and verifies one rooted tree, unique reachable nodes/packets, depth, triangle totals, and payload CRC before atomically replacing the destination. A bad write leaves the previous valid bake in place.
8. The baker writes a matching `.json` report with source checksums and geometry counts. `--debug-obj` optionally writes accepted triangles for tools such as MeshLab.

The version 3 header is 256 bytes and records recipe version 1. Loading rejects unknown flags, nonzero reserved bytes, unsafe names, non-finite or reversed bounds, impossible counts, noncanonical offsets, wrong exact file size, and bad CRC before the data can become active.

## Map-load flow

After registering ConVars during plugin load, and again before every map worker starts, `settings.cpp` asks the server to execute `cfg/cs2fow.cfg`. The previous committed snapshot remains active while the file runs. Only the final `cs2fow_config_loaded` marker commits the candidate values; interruption, a missing marker, or the five-second timeout restores the previous snapshot. A second reload is rejected while one is pending.

1. The Metamod map callback or game-frame check notices a new map.
2. `request_map_change` stops the old worker, starts the configuration transaction, and waits for commit or rollback.
3. `change_map` clears old map/transmit state, then asks the CS2 filesystem for mounted map-VPK candidates. The configured worker-thread count is therefore fixed consistently for this map.
4. `find_map_source` records the selected outer/nested source entry, CRC, and size.
5. `load_bvh8` validates the installed bake. `load_map_bake` also requires the map name, source kind, CRC, and size to match the currently mounted source.
6. A matching bake starts the visibility worker. A missing, old, damaged, or mismatched bake starts the low-priority external baker when its tools are present.
7. While baking, or after any failure, `disabled_reason_` keeps transmit filtering off. A finished automatic bake is accepted only if the mounted source is still the same.

This is why a Valve map update cannot silently reuse old wall geometry.

## Automatic-update flow

1. After the initial configuration settles, the updater checks GitHub's latest stable release after 30 seconds and every six hours. `cs2fow_auto_update 0` cancels an active HTTP request and prevents future checks.
2. The release must be newer, non-draft, and non-prerelease, with exact platform-package and release-manifest asset names under the CS2FOW GitHub repository.
3. Steam's server HTTP service requires verified TLS. GitHub's declared size and SHA-256 digest are checked for both assets.
4. The manifest version and package SHA-256 must agree. Its Windows or Linux fingerprint list must contain the exact currently loaded `server.dll` or `libserver.so` size and CRC before the large package is downloaded.
5. The package is unpacked on a background task with path, file-count, per-file, total-size, duplicate-entry, required-file, and SHA-256 checks. Only CS2FOW's known package paths are accepted.
6. Staging copies the verified new plugin to `cs2fow-update`, writes a pending marker, and points CS2FOW's Metamod VDF at that bootstrap name. The running plugin remains unchanged.
7. On the next full server start, that new bootstrap binary backs up the old stable binary, merges known values from the current config into the new commented template, updates gamedata, baker, documentation/licenses, and stable binary, restores Linux executable modes, and returns the VDF to `cs2fow`.
8. Map bakes under `addons/cs2fow/data/maps` are never copied, deleted, or replaced. Any failed request, validation, or install step keeps protection fail-open where appropriate and retries without guessing.

## Game-state and worker flow

The game thread runs `hook_game_frame`. At most once per configured interval (default `1 ms`), `capture`:

1. reads controllers and pawns through resolved schema fields;
2. rejects HLTV, invalid controller/pawn links, spawning/dead players, non-T/CT teams, invalid bounds, and uncertain lifecycles;
3. asks CS2 for the current pose and copies Valve's nineteen animated hitbox capsules; an incomplete or invalid pose fails open;
4. copies origin, current movement buttons, eye position/yaw, bounds, round-trip latency, team, pawn index, and held-weapon muzzle class;
5. builds/checks visual groups for lifecycle identity, but never gives live engine pointers to the worker; and
6. submits a plain copied `visibility_snapshot` with a rising sequence number.

`visibility_worker::submit` stores only the newest pending snapshot. Work does not form a backlog. The worker wakes, takes ownership of that copy, and computes a new result.

For each eligible living pair the worker:

- makes five fixed recipient origins: eye, RTT-scaled left/right shoulders (half the configured base while idle, with A/D enlarging only the matching shoulder; W/S leave both idle), eye plus 16 units, and feet;
- adds one wall-clipped, RTT-scaled W/S or diagonal intention origin; pure A/D activates the full shoulder offset;
- reuses an active reveal hold, then projects the complete nineteen-capsule body into a target-fitted 32 by 32 CPU depth view;
- only when that silhouette is fully blocked, tries eight AABB corners padded 32 units sideways and 8 units upward, then the held-weapon muzzle;
- proves fully covered capsule regions hidden in batches, tests remaining capsule surface samples against baked walls and copied live smoke, and stops at the first open sample;
- lets an HE clear only smoke that already existed when the detonation was recorded on the same game clock;
- reuses the triangle packet that blocked the same pair's earlier muzzle ray, then traverses the BVH8 if needed;
- publishes a fully visible result if capsule capture, geometry evaluation, or the 75 ms cycle budget becomes uncertain; and
- holds a newly open pair visible for `cs2fow_visibility_hold_ms`.

The finished immutable result contains its sequence, capture/completion times, copied player identity, visibility matrix, timing, and pair counts. Publishing swaps a shared result; it never exposes a half-written matrix.

## CheckTransmit flow

`hook_check_transmit` is deliberately conservative:

1. Return without changes if CS2FOW is disabled, the map is not active, inputs are invalid, or the latest worker result is missing/stale. On the map's first active call, read the recipient array, each recipient record, and both entity lists with guarded reads (`process_vm_readv` / `ReadProcessMemory`); unreadable memory or an implausible record stops filtering for the map.
2. Lock `transmit_state_mutex_`. This protects lifecycle, pair-baseline, quarantined-group, and debug state shared with game-frame capture and console commands. Ray traversal and file work never run under this lock.
3. First scan the recipients for CS2 full updates. For those recipients, clear stored hidden groups, but do not alter that full-update snapshot.
4. Re-read live recipient/target lifecycles and visual groups. Any mismatch with the copied worker player fails open. Also walk each target pawn's scene-node children: every other networked entity attached below it joins an "attached" list that is withheld together with the group, and a hierarchy that cannot be fully accounted for (walk budget, capacity, unresolvable owner, another player attached) reveals that target.
5. Require every live recipient's own pawn to be set in its primary list. A missing own pawn means the recipient slot or list layout is wrong, so filtering stops for the map before any list is changed.
6. Skip self, invalid players, and full-update snapshots. Skip teammates only when optional teammate filtering and `mp_teammates_are_enemies` are both disabled.
7. Require a stable player pair and evidence that a complete current visual group was previously sent on an older worker sequence before the pair is allowed to hide.
8. When hidden, store the exact visual group. For each member whose primary bit is set, set the matching bit through the existing second `CCheckTransmitInfo` pointer, locally treated as `dont_transmit`, and only then clear the primary bit.
9. If either paired-list pointer is unavailable, change neither list and fail open. If a primary bit is already clear, leave both bits alone.
10. If rays later say visible, stop withholding the current group and let ordinary snapshots handle it; CS2FOW does not wait for or request a full update.
11. When a current group cannot be rebuilt, a still-valid quarantined old group may be withheld briefly through the same paired operation. Invalid handles/indexes are skipped rather than guessed.

Those are the only two lists CS2FOW changes. Full-update snapshots, `+16` out-of-PVS updates, and `+24` HLTV storage are untouched. Valve mode `0` uses its compatibility behavior; mode `1` consumes the explicit `dont_transmit` information maintained by the same code.

The primary `IsBitSet` check always runs because only set bits may enter the paired operation. When `cs2fow_debug` is off, clearing skips classname lookup, record search, and record update. When it is on, evidence is recorded only for a primary bit that CS2FOW actually clears. The 256-record fixed array deduplicates by entity handle and source pawn; it aggregates recipients/reasons/counts without heap allocation in the hook.

## Thread and data ownership

| Thread/caller | May read live CS2 objects? | Owns or changes | Coordination |
| --- | --- | --- | --- |
| Game thread | Yes | Map state, schema reads, copied player snapshots, visual-group lifecycle state | Uses `transmit_state_mutex_` when capture touches transmit lifecycle state. |
| Visibility worker | No | One taken snapshot, muzzle-ray caches, reveal holds, worker statistics, next result | `mutex_` protects pending work; `stats_mutex_` protects statistics; published result is shared immutably. |
| CheckTransmit hook | Yes, only for validation/group resolution | Paired primary/`dont_transmit` bits and transmit lifecycle/quarantine/debug state | Holds `transmit_state_mutex_`; does no BVH traversal, file I/O, process work, or heap allocation. |
| Automatic-baker thread | No live engine objects | External process and one completion record | Receives copied paths/map-source metadata; its own mutex protects status/completion. |
| Automatic-update staging task | No live engine objects | One already downloaded package and ignored staging directory | Receives copied paths/version/digest; archive hashing and extraction stay off the game loop. |
| Console commands | No direct player traversal | Read status or read/clear debug records | Debug commands use `transmit_state_mutex_`. |

The BVH8 data is loaded before the worker starts and remains unchanged until that worker is stopped. That gives the worker a stable read-only map tree.

## Safety rules and resets

- Missing, invalid, changed, or stale information always fails open.
- Full-update snapshots are never filtered.
- Only set primary bits and their matching verified `dont_transmit` bits are changed; either missing pointer fails open.
- The worker receives copied data and never dereferences engine objects.
- CheckTransmit uses fixed-size visual groups, caches, and debug records; it performs no heap allocation.
- Player/visual-group lifetime changes reset pair baselines instead of hiding immediately.
- Enabling/disabling filtering resets lifecycle, pair, and hidden-group state but preserves collected debug evidence.
- A map change, level shutdown, or normal plugin-state reset also clears debug evidence.
- Worker start resets pending/published work, cached blocking packets, reveal holds, and timing/pair statistics.
- Automatic-baker stop cancels/joins its task and terminates its full baker process tree before old map state is discarded.
- Automatic updates never hot-swap the running binary, never accept a release for a different CS2 fingerprint, and never touch installed map bakes.

## Where to make common changes

| Change | Start here | Keep in mind |
| --- | --- | --- |
| Valve capsule bindings, AABB padding, input origins, or muzzle sampling | `src/core/visibility_sampling.cpp` | Never add a static capture fallback for the verified build; limited mode uses `visibility_hull_capsules` only when bones are unavailable. |
| Capsule silhouette/depth evaluation | `src/core/capsule_visibility.cpp` | Preserve conservative sub-pixel handling, smoke/HE behavior, and fail-open deadlines. |
| Player/schema field capture | `src/plugin/game_state.cpp` | Live engine reads remain on the game thread and uncertainty fails open. |
| Visibility scheduling, muzzle cache, or reveal hold | `src/plugin/visibility_worker.cpp` | Worker input must stay pointer-free copied data. |
| Which target entities form a visual group | `collect_player_visual_group` in `game_state.cpp` | Fixed capacity, full-group validation, handles, and lifecycle identity protect transmit safety. |
| Withholding rules or evidence | `src/plugin/transmit.cpp` | Set `dont_transmit` before clearing a set primary bit; no filtering on full updates; no allocation in the hook. |
| VPK compatibility | `src/core/vpk.cpp` and `map_source.cpp` | Check every range/CRC and preserve direct-over-nested precedence. |
| BVH traversal math | `src/core/bvh8.cpp` | Tests cover open/blocked rays and packet caching. |
| BVH file layout | `src/core/bvh8_format.cpp` and `bvh8.h` | Validate before allocation and keep replacement atomic. |
| Physics filtering/build recipe | `src/baker/physics_recipe.cpp`, `src/baker/physics_import.cpp`, `src/core/builder.cpp` | Recipe changes require an intentional format/recipe decision and new bakes. |
| Operator settings/commands | `src/plugin/settings.*`, `cfg/cs2fow.cfg`, `README.md` | Preserve the `cs2fow_*` public names and keep the transaction marker last. |
| Binary/schema/private API compatibility | `src/plugin/runtime_compatibility.*`, `gamedata/cs2fow.games.txt` | Preserve exact fingerprint enforcement and the required/optional capability boundary. |
| Automatic-update validation or install ownership | `src/plugin/updater.*`, release manifest, and `package.py` | Keep exact platform assets, SHA-256 checks, restart-only install, config backups, and map-bake preservation. |

## Build, test, package, and release

`build-dependencies.json` is the source of truth for the exact Metamod, HL2SDK, AMBuild, and Steam Runtime 3 inputs. Bootstrap stores ignored source dependencies under `.build-deps/`; GitHub and GitLab CI call the same scripts used locally.

Windows:

```powershell
.\scripts\build-windows.ps1
```

Windows host to the pinned Steam Runtime 3 Linux container:

```powershell
.\scripts\build-steamrt3.ps1
```

Inside a Steam Runtime 3 Linux environment:

```sh
bash scripts/build-linux.sh
```

Each build script fetches exact dependencies, configures and compiles, runs native and SDK-independent tests, verifies Windows imports or SteamRT3 symbol versions, and produces the corresponding ignored `packages/` ZIP.

`package.py` takes the version from top-level `VERSION`. For official maps it asks `cs2fow_baker --inspect-bvh8` to validate every bake and requires matching report metadata. It also checks licenses, duplicate/unsafe ZIP entries, ZIP integrity, Linux modes, and checksums. Every stable release intended for automatic updates must attach both platform ZIPs and the matching `v<version>-manifest.json`; the updater rejects anything incomplete or incompatible.

Every GitHub release and GitLab mirror must include this paragraph without replacing the existing release notes:

> CS2FOW is free and independently maintained. If it helps your server, even a small one-time or monthly contribution helps me keep up with CS2 updates, testing, Windows and Linux builds, report investigation, and community support: https://buymeacoffee.com/karola3vax

Creating a tag, release manifest, release notes, public release, or Bake Service deployment remains a separate explicitly approved task.
