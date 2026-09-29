#pragma once

// Plain copied player input plus recipient-origin and muzzle sampling. Animated
// Valve hitbox capsules are captured on the game thread and consumed as values.

#include "bvh8.h"
#include "dynamic_occluders.h"

#include <array>
#include <cstdint>
#include <span>

namespace cs2glaz
{

	inline constexpr uint32_t k_visibility_origin_count_max = 6;
	inline constexpr uint32_t k_visibility_capsule_count = 19;
	inline constexpr uint32_t k_visibility_hull_capsule_grid = 3;
	inline constexpr uint32_t k_visibility_hull_capsule_count = k_visibility_hull_capsule_grid * k_visibility_hull_capsule_grid;
	static_assert(k_visibility_hull_capsule_count <= k_visibility_capsule_count);
	inline constexpr uint32_t k_visibility_aabb_point_count = 8;
	inline constexpr uint32_t k_visibility_pixel_grid_size = 32;
	inline constexpr uint32_t k_visibility_pixel_count = k_visibility_pixel_grid_size * k_visibility_pixel_grid_size;
	inline constexpr uint32_t k_visibility_debug_beam_count_max = k_visibility_capsule_count + 1 + k_visibility_aabb_point_count;

	inline constexpr uint64_t k_visibility_button_forward = 0x8;
	inline constexpr uint64_t k_visibility_button_back = 0x10;
	inline constexpr uint64_t k_visibility_button_left = 0x200;
	inline constexpr uint64_t k_visibility_button_right = 0x400;

	enum class weapon_muzzle_class : uint8_t
	{
		none,
		pistol,
		smg,
		rifle,
		sniper
	};

	struct visibility_capsule
	{
		vec3 start;
		vec3 end;
		float radius {};
	};

	struct visibility_player
	{
		vec3 eye;
		vec3 origin;
		vec3 mins;
		vec3 maxs;
		float eye_yaw_degrees {};
		float rtt_seconds {};
		uint64_t movement_buttons {};
		weapon_muzzle_class muzzle_class {weapon_muzzle_class::none};
		std::array<visibility_capsule, k_visibility_capsule_count> capsules {};
		uint32_t capsule_count {};
		// World velocity; without it the bounds padding is the same on every side.
		vec3 velocity;
		bool has_velocity {};
	};

	struct visibility_capsule_binding
	{
		const char* bone;
		vec3 local_start;
		vec3 local_end;
		float radius;
	};

	struct visibility_bone_transform
	{
		vec3 position;
		float rotation[4] {};
	};

	extern const std::array<visibility_capsule_binding, k_visibility_capsule_count> k_visibility_capsule_bindings;

	struct visibility_tuning
	{
		float shoulder_base_units {64.0f};
		float shoulder_rtt_scale {0.64f};
		float max_shoulder_units {};
		// Sideways padding of the target's bounds corners.
		float bounds_padding_units {32.0f};
	};

	// Extra target points beyond the body: padded bounds corners and the weapon
	// muzzle, each pulled back to the target's side of any wall in between.
	struct visibility_target_points
	{
		std::array<vec3, k_visibility_aabb_point_count> aabb {};
		vec3 muzzle {};
		bool has_muzzle {};
	};

	// Which viewing origin a point is, for diagnostics.
	enum class visibility_origin_role : uint8_t
	{
		eye,
		left_shoulder,
		right_shoulder,
		above,
		feet,
		movement,
		count
	};

	struct visibility_origin_points
	{
		std::array<vec3, k_visibility_origin_count_max> points {};
		std::array<visibility_origin_role, k_visibility_origin_count_max> roles {};
		uint32_t count {};
	};

	float visibility_shoulder_offset_units(float rtt_seconds, const visibility_tuning& tuning, bool movement_intent);
	vec3 visibility_clip_destination(const bvh8_data& data, vec3 origin, vec3 destination, std::span<const visibility_occluder> occluders = {});
	weapon_muzzle_class weapon_muzzle_class_from_item_definition(uint16_t item_definition);
	float weapon_muzzle_length(weapon_muzzle_class value);
	bool visibility_transform_point(const visibility_bone_transform& transform, vec3 local, vec3& world);
	bool valid_visibility_capsule(const visibility_capsule& capsule);
	// Conservative body for builds without verified bone access: vertical capsules
	// on a 3x3 grid whose union contains the collision hull above its lowest band
	// and never reaches below the feet. Returns 0 when the bounds are unusable,
	// which leaves the target visible.
	uint32_t visibility_hull_capsules(vec3 origin, vec3 mins, vec3 maxs, std::array<visibility_capsule, k_visibility_capsule_count>& capsules);
	visibility_origin_points visibility_origins(const bvh8_data& data, const visibility_player& player, const visibility_tuning& tuning,
												std::span<const visibility_occluder> occluders = {});
	bool visibility_muzzle_point(const visibility_player& player, vec3& point);
	// Padded bounds corners. With a known velocity the padding follows movement:
	// a standing player gets 4 units, and a side grows up to horizontal_padding
	// only as fast as he moves or means to move (movement keys) towards it, over
	// the time a hidden player needs to be sent before he steps into view.
	std::array<vec3, k_visibility_aabb_point_count> visibility_aabb_points(const visibility_player& player, float horizontal_padding = 32.0f);
	// The padded corners reach 16 + padding units from the centre and the muzzle up to 52
	// units ahead, so near a thin wall they end on its far side, where any
	// viewer there sees them and the target is sent through the wall. Each point
	// is clipped on the segment from the body centre (corners) or the eye
	// (muzzle), so only space the target could actually reach counts.
	visibility_target_points visibility_clipped_target_points(const bvh8_data& data, const visibility_player& player,
															  std::span<const visibility_occluder> occluders = {}, float horizontal_padding = 32.0f);

} // namespace cs2glaz
