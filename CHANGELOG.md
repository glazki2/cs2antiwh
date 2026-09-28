# Changelog

## 0.5.1

- `cs2glaz_probe dump` scans every pointer-sized field of one live recipient's CheckTransmit record (and both union lists) for a readable 16384-bit entity list and reports, per list, how many bits are set, whether the recipient's own pawn is set, and how many enemy pawns behind walls and in view it holds; short lists also name their entities. Reads only, through guarded memory access. This can find a don't-transmit list that CS2 1.41.8 may have moved.
- `cs2glaz_transmit_mode 5` clears both recipient lists and then removes every withheld entity that no recipient keeps (full updates and SourceTV keep theirs) from both union lists, in case the new build re-adds union members after CheckTransmit.
- `cs2glaz_status` and `cs2glaz_metrics` finish a completed automatic bake and the pending limited-mode check themselves, so a hibernating empty server no longer shows `BAKING` or `validating map` until a player joins.
- `cfg/cs2glaz.cfg` no longer sets `sv_enable_donttransmit`, which CS2 1.41.8 removed (it only printed `Unknown command`); `cs2glaz_check_config` reports its absence as a note instead of a problem.
- Removed the upstream author's donation message from the load and status output and from the release notes guidance; attribution stays in `LICENSE`, `THIRD_PARTY_NOTICES` and the README.
- README: states the open CS2 1.41.8 leak plainly, explains server hibernation, and documents the diagnostics.

## 0.5.0

- The plugin is now called cs2glaz. Every name changed with it: the Metamod plugin and alias (`cs2glaz`), the folder `addons/cs2glaz`, `cfg/cs2glaz.cfg`, `tools/cs2glaz_baker`, all console variables and commands (`cs2glaz_status`, `cs2glaz_probe`, `cs2glaz_transmit_mode`, ...), the log prefix `[CS2GLAZ]`, and the package and CI artifact names (`cs2glaz-<version>-<platform>.zip`, `cs2glaz-linux-x86_64`).
- Map bakes use a new file signature, so bakes made by earlier versions are not loaded; each map is baked again automatically on first load.
- An existing installation must be removed before this one is installed; otherwise Metamod would load both plugins.

## 0.4.4

