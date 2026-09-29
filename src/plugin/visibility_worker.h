#pragma once

// Owns the background visibility threads. The game thread gives them copied
// player data; it publishes immutable visibility results and never reads live
// CS2 objects. New pending work replaces old pending work instead of queuing.

#include "bvh8.h"
#include "capsule_visibility.h"
#include "smoke_occlusion.h"
#include "visibility_sampling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cs2glaz
{

	inline constexpr uint32_t k_max_players = 64;

	struct player_state
	{
		bool valid {};
		uint8_t team {};
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
		int pawn_entity {-1};
		vec3 velocity;
		bool has_velocity {};
	};

	inline visibility_player visibility_sample(const player_state& player)
	{
		return {player.eye,			player.origin,			 player.mins,		  player.maxs,	   player.eye_yaw_degrees,
				player.rtt_seconds, player.movement_buttons, player.muzzle_class, player.capsules, player.capsule_count,
				player.velocity,	player.has_velocity};
	}

	inline bool valid_player_numbers(const player_state& player)
	{
		const auto finite = [](vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		return finite(player.origin) && finite(player.eye) && finite(player.mins) && finite(player.maxs) && std::isfinite(player.eye_yaw_degrees)
			   && std::isfinite(player.rtt_seconds) && player.mins.x <= player.maxs.x && player.mins.y <= player.maxs.y
			   && player.mins.z <= player.maxs.z
			   && (player.capsule_count == 0
				   || (player.capsule_count == player.capsules.size()
					   && std::all_of(player.capsules.begin(), player.capsules.end(), valid_visibility_capsule)));
	}

	inline bool visibility_pair_enabled(uint32_t recipient, uint32_t target, const player_state& from, const player_state& to, bool filter_teammates)
	{
		return from.valid && to.valid && recipient != target && (filter_teammates || from.team != to.team);
	}

	inline bool visibility_teammate_filter_enabled(bool configured, bool teammates_are_enemies)
	{
		return configured || teammates_are_enemies;
	}

	struct visibility_snapshot
	{
		uint64_t sequence {};
		std::chrono::steady_clock::time_point captured;
		bool filter_teammates {};
		bool smoke_enabled {};
		bool smoke_available {};
		std::shared_ptr<const smoke_snapshot> smokes;
		player_state players[k_max_players];
		// Doors and box props at their current place; they block sight like walls.
		std::vector<visibility_occluder> occluders;
	};

	// What made a pair visible, for diagnostics (the wall-check HUD and metrics).
	enum class visibility_reveal : uint8_t
	{
		none,	   // hidden
		hold,	   // seen within cs2glaz_visibility_hold_ms
		body,	   // the body capsules
		corner,	   // a padded bounds corner
		muzzle,	   // the weapon muzzle
		uncertain, // not provable (no body, budget, geometry), revealed to be safe
		count
	};

	inline constexpr uint8_t k_visibility_reveal_no_origin = 0x0fu;

	// Packs the test and the viewing origin role that revealed a pair.
	inline uint8_t visibility_reveal_code(visibility_reveal reveal, uint8_t origin_role = k_visibility_reveal_no_origin)
	{
		return static_cast<uint8_t>(static_cast<uint8_t>(reveal) | static_cast<uint8_t>(origin_role << 4u));
	}

	inline visibility_reveal visibility_reveal_test(uint8_t code)
	{
		const uint8_t test = code & 0x0fu;
		return test < static_cast<uint8_t>(visibility_reveal::count) ? static_cast<visibility_reveal>(test) : visibility_reveal::none;
	}

	inline uint8_t visibility_reveal_origin(uint8_t code)
	{
		return static_cast<uint8_t>(code >> 4u);
	}

	struct visibility_result
	{
		uint64_t sequence {};
		std::chrono::steady_clock::time_point captured;
		std::chrono::steady_clock::time_point completed;
		player_state players[k_max_players];
		bool filter_teammates {};
		bool smoke_enabled {};
		bool smoke_available {};
		uint32_t smoke_count {};
		uint32_t he_clearance_count {};
		bool visible[k_max_players][k_max_players] {};
		uint8_t reveal[k_max_players][k_max_players] {};
		std::array<uint32_t, static_cast<size_t>(visibility_reveal::count)> reveal_counts {};
		std::array<uint32_t, static_cast<size_t>(visibility_origin_role::count)> reveal_origin_counts {};
		double worker_ms {};
		double worker_active_ms {};
		uint32_t evaluated_pairs {};
		uint32_t visible_pairs {};
		uint32_t hidden_pairs {};
		uint32_t sampled_pixels {};
		uint32_t traced_rays {};
		uint32_t hold_reuses {};
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
		bool budget_exhausted {};
	};

	inline bool visibility_snapshot_fresh(std::chrono::steady_clock::time_point captured, std::chrono::steady_clock::time_point now)
	{
		return now - captured <= std::chrono::milliseconds(100);
	}

	// The player slot whose pawn has this entity index, or -1 (the bomb, a
	// hostage, or an index that is not a live player's pawn).
	inline int radar_entry_target(const visibility_result& result, int entity_index)
	{
		for (uint32_t target = 0; entity_index >= 0 && target < k_max_players; ++target)
		{
			if (result.players[target].valid && result.players[target].pawn_entity == entity_index)
			{
				return static_cast<int>(target);
			}
		}
		return -1;
	}

	// Whose sight keeps an enemy on a player's radar.
	enum class radar_sight
	{
		team, // any living teammate's, the default
		own,  // the player's own only
		none, // nobody's: every enemy is dropped (diagnostic hide-all test)
	};

	// A radar update about an enemy is legitimate team information only while a
	// living member of the recipient's team sees that enemy (the reveal hold
	// included). Anything that is not an enemy of a living recipient is kept:
	// teammates, the bomb, hostages, dead or spectating recipients.
	inline bool radar_entry_allowed(const visibility_result& result, uint32_t recipient, int target_slot, radar_sight sight = radar_sight::team)
	{
		if (recipient >= k_max_players || !result.players[recipient].valid || target_slot < 0 || target_slot >= static_cast<int>(k_max_players))
		{
			return true;
		}
		const uint32_t target = static_cast<uint32_t>(target_slot);
		const player_state& enemy = result.players[target];
		const uint8_t team = result.players[recipient].team;
		if (target == recipient || (enemy.team == team && !result.filter_teammates))
		{
			return true;
		}
		if (sight == radar_sight::none)
		{
			return false;
		}
		for (uint32_t spotter = 0; spotter < k_max_players; ++spotter)
		{
			// With teammates filtered (free for all), or when asked, nobody shares sight.
			const player_state& ally = result.players[spotter];
			if (!ally.valid || ally.team != team || ((result.filter_teammates || sight == radar_sight::own) && spotter != recipient))
			{
				continue;
			}
			if (visibility_pair_enabled(spotter, target, ally, enemy, result.filter_teammates) && result.visible[spotter][target])
			{
				return true;
			}
		}
		return false;
	}

	struct worker_stats
	{
		double latest_ms {};
		double average_ms {};
		double maximum_ms {};
		double recent_p95_ms {};
		double recent_p99_ms {};
		double latest_active_ms {};
		uint64_t cycles {};
		uint32_t thread_count {};
		uint32_t evaluated_pairs {};
		uint32_t visible_pairs {};
		uint32_t hidden_pairs {};
		uint32_t sampled_pixels {};
		uint32_t traced_rays {};
		uint32_t hold_reuses {};
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
		uint64_t budget_exhaustions {};
		// Summed over every pass: what revealed the visible pairs, and from which
		// viewing origin (rays only).
		std::array<uint64_t, static_cast<size_t>(visibility_reveal::count)> reveal_counts {};
		std::array<uint64_t, static_cast<size_t>(visibility_origin_role::count)> reveal_origin_counts {};
	};

	class visibility_worker
	{
	public:
		~visibility_worker();
		bool start(const bvh8_data* data, uint32_t thread_count = 1);
		void stop();
		void submit(visibility_snapshot value, uint32_t hold_ms, visibility_tuning tuning);
		std::shared_ptr<const visibility_result> result() const;
		worker_stats stats() const;

	private:
		struct job;
		void run_coordinator();
		void run_helper(uint32_t worker_index);
		void process(job& current, uint32_t worker_index);
		void publish(job& current);

		const bvh8_data* data_ {};
		mutable std::mutex mutex_;
		std::condition_variable condition_;
		std::optional<visibility_snapshot> pending_;
		std::atomic_bool stopping_ {true};
		uint32_t hold_ms_ {};
		visibility_tuning tuning_;
		uint32_t thread_count_ {1};
		uint32_t workers_done_ {};
		uint64_t job_generation_ {};
		std::vector<std::thread> threads_;
		std::shared_ptr<job> active_job_;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
		std::atomic<std::shared_ptr<const visibility_result>> published_;
#else
		// SteamRT3's GCC 10 uses the C++11 atomic shared_ptr free functions.
		std::shared_ptr<const visibility_result> published_;
#endif
		std::array<std::array<std::array<uint32_t, k_visibility_origin_count_max>, k_max_players>, k_max_players> cached_packets_ {};
		std::array<std::array<std::array<capsule_occluder_cache, k_visibility_origin_count_max>, k_max_players>, k_max_players> cached_occluders_ {};
		std::array<std::array<std::chrono::steady_clock::time_point, k_max_players>, k_max_players> revealed_until_ {};
		mutable std::mutex stats_mutex_;
		worker_stats stats_;
		std::array<double, 128> recent_worker_ms_ {};
		uint32_t recent_worker_count_ {};
		uint32_t recent_worker_next_ {};
	};

} // namespace cs2glaz
