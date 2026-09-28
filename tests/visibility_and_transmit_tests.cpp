#include "test_suites.h"

// Exercises copied visibility sampling/worker behavior and fixed CheckTransmit
// lifecycle, visual-group, mask, and evidence helpers. Fake data keeps engine
// objects and live network lists out of the unit tests.

#include "builder.h"
#include "capsule_visibility.h"
#include "lifecycle_guard.h"
#include "rtti_check.h"
#include "smoke_occlusion.h"
#include "transmit_debug.h"
#include "transmit_masks.h"
#include "visibility_sampling.h"
#include "visibility_worker.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace
{

	using namespace cs2glaz;

	bvh8_data test_world(const std::vector<triangle>& triangles)
	{
		bvh8_data data;
		std::string error;
		assert(build_bvh8(triangles, data, error));
		return data;
	}

	template<typename player_type>
	void set_test_capsules(player_type& player)
	{
		for (uint32_t index = 0; index < player.capsules.size(); ++index)
		{
			const float side = static_cast<float>(static_cast<int>(index % 3u) - 1) * 4.0f;
			const float low = 8.0f + static_cast<float>(index % 5u) * 10.0f;
			player.capsules[index] = {{player.origin.x, player.origin.y + side, player.origin.z + low},
									  {player.origin.x, player.origin.y + side, player.origin.z + low + 8.0f},
									  3.0f};
		}
		player.capsule_count = static_cast<uint32_t>(player.capsules.size());
	}

	visual_group_key test_visual_key(std::initializer_list<uint32_t> handles)
	{
		std::array<uint32_t, 8> values {};
		size_t count = 0;
		for (uint32_t handle : handles)
		{
			assert(count < values.size());
			values[count++] = handle;
		}
		return make_visual_group_key(values, count);
	}

	uint32_t test_morton(uint32_t x, uint32_t y, uint32_t z)
	{
		uint32_t result = 0;
		for (uint32_t bit = 0; bit < 5; ++bit)
		{
			result |= ((x >> bit) & 1u) << (bit * 3u);
			result |= ((y >> bit) & 1u) << (bit * 3u + 1u);
			result |= ((z >> bit) & 1u) << (bit * 3u + 2u);
		}
		return result;
	}

	void test_visibility_pair_eligibility()
	{
		player_state t;
		t.valid = true;
		t.team = 2;
		player_state ct = t;
		ct.team = 3;
		const player_state invalid;
		assert(visibility_pair_enabled(0, 1, t, ct, false));
		assert(visibility_pair_enabled(0, 1, t, ct, true));
		assert(!visibility_pair_enabled(0, 1, t, t, false));
		assert(visibility_pair_enabled(0, 1, t, t, true));
		assert(!visibility_pair_enabled(0, 0, t, ct, true));
		assert(!visibility_pair_enabled(0, 1, invalid, ct, true));
		assert(!visibility_pair_enabled(0, 1, t, invalid, true));
		player_state numbers = t;
		numbers.eye = {};
		numbers.origin = {};
		numbers.mins = {-16, -16, 0};
		numbers.maxs = {16, 16, 72};
		assert(valid_player_numbers(numbers));
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float infinity = std::numeric_limits<float>::infinity();
		numbers.eye.x = nan;
		assert(!valid_player_numbers(numbers));
		numbers.eye.x = 0;
		numbers.mins.y = infinity;
		assert(!valid_player_numbers(numbers));
		numbers.mins.y = -16;
		numbers.rtt_seconds = nan;
		assert(!valid_player_numbers(numbers));
		numbers.rtt_seconds = 0;
		numbers.mins.x = 17;
		assert(!valid_player_numbers(numbers));
	}

	void test_radar_entry_filter()
	{
		// Slots 0-1 are T (2), slots 2-3 are CT (3); pawn index = 100 + slot.
		auto result = std::make_unique<visibility_result>();
		for (uint32_t slot = 0; slot < 4; ++slot)
		{
			result->players[slot].valid = true;
			result->players[slot].team = slot < 2 ? 2 : 3;
			result->players[slot].pawn_entity = 100 + static_cast<int>(slot);
		}
		assert(radar_entry_target(*result, 102) == 2);
		assert(radar_entry_target(*result, 999) == -1);
		assert(radar_entry_target(*result, -1) == -1);
		// Nobody on T sees CT slot 2: its radar position is withheld from T.
		assert(!radar_entry_allowed(*result, 0, 2));
		// A teammate (slot 1) sees it: legitimate team radar, kept for slot 0.
		result->visible[1][2] = true;
		assert(radar_entry_allowed(*result, 0, 2));
		// The strict setting ignores what teammates see.
		assert(!radar_entry_allowed(*result, 0, 2, true));
		result->visible[0][2] = true;
		assert(radar_entry_allowed(*result, 0, 2, true));
		result->visible[0][2] = false;
		result->visible[1][2] = false;
		// Teammates, the bomb/hostages (no target) and oneself are always kept.
		assert(radar_entry_allowed(*result, 0, 1));
		assert(radar_entry_allowed(*result, 0, -1));
		assert(radar_entry_allowed(*result, 0, 0));
		// A dead or spectating recipient keeps everything.
		result->players[0].valid = false;
		assert(radar_entry_allowed(*result, 0, 2));
		result->players[0].valid = true;
		// Free for all: only the recipient's own sight counts.
		result->filter_teammates = true;
		assert(!radar_entry_allowed(*result, 0, 1));
		result->visible[1][2] = true;
		assert(!radar_entry_allowed(*result, 0, 2));
		result->visible[0][2] = true;
		assert(radar_entry_allowed(*result, 0, 2));
	}

	struct rtti_test_header
	{
		virtual ~rtti_test_header() = default;
		char padding[40] {};
	};

	struct rtti_test_payload
	{
		virtual ~rtti_test_payload() = default;
		int value {};
	};

	struct rtti_test_other
	{
		virtual ~rtti_test_other() = default;
	};

	// Mirrors CNetMessagePB<T>: a polymorphic header first, the payload second.
	template<typename payload>
	struct rtti_test_message : rtti_test_header, payload
	{
	};

	void test_rtti_check()
	{
		const auto read = [](const void* address, void* output, size_t size)
		{
			std::memcpy(output, address, size);
			return true;
		};
		const rtti_test_message<rtti_test_payload> message;
		const rtti_test_header* owner = &message;
		const rtti_test_payload* payload = &message;
		assert(rtti_names_class_at_offset(owner, payload, "rtti_test_payload", read));
		// Wrong class name, wrong offset, or no offset at all.
		assert(!rtti_names_class_at_offset(owner, payload, "rtti_test_other", read));
		const auto* shifted = reinterpret_cast<const char*>(payload) - sizeof(void*);
		assert(!rtti_names_class_at_offset(owner, shifted, "rtti_test_payload", read));
		assert(!rtti_names_class_at_offset(owner, owner, "rtti_test_payload", read));
		const rtti_test_message<rtti_test_other> other;
		const rtti_test_other* other_base = &other;
		assert(!rtti_names_class_at_offset(static_cast<const rtti_test_header*>(&other), other_base, "rtti_test_payload", read));
		// An unreadable address stops the check instead of faulting.
		const auto refuse = [](const void*, void*, size_t) { return false; };
		assert(!rtti_names_class_at_offset(owner, payload, "rtti_test_payload", refuse));
	}

	void test_smoke_occlusion()
	{
		he_clearance_history history;
		assert(!history.record({std::numeric_limits<float>::quiet_NaN(), 0, 0}, 10.0f));
		assert(!history.record({}, std::numeric_limits<float>::quiet_NaN()));
		for (uint32_t index = 0; index <= k_max_he_clearances; ++index)
		{
			assert(history.record({static_cast<float>(index), 0, 0}, 10.0f));
		}
		assert(history.count == k_max_he_clearances && history.next == 1 && history.records[0].center.x == 64.0f);
		history.clear();
		assert(history.count == 0 && history.next == 0);

		smoke_snapshot smoke;
		smoke.volumes.reserve(2);
		smoke.volumes.emplace_back();
		smoke_volume_snapshot& volume = smoke.volumes.back();
		volume.age_seconds = 2.0f;
		volume.density.fill(50.0f);
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		assert(smoke_line_blocked(smoke, {100, 0, 0}, {-100, 0, 0}));
		assert(!smoke_line_blocked(smoke, {400, 0, 0}, {500, 0, 0}));
		volume.center = {-1000, 0, 0};
		assert(!smoke_line_blocked(smoke, {0, 0, 0}, {100, 0, 0}));
		volume.center = {1000, 0, 0};
		assert(!smoke_line_blocked(smoke, {0, 0, 0}, {100, 0, 0}));
		volume.center = {};
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float infinity = std::numeric_limits<float>::infinity();
		const float maximum = std::numeric_limits<float>::max();
		assert(!smoke_line_blocked(smoke, {nan, 0, 0}, {100, 0, 0}));
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {infinity, 0, 0}));
		assert(!smoke_line_blocked(smoke, {-maximum, 0, 0}, {maximum, 0, 0}));

		volume.density.fill(5.0f);
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		volume.density.fill(5.5f);
		assert(!smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		volume.density.fill(0.0f);
		volume.density[test_morton(16, 16, 16)] = 40.0f;
		assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
		volume.density.fill(0.0f);
		volume.opaque_cells[test_morton(16, 16, 16) >> 3u] = static_cast<uint8_t>(1u << (test_morton(16, 16, 16) & 7u));
		assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));

		volume.opaque_cells.fill(0);
		volume.density.fill(5.5f);
		smoke.volumes.push_back(volume);
		assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
		smoke.volumes.resize(1);
		volume = smoke.volumes.front();
		volume.density.fill(50.0f);
		volume.age_seconds = 0.75f;
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		volume.age_seconds = 1.25f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		volume.age_seconds = 19.8f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
		volume.age_seconds = 20.3f;
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));

		const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
		const bvh8_data blast_wall =
			test_world({{{-100, 25, -100}, {100, 25, -100}, {100, 25, 100}}, {{-100, 25, -100}, {100, 25, 100}, {-100, 25, 100}}});
		volume.age_seconds = 2.0f;
		volume.center = {};
		volume.density.fill(50.0f);
		smoke.he_clear_radius_units = 100.0f;
		smoke.he_clear_seconds = 3.0f;
		smoke.he_clearance_count = 1;
		smoke.he_clearances[0] = {{0, 50, 0}, 2.999f, 2.0f};
		volume.start_time = 1.0f;
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		volume.start_time = 3.0f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		volume.start_time = 2.0f;
		assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &blast_wall));
		smoke.he_clearances[0].age_seconds = 3.0f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		smoke.he_clearances[0].age_seconds = 2.0f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 1.0f, &open));
		smoke.he_clear_radius_units = 0.0f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		smoke.he_clear_radius_units = 100.0f;
		smoke.he_clear_seconds = 0.0f;
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
		smoke.he_clear_seconds = 3.0f;
		smoke.he_clearances[0] = {{500, 0, 0}, 1.0f, 2.0f};
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {700, 0, 0}, 0.0f, &open));
		smoke.he_clearances[0] = {{0, 0, 0}, 1.0f, 2.0f};
		smoke.volumes.push_back(volume);
		smoke.volumes.back().center = {500, 0, 0};
		assert(smoke_line_blocked(smoke, {-100, 0, 0}, {700, 0, 0}, 0.0f, &open));
		smoke.volumes.resize(1);
		smoke.he_clearance_count = 0;

		std::vector<std::byte> storage(k_smoke_storage_density_offset + 2u * k_smoke_storage_frame_stride);
		storage[k_smoke_storage_mask_offset] = std::byte {1};
		const float first = 12.5f;
		const float second = 25.0f;
		std::memcpy(storage.data() + k_smoke_storage_density_offset, &first, sizeof(first));
		std::memcpy(storage.data() + k_smoke_storage_density_offset + k_smoke_storage_frame_stride, &second, sizeof(second));
		smoke_volume_snapshot copied;
		assert(copy_smoke_frame(storage.data(), 0, {1, 2, 3}, 2.0f, copied));
		assert(copied.center.x == 1.0f && copied.opaque_cells[0] == 1 && copied.density[0] == first);
		assert(copy_smoke_frame(storage.data(), 1, {1, 2, 3}, 2.0f, copied));
		assert(copied.density[0] == second);
		assert(!copy_smoke_frame(storage.data(), 2, {}, 2.0f, copied));
		int frame_reads = 0;
		assert(copy_stable_smoke_frame(storage.data(), {}, 2.0f, copied, [&] { return frame_reads++ == 0 ? 0 : 1; }));
		assert(frame_reads == 4 && copied.density[0] == second);
		frame_reads = 0;
		assert(!copy_stable_smoke_frame(storage.data(), {}, 2.0f, copied, [&] { return frame_reads++ & 1; }));
		const float invalid = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(storage.data() + k_smoke_storage_density_offset, &invalid, sizeof(invalid));
		assert(!copy_smoke_frame(storage.data(), 0, {}, 2.0f, copied));
	}

	void test_visibility_sampling()
	{
		const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
		const visibility_tuning tuning {64.0f, 0.64f, 0.0f};
		visibility_player player {};
		player.eye = {0, 0, 64};
		player.origin = {0, 0, 0};
		player.mins = {-16, -16, 0};
		player.maxs = {16, 16, 72};

		assert(std::fabs(visibility_shoulder_offset_units(-1.0f, tuning, false) - 32.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.0f, tuning, false) - 32.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.024f, tuning, false) - 32.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.025f, tuning, false) - 48.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.049f, tuning, false) - 48.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.05f, tuning, false) - 64.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.075f, tuning, true) - 112.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.1f, tuning, true) - 128.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.125f, tuning, true) - 144.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.15f, tuning, true) - 160.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.175f, tuning, true) - 176.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.199f, tuning, true) - 176.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.2f, tuning, true) - 192.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.3f, tuning, true) - 256.0f) < 0.01f);
		assert(std::fabs(visibility_shoulder_offset_units(0.0f, tuning, true) - 64.0f) < 0.01f);

		auto origins = visibility_origins(open, player, tuning);
		assert(k_visibility_origin_count_max == 6);
		assert(k_visibility_capsule_count == 19);
		assert(k_visibility_pixel_grid_size == 32 && k_visibility_pixel_count == 1024);
		constexpr float pi = 3.14159265358979323846f;
		assert(1.06f * std::cos(pi / 12.0f) * std::cos(pi / 16.0f) >= 1.0f);
		assert(origins.count == 5);
		assert(origins.points[0].x == 0.0f && origins.points[0].z == 64.0f);
		assert(std::fabs(origins.points[1].y - 32.0f) < 0.01f);
		assert(std::fabs(origins.points[2].y + 32.0f) < 0.01f);
		assert(std::fabs(origins.points[3].z - 80.0f) < 0.01f);
		assert(origins.points[4].x == player.origin.x && origins.points[4].y == player.origin.y && origins.points[4].z == player.origin.z);
		player.rtt_seconds = 0.05f;
		origins = visibility_origins(open, player, tuning);
		assert(std::fabs(origins.points[1].y - 64.0f) < 0.01f && std::fabs(origins.points[2].y + 64.0f) < 0.01f);
		player.rtt_seconds = 0.0f;

		player.eye_yaw_degrees = 90.0f;
		origins = visibility_origins(open, player, tuning);
		assert(std::fabs(origins.points[1].x + 32.0f) < 0.01f);
		assert(std::fabs(origins.points[2].x - 32.0f) < 0.01f);
		player.eye_yaw_degrees = 0.0f;

		player.movement_buttons = k_visibility_button_forward;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 6 && std::fabs(origins.points[1].y - 32.0f) < 0.01f && std::fabs(origins.points[2].y + 32.0f) < 0.01f
			   && std::fabs(origins.points[5].x - 64.0f) < 0.01f);
		player.movement_buttons = k_visibility_button_back;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 6 && std::fabs(origins.points[1].y - 32.0f) < 0.01f && std::fabs(origins.points[2].y + 32.0f) < 0.01f
			   && std::fabs(origins.points[5].x + 64.0f) < 0.01f);
		player.movement_buttons = k_visibility_button_left;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 5 && std::fabs(origins.points[1].y - 64.0f) < 0.01f && std::fabs(origins.points[2].y + 32.0f) < 0.01f);
		player.movement_buttons = k_visibility_button_right;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 5 && std::fabs(origins.points[1].y - 32.0f) < 0.01f && std::fabs(origins.points[2].y + 64.0f) < 0.01f);

		const float diagonal = 64.0f / std::sqrt(2.0f);
		const std::array<std::pair<uint64_t, vec3>, 4> diagonals {
			{{k_visibility_button_forward | k_visibility_button_left, {diagonal, diagonal, 64}},
			 {k_visibility_button_forward | k_visibility_button_right, {diagonal, -diagonal, 64}},
			 {k_visibility_button_back | k_visibility_button_left, {-diagonal, diagonal, 64}},
			 {k_visibility_button_back | k_visibility_button_right, {-diagonal, -diagonal, 64}}}};
		for (const auto& [buttons, expected] : diagonals)
		{
			player.movement_buttons = buttons;
			origins = visibility_origins(open, player, tuning);
			assert(origins.count == 6);
			assert(std::fabs(origins.points[5].x - expected.x) < 0.01f && std::fabs(origins.points[5].y - expected.y) < 0.01f);
		}
		player.movement_buttons = k_visibility_button_forward | k_visibility_button_back;
		assert(visibility_origins(open, player, tuning).count == 5);
		player.movement_buttons |= k_visibility_button_left;
		assert(visibility_origins(open, player, tuning).count == 5);
		player.movement_buttons = k_visibility_button_forward | k_visibility_button_left | k_visibility_button_right;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 6 && std::fabs(origins.points[5].x - 64.0f) < 0.01f);
		player.movement_buttons = k_visibility_button_forward;
		player.eye_yaw_degrees = 90.0f;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 6 && std::fabs(origins.points[5].y - 64.0f) < 0.01f);
		player.eye_yaw_degrees = 0.0f;

		const bvh8_data wall = test_world({{{4, -100, -100}, {4, 100, -100}, {4, -100, 100}}, {{4, 100, 100}, {4, -100, 100}, {4, 100, -100}}});
		origins = visibility_origins(wall, player, tuning);
		assert(origins.count == 6 && origins.points[5].x > 3.9f && origins.points[5].x < 4.1f);
		const vec3 clipped_short = visibility_clip_destination(wall, {}, {5, 0, 0});
		const vec3 clipped_long = visibility_clip_destination(wall, {}, {20, 0, 0});
		assert(clipped_short.x > 3.9f && clipped_short.x < 4.1f);
		assert(clipped_long.x >= clipped_short.x - 0.1f && clipped_long.x < 4.1f);

		const bvh8_data side_wall =
			test_world({{{-100, -8, -100}, {100, -8, -100}, {-100, -8, 100}}, {{100, -8, 100}, {-100, -8, 100}, {100, -8, -100}}});
		player.movement_buttons = k_visibility_button_right;
		origins = visibility_origins(side_wall, player, tuning);
		assert(origins.count == 4);

		const bvh8_data ceiling =
			test_world({{{-100, -100, 72}, {100, -100, 72}, {-100, 100, 72}}, {{100, 100, 72}, {-100, 100, 72}, {100, -100, 72}}});
		player.movement_buttons = 0;
		origins = visibility_origins(ceiling, player, tuning);
		assert(origins.count == 4);

		visibility_player target {};
		target.origin = {0, 0, 0};
		target.mins = {-16, -16, 0};
		target.maxs = {16, 16, 72};
		target.muzzle_class = weapon_muzzle_class::rifle;
		const auto aabb = visibility_aabb_points(target);
		assert(aabb.size() == 8 && aabb.front().x == -48.0f && aabb.front().z == 0.0f && aabb.back().x == 48.0f && aabb.back().z == 80.0f);
		vec3 muzzle;
		assert(visibility_muzzle_point(target, muzzle));
		assert(std::fabs(muzzle.x - 36.0f) < 0.01f && std::fabs(muzzle.y) < 0.01f && std::fabs(muzzle.z - 60.0f) < 0.01f);
		target.muzzle_class = weapon_muzzle_class::none;
		assert(!visibility_muzzle_point(target, muzzle));
		target.muzzle_class = weapon_muzzle_class::rifle;
		target.eye_yaw_degrees = 90.0f;
		assert(visibility_muzzle_point(target, muzzle));
		assert(std::fabs(muzzle.x) < 0.01f && std::fabs(muzzle.y - 36.0f) < 0.01f);
		target.eye_yaw_degrees = 0.0f;
		target.maxs.z = 54.0f;
		assert(visibility_muzzle_point(target, muzzle));
		assert(muzzle.z < 50.0f && muzzle.z > 45.0f);

		visibility_bone_transform identity {{10.0f, 20.0f, 30.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
		vec3 transformed;
		assert(visibility_transform_point(identity, {1.0f, 2.0f, 3.0f}, transformed));
		assert(std::fabs(transformed.x - 11.0f) < 0.001f && std::fabs(transformed.y - 22.0f) < 0.001f && std::fabs(transformed.z - 33.0f) < 0.001f);
		const float half_sqrt_two = std::sqrt(0.5f);
		visibility_bone_transform quarter_turn {{}, {0.0f, 0.0f, half_sqrt_two, half_sqrt_two}};
		assert(visibility_transform_point(quarter_turn, {1.0f, 0.0f, 0.0f}, transformed));
		assert(std::fabs(transformed.x) < 0.001f && std::fabs(transformed.y - 1.0f) < 0.001f);
		visibility_bone_transform invalid_transform {};
		assert(!visibility_transform_point(invalid_transform, {}, transformed));
		assert(std::strcmp(k_visibility_capsule_bindings.front().bone, "head_0") == 0);
		assert(std::fabs(k_visibility_capsule_bindings.front().radius - 4.3f) < 0.001f);
		assert(std::strcmp(k_visibility_capsule_bindings.back().bone, "arm_lower_r") == 0);
		set_test_capsules(target);
		assert(std::all_of(target.capsules.begin(), target.capsules.end(), valid_visibility_capsule));
		assert(!valid_visibility_capsule({{}, {}, 0.0f}));

		assert(weapon_muzzle_class_from_item_definition(61) == weapon_muzzle_class::pistol);
		assert(weapon_muzzle_class_from_item_definition(34) == weapon_muzzle_class::smg);
		assert(weapon_muzzle_class_from_item_definition(7) == weapon_muzzle_class::rifle);
		assert(weapon_muzzle_class_from_item_definition(9) == weapon_muzzle_class::sniper);
		assert(weapon_muzzle_class_from_item_definition(999) == weapon_muzzle_class::none);
		assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::pistol) - 18.0f) < 0.01f);
		assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::smg) - 28.0f) < 0.01f);
		assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::rifle) - 36.0f) < 0.01f);
		assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::sniper) - 52.0f) < 0.01f);
		assert(weapon_muzzle_length(weapon_muzzle_class::none) == 0.0f);

		using clock = std::chrono::steady_clock;
		const auto start = clock::time_point {} + std::chrono::seconds(1);
		assert(visibility_snapshot_fresh(start, start + std::chrono::milliseconds(100)));
		assert(!visibility_snapshot_fresh(start, start + std::chrono::milliseconds(101)));
	}

	void test_capsule_visibility()
	{
		visibility_player target;
		target.origin = {64.0f, 0.0f, 0.0f};
		set_test_capsules(target);
		const vec3 viewer {0.0f, 0.0f, 32.0f};
		const auto deadline = std::chrono::steady_clock::time_point::max();
		capsule_query_stats stats;
		const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
		assert(capsule_visible_from_origin(open, viewer, target.capsules, nullptr, 0.0f, deadline, nullptr, &stats) == capsule_query_result::visible);
		assert(stats.visited_nodes != 0);

		const bvh8_data wall = test_world({{{32, -100, -100}, {32, 100, -100}, {32, -100, 100}}, {{32, 100, 100}, {32, -100, 100}, {32, 100, -100}}});
		capsule_occluder_cache cache;
		capsule_query_stats cold_stats;
		assert(capsule_visible_from_origin(wall, viewer, target.capsules, nullptr, 0.0f, deadline, nullptr, &cold_stats, &cache)
			   == capsule_query_result::blocked);
		assert(cache.count != 0 && cold_stats.visited_nodes != 0);
		assert(cold_stats.moc_render_calls != 0 && cold_stats.moc_rect_tests != 0);
		assert(cold_stats.rebuilt_proofs == 1 && cold_stats.rebuilt_proof_leaves != 0);
		assert(cold_stats.max_rebuilt_proof_leaves == cold_stats.rebuilt_proof_leaves);
		assert(cold_stats.cache_saturations == 0 && cold_stats.uncached_blocked == 0);
		capsule_query_stats warm_stats;
		assert(capsule_visible_from_origin(wall, viewer, target.capsules, nullptr, 0.0f, deadline, nullptr, &warm_stats, &cache)
			   == capsule_query_result::blocked);
		assert(warm_stats.visited_nodes == 0 && warm_stats.rasterized_triangles <= cold_stats.rasterized_triangles);
		assert(warm_stats.moc_render_calls != 0 && warm_stats.moc_rect_tests != 0);
		assert(warm_stats.rebuilt_proofs == 0 && warm_stats.occluder_cache_hits == 1);
		std::vector<triangle> compact_triangles(8, {{16, -1, 31}, {16, 1, 31}, {16, 0, 33}});
		compact_triangles.push_back({{32, -100, -100}, {32, 100, -100}, {32, -100, 100}});
		compact_triangles.push_back({{32, 100, 100}, {32, -100, 100}, {32, 100, -100}});
		const bvh8_data compact_wall = test_world(compact_triangles);
		capsule_occluder_cache compact_cache;
		capsule_query_stats compact_stats;
		assert(capsule_visible_from_origin(compact_wall, viewer, target.capsules, nullptr, 0.0f, deadline, nullptr, &compact_stats, &compact_cache)
			   == capsule_query_result::blocked);
		assert(compact_stats.cache_compaction_trials == 1 && compact_stats.cache_compactions == 1);
		assert(compact_stats.cache_compaction_leaves_saved == 1 && compact_cache.count == 1);
		target.origin.x = 16.0f;
		set_test_capsules(target);
		assert(capsule_visible_from_origin(wall, viewer, target.capsules, nullptr, 0.0f, deadline, nullptr, nullptr, &cache)
			   == capsule_query_result::visible);
		assert(cache.count == 0);
		target.origin.x = 64.0f;
		set_test_capsules(target);

		const bvh8_data half_wall =
			test_world({{{32, -100, -100}, {32, 100, -100}, {32, -100, 35}}, {{32, 100, 35}, {32, -100, 35}, {32, 100, -100}}});
		assert(capsule_visible_from_origin(half_wall, viewer, target.capsules, nullptr, 0.0f, deadline) == capsule_query_result::visible);

		assert(capsule_visible_from_origin(open, viewer, std::span<const visibility_capsule>(target.capsules.data(), target.capsules.size() - 1u),
										   nullptr, 0.0f, deadline)
			   == capsule_query_result::visible);
		assert(capsule_visible_from_origin(open, viewer, std::span<const visibility_capsule>(target.capsules.data(), 0u), nullptr, 0.0f, deadline)
			   == capsule_query_result::indeterminate);
		assert(capsule_visible_from_origin(open, viewer, target.capsules, nullptr, 0.0f, std::chrono::steady_clock::now())
			   == capsule_query_result::indeterminate);
	}

	void test_hull_capsules()
	{
		std::array<visibility_capsule, k_visibility_capsule_count> capsules {};
		const vec3 origin {100.0f, -50.0f, 8.0f};
		const vec3 mins {-16.0f, -16.0f, 0.0f};
		const vec3 maxs {16.0f, 16.0f, 72.0f};
		assert(visibility_hull_capsules(origin, mins, maxs, capsules) == k_visibility_hull_capsule_count);
		const auto distance_sq = [](vec3 point, vec3 start, vec3 end)
		{
			const vec3 axis {end.x - start.x, end.y - start.y, end.z - start.z};
			const vec3 offset {point.x - start.x, point.y - start.y, point.z - start.z};
			const float length_sq = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
			const float t =
				length_sq <= 0.0f ? 0.0f : std::clamp((offset.x * axis.x + offset.y * axis.y + offset.z * axis.z) / length_sq, 0.0f, 1.0f);
			const vec3 closest {start.x + axis.x * t, start.y + axis.y * t, start.z + axis.z * t};
			const vec3 delta {point.x - closest.x, point.y - closest.y, point.z - closest.z};
			return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
		};
		constexpr int k_steps = 16;
		for (int ix = 0; ix <= k_steps; ++ix)
		{
			for (int iy = 0; iy <= k_steps; ++iy)
			{
				for (int iz = 0; iz <= k_steps; ++iz)
				{
					const float fx = static_cast<float>(ix) / k_steps;
					const float fy = static_cast<float>(iy) / k_steps;
					const float fz = static_cast<float>(iz) / k_steps;
					const vec3 point {origin.x + mins.x + (maxs.x - mins.x) * fx, origin.y + mins.y + (maxs.y - mins.y) * fy,
									  origin.z + mins.z + (maxs.z - mins.z) * fz};
					bool covered = false;
					for (uint32_t index = 0; index < k_visibility_hull_capsule_count && !covered; ++index)
					{
						covered = distance_sq(point, capsules[index].start, capsules[index].end) <= capsules[index].radius * capsules[index].radius;
					}
					assert(covered);
				}
			}
		}
		for (uint32_t index = 0; index < k_visibility_hull_capsule_count; ++index)
		{
			assert(valid_visibility_capsule(capsules[index]) && capsules[index].radius <= 9.0f);
			assert(capsules[index].start.z == origin.z + mins.z && capsules[index].end.z == origin.z + maxs.z);
		}
		const float nan = std::numeric_limits<float>::quiet_NaN();
		assert(visibility_hull_capsules(origin, maxs, mins, capsules) == 0);
		assert(visibility_hull_capsules(origin, {nan, -16.0f, 0.0f}, maxs, capsules) == 0);

		const vec3 viewer {0.0f, 0.0f, 64.0f};
		const vec3 target {128.0f, 0.0f, 0.0f};
		const uint32_t count = visibility_hull_capsules(target, mins, maxs, capsules);
		const std::span<const visibility_capsule> body(capsules.data(), count);
		const auto deadline = std::chrono::steady_clock::time_point::max();
		const bvh8_data wall =
			test_world({{{64, -200, -100}, {64, 200, -100}, {64, -200, 200}}, {{64, 200, 200}, {64, -200, 200}, {64, 200, -100}}});
		assert(capsule_visible_from_origin(wall, viewer, body, nullptr, 0.0f, deadline) == capsule_query_result::blocked);
		const bvh8_data low_wall =
			test_world({{{64, -200, -100}, {64, 200, -100}, {64, -200, 40}}, {{64, 200, 40}, {64, -200, 40}, {64, 200, -100}}});
		assert(capsule_visible_from_origin(low_wall, viewer, body, nullptr, 0.0f, deadline) == capsule_query_result::visible);
	}

	void test_visibility_worker()
	{
		assert(!visibility_teammate_filter_enabled(false, false));
		assert(visibility_teammate_filter_enabled(true, false));
		assert(visibility_teammate_filter_enabled(false, true));
		assert(visibility_teammate_filter_enabled(true, true));

		const bvh8_data wall =
			test_world({{{24, -1000, -1000}, {24, 1000, -1000}, {24, -1000, 1000}}, {{24, 1000, 1000}, {24, -1000, 1000}, {24, 1000, -1000}}});
		visibility_snapshot value;
		value.sequence = 1;
		value.players[0] = {true, 2, {0, 0, 64}, {0, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
		value.players[1] = {true, 3, {80, 0, 64}, {80, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
		set_test_capsules(value.players[0]);
		set_test_capsules(value.players[1]);
		const visibility_tuning tuning {64.0f, 0.64f, 0.0f};

		auto worker = std::make_unique<visibility_worker>();
		assert(worker->start(&wall, 2));
		assert(worker->stats().cycles == 0 && worker->stats().thread_count == 2);
		const auto wait_for = [&](uint64_t sequence)
		{
			std::shared_ptr<const visibility_result> result;
			for (int attempt = 0; attempt < 2000 && (!result || result->sequence != sequence); ++attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				result = worker->result();
			}
			return result;
		};
		worker->submit(value, 100, tuning);
		auto result = wait_for(1);
		assert(result && !result->visible[0][1]);

		value.sequence = 2;
		value.players[1].eye.x = 16;
		value.players[1].origin.x = 16;
		set_test_capsules(value.players[1]);
		worker->submit(value, 100, tuning);
		result = wait_for(2);
		assert(result && result->visible[0][1]);

		value.sequence = 3;
		value.players[1].eye.x = 80;
		value.players[1].origin.x = 80;
		set_test_capsules(value.players[1]);
		worker->submit(value, 100, tuning);
		result = wait_for(3);
		assert(result && result->visible[0][1] && result->hold_reuses != 0);

		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		value.sequence = 4;
		worker->submit(value, 100, tuning);
		result = wait_for(4);
		assert(result && !result->visible[0][1]);
		value.players[1].team = 2;
		value.sequence = 5;
		value.filter_teammates = false;
		worker->submit(value, 100, tuning);
		result = wait_for(5);
		assert(result && result->visible[0][1] && result->evaluated_pairs == 0 && !result->filter_teammates);
		value.sequence = 6;
		value.filter_teammates = true;
		worker->submit(value, 100, tuning);
		result = wait_for(6);
		assert(result && !result->visible[0][1] && result->evaluated_pairs == 2 && result->filter_teammates);
		assert(worker->stats().cycles == 6);
		assert(worker->start(&wall, 4));
		assert(worker->stats().cycles == 0 && worker->stats().thread_count == 4);
		for (uint64_t sequence = 100; sequence < 164; ++sequence)
		{
			const bool open_side = (sequence & 1u) == 0u;
			value.sequence = sequence;
			value.players[1].eye.x = open_side ? 16.0f : 80.0f;
			value.players[1].origin.x = open_side ? 16.0f : 80.0f;
			set_test_capsules(value.players[1]);
			worker->submit(value, 0, tuning);
			result = wait_for(sequence);
			assert(result && result->visible[0][1] == open_side);
		}
		worker->stop();

		const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
		assert(worker->start(&open, 2));
		auto smokes = std::make_shared<smoke_snapshot>();
		smokes->volumes.emplace_back();
		smokes->volumes.back().age_seconds = 2.0f;
		smokes->volumes.back().density.fill(50.0f);
		value.sequence = 7;
		value.captured = std::chrono::steady_clock::now();
		value.players[0] = {true, 2, {0, 0, 64}, {0, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
		value.players[1] = {true, 2, {64, 0, 64}, {64, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
		set_test_capsules(value.players[0]);
		set_test_capsules(value.players[1]);
		value.filter_teammates = true;
		value.smoke_enabled = true;
		value.smoke_available = true;
		value.smokes = smokes;
		worker->submit(value, 0, tuning);
		result = wait_for(7);
		assert(result && !result->visible[0][1] && result->smoke_count == 1);
		smokes->volumes.back().density.fill(0.0f);
		for (uint32_t x = 16; x < 20; ++x)
		{
			const uint32_t cell = test_morton(x, 16, 19);
			smokes->volumes.back().opaque_cells[cell >> 3u] |= static_cast<uint8_t>(1u << (cell & 7u));
		}
		assert(smoke_line_blocked(*smokes, {0, 0, 64}, {64, 0, 64}));
		value.sequence = 8;
		worker->submit(value, 0, tuning);
		result = wait_for(8);
		assert(result && result->visible[0][1]);
		smokes->volumes.back().opaque_cells.fill(0);
		smokes->volumes.back().density.fill(50.0f);
		value.sequence = 9;
		value.filter_teammates = false;
		worker->submit(value, 0, tuning);
		result = wait_for(9);
		assert(result && result->visible[0][1] && result->evaluated_pairs == 0);
		value.sequence = 10;
		value.filter_teammates = true;
		value.smoke_available = false;
		worker->submit(value, 0, tuning);
		result = wait_for(10);
		assert(result && result->visible[0][1]);
		smokes->he_clear_radius_units = 100.0f;
		smokes->he_clear_seconds = 3.0f;
		smokes->he_clearance_count = 1;
		smokes->he_clearances[0] = {{32, 0, 64}, 1.0f};
		value.sequence = 11;
		value.captured = std::chrono::steady_clock::now();
		value.smoke_available = true;
		worker->submit(value, 0, tuning);
		result = wait_for(11);
		assert(result && result->visible[0][1] && result->he_clearance_count == 1);
		smokes->he_clearances[0].age_seconds = 3.0f;
		value.sequence = 12;
		value.captured = std::chrono::steady_clock::now();
		worker->submit(value, 0, tuning);
		result = wait_for(12);
		assert(result && !result->visible[0][1]);
		assert(worker->stats().recent_p95_ms > 0.0 && worker->stats().recent_p99_ms > 0.0);
		worker->stop();
	}

	void test_lifecycle_guard()
	{
		using clock = std::chrono::steady_clock;
		const auto grace = std::chrono::milliseconds(3000);
		const auto start = clock::time_point {} + std::chrono::seconds(10);
		lifecycle_guard guard;
		lifecycle_key stable;
		stable.has_controller = true;
		stable.pawn_entity = 12;
		stable.team = 2;
		stable.alive = true;

		update_lifecycle_guard(guard, stable, true, start, grace);
		assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(2999)));
		assert(lifecycle_allows_hiding(guard, start + grace));

		update_lifecycle_guard(guard, stable, true, start + grace, grace);
		assert(lifecycle_allows_hiding(guard, start + grace));

		lifecycle_key changed = stable;
		changed.team = 3;
		update_lifecycle_guard(guard, changed, true, start + grace + std::chrono::milliseconds(1), grace);
		assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(5999)));
		assert(lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(6001)));

		lifecycle_key dead = changed;
		dead.alive = false;
		update_lifecycle_guard(guard, dead, false, start + std::chrono::seconds(7), grace);
		update_lifecycle_guard(guard, dead, false, start + std::chrono::milliseconds(7500), grace);
		assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(10499)));

		update_lifecycle_guard(guard, changed, true, start + std::chrono::milliseconds(10500), grace);
		assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(13499)));
		assert(lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(13500)));
	}

	void test_visual_group_key()
	{
		const visual_group_key left = test_visual_key({30, 10, 20, 10});
		const visual_group_key right = test_visual_key({20, 30, 10});
		assert(!visual_group_changed(left, right));
		assert(left.count == 3);

		assert(visual_group_changed(left, test_visual_key({10, 20})));
		assert(visual_group_changed(test_visual_key({326u | (1u << 15)}), test_visual_key({326u | (2u << 15)})));

		std::array<uint32_t, k_pair_visual_group_key_max> full {};
		for (size_t index = 0; index < full.size(); ++index)
		{
			full[index] = static_cast<uint32_t>(index + 1);
		}
		const visual_group_key full_key = make_visual_group_key(full, full.size());
		full.back() += 1000;
		assert(full_key.count == k_pair_visual_group_key_max);
		assert(visual_group_changed(full_key, make_visual_group_key(full, full.size())));
	}

	void test_pair_guard()
	{
		lifecycle_key recipient;
		recipient.has_controller = true;
		recipient.pawn_entity = 10;
		recipient.team = 2;
		recipient.alive = true;
		lifecycle_key target = recipient;
		target.pawn_entity = 20;
		target.team = 3;

		pair_guard guard;
		assert(update_pair_guard(guard, recipient, true, target, true));
		assert(!update_pair_guard(guard, recipient, true, target, true));
		assert(!pair_allows_hiding(guard, 1));
		pair_note_open(guard, 1);
		assert(!pair_allows_hiding(guard, 1));
		assert(pair_allows_hiding(guard, 2));

		pair_guard visual_guard;
		const visual_group_key group = test_visual_key({10, 20, 30});
		const visual_group_key same_group = test_visual_key({30, 20, 10, 10});
		const visual_group_key changed_group = test_visual_key({10, 20, 31});
		update_pair_guard(visual_guard, recipient, true, target, true);
		update_pair_visual_group(visual_guard, group);
		assert(!pair_allows_hiding(visual_guard, 1));
		pair_note_open(visual_guard, 1);
		assert(pair_allows_hiding(visual_guard, 2));
		update_pair_visual_group(visual_guard, same_group);
		assert(pair_allows_hiding(visual_guard, 3));
		update_pair_visual_group(visual_guard, changed_group);
		assert(!pair_allows_hiding(visual_guard, 4));
		// Clear-only modes hide at once after a group change (thrown grenade,
		// pickup); only the legacy mode waits for a sent baseline.
		assert(pair_allows_hiding(visual_guard, 4, false));
		assert(!pair_allows_hiding(visual_guard, 4, true));
		pair_note_open(visual_guard, 4);
		assert(!pair_allows_hiding(visual_guard, 4));
		assert(pair_allows_hiding(visual_guard, 5));
		assert(pair_allows_hiding(visual_guard, 5, true));
		pair_guard uninitialized;
		assert(!pair_allows_hiding(uninitialized, 1, false));

		lifecycle_key changed = target;
		changed.team = 2;
		assert(update_pair_guard(guard, recipient, true, changed, true));
		assert(!pair_allows_hiding(guard, 3));
		pair_note_open(guard, 3);
		assert(!pair_allows_hiding(guard, 3));
		assert(pair_allows_hiding(guard, 4));

		lifecycle_key dead = changed;
		dead.alive = false;
		assert(update_pair_guard(guard, recipient, true, dead, false));
		assert(update_pair_guard(guard, recipient, true, dead, false));
		assert(!pair_allows_hiding(guard, 6));

		assert(update_pair_guard(guard, recipient, true, changed, true));
		assert(!pair_allows_hiding(guard, 7));
		pair_note_open(guard, 7);
		assert(!pair_allows_hiding(guard, 7));
		assert(pair_allows_hiding(guard, 8));
	}

	struct test_transmit_mask
	{
		bool set {};
		int sets {};
		int clears {};
		int checked_index {-1};
		int set_index {-1};
		int cleared_index {-1};
		std::vector<char>* operations {};

		bool IsBitSet(int index)
		{
			checked_index = index;
			return set;
		}

		void Set(int index)
		{
			++sets;
			set_index = index;
			set = true;
			if (operations != nullptr)
			{
				operations->push_back('S');
			}
		}

		void Clear(int index)
		{
			++clears;
			cleared_index = index;
			set = false;
			if (operations != nullptr)
			{
				operations->push_back('C');
			}
		}
	};

	void test_checktransmit_private_offsets()
	{
		uint32_t value {};
		assert(parse_gamedata_uint32(" 580 ", value) && value == 580);
		assert(!parse_gamedata_uint32("580x", value));
		assert(valid_gamedata_offset(580, alignof(bool), 4096));
		assert(!valid_gamedata_offset(0, alignof(void*), 4096));

		struct fake_info
		{
			std::array<std::byte, 32> padding {};
			bool full_update {};
		};

		fake_info info;
		assert(!read_checktransmit_full_update(&info, 32));
		info.full_update = true;
		assert(read_checktransmit_full_update(&info, 32));

		test_transmit_mask null_primary_dont;
		assert(!withhold_transmit_bit<test_transmit_mask>(nullptr, &null_primary_dont, 10));
		assert(!null_primary_dont.set && null_primary_dont.sets == 0);
		test_transmit_mask null_dont_primary {true};
		assert(!withhold_transmit_bit(&null_dont_primary, static_cast<test_transmit_mask*>(nullptr), 10));
		assert(null_dont_primary.set && null_dont_primary.clears == 0);

		test_transmit_mask unset_primary;
		test_transmit_mask untouched_dont;
		assert(!withhold_transmit_bit(&unset_primary, &untouched_dont, 10));
		assert(unset_primary.clears == 0 && !untouched_dont.set && untouched_dont.sets == 0);

		std::vector<char> operations;
		test_transmit_mask primary {true};
		test_transmit_mask dont_transmit;
		primary.operations = &operations;
		dont_transmit.operations = &operations;
		assert(withhold_transmit_bit(&primary, &dont_transmit, 10));
		assert(!primary.set && primary.clears == 1 && dont_transmit.set && dont_transmit.sets == 1);
		assert(primary.checked_index == 10 && primary.cleared_index == 10 && dont_transmit.set_index == 10);
		assert(operations == std::vector<char>({'S', 'C'}));

		test_transmit_mask primary_again {true};
		test_transmit_mask already_dont_transmit {true};
		assert(withhold_transmit_bit(&primary_again, &already_dont_transmit, 10));
		assert(!primary_again.set && already_dont_transmit.set && already_dont_transmit.sets == 1);

		// Diagnostic modes: each touches exactly the lists it names.
		test_transmit_mask mark_primary {true};
		test_transmit_mask mark_second;
		assert(apply_transmit_mode(&mark_primary, &mark_second, 10, transmit_mode::clear_and_mark));
		assert(!mark_primary.set && mark_second.set);
		test_transmit_mask only_primary {true};
		test_transmit_mask only_second {true};
		assert(apply_transmit_mode(&only_primary, &only_second, 10, transmit_mode::clear_primary));
		assert(!only_primary.set && only_second.set && only_second.sets == 0 && only_second.clears == 0);
		test_transmit_mask both_primary {true};
		test_transmit_mask both_second {true};
		assert(apply_transmit_mode(&both_primary, &both_second, 10, transmit_mode::clear_both));
		assert(!both_primary.set && !both_second.set && both_second.clears == 1);
		test_transmit_mask second_only_primary;
		test_transmit_mask second_only_second {true};
		assert(apply_transmit_mode(&second_only_primary, &second_only_second, 10, transmit_mode::clear_both));
		assert(!second_only_second.set && second_only_primary.clears == 0);
		test_transmit_mask watch_primary {true};
		test_transmit_mask watch_second {true};
		assert(!apply_transmit_mode(&watch_primary, &watch_second, 10, transmit_mode::observe));
		assert(watch_primary.set && watch_second.set && watch_primary.clears == 0 && watch_second.clears == 0);
		assert(!apply_transmit_mode<test_transmit_mask>(nullptr, &watch_second, 10, transmit_mode::clear_primary));
		test_transmit_mask extended_primary {true};
		test_transmit_mask extended_second {true};
		assert(apply_transmit_mode(&extended_primary, &extended_second, 10, transmit_mode::clear_extended));
		assert(!extended_primary.set && !extended_second.set);
		test_transmit_mask union_primary {true};
		test_transmit_mask union_second {true};
		assert(apply_transmit_mode(&union_primary, &union_second, 10, transmit_mode::clear_union));
		assert(!union_primary.set && !union_second.set);
	}

	void test_transmit_debug()
	{
		using clock = std::chrono::steady_clock;
		const auto start = clock::time_point {} + std::chrono::seconds(10);
		transmit_debug_log<2, 16> log;
		transmit_debug_event first {10, 100, 1, 1, k_transmit_reason_current, start};
		test_transmit_mask primary {true};
		test_transmit_mask dont_transmit;
		const auto clear_and_record = [&](bool debug)
		{
			if (withhold_transmit_bit(&primary, &dont_transmit, 10) && debug)
			{
				log.record(first, "player");
			}
		};
		clear_and_record(true);
		clear_and_record(true);
		primary.set = true;
		clear_and_record(false);
		assert(log.records()[0].clears == 1);
		primary.set = true;
		first.recipient_slot = 3;
		first.reason = k_transmit_reason_quarantine;
		first.when += std::chrono::milliseconds(5);
		clear_and_record(true);
		const auto& records = log.records();
		assert(records[0].valid && records[0].clears == 2 && records[0].recipients == ((uint64_t {1} << 1) | (uint64_t {1} << 3)));
		assert(records[0].reasons == (k_transmit_reason_current | k_transmit_reason_quarantine));
		assert(records[0].first_seen == start && records[0].last_seen == start + std::chrono::milliseconds(5));

		transmit_debug_log<2, 16> distinct_sources;
		distinct_sources.record({10, 100, 1, 1, k_transmit_reason_current, start}, "player");
		distinct_sources.record({10, 100, 2, 1, k_transmit_reason_current, start}, "player");
		assert(distinct_sources.records()[0].valid && distinct_sources.records()[1].valid);
		assert(distinct_sources.records()[0].source != distinct_sources.records()[1].source);

		transmit_debug_event second {20, 200, 2, 2, k_transmit_reason_current, start};
		transmit_debug_event third {30, 300, 3, 3, k_transmit_reason_current, start};
		log.record(second, "weapon");
		log.record(third, "wearable");
		assert(records[0].handle == 300 && records[1].handle == 200);
		log.clear();
		assert(!records[0].valid && !records[1].valid);
	}

	void test_hidden_entity_group()
	{
		using clock = std::chrono::steady_clock;
		const auto start = clock::time_point {} + std::chrono::seconds(30);
		hidden_entity_group<uint32_t, 3> source;
		std::array<uint32_t, 4> handles {10, 11, 12, 0};
		source.source = 10;
		std::copy_n(handles.begin(), source.handles.size(), source.handles.begin());
		source.count = 3;
		hidden_entity_group<uint32_t, 3> group;

		hidden_group_store(group, source, start, std::chrono::milliseconds(3000));
		assert(group.count == 3);
		assert(group.source == 10u);
		assert(hidden_group_quarantined(group, start + std::chrono::milliseconds(2999)));
		assert(!hidden_group_quarantined(group, start + std::chrono::milliseconds(3000)));
		assert(group.count == 0);

		hidden_group_store(group, source, start, std::chrono::milliseconds(3000));
		std::array<bool, 16> marked {};
		marked[10] = true;
		marked[11] = true;
		assert(!hidden_group_all_of(group, [&](uint32_t handle) { return marked[handle]; }));
		marked[12] = true;
		assert(hidden_group_all_of(group, [&](uint32_t handle) { return marked[handle]; }));

		hidden_group_clear(group);
		assert(hidden_group_append_unique(group, 10u));
		assert(hidden_group_append_unique(group, 10u));
		assert(hidden_group_append_unique(group, 20u));
		assert(hidden_group_append_unique(group, 30u));
		assert(group.count == 3);
		const std::array<uint32_t, 3> expected {10, 20, 30};
		assert(group.handles == expected);
		assert(hidden_group_append_unique(group, 30u));
		assert(!hidden_group_append_unique(group, 40u));
	}

	double benchmark_worker_loop(const bvh8_data& data, const std::string& label)
	{
		if (label == "de_mirage")
		{
			constexpr uint32_t team_size = 5;
			constexpr uint32_t player_count = team_size * 2u;
			constexpr uint32_t pair_count = team_size * team_size * 2u;
			const visibility_tuning blocked_tuning {64.0f, 0.64f, 0.0f};
			std::array<visibility_player, player_count> blocked_players {};
			for (uint32_t index = 0; index < player_count; ++index)
			{
				const bool first_team = index < team_size;
				const float offset = static_cast<float>(static_cast<int>(index % team_size) - 2) * 12.0f;
				const vec3 origin = first_team ? vec3 {-1902.0f + offset, -1976.0f, -212.14798f} : vec3 {1376.0f + offset, -16.0f, -103.96875f};
				blocked_players[index] = {{origin.x, origin.y, origin.z + 64.0f},
										  origin,
										  {-16.0f, -16.0f, 0.0f},
										  {16.0f, 16.0f, 72.0f},
										  0.0f,
										  0.0f,
										  k_visibility_button_forward,
										  weapon_muzzle_class::rifle};
				set_test_capsules(blocked_players[index]);
			}
			std::array<visibility_origin_points, player_count> blocked_origins {};
			std::array<capsule_occluder_cache, pair_count * k_visibility_origin_count_max> blocked_cache {};
			std::array<double, 20> blocked_timings {};
			uint64_t blocked_pairs = 0;
			uint64_t blocked_nodes = 0;
			uint64_t blocked_triangles = 0;
			uint32_t frame = 0;
			for (double& timing : blocked_timings)
			{
				const auto start = std::chrono::steady_clock::now();
				for (uint32_t index = 0; index < player_count; ++index)
				{
					const float motion = std::sin(static_cast<float>(frame) * 0.2f + static_cast<float>(index)) * 2.0f;
					const float base_y = index < team_size ? -1976.0f : -16.0f;
					blocked_players[index].origin.y = base_y + motion;
					blocked_players[index].eye.y = base_y + motion;
					set_test_capsules(blocked_players[index]);
					blocked_origins[index] = visibility_origins(data, blocked_players[index], blocked_tuning);
				}
				uint32_t pair = 0;
				for (uint32_t recipient = 0; recipient < player_count; ++recipient)
				{
					const uint32_t first_target = recipient < team_size ? team_size : 0u;
					for (uint32_t target = first_target; target < first_target + team_size; ++target, ++pair)
					{
						bool blocked = true;
						for (uint32_t origin_index = 0; origin_index < blocked_origins[recipient].count; ++origin_index)
						{
							capsule_query_stats stats;
							const capsule_query_result query =
								capsule_visible_from_origin(data, blocked_origins[recipient].points[origin_index], blocked_players[target].capsules,
															nullptr, 0.0f, std::chrono::steady_clock::time_point::max(), nullptr, &stats,
															&blocked_cache[pair * k_visibility_origin_count_max + origin_index]);
							blocked_nodes += stats.visited_nodes;
							blocked_triangles += stats.rasterized_triangles;
							if (query != capsule_query_result::blocked)
							{
								blocked = false;
								break;
							}
						}
						blocked_pairs += blocked;
					}
				}
				timing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				++frame;
			}
			std::sort(blocked_timings.begin(), blocked_timings.end());
			double blocked_average = 0.0;
			for (double timing : blocked_timings)
			{
				blocked_average += timing;
			}
			blocked_average /= blocked_timings.size();
			std::cout << label << " blocked-5v5: average=" << blocked_average << "ms p99=" << blocked_timings.back() << "ms pairs=" << pair_count
					  << " nodes=" << blocked_nodes / blocked_timings.size() << " triangles=" << blocked_triangles / blocked_timings.size() << '\n'
					  << std::flush;
			assert(blocked_pairs == pair_count * blocked_timings.size());
			assert(blocked_average < 5.0);

			constexpr uint32_t dm_player_count = 32;
			const auto benchmark_runtime =
				[&](uint32_t thread_count, bool free_for_all, uint32_t expected_pairs, uint32_t expected_hidden, const char* scenario)
			{
				auto worker = std::make_unique<visibility_worker>();
				assert(worker->start(&data, thread_count));
				std::array<double, 20> dm_timings {};
				double dm_active = 0.0;
				uint64_t dm_cache_hits = 0;
				uint64_t dm_cache_queries = 0;
				uint64_t dm_triangles = 0;
				uint64_t dm_draws = 0;
				uint64_t dm_rects = 0;
				uint64_t dm_proofs = 0;
				uint64_t dm_proof_leaves = 0;
				uint32_t dm_max_proof_leaves = 0;
				uint64_t dm_cache_saturations = 0;
				uint64_t dm_cache_compaction_trials = 0;
				uint64_t dm_cache_compactions = 0;
				uint64_t dm_cache_compaction_leaves_saved = 0;
				uint64_t dm_uncached_blocked = 0;
				for (uint32_t dm_frame = 0; dm_frame < dm_timings.size(); ++dm_frame)
				{
					visibility_snapshot snapshot;
					snapshot.sequence = 1000u + dm_frame;
					snapshot.captured = std::chrono::steady_clock::now();
					snapshot.filter_teammates = free_for_all;
					for (uint32_t index = 0; index < dm_player_count; ++index)
					{
						const bool first_half = index < dm_player_count / 2u;
						const float offset = (static_cast<float>(index % (dm_player_count / 2u)) - 7.5f) * 4.0f;
						const float motion = std::sin(static_cast<float>(dm_frame) * 0.2f + static_cast<float>(index)) * 2.0f;
						const vec3 origin = first_half ? vec3 {-1902.0f + offset, -1976.0f + motion, -212.14798f}
													   : vec3 {1376.0f + offset, -16.0f + motion, -103.96875f};
						player_state& player = snapshot.players[index];
						player = {true,
								  static_cast<uint8_t>(first_half ? 2 : 3),
								  {origin.x, origin.y, origin.z + 64.0f},
								  origin,
								  {-16.0f, -16.0f, 0.0f},
								  {16.0f, 16.0f, 72.0f},
								  0.0f,
								  0.0f,
								  k_visibility_button_forward,
								  weapon_muzzle_class::rifle};
						set_test_capsules(player);
					}
					worker->submit(std::move(snapshot), 0, blocked_tuning);
					std::shared_ptr<const visibility_result> result;
					for (int attempt = 0; attempt < 5000 && (!result || result->sequence != 1000u + dm_frame); ++attempt)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						result = worker->result();
					}
					assert(result && result->evaluated_pairs == expected_pairs && result->hidden_pairs == expected_hidden
						   && !result->budget_exhausted);
					dm_timings[dm_frame] = result->worker_ms;
					dm_active += result->worker_active_ms;
					dm_cache_hits += result->occluder_cache_hits;
					dm_cache_queries += result->occluder_cache_hits + result->occluder_cache_misses;
					dm_triangles += result->rasterized_triangles;
					dm_draws += result->moc_render_calls;
					dm_rects += result->moc_rect_tests;
					dm_proofs += result->rebuilt_proofs;
					dm_proof_leaves += result->rebuilt_proof_leaves;
					dm_max_proof_leaves = std::max(dm_max_proof_leaves, result->max_rebuilt_proof_leaves);
					dm_cache_saturations += result->cache_saturations;
					dm_cache_compaction_trials += result->cache_compaction_trials;
					dm_cache_compactions += result->cache_compactions;
					dm_cache_compaction_leaves_saved += result->cache_compaction_leaves_saved;
					dm_uncached_blocked += result->uncached_blocked;
				}
				worker->stop();
				std::sort(dm_timings.begin(), dm_timings.end());
				double dm_average = 0.0;
				for (double timing : dm_timings)
				{
					dm_average += timing;
				}
				dm_average /= dm_timings.size();
				std::cout << label << ' ' << scenario << '-' << thread_count << "threads: wall_average=" << dm_average
						  << "ms wall_p99=" << dm_timings.back() << "ms active_average=" << dm_active / dm_timings.size()
						  << "ms pairs=" << expected_pairs << " cache=" << dm_cache_hits / dm_timings.size() << '/'
						  << dm_cache_queries / dm_timings.size() << " triangles=" << dm_triangles / dm_timings.size()
						  << " draws=" << dm_draws / dm_timings.size() << " rects=" << dm_rects / dm_timings.size()
						  << " proofs=" << dm_proofs / dm_timings.size() << " proof_leaves=" << (dm_proofs == 0 ? 0 : dm_proof_leaves / dm_proofs)
						  << '/' << dm_max_proof_leaves << " cache_capacity=" << k_capsule_occluder_cache_size
						  << " saturated=" << dm_cache_saturations / dm_timings.size() << " compact=" << dm_cache_compactions / dm_timings.size()
						  << '/' << dm_cache_compaction_trials / dm_timings.size()
						  << " saved=" << dm_cache_compaction_leaves_saved / dm_timings.size()
						  << " uncached_blocked=" << dm_uncached_blocked / dm_timings.size() << '\n'
						  << std::flush;
				assert(dm_timings.back() < 30.0);
			};
			benchmark_runtime(1, false, 512, 512, "runtime-16v16-hidden");
			benchmark_runtime(2, false, 512, 512, "runtime-16v16-hidden");
			benchmark_runtime(2, true, dm_player_count * (dm_player_count - 1u), 512, "runtime-32ffa-mixed");
		}

		constexpr uint32_t k_players = 20;
		const visibility_tuning tuning {64.0f, 0.64f, 0.0f};
		std::mt19937 random(0x51f0u);
		std::uniform_real_distribution<float> x(data.header.world_min[0], data.header.world_max[0]);
		std::uniform_real_distribution<float> y(data.header.world_min[1], data.header.world_max[1]);
		std::uniform_real_distribution<float> z(data.header.world_min[2], data.header.world_max[2]);
		std::array<visibility_player, k_players> players {};
		for (uint32_t i = 0; i < k_players; ++i)
		{
			const vec3 origin {x(random), y(random), z(random)};
			players[i] = {{origin.x, origin.y, origin.z + 64.0f},
						  origin,
						  {-16.0f, -16.0f, 0.0f},
						  {16.0f, 16.0f, 72.0f},
						  static_cast<float>(i % 8u) * 45.0f,
						  static_cast<float>(i % 4u) * 0.025f,
						  i % 3u == 0 ? k_visibility_button_forward | k_visibility_button_left : k_visibility_button_forward,
						  weapon_muzzle_class::rifle};
			set_test_capsules(players[i]);
		}
		std::vector<uint32_t> cache(k_players * k_players * k_visibility_origin_count_max, k_invalid_ref);
		std::vector<capsule_occluder_cache> occluder_cache(k_players * k_players * k_visibility_origin_count_max);
		std::array<double, 20> timings {};
		uint64_t blocked_pairs = 0;
		uint64_t sampled_pixels = 0;
		uint64_t traced_rays = 0;
		uint64_t visited_nodes = 0;
		uint64_t rasterized_triangles = 0;
		for (double& timing : timings)
		{
			const auto start = std::chrono::steady_clock::now();
			std::array<visibility_origin_points, k_players> origins {};
			for (uint32_t recipient = 0; recipient < k_players; ++recipient)
			{
				origins[recipient] = visibility_origins(data, players[recipient], tuning);
			}
			for (uint32_t recipient = 0; recipient < k_players; ++recipient)
			{
				for (uint32_t target = 0; target < k_players; ++target)
				{
					if (recipient == target)
					{
						continue;
					}
					bool blocked = true;
					for (uint32_t origin_index = 0; origin_index < origins[recipient].count; ++origin_index)
					{
						const vec3& origin = origins[recipient].points[origin_index];
						uint32_t& cached = cache[(recipient * k_players + target) * k_visibility_origin_count_max + origin_index];
						capsule_occluder_cache& cached_occluders =
							occluder_cache[(recipient * k_players + target) * k_visibility_origin_count_max + origin_index];
						capsule_query_stats stats;
						const capsule_query_result query =
							capsule_visible_from_origin(data, origin, players[target].capsules, nullptr, 0.0f,
														std::chrono::steady_clock::time_point::max(), nullptr, &stats, &cached_occluders);
						sampled_pixels += stats.sampled_pixels;
						traced_rays += stats.traced_rays;
						visited_nodes += stats.visited_nodes;
						rasterized_triangles += stats.rasterized_triangles;
						if (query != capsule_query_result::blocked)
						{
							blocked = false;
							break;
						}
						vec3 muzzle;
						if (visibility_muzzle_point(players[target], muzzle))
						{
							const ray_hit hit = segment_blocked(data, origin, muzzle, cached);
							cached = hit.packet_index;
							++traced_rays;
							if (!hit.blocked)
							{
								blocked = false;
								break;
							}
						}
					}
					blocked_pairs += blocked;
				}
			}
			timing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}
		std::sort(timings.begin(), timings.end());
		double average = 0;
		for (double timing : timings)
		{
			average += timing;
		}
		average /= timings.size();
		constexpr uint32_t k_pairs = k_players * (k_players - 1u);
		std::cout << label << " worker-loop: average=" << average << "ms p99=" << timings.back() << "ms pairs=" << k_pairs
				  << " pixels=" << sampled_pixels / timings.size() << " rays=" << traced_rays / timings.size()
				  << " nodes=" << visited_nodes / timings.size() << " triangles=" << rasterized_triangles / timings.size()
				  << " blocked_pairs=" << blocked_pairs << '\n'
				  << std::flush;
		assert(timings.back() < 75.0);
		return average;
	}

} // namespace