- On CS2 1.41.8 (September 2026, where `sv_enable_donttransmit` no longer exists) a live test showed hidden enemies still moving in real time on a wallhack, even though the plugin cleared their bits every snapshot. This release adds diagnostics to find out what the new build honours:
  - `cs2glaz_probe start` samples, for ~10 seconds, whether each enemy pawn is set in the recipient lists at +0, +8, +16 and +24 and in the two union lists passed to CheckTransmit, split into pawns behind walls and visible pawns; `cs2glaz_probe` prints the counts. It only reads, and reads the unknown lists through guarded memory access.
  - `cs2glaz_transmit_mode` (not saved in `cs2glaz.cfg`, resets to 0): `0` the CE behaviour (clear the primary bit, set the second list's bit), `1` clear the primary bit only, `2` clear the primary and the second list's bit, `3` observe only, `4` like `2` and also clear the pawn's bit in +16/+24 when a finished probe showed that list carrying nearly every hidden pawn; those writes go through guarded memory access.

## 0.4.3

- Limited mode now validates the entity system on the map's first simulated frame instead of at activation. A map whose bake already existed activated while the level was still loading, before the world entity was spawned, so the check failed and protection stayed off for that map after every server restart; only freshly baked maps (activated later) were protected.
- Every reason that turns protection off for a map is now printed to the server console as `[CS2GLAZ] protection off: ...`, so a silent failure like the one above shows up in the log.

## 0.4.2

- On Linux, restore the execute bit on `tools/cs2glaz_baker` when it is missing instead of disabling the map. Hosting-panel file managers drop it when they unpack the package zip, which left protection off with `baker missing execute permission`.

## 0.4.1

- Hide networked entities attached below a hidden player (for example another plugin's glow prop, hat, or trail) together with that player. Before, only weapons, wearables, and a carried hostage were withheld, so an attached entity could stay on the client without its parent, which is a known way to crash CS2 clients, and it also showed where the player was. A hierarchy that cannot be fully accounted for, or has another player attached, reveals the player instead.
- Before a map's first filtered snapshot, read the CheckTransmit recipient array, each recipient record, and both entity lists with guarded reads, so a CS2 update that moved these layouts turns filtering off for the map instead of crashing the server.
- Check every snapshot that each live recipient's own pawn is in its transmit list. If it is not, the recipient slot or list layout is wrong, so filtering stops for the map before any player could be hidden from the wrong person.
- Limited mode now validates the entity list (world entity, its back-pointer, and its name) with guarded reads only.
- On Linux, the automatic baker runs at the lowest CPU priority and asks the kernel to kill it first if memory runs out, so a bake cannot get the game server killed on a small machine.
- Plugin metadata now states the MIT license, matching `LICENSE`.
- The repository was renamed to `glazki2/cs2antiwh`: the plugin URL, `cs2glaz.cfg`, and the automatic updater now point there. The updater only accepts download links under this prefix, so with the old name it would have rejected every release asset.

## 0.4.0 (first glazki2/cs2antiwh release, based on the MIT-licensed Community Edition 0.3.8)

- Load on current Metamod:Source 2.0 (plugin API 18): the GameFrame, CheckTransmit and LoadEventsFromFile hooks now use KHook, because Metamod removed SourceHook on 2026-09-08 and refuses older plugins.
- Build against the latest HL2SDK and Metamod for the September 2026 CS2 update (ConVar registration, IFileSystem, CGlobalVars and entity-system header changes); the plugin is linked with `-fno-gnu-unique` so Metamod can unload it.
- Add limited mode (`cs2glaz_limited_mode 1`, default): when the server binary is not the build the gamedata was verified for, walls-only filtering keeps running with a conservative hull-shaped body instead of turning protection off. It never calls private functions or reads private smoke layouts, validates the entity system before use, and stops filtering for the map if a CheckTransmit recipient list looks structurally wrong. `cs2glaz_status` shows which mode is active.
- Replace ValveResourceFormat (a .NET program) with a native C++ map-physics reader in `cs2glaz_baker`: binary KV3 versions 0-5 with LZ4/Zstandard, resource blocks, and hull/mesh/sphere/capsule shapes grouped exactly like the VRF 19.2 export. Packages no longer ship `tools/vrf`. `--compare-glb` optionally checks a bake against a GLB from another tool. The .NET-based Visibility Studio tooling was removed with it.
- Automatic updates are off by default and, when enabled, only look at this fork's GitHub releases.

## 0.3.7

- Increased the default horizontal AABB padding from 8 to 32 units while keeping the top padding at 8 units.
- Refined viewing origins: idle shoulders use half the configured base, A/D enlarge only the matching shoulder, and W/S only add the forward/back movement-origin check. RTT scaling remains 16 units per 25 ms.
- Aligned Visibility Studio's decision-stage display, capsule/AABB/muzzle tracing, movement controls, and runtime checks with the native plugin.

## 0.3.6

- Updated the strict Windows and Linux gamedata fingerprints and private addresses for CS2 build `24537688` (`1.41.7.4`). Unknown binaries remain fail-open.
- Added `cs2glaz_check_update` for an immediate update check, including clear results when the server is current, an update is already prepared, or automatic updates are disabled.
- Freshly baked and validated all 23 official maps from build `24537688`. Five changed map sources (`cs_shelter`, `de_boulder`, `de_cache`, `de_debris`, and `de_fachwerk`) now have new matching geometry.

## 0.3.5

- Added verified automatic updates, enabled by default with `cs2glaz_auto_update 1`. CS2GLAZ checks GitHub's stable releases, requires an exact Windows/Linux package and release manifest, verifies both SHA-256 digests and the current CS2 server-binary fingerprint, and stages the complete platform package outside the game loop.
- Install prepared updates only on the next full server restart through a separate bootstrap binary. Preserve known configuration values and all map bakes, keep backups of the previous configuration and plugin binary, restore Linux tool permissions, and leave the current installation running when any check or filesystem operation fails.

## 0.3.4

- Ship `mp_playerid 1` by default so CS2 does not display an enemy name over that player's stale last-transmitted position while CS2GLAZ is hiding them.
- Report whether target IDs are safe in `cs2glaz_status`, and make `cs2glaz_check_config` explain how to correct an unrestricted `mp_playerid` setting.
- Keep visibility decisions, transmitted entity groups, gunshot behavior, performance settings, strict CS2 compatibility checks, and fail-open behavior unchanged from 0.3.3.

## 0.3.3

- Revalidated every private Windows and Linux gamedata value against the current public CS2 `1.41.7.3` binaries. The strict gate now accepts both exact verified Linux files Valve distributed for that build, whose required functions and layouts are identical, while continuing to reject every unknown fingerprint.
- Centralized every CS2GLAZ setting behind a committed runtime snapshot. `cs2glaz.cfg` now loads transactionally, retains the previous known-good settings until its final marker, rolls back incomplete loads after five seconds, and defers worker-thread changes until the next map.
- Added `cs2glaz_help`, `cs2glaz_reload`, `cs2glaz_check_config`, and `cs2glaz_metrics`. `cs2glaz_status` is now a short operator dashboard with explicit health, configuration, map, protection, player/pair, p99, snapshot-age, and next-action information; the former detailed counters remain in `cs2glaz_metrics`.
- Extracted strict CS2/OS/AVX/gamedata/schema capability checks into a structured compatibility component without weakening the exact server-binary gate or fail-open behavior.
- Made Visibility Studio runtime-only: Preview and Play now use the real nineteen animated capsule bindings, eight padded AABB corners, muzzle, viewing origins, smoke/HE rules, and native LOS order. The legacy fifteen-point editor, preset, import/export, rays, and static fallback were removed.
- Centralized pinned Metamod, HL2SDK, AMBuild, VRF, and Steam Runtime 3 inputs in one dependency manifest. Shared Windows/Linux/SteamRT3 scripts now perform bootstrap, tests, ABI/import checks, and packaging for GitHub CI, GitLab CI, and local builds.
- Increased the default reveal hold to 1000 ms to cover brief LOS gaps such as the CT-to-T angle through Dust II mid doors; operators can still tune it with `cs2glaz_visibility_hold_ms`.
- Made Valve's full nineteen-capsule silhouette the primary LOS decision. The eight padded AABB corners and weapon muzzle are now forgiving fallbacks when the capsule silhouette is fully blocked; the redundant chest probe was removed.
- Preserved runtime ConVar names, package layout, and strict fail-open compatibility enforcement.

## 0.3.2

- Updated strict Windows and Linux gamedata for CS2 build `24442510` (`1.41.7.3`).
- Rebuilt against the latest HL2SDK `159cddd`; Metamod:Source remains current at `2667e8e`.

## 0.3.1

- Restored eight padded AABB corner checks as a fast, forgiving visibility fallback before the full animated-capsule test. The runtime now tries reveal-hold reuse, chest, AABB corners, muzzle, and finally capsules, so an obvious visible point avoids the more expensive capsule pass.
- Increased the default reveal hold from 16 ms to 47 ms (about three 64-tick server ticks) to reduce edge flicker and short pop-outs.
- Added the AABB corner samples to the temporary in-game LOS debug view and aligned Visibility Studio with the runtime's current LOS order, padding, hold, cache, validation, and pose behavior.
- Rebuilt against Metamod:Source `2667e8e` and HL2SDK `c9e9477` while retaining Steam Runtime 3 compatibility.

## 0.3.0

- Replaced the fifteen hand-tuned runtime LOS dots and eight AABB corners with Valve's nineteen live animated hitbox capsules. Visibility now evaluates the capsule silhouette through a bounded CPU depth buffer, preserves the muzzle/smoke/HE rules, and fails open on invalid capture, uncertainty, or a 75 ms worker budget.
- Added a configurable 1-4-thread visibility pool (two by default), fair budget rotation, a safe visible-ray prepass, reveal-hold reuse, and a larger verified occluder cache that compacts proven blocker sets for 32-player servers. Status now separates wall latency from aggregate worker activity and reports recent tail latency, prepass, hold, and cache behavior.

## 0.2.6-preview

- Removed the private CS2 debug-overlay calls that could corrupt the client HUD and spam missing-texture errors.

## 0.2.5-preview

- Made all fifteen tuned body samples follow each player's current animation. If CS2 cannot provide a safe pose, visibility falls back to the existing fixed samples.
- Added a separate `bones` line to `cs2glaz_status` for the game-thread cost of capturing animated body points and the current animated/fallback player counts.
- Made visibility and automatic-baker startup fail open when their worker threads cannot be created, and made large BVH8 loads cancellable so map changes and shutdown do not wait on obsolete work.
- Reduced repeated runtime work by calculating each player's target samples once per cycle, skipping smoke capture when disabled, rejecting smoke volumes outside a ray early, listing each VPK once, and using a table-based streaming CRC32.
- Retried unavailable bone lookups, checked POSIX process setup failures, preserved native Windows paths and empty process arguments, and limited AVX code generation to the ray-traversal functions that require it.
- Tightened VPK source-path parsing, retained useful direct/nested lookup errors, and preserved native path encoding for temporary files and Unicode GLB/BVH8 paths.
- Expanded Visibility Studio from a point editor into a local 64 Hz first-person runtime simulator with direct BVH8 loading, map collision, navigation, bots, weapons, grenades, smoke, HE and bullet clearing, sounds, particles, and Real/Debug visibility.
- Made Studio use the runtime's animated body samples, AABB corners, muzzle point, ping-scaled viewing origins, wall decisions, smoke decisions, and ray counts; added interpolation for actors, grenades, LOS points, skeletons, AABBs, and debug geometry.
- Added 16 Hz LOS/BVH diagnostics, consistent depth-independent debug overlays, per-bot visibility-gate counts, and a 33% orange BVH fill with a 16% black outline.
- Added a pinned local Studio asset pipeline, compact player-animation exports, CS2 navigation export, optional baker surface sidecars, and automated checks for BVH8 traversal, movement, collision, smoke, HE, navigation, and runtime-layout consistency.
- Removed generated .NET `bin`/`obj` output from version control, ignored future generated output, and pinned Studio's Node dependencies.
- Pointed project and release downloads to the temporary GitLab home.

## 0.2.4-preview

- Rebuilt against the current Metamod:Source and HL2SDK so CS2GLAZ commands and settings register correctly after the July 17 CS2 tooling update.
- Tightened the upper eye origin from 24 to 16 units and changed ping preload to a 48-128 unit table: every 25 ms adds 10 units until the 200 ms cap. AABB side/top padding remains 8 units.
- Automatically treat every other living player as an enemy when `mp_teammates_are_enemies 1` is active.
- Reduced the lifecycle fail-open window from 3 seconds to 1 second and removed the separate 1.5-second visual warmup while preserving the complete-group baseline check.
- Updated Visibility Studio with a second SAS model 256 units away and the same stationary origins, target samples, and ray count used by the runtime.
- Replaced velocity/lookahead prediction with a ping-scaled W/S/diagonal intention origin, added a permanent feet origin, and reduced rays by 37.5% to 62.5% per player pair (from 192-384 to 120-144).

## 0.2.3-preview

- Verified that CS2 build `24248951` keeps the same private runtime layout and updated the strict Windows/Linux server fingerprints.
- Rebaked `cs_shelter`, `de_boulder`, `de_eldorado`, and `de_fachwerk` after their mounted map sources changed in the same update.

## 0.2.2-preview

- Verified the private runtime layout and updated the strict Windows/Linux fingerprints for CS2 build `24209309`.
- Removed the obsolete Valve string-token database import dropped by that update.
- Rewrote the README for ordinary server owners and added new visibility, smoke, HE, and map demonstrations.

## 0.2.1-preview

- Stopped generic owner/effect links from pulling independent gameplay entities into a hidden player's visual group.
- Kept planted C4, dropped objectives, grenade projectiles, infernos, sounds, and unknown entities independent so player culling cannot hide core gameplay state.
- Kept explicit player visuals together: pawn, known carried weapons (including carried C4), wearables, and a currently carried hostage prop.
- Simplified `cs2glaz_entity` evidence to direct visual-group membership.

## 0.2.0-preview

- Added default-on smoke occlusion from CS2's live voxel grid, with copied worker data and smoke-only fail-open behavior.
- Added wall-safe, configurable 2.5-second visibility channels through smoke disturbed by HE grenades.
- Fixed HE event discovery, post-initialization listener registration, and detonation-position reading without making ordinary smoke depend on HE support.
- Prevented an old HE event from clearing a smoke that detonated later by ordering both on CS2 game time.
- Matched visible smoke timing more closely by delaying initial occlusion and revealing fading smoke 0.5 seconds earlier.
- Added optional teammate visibility filtering with the same wall, smoke, prediction, and full-group rules used for enemies.
- Reorganized the runtime into map/game-state, worker, transmit, and automatic-baker responsibilities without intentionally changing proven visibility behavior.
- Updated CheckTransmit hiding to set CS2's matching `dont_transmit` bit before clearing a set primary transmit bit; missing lists fail open, while full updates and the other mask storage remain untouched.
- Bundled `sv_enable_donttransmit 0` as the compatibility default and automatically execute `cs2glaz.cfg` after convar registration and at every map start; paired-list handling also supports mode `1`.
- Let visible enemies return through ordinary snapshots instead of waiting for CS2 to schedule a full update.
- Tuned movement preload to a 75 ms base plus 1.5 times recipient RTT, capped at 375 ms and 96 units per player, with a smooth 75-100 speed ramp.
- Made left/right shoulder origins scale from 24 to 128 units with recipient RTT through public tuning controls.
- Kept safe movement up to baked walls, replaced merged target boxes with separate current/future boxes, and corrected stale-result age to use snapshot capture time.
- Added fixed-size `cs2glaz_entity` evidence for entity bits actually hidden by CS2GLAZ, including direct and owner/effect-linked membership.
- Hardened player lifecycles, visual-group identity, linked entities, stale results, and fail-open resets.
- Bound private gamedata to verified Windows and Linux server binaries and rejected unsafe player numbers before ray casting.
- Added snapshot-capture and CheckTransmit timings, full networked-edict linked-visual coverage, and accurate active-HE status wording.
- Captured bounded VRF and automatic-baker error output so failures include their useful final messages.
- Made Linux bake cancellation and timeout terminate and reap the complete baker/VRF process tree.
- Added validated BVH8 version 3 files with rooted-tree, reachability, depth, triangle, and streaming CRC checks plus verified atomic replacement; older or structurally invalid bakes are rejected.
- Restricted VPK version 2 embedded entries to the declared file-data section instead of accepting undeclared footer bytes.
- Added a machine-readable `--inspect-bvh8` command and require every official-map bake/report pair to match before packaging.
- Kept checksums from sequential platform packaging, required all three final archives, and bundled exact cgltf, ValveResourceFormat, native-library, and .NET redistribution notices.
- Prevented the LOS editor from exporting blank/duplicate names, invalid coordinates, or zero points.
- Moved all Workshop VPK discovery and extraction into the C++ baker, including the public `--list-maps` command.
- Added held-weapon muzzle sampling alongside body and axis-aligned bounding box target points.
- Split and expanded map/BVH and visibility/transmit tests, package verification, and the line-of-sight point editor checks.
- Added a plain-language code tour and corrected operator documentation.

## 0.1.2-preview

- Further hardened CheckTransmit player lifecycle checks.
- Hide pawn, current weapons, wearables, and carried hostage prop as one group.
- Preserve fail-open behavior when live player state is uncertain.

## 0.1.1-preview

- Hardened CheckTransmit against invalid indexes, stale player state, and stale weapon handles.
- Built Linux packages against SteamRT3 Sniper for CS2 server compatibility.
- Added CI checks for glibc, libstdc++, and C++ ABI requirements.

## 0.1.0-preview

First public preview of CS2GLAZ.

- Native Metamod plugin for server-side CS2 visibility culling.
- Offline and automatic map baker for official, custom, and Workshop maps.
- BVH8 runtime map data with AVX traversal.
- Smooth reveal envelope to reduce corner pop-in.
- Windows x86_64 and Linux x86_64 packages.
- Optional official map prebakes as a separate release asset.
