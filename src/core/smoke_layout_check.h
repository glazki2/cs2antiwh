#pragma once

// Decides whether memory read through a candidate smoke layout looks like a
// live CS2 smoke volume, so limited mode can use a smoke layout it could not
// verify by the server binary. The plugin reads everything through guarded
// memory access and hands plain copies here; nothing here touches the game.

#include "smoke_occlusion.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cs2glaz
{

	// A candidate layout is probed on a live smoke every k_smoke_probe_interval
	// from k_smoke_probe_start after the smoke appears, and accepted as soon as
	// the smoke has spread enough to judge (long before it is opaque); it
	// counts as failed on that smoke only at k_smoke_probe_deadline.
	inline constexpr float k_smoke_probe_start = 0.2f;
	inline constexpr float k_smoke_probe_interval = 0.1f;
	inline constexpr float k_smoke_probe_deadline = 4.0f;
	// The header's start time must say the smoke has started and not faded.
	inline constexpr float k_smoke_check_min_age = 0.0f;
	inline constexpr float k_smoke_check_max_age = 20.0f;
	// The voxel grid is centred near the detonation point.
	inline constexpr float k_smoke_check_center_tolerance = 64.0f;

	struct smoke_volume_header
	{
		vec3 center;
		float start_time {};
		int32_t frame {-1};
		const std::byte* storage {};
	};

	inline bool smoke_header_plausible(const smoke_volume_header& header, vec3 detonation, float game_time)
	{
		const auto finite = [](vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		if (!finite(header.center) || !finite(detonation) || !std::isfinite(header.start_time) || !std::isfinite(game_time)
			|| (header.frame != 0 && header.frame != 1) || header.storage == nullptr)
		{
			return false;
		}
		const float dx = header.center.x - detonation.x;
		const float dy = header.center.y - detonation.y;
		const float dz = header.center.z - detonation.z;
		const float age = game_time - header.start_time;
		return dx * dx + dy * dy + dz * dz <= k_smoke_check_center_tolerance * k_smoke_check_center_tolerance && age >= k_smoke_check_min_age
			   && age <= k_smoke_check_max_age;
	}

	// What one copied frame of a candidate smoke storage holds. mask:
	// k_smoke_mask_bytes of the cell mask (may be null). density: one frame of
	// k_smoke_cell_count cells, k_smoke_storage_cell_stride bytes apart, with
	// the density float first.
	struct smoke_voxel_stats
	{
		uint32_t filled {};		  // cells with any density
		uint32_t dense {};		  // cells at 2.5 or more (about 5% of the blocking scale)
		uint32_t marked {};		  // mask bits set
		uint32_t marked_filled {}; // mask bits set on cells with any density
		uint32_t non_finite {};
		uint32_t out_of_range {};
		float peak {};
	};

	inline smoke_voxel_stats smoke_voxel_statistics(const uint8_t* mask, const std::byte* density)
	{
		smoke_voxel_stats stats;
		if (density == nullptr)
		{
			return stats;
		}
		for (uint32_t cell = 0; cell < k_smoke_cell_count; ++cell)
		{
			float value = 0.0f;
			std::memcpy(&value, density + static_cast<size_t>(cell) * k_smoke_storage_cell_stride, sizeof(value));
			const bool is_marked = mask != nullptr && ((mask[cell >> 3] >> (cell & 7u)) & 1u) != 0;
			stats.marked += is_marked ? 1u : 0u;
			if (!std::isfinite(value))
			{
				++stats.non_finite;
				continue;
			}
			if (value < -1.0f || value > 10000.0f)
			{
				++stats.out_of_range;
				continue;
			}
			const bool is_filled = value > 0.01f;
			stats.filled += is_filled ? 1u : 0u;
			stats.dense += value >= 2.5f ? 1u : 0u;
			stats.marked_filled += is_marked && is_filled ? 1u : 0u;
			stats.peak = std::max(stats.peak, value);
		}
		return stats;
	}

	// A real spread smoke has finite, bounded densities and fills a plausible
	// share of the grid; zeroed, random or shifted memory fails at least one of
	// these. The mask is not judged: what it marks (dense cells or cells the
	// map blocks) is not known for every build, and smoke_line_blocked treats
	// a marked cell as blocking either way, as CS2FOW does on verified builds.
	inline bool smoke_voxels_plausible(const smoke_voxel_stats& stats)
	{
		constexpr uint32_t minimum = 64;
		constexpr uint32_t maximum = k_smoke_cell_count * 6u / 10u;
		return stats.non_finite == 0 && stats.out_of_range == 0 && stats.filled >= minimum && stats.filled <= maximum && stats.peak > 0.0f;
	}

	inline bool smoke_voxels_plausible(const uint8_t* mask, const std::byte* density)
	{
		return mask != nullptr && density != nullptr && smoke_voxels_plausible(smoke_voxel_statistics(mask, density));
	}

} // namespace cs2glaz
