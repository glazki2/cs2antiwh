# Changelog

## 0.5.17

A live `cs2glaz_why` session on Dust II (two players at corners, limited mode) showed every reveal coming from a single viewing origin to the side or above, with the eye, feet and every other origin blocked: the walls were right, the peek allowances were wider than a standing player needs.

- A standing player's shoulders now cover only what he can accelerate through from rest before his first step reaches the server and the moving shoulder's sighting comes back: 687.5 * (ping + 0.08 s)^2 units (1375 u/s^2 acceleration), at least 4 and at most half the base. About 17 units at 77 ms ping instead of 24, 7 at 20 ms. Strafing or sliding shoulders are unchanged.
- The origin above the eye is 16 units only while jumping, moving vertically or crouched (standing up lifts the eye); a player on the ground gets 4.
- Without bones (limited mode) the body is built from the collision hull less 4 units on each side: the hull is 32 units wide, a model's shoulders about 24. The capsules used to reach 18.7 units from the centre, now 14.2.
- A viewing origin never ends inside a door or box occluder; one that would is dropped (shoulder, above, feet) or pulled back out (movement origin). A point inside a box used to see straight through it.

## 0.5.16

- Idle viewing shoulders no longer get the ping allowance. A live `cs2glaz_why` on Dust II showed `rtt=77ms shoulders idle=54`: a player standing still had viewing origins 54 units to each side, enough to look around a corner and receive an enemy nobody could see. The ping allowance now applies to a shoulder only while its key is held or the player still slides that way (lateral speed above 30 u/s, from the pawn velocity); a standing player's shoulders are half the base (24 units by default) at any ping. A player starting to strafe covers only a few units before the next sighting reaches him.
- The HUD, `cs2glaz_why` and the reveal log no longer hide the real reason behind the reveal hold. The live log showed `VISIBLE (hold)` every time, because a pair reported visible by the hold was re-proven visible every 300 ms but most passes fell inside the hold. They now say what opened the hold (`hold after body from right shoulder`, `удержание после: тело, плечо П`).
- `cs2glaz_why` re-runs every test from every viewing origin at the moment of the command (without smoke) and prints each origin's position with whether the body, how many of the eight bounds corners and the muzzle are open, so the exact point that sees an enemy is visible in the console.

## 0.5.15

- An enemy standing near a corner is no longer sent around it. The bounds corners that reveal an enemy a moment before he steps out were padded the same amount on every side (16 units, 32 before 0.5.14), so a player standing still just behind a corner, whom nobody could see, was sent to everyone around it. The padding now follows movement from the pawn's velocity (`m_vecAbsVelocity`) and movement keys: 4 units on every side for a standing player, growing towards where he moves or steers by his speed over 64 ms (two snapshots of interpolation plus a visibility pass), capped by `cs2glaz_bounds_padding_units` (16 units at running speed). Without the velocity field the old uniform padding applies.

## 0.5.14

- Doors and box-shaped props now block sight. The baked map holds only the static world, so a closed door or a crate that is an entity was transparent to the visibility check and a wallhack saw anyone behind it (reported on Dust II doors and boxes). Doors (`prop_door*`, `func_door*`) and props whose model name contains `crate`, `box`, `container` or `dumpster` (`prop_dynamic*`, `prop_physics*`) are found once a second by index through the verified entity table; every snapshot copies each one's collision box, current rotation and solidity. Doors keep their thickness and lose a unit at the edges, props shrink to 90%, so the box stays inside the real shape; non-solid, trigger and oddly shaped ones, and models with glass, windows, fences, gates, grates, mesh, vehicles and similar in their name are left out. The boxes block the exact rays that decide visibility, the shoulder, feet, above-head and movement origins, and the target's corner and muzzle points. A segment that starts or ends inside a box is never blocked by it. Model names are read once per entity through guarded reads. `cs2glaz_dynamic_occluders 0` turns this off (resets on restart).
- New `cs2glaz_props [radius]` lists the solid entities near each living human with class, model, solidity and size, marking those used as occluders, to find what a wallhack still sees through.
- New `cs2glaz_bounds_padding_units`, default 16 (was a fixed 32): the sideways padding of an enemy's bounds corners, which reveal him just before he steps out from a corner. With 32 an enemy was sent while 32 units behind a corner edge. Existing `cs2glaz.cfg` files do not have the line; add it or the default applies.

## 0.5.13

