#include "decoy_logic.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

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

		float angle_degrees(vec3 a, vec3 b)
		{
			const float lengths = std::sqrt((a.x * a.x + a.y * a.y + a.z * a.z) * (b.x * b.x + b.y * b.y + b.z * b.z));
			if (!std::isfinite(lengths) || !(lengths > 0.0f))
			{
				return 0.0f;
			}
			return std::acos(std::clamp((a.x * b.x + a.y * b.y + a.z * b.z) / lengths, -1.0f, 1.0f)) / k_degrees_to_radians;
		}

		bool hidden_from_eye(const bvh8_data& data, std::span<const visibility_occluder> occluders, vec3 eye, vec3 point)
		{
			return segment_blocked(data, eye, point).blocked || occluders_block_segment(occluders, eye, point);
		}

		bool spot_rules(const decoy_spot_query& query, vec3 candidate)
		{
			const float distance = distance_sq(candidate, query.viewer_eye);
			return distance >= k_decoy_min_distance * k_decoy_min_distance && distance <= k_decoy_max_distance * k_decoy_max_distance
				   && far_from_all(candidate, query.enemies, k_decoy_enemy_clearance) && far_from_all(candidate, query.players, k_decoy_player_clearance)
				   && far_from_all(candidate, query.taken, k_decoy_spacing);
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
		// Spots behind the viewer rank after every spot in front of him.
		constexpr float k_behind_penalty = 1.0e12f;
		const bool prefer_view = std::isfinite(query.view_yaw_degrees);
		float best = std::numeric_limits<float>::max();
		bool found = false;
		for (uint32_t attempt = 0; attempt < k_spot_attempts; ++attempt)
		{
			const vec3 candidate = points[decoy_random(state) % points.size()];
			float score = distance_sq(candidate, query.viewer_eye);
			if (prefer_view)
			{
				const float yaw = std::atan2(candidate.y - query.viewer_eye.y, candidate.x - query.viewer_eye.x) / k_degrees_to_radians;
				const float off = std::fabs(std::remainder(yaw - query.view_yaw_degrees, 360.0f));
				score += off <= k_decoy_view_half_angle ? 0.0f : k_behind_penalty;
			}
			if (score >= best || !spot_rules(query, candidate) || !hidden_from_eye(data, occluders, query.viewer_eye, body_center(candidate)))
			{
				continue;
			}
			best = score;
			spot = candidate;
			found = true;
		}
		return found;
	}

	bool choose_decoy_step(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3 from, vec3& next)
	{
		const std::span<const vec3> points = history.points();
		if (points.empty() || !finite(from) || !finite(query.viewer_eye))
		{
			return false;
		}
		uint32_t state = query.seed == 0 ? 0x85ebca6bu : query.seed;
		for (uint32_t attempt = 0; attempt < k_spot_attempts; ++attempt)
		{
			const vec3 candidate = points[decoy_random(state) % points.size()];
			const float dx = candidate.x - from.x;
			const float dy = candidate.y - from.y;
			const float run = std::sqrt(dx * dx + dy * dy);
			const float rise = std::fabs(candidate.z - from.z);
			if (run < k_decoy_step_min || run > k_decoy_step_max || rise > 0.5f * run + 8.0f || !spot_rules(query, candidate))
			{
				continue;
			}
			const vec3 middle {0.5f * (from.x + candidate.x), 0.5f * (from.y + candidate.y), 0.5f * (from.z + candidate.z)};
			if (!hidden_from_eye(data, occluders, query.viewer_eye, body_center(candidate))
				|| !hidden_from_eye(data, occluders, query.viewer_eye, body_center(middle)))
			{
				continue;
			}
			bool walkable = true;
			for (const float height : {18.0f, 54.0f})
			{
				const vec3 start {from.x, from.y, from.z + height};
				const vec3 end {candidate.x, candidate.y, candidate.z + height};
				walkable = walkable && !segment_blocked(data, start, end).blocked && !occluders_block_segment(occluders, start, end);
			}
			for (const float fraction : {0.25f, 0.5f, 0.75f})
			{
				const vec3 point {from.x + dx * fraction, from.y + dy * fraction, from.z + (candidate.z - from.z) * fraction};
				walkable = walkable && segment_blocked(data, {point.x, point.y, point.z + 24.0f}, {point.x, point.y, point.z - 48.0f}).blocked;
			}
			if (!walkable)
			{
				continue;
			}
			next = candidate;
			return true;
		}
		return false;
	}

	decoy_proof prove_decoy_hidden(const bvh8_data& data, const visibility_origin_points& origins, vec3 origin,
								   std::span<const visibility_occluder> occluders, std::chrono::steady_clock::time_point deadline)
	{
		if (!finite(origin) || origins.count == 0)
		{
			return decoy_proof::unproven;
		}
		std::array<visibility_capsule, k_visibility_capsule_count> capsules {};
		const uint32_t capsule_count = visibility_hull_capsules(origin, k_decoy_mins, k_decoy_maxs, capsules);
		if (capsule_count == 0)
		{
			return decoy_proof::unproven;
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
				return decoy_proof::unproven;
			}
			const capsule_query_result body = capsule_visible_from_origin(data, eye, std::span<const visibility_capsule>(capsules.data(), capsule_count),
																		  nullptr, 0.0f, deadline, nullptr, nullptr, nullptr, occluders);
			if (body == capsule_query_result::visible)
			{
				return decoy_proof::seen;
			}
			if (body != capsule_query_result::blocked)
			{
				return decoy_proof::unproven;
			}
			for (const vec3& corner : corners)
			{
				if (!segment_blocked(data, eye, corner).blocked && !occluders_block_segment(occluders, eye, corner))
				{
					return decoy_proof::seen;
				}
			}
		}
		return decoy_proof::hidden;
	}

	bool front_crowds_viewer(vec3 viewer_feet, vec3 origin)
	{
		if (!finite(viewer_feet) || !finite(origin))
		{
			return false;
		}
		const float x = origin.x - viewer_feet.x;
		const float y = origin.y - viewer_feet.y;
		return x * x + y * y < k_front_hittable_clearance * k_front_hittable_clearance && std::fabs(origin.z - viewer_feet.z) < 96.0f;
	}

	float front_offset_degrees(vec3 eye, float pitch_degrees, float yaw_degrees, vec3 origin)
	{
		if (!finite(eye) || !finite(origin) || !std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees))
		{
			return 180.0f;
		}
		const vec3 center = body_center(origin);
		return angle_degrees(view_forward(pitch_degrees, yaw_degrees), {center.x - eye.x, center.y - eye.y, center.z - eye.z});
	}

	namespace
	{

		// front_spot_rules with the view direction (unit length) worked out once:
		// most spots fail the distance or the cone, cheaply, before the rest.
		bool front_rules(const front_spot_query& query, vec3 forward, vec3 candidate)
		{
			static const float cos_min = std::cos(k_front_min_degrees * k_degrees_to_radians);
			static const float cos_max = std::cos(k_front_max_degrees * k_degrees_to_radians);
			const vec3 eye = query.viewer_eye;
			const vec3 center = body_center(candidate);
			const vec3 to_center {center.x - eye.x, center.y - eye.y, center.z - eye.z};
			const float distance_squared = to_center.x * to_center.x + to_center.y * to_center.y + to_center.z * to_center.z;
			if (!std::isfinite(distance_squared) || distance_squared < k_front_min_distance * k_front_min_distance
				|| distance_squared > k_front_max_distance * k_front_max_distance)
			{
				return false;
			}
			const float cosine = (forward.x * to_center.x + forward.y * to_center.y + forward.z * to_center.z) / std::sqrt(distance_squared);
			if (!(cosine <= cos_min && cosine >= cos_max) || direction_on_decoy(eye, forward, candidate)
				|| !far_from_all(candidate, query.players, k_front_player_clearance) || front_crowds_viewer(query.viewer_feet, candidate))
			{
				return false;
			}
			// Not in line with a player either way: an aim at one would be an aim
			// at the other.
			if (direction_on_any_player(eye, to_center, query.players)
				|| std::any_of(query.players.begin(), query.players.end(),
							   [&](vec3 player)
							   {
								   const vec3 player_center = body_center(player);
								   return direction_on_decoy(eye, {player_center.x - eye.x, player_center.y - eye.y, player_center.z - eye.z}, candidate);
							   }))
			{
				return false;
			}
			if (std::any_of(query.taken.begin(), query.taken.end(),
							[&](vec3 other) { return decoy_angle_between(eye, other, candidate) < k_decoy_jump_min_degrees; }))
			{
				return false;
			}
			return query.from == nullptr || decoy_jump_distance_ok(eye, *query.from, candidate);
		}

		bool front_query_valid(const front_spot_query& query)
		{
			return finite(query.viewer_eye) && std::isfinite(query.view_pitch_degrees) && std::isfinite(query.view_yaw_degrees);
		}

	} // namespace

	bool front_spot_rules(const front_spot_query& query, vec3 candidate)
	{
		return front_query_valid(query) && finite(candidate)
			   && front_rules(query, view_forward(query.view_pitch_degrees, query.view_yaw_degrees), candidate);
	}

	bool choose_front_spot(const bvh8_data& data, const decoy_spot_history& history, const front_spot_query& query, vec3& spot)
	{
		const std::span<const vec3> points = history.points();
		if (points.empty() || !front_query_valid(query))
		{
			return false;
		}
		const vec3 forward = view_forward(query.view_pitch_degrees, query.view_yaw_degrees);
		uint32_t state = query.seed == 0 ? 0x27d4eb2fu : query.seed;
		// Near the crosshair first, spread by up to 6 degrees so it does not
		// always take the same place.
		// Kept between calls: up to k_max_decoy_spots entries, allocated once.
		thread_local std::vector<std::pair<float, uint32_t>> passing;
		passing.clear();
		for (uint32_t index = 0; index < points.size(); ++index)
		{
			if (front_rules(query, forward, points[index]))
			{
				const float spread = static_cast<float>(decoy_random(state) % 600u) / 100.0f;
				passing.emplace_back(front_offset_degrees(query.viewer_eye, query.view_pitch_degrees, query.view_yaw_degrees, points[index]) + spread,
									 index);
			}
		}
		constexpr size_t k_traced = 8;
		const size_t traced = std::min(passing.size(), k_traced);
		std::partial_sort(passing.begin(), passing.begin() + static_cast<std::ptrdiff_t>(traced), passing.end());
		for (size_t rank = 0; rank < traced; ++rank)
		{
			const vec3 candidate = points[passing[rank].second];
			const vec3 head {candidate.x, candidate.y, candidate.z + k_front_head_height};
			if (!segment_blocked(data, query.viewer_eye, body_center(candidate)).blocked && !segment_blocked(data, query.viewer_eye, head).blocked)
			{
				spot = candidate;
				return true;
			}
		}
		return false;
	}

	bool front_spot_keeps(const bvh8_data& data, const front_spot_query& query, vec3 origin)
	{
		const vec3 eye = query.viewer_eye;
		if (!finite(origin) || !finite(eye))
		{
			return false;
		}
		const vec3 center = body_center(origin);
		const float distance = std::sqrt(distance_sq(center, eye));
		return distance >= k_front_keep_min_distance && distance <= k_front_keep_max_distance
			   && front_offset_degrees(eye, query.view_pitch_degrees, query.view_yaw_degrees, origin) <= k_front_keep_degrees
			   && far_from_all(origin, query.players, k_front_player_clearance * 0.5f) && !front_crowds_viewer(query.viewer_feet, origin)
			   && !segment_blocked(data, eye, center).blocked;
	}

	bool front_body_stopped_bullet(const bvh8_data& data, vec3 body_origin, vec3 eye, vec3 impact)
	{
		if (!finite(body_origin) || !finite(eye) || !finite(impact) || std::fabs(impact.x - body_origin.x) > k_front_body_half_width
			|| std::fabs(impact.y - body_origin.y) > k_front_body_half_width || impact.z < body_origin.z + k_front_body_floor_margin
			|| impact.z > body_origin.z + k_front_body_height)
		{
			return false;
		}
		const vec3 line {impact.x - eye.x, impact.y - eye.y, impact.z - eye.z};
		const float length = std::sqrt(line.x * line.x + line.y * line.y + line.z * line.z);
		if (!(length > 1.0f) || !std::isfinite(length))
		{
			return false;
		}
		// A wall, a box or the floor at the impact: the bullet stopped there, not
		// in the body.
		constexpr float k_margin = 8.0f;
		const vec3 step {line.x / length * k_margin, line.y / length * k_margin, line.z / length * k_margin};
		return !segment_blocked(data, {impact.x - step.x, impact.y - step.y, impact.z - step.z}, {impact.x + step.x, impact.y + step.y, impact.z + step.z})
					.blocked;
	}

	float decoy_follow_degrees(vec3 eye_before, vec3 forward_before, vec3 decoy_before, vec3 eye_now, vec3 forward_now, vec3 decoy_now)
	{
		const vec3 center_before = body_center(decoy_before);
		const vec3 center_now = body_center(decoy_now);
		const float direction_turn = angle_degrees({center_before.x - eye_before.x, center_before.y - eye_before.y, center_before.z - eye_before.z},
												   {center_now.x - eye_now.x, center_now.y - eye_now.y, center_now.z - eye_now.z});
		const float view_turn = angle_degrees(forward_before, forward_now);
		return std::min(direction_turn, view_turn);
	}

	vec3 view_forward(float pitch_degrees, float yaw_degrees)
	{
		const float pitch = pitch_degrees * k_degrees_to_radians;
		const float yaw = yaw_degrees * k_degrees_to_radians;
		return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
	}

	bool direction_on_any_player(vec3 eye, vec3 direction, std::span<const vec3> players)
	{
		return std::any_of(players.begin(), players.end(), [&](vec3 player) { return direction_on_decoy(eye, direction, player); });
	}

	bool direction_on_decoy(vec3 eye, vec3 direction, vec3 decoy_origin)
	{
		if (!finite(eye) || !finite(decoy_origin) || !finite(direction))
		{
			return false;
		}
		const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
		const vec3 center = body_center(decoy_origin);
		const vec3 to_center {center.x - eye.x, center.y - eye.y, center.z - eye.z};
		const float distance = std::sqrt(to_center.x * to_center.x + to_center.y * to_center.y + to_center.z * to_center.z);
		if (distance < 1.0f || !(length > 1.0e-6f) || !std::isfinite(length))
		{
			return false;
		}
		const float cosine =
			std::clamp((direction.x * to_center.x + direction.y * to_center.y + direction.z * to_center.z) / (length * distance), -1.0f, 1.0f);
		const float angle = std::acos(cosine);
		const float allowed = std::atan2(k_aim_body_radius, distance) + k_aim_tolerance_degrees * k_degrees_to_radians;
		return angle <= allowed;
	}

} // namespace cs2glaz
