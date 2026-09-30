#include "decoy_logic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cs2glaz
{
	namespace
	{

		constexpr float k_degrees_to_radians = 0.017453292519943295769f;
		constexpr float k_spot_cell = 48.0f;
		constexpr float k_spot_cell_height = 64.0f;
		constexpr float k_corner_padding = 24.0f;
		constexpr float k_corner_top = 80.0f;
		constexpr float k_aim_body_radius = 20.0f;
		constexpr float k_aim_tolerance_degrees = 1.5f;
		constexpr uint32_t k_spot_attempts = 32;

		float distance_sq(vec3 a, vec3 b)
		{
			const float x = a.x - b.x;
			const float y = a.y - b.y;
			const float z = a.z - b.z;
			return x * x + y * y + z * z;
		}

		bool finite(vec3 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool far_from_all(vec3 point, std::span<const vec3> others, float distance)
		{
			return std::all_of(others.begin(), others.end(), [&](vec3 other) { return distance_sq(point, other) >= distance * distance; });
		}

		vec3 body_center(vec3 origin)
		{
			return {origin.x, origin.y, origin.z + k_decoy_center_height};
		}

	} // namespace

	void decoy_spot_history::clear()
	{
		spots_.clear();
		keys_.clear();
		occupied_.clear();
		next_ = 0;
	}

	void decoy_spot_history::record(vec3 feet)
	{
		if (!finite(feet) || std::fabs(feet.x) > 65536.0f || std::fabs(feet.y) > 65536.0f || std::fabs(feet.z) > 65536.0f)
		{
			return;
		}
		const auto cell = [](float value, float size) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(value / size)) + 32768) & 0xfffffu; };
		const uint64_t key = cell(feet.x, k_spot_cell) | (cell(feet.y, k_spot_cell) << 20u) | (cell(feet.z, k_spot_cell_height) << 40u);
		if (!occupied_.insert(key).second)
		{
			return;
		}
		if (spots_.size() < k_max_decoy_spots)
		{
			spots_.push_back(feet);
			keys_.push_back(key);
			return;
		}
		occupied_.erase(keys_[next_]);
		spots_[next_] = feet;
		keys_[next_] = key;
		next_ = (next_ + 1u) % k_max_decoy_spots;
	}

	bool choose_decoy_spot(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3& spot)
	{
		const std::span<const vec3> points = history.points();
		if (points.empty() || !finite(query.viewer_eye))
		{
			return false;
		}
		uint32_t state = query.seed == 0 ? 0x9e3779b9u : query.seed;
		float best = std::numeric_limits<float>::max();
		bool found = false;
		for (uint32_t attempt = 0; attempt < k_spot_attempts; ++attempt)
		{
			const vec3 candidate = points[decoy_random(state) % points.size()];
			const float distance = distance_sq(candidate, query.viewer_eye);
			if (distance < k_decoy_min_distance * k_decoy_min_distance || distance > k_decoy_max_distance * k_decoy_max_distance
				|| distance >= best || !far_from_all(candidate, query.enemies, k_decoy_enemy_clearance)
				|| !far_from_all(candidate, query.players, k_decoy_player_clearance) || !far_from_all(candidate, query.taken, k_decoy_spacing))
			{
				continue;
			}
			const vec3 center = body_center(candidate);
			if (!segment_blocked(data, query.viewer_eye, center).blocked && !occluders_block_segment(occluders, query.viewer_eye, center))
			{
				continue;
			}
			best = distance;
			spot = candidate;
			found = true;
		}
		return found;
	}

	bool decoy_hidden_from_origins(const bvh8_data& data, const visibility_origin_points& origins, vec3 origin,
								   std::span<const visibility_occluder> occluders, std::chrono::steady_clock::time_point deadline)
	{
		if (!finite(origin) || origins.count == 0)
		{
			return false;
		}
		std::array<visibility_capsule, k_visibility_capsule_count> capsules {};
		const uint32_t capsule_count = visibility_hull_capsules(origin, k_decoy_mins, k_decoy_maxs, capsules);
		if (capsule_count == 0)
		{
			return false;
		}
		std::array<vec3, 8> corners {};
		for (uint32_t index = 0; index < corners.size(); ++index)
		{
			corners[index] = {origin.x + ((index & 1u) != 0 ? k_decoy_maxs.x + k_corner_padding : k_decoy_mins.x - k_corner_padding),
							  origin.y + ((index & 2u) != 0 ? k_decoy_maxs.y + k_corner_padding : k_decoy_mins.y - k_corner_padding),
							  origin.z + ((index & 4u) != 0 ? k_corner_top : 1.0f)};
		}
		for (uint32_t origin_index = 0; origin_index < origins.count; ++origin_index)
		{
			const vec3 eye = origins.points[origin_index];
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			const capsule_query_result body = capsule_visible_from_origin(data, eye, std::span<const visibility_capsule>(capsules.data(), capsule_count),
																		  nullptr, 0.0f, deadline, nullptr, nullptr, nullptr, occluders);
			if (body != capsule_query_result::blocked)
			{
				return false;
			}
			for (const vec3& corner : corners)
			{
				if (!segment_blocked(data, eye, corner).blocked && !occluders_block_segment(occluders, eye, corner))
				{
					return false;
				}
			}
		}
		return true;
	}

	float decoy_tracking_degrees(vec3 eye_before, vec3 eye_now, vec3 decoy_origin)
	{
		const vec3 center = body_center(decoy_origin);
		const vec3 a {center.x - eye_before.x, center.y - eye_before.y, center.z - eye_before.z};
		const vec3 b {center.x - eye_now.x, center.y - eye_now.y, center.z - eye_now.z};
		const float lengths = std::sqrt((a.x * a.x + a.y * a.y + a.z * a.z) * (b.x * b.x + b.y * b.y + b.z * b.z));
		if (!std::isfinite(lengths) || !(lengths > 0.0f))
		{
			return 0.0f;
		}
		return std::acos(std::clamp((a.x * b.x + a.y * b.y + a.z * b.z) / lengths, -1.0f, 1.0f)) / k_degrees_to_radians;
	}

	vec3 view_forward(float pitch_degrees, float yaw_degrees)
	{
		const float pitch = pitch_degrees * k_degrees_to_radians;
		const float yaw = yaw_degrees * k_degrees_to_radians;
		return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
	}

	bool aim_on_decoy(vec3 eye, float pitch_degrees, float yaw_degrees, vec3 decoy_origin)
	{
		if (!finite(eye) || !finite(decoy_origin) || !std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees))
		{
			return false;
		}
		const vec3 center = body_center(decoy_origin);
		const vec3 to_center {center.x - eye.x, center.y - eye.y, center.z - eye.z};
		const float distance = std::sqrt(to_center.x * to_center.x + to_center.y * to_center.y + to_center.z * to_center.z);
		if (distance < 1.0f)
		{
			return false;
		}
		const vec3 forward = view_forward(pitch_degrees, yaw_degrees);
		const float cosine = std::clamp((forward.x * to_center.x + forward.y * to_center.y + forward.z * to_center.z) / distance, -1.0f, 1.0f);
		const float angle = std::acos(cosine);
		const float allowed = std::atan2(k_aim_body_radius, distance) + k_aim_tolerance_degrees * k_degrees_to_radians;
		return angle <= allowed;
	}

} // namespace cs2glaz
