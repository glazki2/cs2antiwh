#pragma once

// Decoys: per viewer, a fake enemy placed where the viewer cannot see it and
// sent to that viewer only. An honest player never receives one while it could
// be seen; a wallhack that draws it shows an enemy that is not there, and
// aiming or shooting at it through the wall is logged for moderators.
//
// Spots come from places players have stood on this map (so a decoy is on a
// floor, never inside a wall), away from every real enemy of the viewer (so a
// decoy never hints where one is), behind the map geometry from the viewer.

#include "bvh8.h"
#include "capsule_visibility.h"
#include "dynamic_occluders.h"
#include "visibility_sampling.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace cs2glaz
{

	inline constexpr uint32_t k_max_decoys_per_viewer = 4;
	inline constexpr uint32_t k_max_decoy_spots = 4096;
	inline constexpr vec3 k_decoy_mins {-16.0f, -16.0f, 0.0f};
	inline constexpr vec3 k_decoy_maxs {16.0f, 16.0f, 72.0f};
	inline constexpr float k_decoy_center_height = 40.0f;
	inline constexpr float k_decoy_min_distance = 256.0f;
	inline constexpr float k_decoy_max_distance = 2048.0f;
	inline constexpr float k_decoy_enemy_clearance = 400.0f;
	inline constexpr float k_decoy_player_clearance = 96.0f;
	inline constexpr float k_decoy_spacing = 128.0f;
	// A walking decoy's legs: between recorded floor spots this far apart.
	inline constexpr float k_decoy_step_min = 64.0f;
	inline constexpr float k_decoy_step_max = 320.0f;

	// One candidate or live decoy as the worker sees it; id 0 is an empty slot.
	struct decoy_probe
	{
		uint32_t id {};
		vec3 origin;
	};

	using decoy_probe_set = std::array<decoy_probe, k_max_decoys_per_viewer>;

	// Floor points players have stood on, one per 48x48x64 cell, oldest replaced
	// first once full.
	class decoy_spot_history
	{
	public:
		void clear();
		void record(vec3 feet);
		std::span<const vec3> points() const
		{
			return spots_;
		}

	private:
		std::vector<vec3> spots_;
		std::vector<uint64_t> keys_;
		std::unordered_set<uint64_t> occupied_;
		uint32_t next_ {};
	};

	struct decoy_spot_query
	{
		vec3 viewer_eye;
		std::span<const vec3> enemies;	// the viewer's living enemies (feet)
		std::span<const vec3> players;	// every living player (feet)
		std::span<const vec3> taken;	// the viewer's other decoys
		uint32_t seed {};
	};

	// Tries up to 32 random spots and keeps the closest one to the viewer whose
	// body centre the map geometry or an occluder hides from the viewer's eye.
	// The worker proves it hidden from every viewing origin before it is sent.
	bool choose_decoy_spot(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3& spot);

	// The next point for a walking decoy standing at from: a recorded floor
	// spot 64-320 units away on a straight path clear of the map and occluders
	// at knee and chest height, with floor under it and a slope a player can
	// walk, meeting every rule of choose_decoy_spot; its end and middle are
	// hidden from the viewer's eye. query.taken must not include this decoy.
	bool choose_decoy_step(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3 from, vec3& next);

	// Whether a decoy standing at origin is hidden from every viewing origin:
	// the exact body test, then padded bounds corners. Smoke does not count.
	// Anything unproven (deadline, bad input) counts as seen.
	bool decoy_hidden_from_origins(const bvh8_data& data, const visibility_origin_points& origins, vec3 origin,
								   std::span<const visibility_occluder> occluders, std::chrono::steady_clock::time_point deadline);

	vec3 view_forward(float pitch_degrees, float yaw_degrees);

	// Whether the view direction points at a decoy's body: within its angular
	// size plus a small tolerance.
	bool aim_on_decoy(vec3 eye, float pitch_degrees, float yaw_degrees, vec3 decoy_origin);

	// How far the aim followed a decoy between two samples that were both on
	// it, in degrees: the smaller of how much the direction to it turned (the
	// viewer or the decoy moved) and how much the view turned. A decoy walking
	// through a still crosshair, a crosshair sweeping over a still decoy, or
	// walking straight at it follows nothing.
	float decoy_follow_degrees(vec3 eye_before, vec3 forward_before, vec3 decoy_before, vec3 eye_now, vec3 forward_now, vec3 decoy_now);

	// Small deterministic generator for spots and lifetimes.
	inline uint32_t decoy_random(uint32_t& state)
	{
		state ^= state << 13u;
		state ^= state >> 17u;
		state ^= state << 5u;
		return state;
	}

} // namespace cs2glaz
