#pragma once

// Decides whether memory read through a candidate smoke layout looks like a
// live CS2 smoke volume, so limited mode can use a smoke layout it could not
// verify by the server binary. The plugin reads everything through guarded
// memory access and hands plain copies here; nothing here touches the game.

#include "smoke_occlusion.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cs2glaz
{

	// A smoke older than this has spread enough to judge its voxels.
	inline constexpr float k_smoke_check_min_age = 2.0f;
	inline constexpr float k_smoke_check_max_age = 20.0f;
	// The voxel grid is centred on the detonation point (snapped to the grid).
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

	// mask: k_smoke_mask_bytes of the occupancy mask. density: one frame of
	// k_smoke_cell_count cells, k_smoke_storage_cell_stride bytes apart, with
	// the density float first. A real spread smoke has finite, bounded densities,
	// a plausible share of dense and marked cells, and a mask that mostly marks
	// dense cells; zeroed, random or shifted memory fails at least one of these.
	inline bool smoke_voxels_plausible(const uint8_t* mask, const std::byte* density)
	{
		if (mask == nullptr || density == nullptr)
		{
			return false;
		}
		uint32_t dense = 0;
		uint32_t marked = 0;
		uint32_t marked_dense = 0;
		for (uint32_t cell = 0; cell < k_smoke_cell_count; ++cell)
		{
			float value = 0.0f;
			std::memcpy(&value, density + static_cast<size_t>(cell) * k_smoke_storage_cell_stride, sizeof(value));
			// Raw densities are about 0-50 (smoke_line_blocked divides by 50).
			if (!std::isfinite(value) || value < -0.01f || value > 1000.0f)
			{
				return false;
			}
			const bool is_dense = value >= 2.5f;
			const bool is_marked = ((mask[cell >> 3] >> (cell & 7u)) & 1u) != 0;
			dense += is_dense ? 1u : 0u;
			marked += is_marked ? 1u : 0u;
			marked_dense += is_dense && is_marked ? 1u : 0u;
		}
		constexpr uint32_t minimum = 64;
		constexpr uint32_t maximum = k_smoke_cell_count * 6u / 10u;
		return dense >= minimum && dense <= maximum && marked >= minimum && marked <= maximum && marked_dense * 2u >= marked;
	}

} // namespace cs2glaz
