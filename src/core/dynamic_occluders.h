#pragma once

// Live occluders the baked map does not contain: doors and box-shaped props.
// Each is an oriented box built from the entity's collision bounds and current
// transform, shrunk so it stays inside the real shape; a sightline through one
// counts as blocked. A segment that starts or ends inside a box is never
// blocked by it, so an overlap from an imprecise box cannot hide a player.

#include "bvh8.h"

#include <array>
#include <cstdint>
#include <span>

namespace cs2glaz
{

	inline constexpr uint32_t k_max_dynamic_occluders = 256;

	enum class occluder_kind : uint8_t
	{
		door,
		prop
	};

	struct visibility_occluder
	{
		vec3 center;
		std::array<vec3, 3> axes {}; // unit axes of the box
		vec3 half;					 // half extents along the axes
		float radius {};			 // bounding sphere around the centre
	};

	// Builds a box from local collision bounds and a world transform given as
	// Source angles in degrees (pitch, yaw, roll). Doors keep their thickness and
	// lose one unit on the other axes; props shrink to 90% on every axis. Returns
	// false for bounds that are not finite, are flat or exceed 512 units.
	bool make_occluder(vec3 origin, vec3 angles_degrees, vec3 mins, vec3 maxs, occluder_kind kind, visibility_occluder& output);

	// Whether the open segment passes through the inside of the box, with both
	// end points outside it.
	bool occluder_blocks_segment(const visibility_occluder& occluder, vec3 start, vec3 end);
	bool occluders_block_segment(std::span<const visibility_occluder> occluders, vec3 start, vec3 end);

	// Keeps the occluders that can lie between an origin and a sphere (a target's
	// bounds); writes at most output.size() and returns how many, or returns
	// output.size() + 1 when more would be needed (use the full list then).
	uint32_t occluders_between(std::span<const visibility_occluder> occluders, vec3 origin, vec3 target_center, float target_radius,
							   std::span<visibility_occluder> output);

} // namespace cs2glaz
