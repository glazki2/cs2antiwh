#pragma once

#include "smoke_occlusion.h"
#include "visibility_sampling.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>

namespace cs2glaz
{

	enum class capsule_query_result : uint8_t
	{
		blocked,
		visible,
		indeterminate
	};

	struct capsule_query_stats
	{
		uint32_t sampled_pixels {};
		uint32_t traced_rays {};
		uint32_t visited_nodes {};
		uint32_t rasterized_triangles {};
		uint32_t occluder_cache_hits {};
		uint32_t occluder_cache_misses {};
		uint32_t moc_render_calls {};
		uint32_t moc_rect_tests {};
		uint32_t rebuilt_proofs {};
		uint32_t rebuilt_proof_leaves {};
		uint32_t max_rebuilt_proof_leaves {};
		uint32_t cache_saturations {};
		uint32_t cache_compaction_trials {};
		uint32_t cache_compactions {};
		uint32_t cache_compaction_leaves_saved {};
		uint32_t uncached_blocked {};
	};

	inline constexpr uint32_t k_capsule_occluder_cache_size = 96;

	struct capsule_occluder_cache
	{
		std::array<uint32_t, k_capsule_occluder_cache_size> leaves {};
		uint32_t count {};
	};

	capsule_query_result capsule_visible_from_origin(const bvh8_data& geometry, vec3 origin, std::span<const visibility_capsule> capsules,
													 const smoke_snapshot* smokes, float smoke_age_advance,
													 std::chrono::steady_clock::time_point deadline, const std::atomic_bool* stopping = nullptr,
													 capsule_query_stats* stats = nullptr, capsule_occluder_cache* occluder_cache = nullptr);

} // namespace cs2glaz