namespace
{

	struct fake_scene_node
	{
		fake_scene_node* child {};
		fake_scene_node* next {};
		int id {};
	};

	void test_scene_descendant_walk()
	{
		using namespace cs2glaz;
		const auto next = [](fake_scene_node* node) { return node->next; };
		const auto child = [](fake_scene_node* node) { return node->child; };
		std::vector<int> seen;
		const auto record = [&](fake_scene_node* node)
		{
			seen.push_back(node->id);
			return true;
		};

		// root -> {a -> {c}, b}: every descendant once, root itself never.
		fake_scene_node c {nullptr, nullptr, 3};
		fake_scene_node b {nullptr, nullptr, 2};
		fake_scene_node a {&c, &b, 1};
		assert(walk_scene_descendants<8>(&a, next, child, record));
		std::sort(seen.begin(), seen.end());
		assert((seen == std::vector<int> {1, 2, 3}));

		seen.clear();
		assert(walk_scene_descendants<8>(static_cast<fake_scene_node*>(nullptr), next, child, record) && seen.empty());

		// A cyclic sibling list must stop at the budget instead of spinning.
		fake_scene_node loop_a {nullptr, nullptr, 1};
		fake_scene_node loop_b {nullptr, &loop_a, 2};
		loop_a.next = &loop_b;
		assert(!walk_scene_descendants<16>(&loop_a, next, child, [](fake_scene_node*) { return true; }));
		fake_scene_node self_child {nullptr, nullptr, 1};
		self_child.child = &self_child;
		assert(!walk_scene_descendants<16>(&self_child, next, child, [](fake_scene_node*) { return true; }));

		// More nodes than the budget fails open; exactly the budget passes.
		std::array<fake_scene_node, 9> chain {};
		for (size_t index = 0; index + 1 < chain.size(); ++index)
		{
			chain[index].next = &chain[index + 1];
		}
		assert(!walk_scene_descendants<8>(&chain[0], next, child, [](fake_scene_node*) { return true; }));
		chain[7].next = nullptr;
		assert(walk_scene_descendants<8>(&chain[0], next, child, [](fake_scene_node*) { return true; }));

		// A visitor that rejects a node ends the walk with failure.
		assert(!walk_scene_descendants<8>(&a, next, child, [](fake_scene_node* node) { return node->id != 3; }));
	}

} // namespace

void run_visibility_and_transmit_tests()
{
	test_scene_descendant_walk();
	test_visibility_pair_eligibility();
	test_radar_entry_filter();
	test_rtti_check();
	test_smoke_occlusion();
	test_visibility_sampling();
	test_capsule_visibility();
	test_hull_capsules();
	test_visibility_worker();
	test_lifecycle_guard();
	test_visual_group_key();
	test_pair_guard();
	test_checktransmit_private_offsets();
	test_transmit_debug();
	test_hidden_entity_group();
}

double run_worker_benchmark(const cs2glaz::bvh8_data& data, const std::string& label)
{
	return benchmark_worker_loop(data, label);
}
