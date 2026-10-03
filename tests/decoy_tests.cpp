#include "builder.h"
#include "decoy_logic.h"
#include "signature_scan.h"
#include "test_suites.h"
#include "visibility_worker.h"

// Decoy geometry and aim checks, and the byte-pattern search the decoys use
// to find server functions on unknown builds.

#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

	using namespace cs2glaz;

	bvh8_data decoy_world(const std::vector<triangle>& triangles)
	{
		bvh8_data data;
		std::string error;
		assert(build_bvh8(triangles, data, error));
		return data;
	}

	// A wall across x = 200, from y -400 to 400 and z -100 to 300.
	std::vector<triangle> wall_at_200()
	{
		return {{{200, -400, -100}, {200, 400, -100}, {200, -400, 300}}, {{200, 400, 300}, {200, -400, 300}, {200, 400, -100}}};
	}

	std::span<const std::byte> bytes_of(const std::vector<uint8_t>& memory)
	{
		return {reinterpret_cast<const std::byte*>(memory.data()), memory.size()};
	}

	void test_byte_patterns()
	{
		byte_pattern pattern;
		assert(parse_byte_pattern("48 8D 05 ? ? ? ? 55 48 89 FA", pattern));
		assert(pattern.bytes.size() == 11 && pattern.mask[3] == 0 && pattern.mask[7] == 1 && pattern.bytes[10] == 0xfa);
		assert(parse_byte_pattern("  e8 ?? 90  ", pattern) && pattern.bytes.size() == 3 && pattern.mask[1] == 0);
		assert(!parse_byte_pattern("", pattern));
		assert(!parse_byte_pattern("? 48", pattern));
		assert(!parse_byte_pattern("48 8G", pattern));
		assert(!parse_byte_pattern("488D", pattern));

		std::vector<uint8_t> memory(4096, 0xcc);
		const std::vector<uint8_t> function {0x48, 0x8d, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x48, 0x89, 0xfa};
		std::memcpy(memory.data() + 1000, function.data(), function.size());
		assert(parse_byte_pattern("48 8D 05 ? ? ? ? 55 48 89 FA", pattern));
		pattern_matches matches = find_byte_pattern(bytes_of(memory), pattern);
		assert(matches.count == 1 && matches.first == reinterpret_cast<const std::byte*>(memory.data()) + 1000);

		// A second copy makes the pattern ambiguous.
		std::memcpy(memory.data() + 3000, function.data(), function.size());
		matches = find_byte_pattern(bytes_of(memory), pattern);
		assert(matches.count == 2);

		// A match cut off by the end of the range does not count.
		std::vector<uint8_t> tail(function.begin(), function.end() - 1);
		assert(find_byte_pattern(bytes_of(tail), pattern).count == 0);
		assert(find_byte_pattern(bytes_of(function), pattern).count == 1);
	}

	void test_decoy_spots()
	{
		const bvh8_data wall = decoy_world(wall_at_200());
		decoy_spot_history history;
		// Standing spots on both sides of the wall; the history keeps one per cell.
		for (int step = 0; step < 6; ++step)
		{
			history.record({-300.0f + step * 100.0f, 0.0f, 0.0f});
			history.record({300.0f + step * 60.0f, 200.0f, 0.0f});
			history.record({300.0f + step * 60.0f, 200.0f, 0.0f});
		}
		assert(history.points().size() == 12);

		const vec3 eye {0, 0, 64};
		std::vector<vec3> enemies {{900, -300, 0}};
		std::vector<vec3> players {{0, 0, 0}, {900, -300, 0}};
		vec3 spot;
		bool found = false;
		for (uint32_t seed = 1; seed < 40 && !found; ++seed)
		{
			found = choose_decoy_spot(wall, {}, history, {eye, enemies, players, {}, seed}, spot);
		}
		assert(found);
		// Behind the wall from the viewer, never in the open.
		assert(spot.x > 200.0f);
		assert(segment_blocked(wall, eye, {spot.x, spot.y, spot.z + k_decoy_center_height}).blocked);

		// A real enemy next to every hidden spot: no decoy there, so a decoy
		// never hints where an enemy is.
		std::vector<vec3> crowded {{450, 200, 0}};
		for (uint32_t seed = 1; seed < 40; ++seed)
		{
			vec3 other;
			if (choose_decoy_spot(wall, {}, history, {eye, crowded, players, {}, seed}, other))
			{
				const float x = other.x - 450.0f;
				const float y = other.y - 200.0f;
				assert(std::sqrt(x * x + y * y) >= k_decoy_enemy_clearance);
			}
		}

		// Without a wall nothing is hidden, so nothing is chosen.
		const bvh8_data open = decoy_world({{{5000, 5000, -10}, {5010, 5000, -10}, {5000, 5010, -10}}});
		for (uint32_t seed = 1; seed < 20; ++seed)
		{
			vec3 other;
			assert(!choose_decoy_spot(open, {}, history, {eye, enemies, players, {}, seed}, other));
		}

		history.clear();
		assert(history.points().empty());
		assert(!choose_decoy_spot(wall, {}, history, {eye, enemies, players, {}, 1}, spot));
	}

	void test_decoy_steps()
	{
		// A floor over x < 700, the wall at x = 200 and a second wall at y = 320
		// behind it.
		std::vector<triangle> triangles = wall_at_200();
		triangles.push_back({{-2000, -2000, 0}, {700, -2000, 0}, {-2000, 2000, 0}});
		triangles.push_back({{700, 2000, 0}, {-2000, 2000, 0}, {700, -2000, 0}});
		triangles.push_back({{250, 320, -100}, {800, 320, -100}, {250, 320, 300}});
		triangles.push_back({{800, 320, 300}, {250, 320, 300}, {800, 320, -100}});
		const bvh8_data world = decoy_world(triangles);
		decoy_spot_history history;
		for (const vec3 spot : {vec3 {400, 200, 0}, vec3 {500, 200, 0}, vec3 {600, 200, 0}, vec3 {400, 450, 0}, vec3 {900, 200, 0},
								vec3 {500, 100, 250}})
		{
			history.record(spot);
		}
		const vec3 eye {0, 0, 64};
		std::vector<vec3> none;
		bool moved = false;
		for (uint32_t seed = 1; seed < 200; ++seed)
		{
			vec3 next;
			if (!choose_decoy_step(world, {}, history, {eye, none, none, none, seed}, {400, 200, 0}, next))
			{
				continue;
			}
			moved = true;
			// Only the spots along the floor behind the first wall: never through
			// the second wall, off the floor, or up a cliff.
			assert(next.y == 200.0f && next.z == 0.0f && (next.x == 500.0f || next.x == 600.0f));
		}
		assert(moved);
		// A real enemy next to those spots: no step towards him.
		std::vector<vec3> enemy {{550, 200, 0}};
		for (uint32_t seed = 1; seed < 100; ++seed)
		{
			vec3 next;
			assert(!choose_decoy_step(world, {}, history, {eye, enemy, none, none, seed}, {400, 200, 0}, next));
		}
	}

	void test_decoy_hidden_proof()
	{
		const bvh8_data wall = decoy_world(wall_at_200());
		visibility_origin_points origins;
		origins.points[0] = {0, 0, 64};
		origins.count = 1;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		assert(decoy_hidden_from_origins(wall, origins, {400, 0, 0}, {}, deadline));
		// In front of the wall it is seen.
		assert(!decoy_hidden_from_origins(wall, origins, {100, 0, 0}, {}, deadline));
		// Past the wall's end it is seen.
		assert(!decoy_hidden_from_origins(wall, origins, {400, 900, 0}, {}, deadline));
		// A second viewing origin that looks around the wall's end reveals it.
		origins.points[1] = {150, 450, 64};
		origins.count = 2;
		assert(!decoy_hidden_from_origins(wall, origins, {400, 300, 0}, {}, deadline));
		// An expired deadline never proves anything hidden.
		origins.count = 1;
		assert(!decoy_hidden_from_origins(wall, origins, {400, 0, 0}, {}, std::chrono::steady_clock::now() - std::chrono::seconds(1)));
		visibility_origin_points none;
		assert(!decoy_hidden_from_origins(wall, none, {400, 0, 0}, {}, deadline));

		// The three verdicts: an expired deadline or bad input proves nothing
		// either way, which is not the same as being seen.
		assert(prove_decoy_hidden(wall, origins, {400, 0, 0}, {}, deadline) == decoy_proof::hidden);
		assert(prove_decoy_hidden(wall, origins, {100, 0, 0}, {}, deadline) == decoy_proof::seen);
		assert(prove_decoy_hidden(wall, origins, {400, 0, 0}, {}, std::chrono::steady_clock::now() - std::chrono::seconds(1))
			   == decoy_proof::unproven);
		assert(prove_decoy_hidden(wall, none, {400, 0, 0}, {}, deadline) == decoy_proof::unproven);
		assert(prove_decoy_hidden(wall, origins, {std::nanf(""), 0, 0}, {}, deadline) == decoy_proof::unproven);
	}

	void test_decoy_aim()
	{
		const vec3 eye {0, 0, 64};
		const vec3 decoy {500, 0, 64 - k_decoy_center_height};
		assert(aim_on_decoy(eye, 0.0f, 0.0f, decoy));
		assert(aim_on_decoy(eye, 1.0f, 2.0f, decoy));
		assert(!aim_on_decoy(eye, 0.0f, 10.0f, decoy));
		assert(!aim_on_decoy(eye, 0.0f, 180.0f, decoy));
		// Pitch is positive downwards in Source.
		const vec3 below {500, 0, -300};
		assert(!aim_on_decoy(eye, 0.0f, 0.0f, below));
		const float down = std::atan2(64.0f + 300.0f - k_decoy_center_height, 500.0f) * 57.29578f;
		assert(aim_on_decoy(eye, down, 0.0f, below));
		assert(!aim_on_decoy(eye, std::nanf(""), 0.0f, decoy));
		// Following: the aim has to turn with the direction to the decoy.
		const vec3 ahead {1, 0, 0};
		const auto towards = [](vec3 eye, vec3 origin)
		{
			const vec3 d {origin.x - eye.x, origin.y - eye.y, origin.z + k_decoy_center_height - eye.z};
			const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			return vec3 {d.x / length, d.y / length, d.z / length};
		};
		// Strafing 50 units while keeping the crosshair on it: about 5.7 degrees.
		const float strafe = decoy_follow_degrees({0, 0, 64}, ahead, decoy, {0, 50, 64}, towards({0, 50, 64}, decoy), decoy);
		assert(strafe > 5.0f && strafe < 6.5f);
		// Strafing with a fixed crosshair, sweeping over a still decoy, a decoy
		// walking through a still crosshair, walking straight at it: nothing.
		assert(decoy_follow_degrees({0, 0, 64}, ahead, decoy, {0, 50, 64}, ahead, decoy) < 0.01f);
		assert(decoy_follow_degrees({0, 0, 64}, ahead, decoy, {0, 0, 64}, towards({0, 0, 64}, {500, 40, 24}), decoy) < 0.01f);
		assert(decoy_follow_degrees({0, 0, 64}, ahead, {500, -20, 24}, {0, 0, 64}, ahead, {500, 20, 24}) < 0.01f);
		assert(decoy_follow_degrees({0, 0, 64}, ahead, decoy, {100, 0, 64}, ahead, decoy) < 0.01f);
		// A walking decoy followed by the crosshair.
		const vec3 walked {500, 50, 24};
		const float follow = decoy_follow_degrees({0, 0, 64}, ahead, decoy, {0, 0, 64}, towards({0, 0, 64}, walked), walked);
		assert(follow > 5.0f && follow < 6.5f);
		const vec3 forward = view_forward(0.0f, 90.0f);
		assert(std::fabs(forward.x) < 1.0e-5f && std::fabs(forward.y - 1.0f) < 1.0e-5f);
	}

	void test_worker_checks_decoys()
	{
		const bvh8_data wall = decoy_world(wall_at_200());
		// The worker's caches are megabytes: never on the stack.
		const auto worker = std::make_unique<visibility_worker>();
		assert(worker->start(&wall, 1));
		visibility_snapshot snapshot;
		snapshot.sequence = 1;
		snapshot.captured = std::chrono::steady_clock::now();
		player_state& viewer = snapshot.players[0];
		viewer.valid = true;
		viewer.team = 2;
		viewer.origin = {0, 0, 0};
		viewer.eye = {0, 0, 64};
		viewer.mins = {-16, -16, 0};
		viewer.maxs = {16, 16, 72};
		viewer.capsule_count = visibility_hull_capsules(viewer.origin, viewer.mins, viewer.maxs, viewer.capsules);
		snapshot.decoys[0][0] = {7, {400, 0, 0}};
		snapshot.decoys[0][1] = {8, {100, 0, 0}};
		// The fifth slot: one decoy per hidden enemy in a 5 on 5 match.
		snapshot.decoys[0][4] = {10, {400, 0, 0}};
		// A dead viewer's decoys are never proven hidden.
		snapshot.decoys[1][0] = {9, {400, 0, 0}};
		worker->submit(snapshot, 0, {});
		std::shared_ptr<const visibility_result> result;
		for (int attempt = 0; attempt < 500 && (result == nullptr || result->sequence != 1); ++attempt)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			result = worker->result();
		}
		assert(result != nullptr && result->sequence == 1);
		assert(result->decoys[0][0].id == 7 && result->decoy_hidden[0][0] && result->decoy_proven[0][0]);
		assert(result->decoys[0][1].id == 8 && !result->decoy_hidden[0][1] && result->decoy_proven[0][1]);
		assert(!result->decoy_hidden[0][2] && !result->decoy_proven[0][2]); // an empty slot proves nothing
		assert(result->decoys[0][4].id == 10 && result->decoy_hidden[0][4] && result->decoy_proven[0][4]);
		assert(result->decoys[1][0].id == 9 && !result->decoy_hidden[1][0] && !result->decoy_proven[1][0]);
		worker->stop();
	}

	void test_decoy_kick()
	{
		static_assert(k_max_decoys_per_viewer == 5);
		const decoy_exposure empty_server {};
		// No data: the prior rate, one coincidence per 500 decoy-seconds.
		assert(std::fabs(decoy_server_rate(empty_server) - k_decoy_prior_rate) < 1e-12);
		assert(decoy_expected_reports({}, empty_server) == 0.0);
		assert(decoy_evidence({}, empty_server) == 0.0);
		// A player with few coincidences: one report in 300 s of real decoys is
		// within chance, six are not.
		const decoy_exposure honest {1, 0, 300.0, 150.0};
		const decoy_exposure cheater {6, 0, 300.0, 150.0};
		assert(decoy_evidence(honest, empty_server) == 0.0);
		const double expected = decoy_expected_reports(cheater, empty_server);
		assert(expected > 0.1 && expected < 0.25);
		const double evidence = decoy_evidence(cheater, empty_server);
		assert(evidence > 4.0 && evidence < 5.0);
		// A player who often aims where decoys happen to be (his controls say so)
		// is expected at many real reports too.
		const decoy_exposure pre_aimer {8, 4, 600.0, 300.0};
		assert(decoy_expected_reports(pre_aimer, empty_server) > 6.0);
		assert(decoy_evidence(pre_aimer, empty_server) == 0.0);
		// The server's controls set the baseline for players with little of
		// their own.
		const decoy_exposure busy_server {0, 100, 0.0, 10000.0};
		assert(decoy_server_rate(busy_server) > 0.009 && decoy_server_rate(busy_server) < 0.01);
		assert(decoy_expected_reports({0, 0, 100.0, 0.0}, busy_server) > 0.9);
		assert(decoy_evidence(cheater, busy_server) < evidence);
		// Ten hours at the honest rate stay at no evidence: the margin grows with
		// the square root of the expected count.
		const decoy_exposure long_honest {90, 36, 36000.0, 18000.0};
		assert(decoy_evidence(long_honest, empty_server) == 0.0);
		// More real reports, more evidence; more control reports, less.
		assert(decoy_evidence({7, 0, 300.0, 150.0}, empty_server) > evidence);
		assert(decoy_evidence({6, 2, 300.0, 150.0}, empty_server) < evidence);
		// Kicks.
		assert(!decoy_kick_due(100.0, 0, 0.0));
		assert(!decoy_kick_due(100.0, -1, 0.0));
		assert(!decoy_kick_due(2.9, 3, 0.0));
		assert(decoy_kick_due(3.0, 3, 0.0));
		assert(!decoy_kick_due(6.4, 3, 3.5));
		assert(decoy_kick_due(6.5, 3, 3.5));
		assert(!decoy_kick_due(std::nan(""), 3, 0.0));
	}

	void test_decoy_precision()
	{
		// Reports count only after the decoy reached the client for the round
		// trip plus a reaction, and while it still does.
		assert(!decoy_delivery_ready(false, 10000.0f, 0.0f));
		assert(!decoy_delivery_ready(true, k_decoy_reaction_ms - 1.0f, 0.0f));
		assert(decoy_delivery_ready(true, k_decoy_reaction_ms, 0.0f));
		assert(!decoy_delivery_ready(true, k_decoy_reaction_ms + 50.0f, 80.0f));
		assert(decoy_delivery_ready(true, k_decoy_reaction_ms + 80.0f, 80.0f));
		assert(!decoy_delivery_ready(true, std::nanf(""), 0.0f));
		assert(decoy_delivery_ready(true, k_decoy_reaction_ms, std::nanf(""))); // unknown latency counts as 0
		assert(!decoy_delivery_ready(true, k_decoy_reaction_ms + 499.0f, 9999.0f)); // capped at 500 ms
		assert(decoy_delivery_ready(true, k_decoy_reaction_ms + 500.0f, 9999.0f));

		// An aim that is also on a real player, enemy or teammate, explains itself.
		const vec3 eye {0, 0, 64};
		const vec3 decoy {500, 0, 64 - k_decoy_center_height};
		assert(aim_on_decoy(eye, 0.0f, 0.0f, decoy));
		const std::vector<vec3> behind_the_decoy {{800, 10, 24}};
		const std::vector<vec3> elsewhere {{0, 600, 24}, {-500, 0, 24}};
		assert(aim_on_any_player(eye, 0.0f, 0.0f, behind_the_decoy));
		assert(!aim_on_any_player(eye, 0.0f, 0.0f, elsewhere));
		assert(!aim_on_any_player(eye, 0.0f, 0.0f, {}));
		static_assert(k_decoy_control_one_in >= 2);

		// A bullet's direction: from the eye to where it hit, any length. The
		// impact on the wall in front of the decoy lies on the line to it.
		assert(direction_on_decoy(eye, {200.0f, 0.0f, 0.0f}, decoy));
		assert(direction_on_decoy(eye, {0.001f, 0.0f, 0.0f}, decoy));
		assert(!direction_on_decoy(eye, {200.0f, 60.0f, 0.0f}, decoy));
		assert(!direction_on_decoy(eye, {-200.0f, 0.0f, 0.0f}, decoy));
		assert(!direction_on_decoy(eye, {0.0f, 0.0f, 0.0f}, decoy));
		assert(!direction_on_decoy(eye, {std::nanf(""), 0.0f, 0.0f}, decoy));
		assert(direction_on_any_player(eye, {200.0f, 4.0f, 0.0f}, behind_the_decoy));
		assert(!direction_on_any_player(eye, {200.0f, 4.0f, 0.0f}, elsewhere));
		// The view and the bullet agree for a shot straight ahead.
		assert(aim_on_decoy(eye, 0.0f, 0.0f, decoy) == direction_on_decoy(eye, view_forward(0.0f, 0.0f), decoy));
	}

	void test_decoy_view_preference()
	{
		// Walls in front (x = 200) and behind (x = -200) the viewer at the origin.
		std::vector<triangle> walls = wall_at_200();
		walls.push_back({{-200, -400, -100}, {-200, 400, -100}, {-200, -400, 300}});
		walls.push_back({{-200, 400, 300}, {-200, -400, 300}, {-200, 400, -100}});
		const bvh8_data world = decoy_world(walls);
		decoy_spot_history history;
		history.record({600, 0, 0});  // in front, farther
		history.record({-300, 0, 0}); // behind, closer
		const vec3 eye {0, 0, 64};
		const std::vector<vec3> players {{0, 0, 0}};
		const auto choose = [&](float yaw)
		{
			vec3 spot {};
			bool found = false;
			for (uint32_t seed = 1; seed < 40; ++seed)
			{
				vec3 candidate;
				decoy_spot_query query {eye, {}, players, {}, seed};
				query.view_yaw_degrees = yaw;
				if (choose_decoy_spot(world, {}, history, query, candidate))
				{
					// Every seed must agree once both spots were tried.
					spot = candidate;
					found = true;
				}
			}
			assert(found);
			return spot;
		};
		// Looking along +x: the spot in front wins although it is farther.
		assert(choose(0.0f).x > 0.0f);
		assert(choose(350.0f).x > 0.0f);
		// Looking back: the spot behind is the one on screen.
		assert(choose(180.0f).x < 0.0f);
		assert(choose(-170.0f).x < 0.0f);
		// No view: the closest.
		assert(choose(std::nanf("")).x < 0.0f);
		// Looking sideways: neither is on screen, so the closest.
		assert(choose(90.0f).x < 0.0f);
	}

	void test_decoy_one_run()
	{
		// Not sent yet: only a fresh proof starts a run.
		decoy_delivery_state state;
		assert(!decoy_may_deliver(state));
		state.proven_hidden = true;
		assert(decoy_may_deliver(state));
		// A proof that it is seen, its enemy in view, or the viewer gone stop it.
		for (int reason = 0; reason < 3; ++reason)
		{
			decoy_delivery_state stopped = state;
			stopped.proven_exposed = reason == 0;
			stopped.target_visible = reason == 1;
			stopped.viewer_gone = reason == 2;
			assert(!decoy_may_deliver(stopped));
		}
		// Running: a moment without a fresh proof keeps it, up to the latch.
		decoy_delivery_state running;
		running.running = true;
		running.unproven_ms = 0.0f;
		assert(decoy_may_deliver(running));
		running.unproven_ms = k_decoy_latch_ms;
		assert(decoy_may_deliver(running));
		running.unproven_ms = k_decoy_latch_ms + 1.0f;
		assert(!decoy_may_deliver(running));
		running.unproven_ms = std::nanf("");
		assert(!decoy_may_deliver(running));
		// The latch never outlasts a fresh "seen".
		running.unproven_ms = 10.0f;
		running.proven_exposed = true;
		assert(!decoy_may_deliver(running));
		// A run that ended is over even with a new proof: the decoy is retired.
		decoy_delivery_state ended;
		ended.ended = true;
		ended.proven_hidden = true;
		assert(!decoy_may_deliver(ended));
		// The latch is shorter than the reuse quarantine, which outlasts any
		// acknowledgement round trip the delivery check accepts.
		static_assert(k_decoy_latch_ms < k_decoy_reuse_quarantine_ms);
		static_assert(k_decoy_reuse_quarantine_ms >= 2.0f * 500.0f);
	}

	void test_decoy_jump()
	{
		// Angles as seen from the eye: 500 units ahead, 200 to the side is ~21.8 degrees.
		const vec3 eye {0, 0, 64};
		const vec3 ahead {500, 0, 64 - k_decoy_center_height};
		const vec3 side {500, 200, 64 - k_decoy_center_height};
		assert(std::fabs(decoy_angle_between(eye, ahead, side) - 21.8f) < 0.1f);
		assert(decoy_angle_between(eye, ahead, ahead) < 0.01f);
		assert(decoy_jump_distance_ok(eye, ahead, side));
		assert(!decoy_jump_distance_ok(eye, ahead, {500, 50, 64 - k_decoy_center_height}));	 // too close: ~5.7
		assert(!decoy_jump_distance_ok(eye, ahead, {-500, 0, 64 - k_decoy_center_height})); // behind him: 180
		assert(decoy_angle_between(eye, eye, ahead) == 0.0f || std::isfinite(decoy_angle_between(eye, eye, ahead)));

		// Followed: off the new spot until the reaction, then on it for the hold.
		decoy_jump_state followed;
		assert(!decoy_jump_update(followed, 50.0f, false, 15.6f));
		assert(!decoy_jump_update(followed, 200.0f, true, 15.6f));
		bool reported = false;
		for (float since = 215.6f; since < 400.0f && !reported; since += 15.6f)
		{
			reported = decoy_jump_update(followed, since, true, 15.6f);
		}
		assert(reported && followed.done);
		assert(!decoy_jump_update(followed, 450.0f, true, 15.6f)); // once only

		// Already there before anyone could react: nothing, and the test ends.
		decoy_jump_state early;
		assert(!decoy_jump_update(early, 60.0f, true, 15.6f));
		assert(early.done);
		assert(!decoy_jump_update(early, 300.0f, true, 500.0f));

		// Leaving it resets the hold; the window closes the test.
		decoy_jump_state flick;
		assert(!decoy_jump_update(flick, 300.0f, true, 100.0f));
		assert(!decoy_jump_update(flick, 315.0f, false, 15.0f));
		assert(flick.on_ms == 0.0f);
		assert(!decoy_jump_update(flick, 330.0f, true, 100.0f));
		assert(!decoy_jump_update(flick, k_decoy_jump_window_ms + 1.0f, true, 100.0f));
		assert(flick.done);
		decoy_jump_state broken;
		assert(!decoy_jump_update(broken, std::nanf(""), true, 15.0f) && broken.done);
	}

	void test_blind_hits()
	{
		// Before data, the prior share (5%).
		assert(std::fabs(blind_hit_server_share(0, 0) - k_blind_prior_share) < 1e-9);
		// Many hits move it to the server's own share.
		assert(std::fabs(blind_hit_server_share(1000, 10000) - (1000.0 + 25.0) / 10500.0) < 1e-9);
		// An honest player at the server's share has no evidence however long he plays.
		assert(blind_hit_evidence(5, 100, 0.05) == 0.0);
		assert(blind_hit_evidence(50, 1000, 0.05) == 0.0);
		// 20 hits: expected 1, plus 3 deviations = 4; 6 blind hits give 2.
		assert(std::fabs(blind_hit_evidence(6, 20, 0.05) - 2.0) < 1e-9);
		assert(blind_hit_evidence(0, 0, 0.05) == 0.0);
		assert(blind_hit_evidence(3, 3, std::nan("")) == 0.0); // unknown share counts as everything blind
	}

} // namespace

void run_decoy_tests()
{
	test_byte_patterns();
	test_decoy_spots();
	test_decoy_steps();
	test_decoy_hidden_proof();
	test_decoy_aim();
	test_worker_checks_decoys();
	test_decoy_kick();
	test_decoy_precision();
	test_decoy_view_preference();
	test_decoy_one_run();
	test_decoy_jump();
	test_blind_hits();
}
