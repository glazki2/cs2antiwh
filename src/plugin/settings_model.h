#pragma once

#include <chrono>
#include <cstdint>

namespace cs2glaz
{

	struct runtime_configuration
	{
		bool enable {true};
		bool smoke_occlusion {true};
		float he_clear_radius_units {180.0f};
		float he_clear_seconds {3.0f};
		bool filter_teammates {};
		int update_interval_ms {1};
		int worker_threads {2};
		float shoulder_base_units {48.0f};
		float shoulder_rtt_scale {0.4f};
		float max_shoulder_units {128.0f};
		int visibility_hold_ms {150};
		bool debug {};
		int debug_los_player {};
		bool automatic_updates {false};
		bool limited_mode {true};
		float bounds_padding_units {16.0f};

		bool operator==(const runtime_configuration&) const = default;
	};

	enum class configuration_load_state : uint8_t
	{
		defaults,
		pending,
		loaded,
		failed
	};

	enum setting_change : uint32_t
	{
		setting_change_none = 0,
		setting_change_visibility = 1u << 0u,
		setting_change_worker_threads = 1u << 1u,
		setting_change_debug = 1u << 2u
	};

	enum configuration_finding : uint32_t
	{
		configuration_finding_none = 0,
		configuration_finding_disabled = 1u << 0u,
		configuration_finding_shoulder_range = 1u << 1u,
		configuration_finding_he_pair = 1u << 2u,
		configuration_finding_debug_los = 1u << 3u
	};

	inline uint32_t configuration_change_mask(const runtime_configuration& before, const runtime_configuration& after)
	{
		uint32_t changes = setting_change_none;
		if (before.enable != after.enable || before.smoke_occlusion != after.smoke_occlusion
			|| before.he_clear_radius_units != after.he_clear_radius_units || before.he_clear_seconds != after.he_clear_seconds
			|| before.filter_teammates != after.filter_teammates || before.update_interval_ms != after.update_interval_ms
			|| before.shoulder_base_units != after.shoulder_base_units || before.shoulder_rtt_scale != after.shoulder_rtt_scale
			|| before.max_shoulder_units != after.max_shoulder_units || before.visibility_hold_ms != after.visibility_hold_ms
			|| before.bounds_padding_units != after.bounds_padding_units)
		{
			changes |= setting_change_visibility;
		}
		if (before.worker_threads != after.worker_threads || before.limited_mode != after.limited_mode)
		{
			changes |= setting_change_worker_threads;
		}
		if (before.debug != after.debug || before.debug_los_player != after.debug_los_player)
		{
			changes |= setting_change_debug;
		}
		return changes;
	}

	inline uint32_t validate_configuration(const runtime_configuration& value)
	{
		uint32_t findings = configuration_finding_none;
		if (!value.enable)
		{
			findings |= configuration_finding_disabled;
		}
		if (value.max_shoulder_units > 0.0f && value.max_shoulder_units < value.shoulder_base_units)
		{
			findings |= configuration_finding_shoulder_range;
		}
		if ((value.he_clear_radius_units == 0.0f) != (value.he_clear_seconds == 0.0f))
		{
			findings |= configuration_finding_he_pair;
		}
		if (value.debug_los_player != 0)
		{
			findings |= configuration_finding_debug_los;
		}
		return findings;
	}

	class configuration_transaction
	{
	public:
		bool begin(std::chrono::steady_clock::time_point now)
		{
			if (state_ == configuration_load_state::pending)
			{
				return false;
			}
			baseline_ = active_;
			started_ = now;
			state_ = configuration_load_state::pending;
			return true;
		}

		uint32_t commit(runtime_configuration candidate, std::chrono::steady_clock::time_point now)
		{
			if (state_ != configuration_load_state::pending)
			{
				return setting_change_none;
			}
			const uint32_t changes = configuration_change_mask(active_, candidate);
			active_ = candidate;
			state_ = configuration_load_state::loaded;
			last_loaded_ = now;
			return changes;
		}

		bool rollback()
		{
			if (state_ != configuration_load_state::pending)
			{
				return false;
			}
			active_ = baseline_;
			state_ = configuration_load_state::failed;
			return true;
		}

		uint32_t apply_direct(runtime_configuration candidate)
		{
			const uint32_t changes = configuration_change_mask(active_, candidate);
			active_ = candidate;
			return changes;
		}

		bool timed_out(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const
		{
			return state_ == configuration_load_state::pending && now - started_ >= timeout;
		}

		const runtime_configuration& active() const
		{
			return active_;
		}

		configuration_load_state state() const
		{
			return state_;
		}

		std::chrono::steady_clock::time_point last_loaded() const
		{
			return last_loaded_;
		}

	private:
		runtime_configuration active_;
		runtime_configuration baseline_;
		configuration_load_state state_ {configuration_load_state::defaults};
		std::chrono::steady_clock::time_point started_ {};
		std::chrono::steady_clock::time_point last_loaded_ {};
	};

} // namespace cs2glaz
