#pragma once

// Plain copied smoke data and the server's line-density decision. Live CS2
// pointers never enter this module or the visibility worker.

#include "bvh8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cs2glaz
{

	inline constexpr uint32_t k_smoke_axis_cells = 32;
	inline constexpr uint32_t k_smoke_cell_count = k_smoke_axis_cells * k_smoke_axis_cells * k_smoke_axis_cells;
	inline constexpr uint32_t k_smoke_mask_bytes = k_smoke_cell_count / 8;
	inline constexpr uint32_t k_max_smoke_volumes = 32;
	inline constexpr uint32_t k_max_he_clearances = 64;
	inline constexpr size_t k_smoke_storage_mask_offset = 8;
	inline constexpr size_t k_smoke_storage_density_offset = 0x3008;
	inline constexpr size_t k_smoke_storage_frame_stride = 0x80000;
	inline constexpr size_t k_smoke_storage_cell_stride = 0x10;

	struct smoke_volume_snapshot
	{
		vec3 center;
		float age_seconds {};
		float start_time {};
		std::array<uint8_t, k_smoke_mask_bytes> opaque_cells {};
		std::array<float, k_smoke_cell_count> density {};
	};

	struct he_smoke_clearance
	{
		vec3 center;
		float age_seconds {};
		float detonation_time {};
	};

	struct live_he_clearance
	{
		vec3 center;
		float detonation_time {};
	};

	struct he_clearance_history
	{
		std::array<live_he_clearance, k_max_he_clearances> records {};
		uint32_t count {};
		uint32_t next {};

		bool record(vec3 center, float detonation_time)
		{
			if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(detonation_time))
			{
				return false;
			}
			records[next] = {center, detonation_time};
			next = (next + 1u) % static_cast<uint32_t>(records.size());
			count = std::min(count + 1u, static_cast<uint32_t>(records.size()));
			return true;
		}

		void clear()
		{
			records = {};
			count = 0;
			next = 0;
		}
	};

	struct smoke_snapshot
	{
		std::vector<smoke_volume_snapshot> volumes;
		std::array<he_smoke_clearance, k_max_he_clearances> he_clearances {};
		uint32_t he_clearance_count {};
		float he_clear_radius_units {};
		float he_clear_seconds {};
	};

	bool copy_smoke_frame(const std::byte* storage, int32_t frame, vec3 center, float age_seconds, smoke_volume_snapshot& output);

	template<typename ReadFrame>
	bool copy_stable_smoke_frame(const std::byte* storage, vec3 center, float age_seconds, smoke_volume_snapshot& output, ReadFrame read_frame)
	{
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			const int32_t frame = read_frame();
			if (!copy_smoke_frame(storage, frame, center, age_seconds, output))
			{
				return false;
			}
			if (frame == read_frame())
			{
				return true;
			}
		}
		return false;
	}

	bool smoke_line_blocked(const smoke_snapshot& snapshot, vec3 origin, vec3 target, float age_advance_seconds = 0.0f,
							const bvh8_data* geometry = nullptr);

} // namespace cs2glaz