- New `cs2glaz_why [name or slot]` for the server console: for every living human (or the named player) it prints position, latency, shoulder sizes and body mode, then for each enemy the distance, whether the line of sight is open and what opened it (body, bounds corner, muzzle, hold or unproven, and from which viewing origin), and what CheckTransmit last decided in both directions. It turns a screenshot of a leak into the exact reason.
- With `cs2glaz_wallcheck` on, the server console logs every moment a human starts seeing an enemy, with the reason and distance (`[CS2GLAZ] wallcheck: A now sees B (body from right shoulder, 812 units)`).
- The player latency that sizes the viewing origins is clamped to 0-500 ms and must be finite. It comes from an engine virtual; a wrong value used to push the shoulders and the movement origin far out (there was no cap before 0.5.12).

## 0.5.12

- Enemies are now withheld in client full updates too. A full update is a snapshot the client rebuilds its entities from, and what it lacks is simply not created, as with ordinary PVS culling; the plugin used to send every enemy in it. A client can force one (starting a demo recording, or a forged network message from a cheat), so a wallhack could see every enemy on demand. `cs2glaz_filter_full_updates 0` restores the old behaviour; `cs2glaz_metrics` counts filtered full updates.
- A player on a thin floor is no longer revealed to the level below. The hull body used without bones (limited mode) had its bottom cap 8 units under the feet, and the bounds corners lay exactly on the floor surface, which a ray from below ending there did not count as crossed. The hull capsules now end half a unit above the feet and the bottom corners two units above them. The viewer's feet origin is lifted 4 units and, like the other origins, needs a clear line from the eye, so a rounded origin just under the floor cannot look below it.
- Narrower viewing shoulders by default: `cs2glaz_shoulder_base_units 48`, `cs2glaz_shoulder_rtt_scale 0.4`, `cs2glaz_max_shoulder_units 128`, the values upstream CS2FOW was tuned with (this fork's base had 64 / 0.64 / no cap). The shoulder origins stand beside a player's eye to reveal enemies a moment before a peek, and near a corner they looked around it: at 100 ms ping an idle shoulder reached 96 units, 128 when moving and 256 at 300 ms. Now 64, 88 and 128. Existing `cs2glaz.cfg` files keep their values.
- The wall-check HUD says why a pair is visible: the test (`тело`, `угол рамки`, `ствол`, `удержание`, `не доказано`) and the viewing origin (`глаза`, `плечо Л/П`, `над головой`, `ноги`, `шаг`). `cs2glaz_metrics` has a `reveals` line with the same counts summed over the map, so a leak can be traced to the origin or test that caused it.

## 0.5.11

- An enemy standing close behind a wall is no longer sent through it. The body check draws the map into a 32 by 32 depth buffer that keeps one conservative depth per tile, so for a wall tilted to the view it could not prove the wall nearer than a body within roughly ten units behind it, and "not proven hidden" counted as visible. This is the case reported live: the wallhack showed an enemy right behind a wall but not a little farther from it. Where the buffer cannot decide, the check now traces one exact ray per pixel to the nearest body point against the baked map triangles; where the conservative capsule mesh test (kept for openings between pixel centres) cannot prove a capsule, exact rays at four points per pixel decide instead of revealing. A body none of these rays reaches still reveals. In a synthetic scene of players standing 20-40 units from walls, 135 of 260 revealed pairs were behind walls and are now hidden; every pair the new check hides was confirmed by tracing 27,000 random points on the body, none open. Only these previously leaking pairs cost more (about 0.2 ms each); others are unchanged.
- A body too close to fit in front of one camera (a few dozen units from a viewing origin, such as the forward movement origin clipped against a wall while walking into it) is no longer revealed as uncertain. Its capsules are cut into pieces of at most 16 units, halved down to 2 units when a piece still does not fit, and each piece is proven with its own camera.
- `cs2glaz_visibility_hold_ms` now defaults to 300 ms instead of 1000 ms, so an enemy leaves a wallhack about 0.3 s after going behind cover instead of a full second. Existing `cs2glaz.cfg` files keep their value; set it there.
- Compared the plugin with three other open-source CS2 anti-wallhacks: SAWH (a 2D radar-grid visibility table, clears the pawn and weapons), Upex (CounterStrikeSharp, engine traces to a few points, 150 ms hold) and upstream CS2FOW 0.3.6 (older than this fork's 0.3.8 base; its bounds padding was 8 units, which the base raised to 32). None of them handles the second transmit list, the radar message or smokes; the shorter hold was taken from this comparison.

## 0.5.10

- An enemy standing near a thin wall is no longer sent through it. Besides the body, the visibility check tests a few extra points around an enemy so that a peek is revealed a moment early: the corners of his bounds padded by 32 units (48 units from his centre) and his weapon muzzle (up to 52 units ahead). These points were not stopped by walls, so within about 45 units of a wall thinner than that, a corner or the muzzle ended on the other side and anyone there "saw" it. A live test matched: the wallhack worked right behind a wall but not a bit farther away or behind a thicker wall. Each corner is now clipped on the segment from the enemy's body centre and the muzzle on the segment from his eye, so only space he could actually reach counts; in the open the points are unchanged. The points are computed once per enemy per visibility pass, and the line-of-sight debug beams show the clipped points.

## 0.5.9

- The wall-check HUD now shows line of sight on its own: each enemy line starts with `НА ВИДУ` or `ЗА СТЕНОЙ` (walls and smoke, from the visibility worker), followed by the transmit decision in both directions. With `cs2glaz_hide_all_enemies 1` every transmit decision reads `СКРЫТ` by design, which made the HUD look broken; the header now says hide-all is on and the line-of-sight part still changes.
- `cs2glaz_wallcheck 2` sends the same lines to chat instead of the centre of the screen, only when they change and at most once a second, in case the centre text is not shown.
- Dead and spectating humans get a HUD line saying so instead of nothing, so it is clear the HUD reaches them.
- New `cs2glaz_wallcheck_status` prints why the HUD is or is not shown (mode, state, players reached, messages sent, whether the TextMsg message was found, and whether CS2GLAZ is disabled). The server console also logs when the HUD starts reaching players.
- The TextMsg message is looked up by its exact name `CUserMessageTextMsg` first, and by partial name only as a fallback.

## 0.5.8

- Test HUD `cs2glaz_wallcheck 1` for checking walls with a second player: four times a second every living human gets a centre-screen message listing, for each enemy, what CheckTransmit last decided in both directions (`тебя ему` — does the enemy receive you, `его тебе` — do you receive the enemy) and, when an enemy is sent for another reason than sight, which one (spawn/death window, full update, attachment, weapons not listed, mode 0). It shows the real transmit decisions recorded per pair, not a separate estimate. It sends `CUserMessageTextMsg` through `IGameEventSystem`, proves the allocated message from its RTTI before filling it, and turns itself off if the message is missing or looks different. Off by default and not saved in `cs2glaz.cfg`.

## 0.5.7

- Smoke occlusion can now work in limited mode. A wallhack otherwise sees every enemy in or behind a smoke there, because the private smoke layout was dropped with the rest of the unverified gamedata. The gamedata smoke layout is now kept as a candidate and proven on the map's first live smokes through guarded reads: the voxel grid's centre must match the public `m_vSmokeDetonationPos`, its start time and frame must be plausible, and its occupancy mask and densities must look like a spread smoke (finite, bounded, a plausible share of dense cells, and a mask that mostly marks dense cells). If the volume moved inside the entity, offsets up to 1 KiB either side are tried and one is accepted only if it is the only match. Until then, and if three smokes fail, smoke occlusion stays off (retried on the next map). Every smoke is re-checked through guarded reads before its voxels are copied.
- Without the HE event listener (limited mode), an HE grenade projectile that disappears is taken as its detonation at the last position seen, so HE holes in smokes open as they do with the listener.
- `cs2glaz_status` shows a `Smoke:` line in limited mode, and `cs2glaz_metrics` a `smoke layout` line (state, offset, shift from gamedata, HE tracking).
- Research note: public CS2 ESP projects read the always-networked player controller (team, health, alive state, pawn handle) and the pawn (position, angles, bones, weapon, flags, spotted mask). Withholding the pawn and filtering the radar already cover the position data; a pawn the client once received stays as a frozen copy at the last position the player legitimately saw.

## 0.5.6

- Diagnostic `cs2glaz_hide_all_enemies 1` withholds every enemy even in plain view and drops every enemy from the radar, to show what still reaches a wallhack by other channels (sounds, grenades, the spawn/death safety window). It breaks normal play, is not saved in `cs2glaz.cfg` and resets to 0 on restart; the spawn/death safety window, full updates and the attachment checks still apply.

## 0.5.5

- An enemy is no longer left visible through walls after he threw a grenade or dropped a weapon. The plugin lists an enemy's pawn, weapons and wearables before hiding them, and any handle that no longer resolved made the whole list unusable, so the enemy was sent to everyone (after the 3-second quarantine of his last good list, or at once if the recipient had just seen him). The previous weapon handle (`m_hLastWeapon`) stays pointed at a thrown grenade or a dropped weapon, so this happened constantly: a live test counted 7541 such pair decisions (`weapons_unlisted`) against 130497 hidden ones, and it matched the report that an enemy stayed on a wallhack after being seen. Handles whose entity is gone are now skipped (there is nothing to withhold), and the previous weapon is no longer collected (while still owned it is in the weapons list; once dropped it may belong to another player, who must not lose it when this one is hidden). The pawn itself must still resolve.
- A live test confirmed the radar filter: the message was recognised and verified, and 1520 radar entries about hidden enemies were removed.

## 0.5.4

- Hidden enemies no longer reach a wallhack through the radar. CS2 sends each player the position and yaw of spotted enemies that are not in that player's snapshot (`CCSUsrMsg_ProcessSpottedEntityUpdate`), and withholding an enemy put him exactly there, so a cheat kept drawing an enemy for as long as he stayed spotted (about 30 seconds in a live test). The plugin now hooks `IGameEventSystem::PostEventAbstract` and removes from each player's copy the entries of enemies that nobody on that player's team currently sees; teammates, the bomb, hostages, and enemies a teammate sees stay on the radar. It proves the message object from its RTTI through guarded reads before touching it, only edits messages addressed to exactly one player, and turns itself off for good if the message does not look as expected. `cs2glaz_radar_filter 0` turns it off and `2` counts only the player's own sight (teammates then no longer share hidden enemies on the radar); `cs2glaz_metrics` reports what it kept and dropped.
- The Linux plugin no longer exports the symbols of the SDK's static protobuf library, which the radar filter pulls in, so they cannot bind to or shadow the server's own protobuf.
- An enemy is no longer shown through walls for a moment each time his weapons change (a grenade thrown, a weapon bought, picked up or dropped, the bomb planted). After every such change the plugin used to wait until the new set of entities had been sent once before hiding the player again. That wait comes from the CE design, where the second list froze entities on the client and the client had to hold them first; clearing a transmit bit is ordinary PVS culling, so the default mode now hides at once and only the legacy mode 0 keeps the wait.
- `cs2glaz_metrics` (for the whole map) and `cs2glaz_probe` (for the probe window) count why each enemy was withheld or let through: hidden, in view, spawning or dying (the one-second safety window), waiting for a baseline (mode 0 only), an attachment that cannot be hidden with the player, or weapons that could not be listed; and how many recipient snapshots were full updates or belonged to a player who had just spawned or died. A wallhack that flickers can now be traced to a reason.
- The probe on CS2 1.41.8 found every hidden enemy's weapons and attachments only in the primary list (6759 samples, none in the second list), so modes 1 and 2 withhold the same entities there.

## 0.5.3

- `cs2glaz_probe` also reports, for the sampled enemies, how many of their weapons, wearables and attached entities the game had set in the primary list (+0) and in the second list (+8). A clean client crashed once while only the primary bit was cleared (mode 1); if the game keeps such members in +8, mode 1 sent them without their player, which the default mode 2 prevents. The probe only reads.
- Tests with a wallhack showed heavy stutter near enemies only on the client running the cheat; a clean client did not stutter in mode 3 or in mode 2.

## 0.5.2

- Hidden enemies are now withheld with `cs2glaz_transmit_mode 2` by default: the plugin clears the entity's bit in the primary list and in the second list, and never sets a bit. On CS2 1.41.8 the second list (the SDK's `m_pTransmitAlways`) sends its entities to the client. A probe on a live server found no enemy pawn in it across 6400 samples, and a wallhack test showed that the old default (mode 0, which set the hidden entity's bit there) made the server send hidden enemies live every tick, while clearing the primary bit only (mode 1) kept enemies behind walls off the wallhack. Mode 2 also clears any member of the hidden group (weapons, wearables, attached entities) that the game had put in the second list, so no child is sent without its player.
- Mode 0 is kept for comparison and marked as leaking on CS2 1.41.8.
- README: describes the fix and what is still being checked.

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
