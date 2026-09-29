#include "dynamic_occluders.h"

#include <algorithm>
#include <cmath>

namespace cs2glaz
{
	namespace
	{

		constexpr float k_degrees_to_radians = 0.017453292519943295769f;
		constexpr float k_max_occluder_extent = 512.0f;
		constexpr float k_door_edge_shrink = 1.0f;
		constexpr float k_prop_shrink = 0.9f;
		constexpr float k_segment_epsilon = 1.0e-4f;

		float dot(vec3 a, vec3 b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		vec3 subtract(vec3 a, vec3 b)
		{
			return {a.x - b.x, a.y - b.y, a.z - b.z};
		}

		vec3 add(vec3 a, vec3 b)
		{
			return {a.x + b.x, a.y + b.y, a.z + b.z};
		}

		vec3 scale(vec3 value, float amount)
		{
			return {value.x * amount, value.y * amount, value.z * amount};
		}

		bool finite(vec3 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Distance from a point to the segment, squared.
		float segment_distance_sq(vec3 point, vec3 start, vec3 end)
		{
			const vec3 axis = subtract(end, start);
			const float length_sq = dot(axis, axis);
			const float amount = length_sq <= 0.0f ? 0.0f : std::clamp(dot(subtract(point, start), axis) / length_sq, 0.0f, 1.0f);
			const vec3 offset = subtract(point, add(start, scale(axis, amount)));
			return dot(offset, offset);
		}

	} // namespace

	bool make_occluder(vec3 origin, vec3 angles_degrees, vec3 mins, vec3 maxs, occluder_kind kind, visibility_occluder& output)
	{
		if (!finite(origin) || !finite(angles_degrees) || !finite(mins) || !finite(maxs))
		{
			return false;
		}
		vec3 half {0.5f * (maxs.x - mins.x), 0.5f * (maxs.y - mins.y), 0.5f * (maxs.z - mins.z)};
		const vec3 local_center {0.5f * (maxs.x + mins.x), 0.5f * (maxs.y + mins.y), 0.5f * (maxs.z + mins.z)};
		if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f || half.x > k_max_occluder_extent || half.y > k_max_occluder_extent
			|| half.z > k_max_occluder_extent)
		{
			return false;
		}
		if (kind == occluder_kind::door)
		{
			// A door is a slab: keep its thickness, trim its edges.
			float* extents[3] = {&half.x, &half.y, &half.z};
			std::sort(std::begin(extents), std::end(extents), [](const float* left, const float* right) { return *left < *right; });
			*extents[1] -= k_door_edge_shrink;
			*extents[2] -= k_door_edge_shrink;
		}
		else
		{
			half = scale(half, k_prop_shrink);
		}
		if (half.x < 0.25f || half.y < 0.25f || half.z < 0.25f)
		{
			return false;
		}
		// Source angles: yaw about z, then pitch about y, then roll about x.
		const float pitch = angles_degrees.x * k_degrees_to_radians;
		const float yaw = angles_degrees.y * k_degrees_to_radians;
		const float roll = angles_degrees.z * k_degrees_to_radians;
		const float sp = std::sin(pitch), cp = std::cos(pitch);
		const float sy = std::sin(yaw), cy = std::cos(yaw);
		const float sr = std::sin(roll), cr = std::cos(roll);
		const vec3 forward {cp * cy, cp * sy, -sp};
		const vec3 left {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
		const vec3 up {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
		output.axes = {forward, left, up};
		output.half = half;
		output.center = add(origin, add(scale(forward, local_center.x), add(scale(left, local_center.y), scale(up, local_center.z))));
		output.radius = std::sqrt(dot(half, half));
		return finite(output.center) && std::isfinite(output.radius);
	}

	bool occluder_blocks_segment(const visibility_occluder& occluder, vec3 start, vec3 end)
	{
		const vec3 start_offset = subtract(start, occluder.center);
		const vec3 direction = subtract(end, start);
		const float halves[3] = {occluder.half.x, occluder.half.y, occluder.half.z};
		float enter = 0.0f;
		float leave = 1.0f;
		bool start_inside = true;
		bool end_inside = true;
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			const float position = dot(start_offset, occluder.axes[axis]);
			const float step = dot(direction, occluder.axes[axis]);
			const float half = halves[axis];
			start_inside = start_inside && std::fabs(position) < half;
			end_inside = end_inside && std::fabs(position + step) < half;
			if (std::fabs(step) < 1.0e-8f)
			{
				if (std::fabs(position) >= half)
				{
					return false;
				}
				continue;
			}
			float near_t = (-half - position) / step;
			float far_t = (half - position) / step;
			if (near_t > far_t)
			{
				std::swap(near_t, far_t);
			}
			enter = std::max(enter, near_t);
			leave = std::min(leave, far_t);
			if (enter >= leave)
			{
				return false;
			}
		}
		return !start_inside && !end_inside && enter > k_segment_epsilon && leave < 1.0f - k_segment_epsilon && leave - enter > k_segment_epsilon;
	}

	bool occluders_block_segment(std::span<const visibility_occluder> occluders, vec3 start, vec3 end)
	{
		for (const visibility_occluder& occluder : occluders)
		{
			if (segment_distance_sq(occluder.center, start, end) <= occluder.radius * occluder.radius && occluder_blocks_segment(occluder, start, end))
			{
				return true;
			}
		}
		return false;
	}

	bool occluders_contain(std::span<const visibility_occluder> occluders, vec3 point)
	{
		for (const visibility_occluder& occluder : occluders)
		{
			const vec3 offset = subtract(point, occluder.center);
			if (dot(offset, offset) <= occluder.radius * occluder.radius && std::fabs(dot(offset, occluder.axes[0])) < occluder.half.x
				&& std::fabs(dot(offset, occluder.axes[1])) < occluder.half.y && std::fabs(dot(offset, occluder.axes[2])) < occluder.half.z)
			{
				return true;
			}
		}
		return false;
	}

	uint32_t occluders_between(std::span<const visibility_occluder> occluders, vec3 origin, vec3 target_center, float target_radius,
							   std::span<visibility_occluder> output)
	{
		uint32_t count = 0;
		for (const visibility_occluder& occluder : occluders)
		{
			// Any sightline to the target stays within target_radius of the segment
			// from the origin to its centre.
			const float reach = occluder.radius + target_radius;
			if (segment_distance_sq(occluder.center, origin, target_center) > reach * reach)
			{
				continue;
			}
			if (count == output.size())
			{
				return static_cast<uint32_t>(output.size()) + 1u;
			}
			output[count++] = occluder;
		}
		return count;
	}

} // namespace cs2glaz
