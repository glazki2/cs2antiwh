#include "smoke_occlusion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace cs2glaz
{
	namespace
	{

		constexpr float k_half_extent = 320.0f;
		constexpr float k_cell_size = 20.0f;
		constexpr float k_density_scale = 50.0f;
		constexpr float k_ignore_density = 0.1f;
		constexpr float k_opaque_density = 0.8f;
		constexpr float k_block_density = 0.2f;
		constexpr float k_visual_timing_margin = 0.5f;
		constexpr uint32_t k_max_steps = 128;
		constexpr float k_he_lift = 16.0f;

		bool finite(vec3 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		float smoothstep(float value)
		{
			value = std::clamp(value, 0.0f, 1.0f);
			return value * value * (3.0f - 2.0f * value);
		}

		float age_scale(float age)
		{
			const float delayed_growth_age = age - k_visual_timing_margin;
			const float early_fade_age = age + k_visual_timing_margin;
			return smoothstep((delayed_growth_age - 0.1f) / 1.4f) * smoothstep((22.0f - early_fade_age) / 5.0f);
		}

		uint32_t morton_index(uint32_t x, uint32_t y, uint32_t z)
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

		bool clip_axis(float origin, float direction, float minimum, float maximum, float& first, float& last)
		{
			if (std::fabs(direction) < 1.0e-7f)
			{
				return origin >= minimum && origin <= maximum;
			}
			float left = (minimum - origin) / direction;
			float right = (maximum - origin) / direction;
			if (left > right)
			{
				std::swap(left, right);
			}
			first = std::max(first, left);
			last = std::min(last, right);
			return first <= last;
		}

		bool clip_to_volume(const smoke_volume_snapshot& volume, vec3 origin, vec3 direction, float& first, float& last)
		{
			const vec3 minimum {volume.center.x - k_half_extent, volume.center.y - k_half_extent, volume.center.z - k_half_extent};
			const vec3 maximum {volume.center.x + k_half_extent, volume.center.y + k_half_extent, volume.center.z + k_half_extent};
			return clip_axis(origin.x, direction.x, minimum.x, maximum.x, first, last)
				   && clip_axis(origin.y, direction.y, minimum.y, maximum.y, first, last)
				   && clip_axis(origin.z, direction.z, minimum.z, maximum.z, first, last);
		}

		int cell_coordinate(float value, float minimum)
		{
			return std::clamp(static_cast<int>(std::floor((value - minimum) / k_cell_size)), 0, static_cast<int>(k_smoke_axis_cells - 1));
		}

		float volume_density(const smoke_volume_snapshot& volume, vec3 origin, vec3 target)
		{
			const vec3 direction {target.x - origin.x, target.y - origin.y, target.z - origin.z};
			float first = 0.0f;
			float last = 1.0f;
			if (!clip_to_volume(volume, origin, direction, first, last) || last < 0.0f || first > 1.0f)
			{
				return 0.0f;
			}
			const float entry = std::clamp(first, 0.0f, 1.0f);
			const float exit = std::clamp(last, 0.0f, 1.0f);
			if (entry > exit)
			{
				return 0.0f;
			}
			const vec3 minimum {volume.center.x - k_half_extent, volume.center.y - k_half_extent, volume.center.z - k_half_extent};
			vec3 start {origin.x + direction.x * entry, origin.y + direction.y * entry, origin.z + direction.z * entry};
			const vec3 finish {origin.x + direction.x * exit, origin.y + direction.y * exit, origin.z + direction.z * exit};
			float start_parameter = entry;
			if (entry > 0.0f)
			{
				const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
				if (length > 0.0f)
				{
					const float inward = std::min(1.0f / length, std::max(exit - entry, 0.0f));
					start_parameter += inward;
					start.x += direction.x * inward;
					start.y += direction.y * inward;
					start.z += direction.z * inward;
				}
			}
			int cell[3] {cell_coordinate(start.x, minimum.x), cell_coordinate(start.y, minimum.y), cell_coordinate(start.z, minimum.z)};
			const int end[3] {cell_coordinate(finish.x, minimum.x), cell_coordinate(finish.y, minimum.y), cell_coordinate(finish.z, minimum.z)};
			const float start_values[3] {start.x, start.y, start.z};
			const float directions[3] {direction.x, direction.y, direction.z};
			const float minimum_values[3] {minimum.x, minimum.y, minimum.z};
			int step[3] {};
			float next[3] {};
			float delta[3] {};
			for (int axis = 0; axis < 3; ++axis)
			{
				if (directions[axis] > 0.0f)
				{
					step[axis] = 1;
					next[axis] = start_parameter
								 + (minimum_values[axis] + static_cast<float>(cell[axis] + 1) * k_cell_size - start_values[axis]) / directions[axis];
					delta[axis] = k_cell_size / directions[axis];
				}
				else if (directions[axis] < 0.0f)
				{
					step[axis] = -1;
					next[axis] = start_parameter
								 + (minimum_values[axis] + static_cast<float>(cell[axis]) * k_cell_size - start_values[axis]) / directions[axis];
					delta[axis] = -k_cell_size / directions[axis];
				}
				else
				{
					next[axis] = std::numeric_limits<float>::infinity();
					delta[axis] = std::numeric_limits<float>::infinity();
				}
			}

			float accumulated = 0.0f;
			for (uint32_t visited = 0; visited < k_max_steps; ++visited)
			{
				if (cell[0] < 0 || cell[1] < 0 || cell[2] < 0 || cell[0] >= static_cast<int>(k_smoke_axis_cells)
					|| cell[1] >= static_cast<int>(k_smoke_axis_cells) || cell[2] >= static_cast<int>(k_smoke_axis_cells))
				{
					break;
				}
				const uint32_t index = morton_index(static_cast<uint32_t>(cell[0]), static_cast<uint32_t>(cell[1]), static_cast<uint32_t>(cell[2]));
				if ((volume.opaque_cells[index >> 3u] & static_cast<uint8_t>(1u << (index & 7u))) != 0)
				{
					return 1.0f;
				}
				const float density = std::clamp(volume.density[index] / k_density_scale, 0.0f, 1.0f);
				if (density >= k_opaque_density)
				{
					return 1.0f;
				}
				if (density > k_ignore_density)
				{
					accumulated += density;
					if (accumulated >= k_block_density)
					{
						return 1.0f;
					}
				}
				if (cell[0] == end[0] && cell[1] == end[1] && cell[2] == end[2])
				{
					break;
				}
				const int axis = next[1] <= next[0] ? (next[1] <= next[2] ? 1 : 2) : (next[0] <= next[2] ? 0 : 2);
				if (next[axis] > exit)
				{
					break;
				}
				cell[axis] += step[axis];
				next[axis] += delta[axis];
			}
			return accumulated;
		}

		float square(float value)
		{
			return value * value;
		}

		bool cell_opaque(const smoke_volume_snapshot& volume, uint32_t x, uint32_t y, uint32_t z)
		{
			const uint32_t index = morton_index(x, y, z);
			return (volume.opaque_cells[index >> 3u] & static_cast<uint8_t>(1u << (index & 7u))) != 0
				   || std::clamp(volume.density[index] / k_density_scale, 0.0f, 1.0f) >= k_opaque_density;
		}

		// A volume dense enough to block on its own, with no HE grenade near it
		// (a clearance could open any line through it).
		bool volume_solid_now(const smoke_snapshot& snapshot, const smoke_volume_snapshot& volume, float age_advance)
		{
			if (age_scale(volume.age_seconds + std::max(age_advance, 0.0f)) < k_block_density)
			{
				return false;
			}
			for (uint32_t index = 0; index < snapshot.he_clearance_count && index < snapshot.he_clearances.size(); ++index)
			{
				const he_smoke_clearance& clearance = snapshot.he_clearances[index];
				const float age = clearance.age_seconds + std::max(age_advance, 0.0f);
				if (snapshot.he_clear_radius_units <= 0.0f || snapshot.he_clear_seconds <= 0.0f || age < 0.0f || age >= snapshot.he_clear_seconds)
				{
					continue;
				}
				const float box_dx = std::max(std::fabs(clearance.center.x - volume.center.x) - k_half_extent, 0.0f);
				const float box_dy = std::max(std::fabs(clearance.center.y - volume.center.y) - k_half_extent, 0.0f);
				const float box_dz = std::max(std::fabs(clearance.center.z - volume.center.z) - k_half_extent, 0.0f);
				if (square(box_dx) + square(box_dy) + square(box_dz) <= square(snapshot.he_clear_radius_units))
				{
					return false;
				}
			}
			return true;
		}

		// Cell coordinates of a point strictly inside the volume, or false.
		bool inside_cell(const smoke_volume_snapshot& volume, vec3 point, int (&cell)[3])
		{
			const float values[3] {point.x - (volume.center.x - k_half_extent), point.y - (volume.center.y - k_half_extent),
								   point.z - (volume.center.z - k_half_extent)};
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!(values[axis] >= 0.0f) || !(values[axis] < 2.0f * k_half_extent))
				{
					return false;
				}
				cell[axis] = std::min(static_cast<int>(values[axis] / k_cell_size), static_cast<int>(k_smoke_axis_cells) - 1);
			}
			return true;
		}

		bool clearance_opens_volume(const he_smoke_clearance& clearance, const smoke_volume_snapshot& volume, vec3 origin, vec3 target, float radius,
									float duration, float age_advance, const bvh8_data* geometry)
		{
			const float age = clearance.age_seconds + std::max(age_advance, 0.0f);
			if (geometry == nullptr || radius <= 0.0f || duration <= 0.0f || age < 0.0f || age >= duration)
			{
				return false;
			}
			if (!std::isfinite(clearance.detonation_time) || !std::isfinite(volume.start_time) || clearance.detonation_time < volume.start_time)
			{
				return false;
			}
			const float box_dx = std::max(std::fabs(clearance.center.x - volume.center.x) - k_half_extent, 0.0f);
			const float box_dy = std::max(std::fabs(clearance.center.y - volume.center.y) - k_half_extent, 0.0f);
			const float box_dz = std::max(std::fabs(clearance.center.z - volume.center.z) - k_half_extent, 0.0f);
			const float radius_squared = square(radius);
			if (square(box_dx) + square(box_dy) + square(box_dz) > radius_squared)
			{
				return false;
			}
			const vec3 direction {target.x - origin.x, target.y - origin.y, target.z - origin.z};
			const float length_squared = square(direction.x) + square(direction.y) + square(direction.z);
			const vec3 from_origin {clearance.center.x - origin.x, clearance.center.y - origin.y, clearance.center.z - origin.z};
			const float parameter =
				length_squared <= 0.0f
					? 0.0f
					: std::clamp((from_origin.x * direction.x + from_origin.y * direction.y + from_origin.z * direction.z) / length_squared, 0.0f,
								 1.0f);
			const vec3 closest {origin.x + direction.x * parameter, origin.y + direction.y * parameter, origin.z + direction.z * parameter};
			if (square(clearance.center.x - closest.x) + square(clearance.center.y - closest.y) + square(clearance.center.z - closest.z)
				> radius_squared)
			{
				return false;
			}
			// A grenade lies on the floor and its blast opens the smoke above it
			// too: the channel is open if the line is in sight of the grenade or
			// of a point a little above it (a kerb or step next to the grenade
			// must not close it). Opening too much only reveals.
			const vec3 lifted {clearance.center.x, clearance.center.y, clearance.center.z + k_he_lift};
			return !segment_blocked(*geometry, clearance.center, closest).blocked || !segment_blocked(*geometry, lifted, closest).blocked;
		}

	} // namespace

	bool copy_smoke_frame(const std::byte* storage, int32_t frame, vec3 center, float age_seconds, smoke_volume_snapshot& output)
	{
		if (storage == nullptr || (frame != 0 && frame != 1) || !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)
			|| !std::isfinite(age_seconds) || age_seconds < -0.25f || age_seconds > 30.0f)
		{
			return false;
		}
		output.center = center;
		output.age_seconds = age_seconds;
		std::memcpy(output.opaque_cells.data(), storage + k_smoke_storage_mask_offset, output.opaque_cells.size());
		const std::byte* density = storage + k_smoke_storage_density_offset + static_cast<size_t>(frame) * k_smoke_storage_frame_stride;
		for (uint32_t index = 0; index < k_smoke_cell_count; ++index)
		{
			float value = 0.0f;
			std::memcpy(&value, density + static_cast<size_t>(index) * k_smoke_storage_cell_stride, sizeof(value));
			if (!std::isfinite(value))
			{
				return false;
			}
			output.density[index] = value;
		}
		return true;
	}

	bool smoke_line_blocked(const smoke_snapshot& snapshot, vec3 origin, vec3 target, float age_advance_seconds, const bvh8_data* geometry)
	{
		const vec3 direction {target.x - origin.x, target.y - origin.y, target.z - origin.z};
		if (!finite(origin) || !finite(target) || !finite(direction))
		{
			return false;
		}
		float total = 0.0f;
		for (const smoke_volume_snapshot& volume : snapshot.volumes)
		{
			const float scale = age_scale(volume.age_seconds + std::max(age_advance_seconds, 0.0f));
			float first = 0.0f;
			float last = 1.0f;
			if (scale <= 0.0f || !clip_to_volume(volume, origin, direction, first, last) || last < 0.0f || first > 1.0f)
			{
				continue;
			}
			bool cleared = false;
			for (uint32_t index = 0; index < snapshot.he_clearance_count && index < snapshot.he_clearances.size(); ++index)
			{
				if (clearance_opens_volume(snapshot.he_clearances[index], volume, origin, target, snapshot.he_clear_radius_units,
										   snapshot.he_clear_seconds, age_advance_seconds, geometry))
				{
					cleared = true;
					break;
				}
			}
			if (cleared)
			{
				continue;
			}
			total += volume_density(volume, origin, target) * scale;
			if (total >= k_block_density)
			{
				return true;
			}
		}
		return false;
	}

	bool smoke_point_opaque(const smoke_snapshot& snapshot, vec3 point, float age_advance_seconds)
	{
		if (!finite(point))
		{
			return false;
		}
		for (const smoke_volume_snapshot& volume : snapshot.volumes)
		{
			int cell[3] {};
			if (inside_cell(volume, point, cell) && volume_solid_now(snapshot, volume, age_advance_seconds)
				&& cell_opaque(volume, static_cast<uint32_t>(cell[0]), static_cast<uint32_t>(cell[1]), static_cast<uint32_t>(cell[2])))
			{
				return true;
			}
		}
		return false;
	}

	bool smoke_box_opaque(const smoke_snapshot& snapshot, vec3 minimum, vec3 maximum, float age_advance_seconds)
	{
		if (!finite(minimum) || !finite(maximum) || minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z)
		{
			return false;
		}
		for (const smoke_volume_snapshot& volume : snapshot.volumes)
		{
			int low[3] {};
			int high[3] {};
			if (!inside_cell(volume, minimum, low) || !inside_cell(volume, maximum, high) || !volume_solid_now(snapshot, volume, age_advance_seconds))
			{
				continue;
			}
			bool opaque = true;
			for (int x = low[0]; opaque && x <= high[0]; ++x)
			{
				for (int y = low[1]; opaque && y <= high[1]; ++y)
				{
					for (int z = low[2]; opaque && z <= high[2]; ++z)
					{
						opaque = cell_opaque(volume, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z));
					}
				}
			}
			if (opaque)
			{
				return true;
			}
		}
		return false;
	}

	bool smoke_sphere_hidden(const smoke_snapshot& snapshot, vec3 origin, vec3 center, float radius, float age_advance_seconds)
	{
		const vec3 axis {center.x - origin.x, center.y - origin.y, center.z - origin.z};
		const float distance = std::sqrt(square(axis.x) + square(axis.y) + square(axis.z));
		if (!finite(origin) || !finite(center) || !std::isfinite(radius) || radius < 0.0f || !std::isfinite(distance) || distance <= radius + 1.0f)
		{
			return false;
		}
		// A line from the origin to a point of the sphere crosses the plane at
		// depth t (along the axis, t below distance - radius) within
		// t * spread of the axis: inside a disc facing the axis. If every cell
		// overlapping that disc's bounding box (plus a margin, so a piece of the
		// line lies inside) is opaque, the line enters an opaque cell before the
		// sphere, where volume_density returns 1.
		constexpr float k_margin = 0.5f;
		constexpr float k_step = 0.5f * k_cell_size;
		constexpr int k_max_cells = 216;
		const vec3 unit {axis.x / distance, axis.y / distance, axis.z / distance};
		const float spread = radius / std::sqrt((distance - radius) * (distance + radius));
		const float reach = distance - radius;
		const vec3 span {unit.x * reach, unit.y * reach, unit.z * reach};
		// Half extent of a unit disc facing the axis, per world axis.
		const float disc[3] {std::sqrt(std::max(1.0f - square(unit.x), 0.0f)), std::sqrt(std::max(1.0f - square(unit.y), 0.0f)),
							 std::sqrt(std::max(1.0f - square(unit.z), 0.0f))};
		for (const smoke_volume_snapshot& volume : snapshot.volumes)
		{
			float first = 0.0f;
			float last = 1.0f;
			if (!volume_solid_now(snapshot, volume, age_advance_seconds) || !clip_to_volume(volume, origin, span, first, last) || last < 0.0f
				|| first > 1.0f)
			{
				continue;
			}
			const vec3 minimum {volume.center.x - k_half_extent, volume.center.y - k_half_extent, volume.center.z - k_half_extent};
			const float begin = std::max(first, 0.0f) * reach;
			const float end = std::min(last, 1.0f) * reach;
			for (float depth = begin; depth <= end; depth += k_step)
			{
				const vec3 point {origin.x + unit.x * depth, origin.y + unit.y * depth, origin.z + unit.z * depth};
				const float lateral = depth * spread;
				int low[3] {};
				int high[3] {};
				int count = 1;
				bool inside = true;
				const float values[3] {point.x - minimum.x, point.y - minimum.y, point.z - minimum.z};
				for (int index = 0; index < 3 && inside; ++index)
				{
					const float from = values[index] - lateral * disc[index] - k_margin;
					const float to = values[index] + lateral * disc[index] + k_margin;
					inside = from >= 0.0f && to < 2.0f * k_half_extent;
					low[index] = inside ? static_cast<int>(from / k_cell_size) : 0;
					high[index] = inside ? std::min(static_cast<int>(to / k_cell_size), static_cast<int>(k_smoke_axis_cells) - 1) : 0;
					count *= high[index] - low[index] + 1;
				}
				int center_cell[3] {};
				if (!inside || count > k_max_cells || !inside_cell(volume, point, center_cell)
					|| !cell_opaque(volume, static_cast<uint32_t>(center_cell[0]), static_cast<uint32_t>(center_cell[1]),
									static_cast<uint32_t>(center_cell[2])))
				{
					continue;
				}
				bool opaque = true;
				for (int x = low[0]; opaque && x <= high[0]; ++x)
				{
					for (int y = low[1]; opaque && y <= high[1]; ++y)
					{
						for (int z = low[2]; opaque && z <= high[2]; ++z)
						{
							opaque = cell_opaque(volume, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z));
						}
					}
				}
				if (opaque)
				{
					return true;
				}
			}
		}
		return false;
	}

} // namespace cs2glaz
