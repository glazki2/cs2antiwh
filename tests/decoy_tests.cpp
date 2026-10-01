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
		assert(result->decoys[0][0].id == 7 && result->decoy_hidden[0][0]);
		assert(result->decoys[0][1].id == 8 && !result->decoy_hidden[0][1]);
		assert(!result->decoy_hidden[0][2]);
		assert(result->decoys[0][4].id == 10 && result->decoy_hidden[0][4]);
		assert(result->decoys[1][0].id == 9 && !result->decoy_hidden[1][0]);
		worker->stop();
	}

	void test_decoy_kick()
	{
		static_assert(k_max_decoys_per_viewer == 5);
		// Evidence: real reports beyond twice the control ones.
		assert(decoy_evidence(0, 0) == 0);
		assert(decoy_evidence(4, 0) == 4);
		assert(decoy_evidence(4, 2) == 0);	 // as many coincidences as an honest player
		assert(decoy_evidence(6, 1) == 4);
		assert(decoy_evidence(1, 5) == 0);
		assert(decoy_evidence(0xffffffffffull, 0) == 0xffffffffffull);
		// 0 only logs, however much evidence.
		assert(!decoy_kick_due(100, 0, 0));
		assert(!decoy_kick_due(100, -1, 0));
		assert(!decoy_kick_due(2, 3, 0));
		assert(decoy_kick_due(3, 3, 0));
		// After a kick at evidence 3, a returning player needs 3 more.
		assert(!decoy_kick_due(5, 3, 3));
		assert(decoy_kick_due(6, 3, 3));
		assert(decoy_kick_due(0xffffffffffull, 100, 0));
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
}
