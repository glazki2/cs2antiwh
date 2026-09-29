#include "visibility_sampling.h"

// Builds current eye/input origins and the legacy weapon-muzzle point from
// copied player values. Body visibility uses the separately captured capsules.

#include <algorithm>
#include <cmath>

namespace cs2glaz
{
	namespace
	{

		constexpr float k_vertical_origin_offset = 16.0f;
		constexpr float k_feet_origin_lift = 4.0f;
		constexpr float k_ping_step_ms = 25.0f;
		constexpr float k_same_point_epsilon_sq = 1.0e-4f;
		constexpr uint32_t k_wall_clip_steps = 8;
		constexpr float k_degrees_to_radians = 0.017453292519943295769f;
		constexpr float k_standing_player_height = 72.0f;
		constexpr float k_pelvis_height = 38.0f;
		constexpr float k_muzzle_z = 60.0f;
		constexpr float k_top_bounds_padding = 8.0f;
		// Bounds corners and the hull body's lowest point stay this far above the
		// feet, so a point never lies on the floor surface, which a ray from below
		// ending there does not count as crossed.
		constexpr float k_bottom_bounds_lift = 2.0f;
		// Movement-following bounds padding: a standing player's margin, the
		// running speed the movement keys imply, and how far ahead a hidden player
		// is revealed (two snapshots of interpolation plus a visibility pass).
		constexpr float k_idle_bounds_padding = 4.0f;
		constexpr float k_run_speed = 250.0f;
		constexpr float k_padding_lookahead_seconds = 0.064f;
		constexpr float k_hull_floor_clearance = 0.5f;

		float distance_sq(vec3 a, vec3 b)
		{
			const float x = a.x - b.x;
			const float y = a.y - b.y;
			const float z = a.z - b.z;
			return x * x + y * y + z * z;
		}

		vec3 add(vec3 a, vec3 b)
		{
			return {a.x + b.x, a.y + b.y, a.z + b.z};
		}

		vec3 scale(vec3 value, float amount)
		{
			return {value.x * amount, value.y * amount, value.z * amount};
		}

		vec3 subtract(vec3 a, vec3 b)
		{
			return {a.x - b.x, a.y - b.y, a.z - b.z};
		}

		vec3 eye_right(float yaw_degrees)
		{
			const float yaw = yaw_degrees * k_degrees_to_radians;
			return {std::sin(yaw), -std::cos(yaw), 0.0f};
		}

		vec3 eye_forward(float yaw_degrees)
		{
			const float yaw = yaw_degrees * k_degrees_to_radians;
			return {std::cos(yaw), std::sin(yaw), 0.0f};
		}

		vec3 safe_origin(const bvh8_data& data, vec3 eye, vec3 candidate, std::span<const visibility_occluder> occluders)
		{
			if (distance_sq(eye, candidate) <= k_same_point_epsilon_sq || segment_blocked(data, eye, candidate).blocked
				|| occluders_block_segment(occluders, eye, candidate))
			{
				return eye;
			}
			return candidate;
		}

		float adjusted_local_z(const visibility_player& player, float z)
		{
			const float height = std::max(1.0f, player.maxs.z - player.mins.z);
			if (z < k_pelvis_height)
			{
				return player.mins.z + z;
			}
			const float standing_upper = k_standing_player_height - k_pelvis_height;
			const float live_upper = std::max(0.0f, height - k_pelvis_height);
			return player.mins.z + k_pelvis_height + (z - k_pelvis_height) * live_upper / standing_upper;
		}

		vec3 local_to_world(const visibility_player& player, vec3 local)
		{
			const vec3 forward = eye_forward(player.eye_yaw_degrees);
			const vec3 right = eye_right(player.eye_yaw_degrees);
			return {player.origin.x + forward.x * local.x - right.x * local.y, player.origin.y + forward.y * local.x - right.y * local.y,
					player.origin.z + adjusted_local_z(player, local.z)};
		}

		void add_origin(visibility_origin_points& origins, vec3 point, visibility_origin_role role)
		{
			for (uint32_t index = 0; index < origins.count; ++index)
			{
				if (distance_sq(origins.points[index], point) <= k_same_point_epsilon_sq)
				{
					return;
				}
			}
			if (origins.count < origins.points.size())
			{
				origins.roles[origins.count] = role;
				origins.points[origins.count++] = point;
			}
		}

	} // namespace

	// Valve's shared player HitboxCapsule set, extracted from the current SAS and
	// Phoenix VMDLs. Endpoints and radii are Source units in each named bone.
	const std::array<visibility_capsule_binding, k_visibility_capsule_count> k_visibility_capsule_bindings {
		{{"head_0", {-1.0f, 1.8f, 0.0f}, {3.5f, 0.2f, 0.0f}, 4.3f},
		 {"neck_0", {0.0f, -0.4f, 0.0f}, {1.4f, -0.2f, 0.0f}, 3.5f},
		 {"pelvis", {-2.7f, 1.1f, -3.2f}, {-2.7f, 1.1f, 3.2f}, 6.0f},
		 {"spine_0", {1.4f, 0.8f, 3.1f}, {1.4f, 0.8f, -3.1f}, 6.0f},
		 {"spine_1", {3.8f, 0.8f, -2.4f}, {3.8f, 0.4f, 2.4f}, 6.5f},
		 {"spine_2", {4.8f, 0.15f, -4.1f}, {4.8f, 0.15f, 4.1f}, 6.2f},
		 {"spine_3", {2.5f, -0.6f, -6.0f}, {2.5f, -0.6f, 6.0f}, 5.0f},
		 {"leg_upper_l", {1.3f, -0.2f, 0.0f}, {16.5f, -0.7f, 0.0f}, 5.0f},
		 {"leg_upper_r", {-1.3f, 0.0f, -0.6f}, {-16.5f, 0.0f, -0.7f}, 5.0f},
		 {"leg_lower_l", {0.1f, -0.4f, 0.2f}, {17.0f, -0.4f, 0.7f}, 4.0f},
		 {"leg_lower_r", {-0.1f, 0.0f, -0.2f}, {-17.0f, 0.4f, -0.7f}, 4.0f},
		 {"ankle_l", {0.0f, -3.43f, -0.52f}, {8.0f, 0.74f, 0.33f}, 2.6f},
		 {"ankle_r", {-7.98f, -0.75f, -0.27f}, {-0.02f, 3.44f, 0.58f}, 2.6f},
		 {"hand_l", {0.0f, 0.3f, 0.0f}, {3.59f, 1.15f, 0.11f}, 2.3f},
		 {"hand_r", {0.0f, -0.3f, 0.02f}, {-3.44f, -1.17f, -0.09f}, 2.3f},
		 {"arm_upper_l", {0.0f, 0.0f, 0.0f}, {11.2f, 0.0f, 0.0f}, 3.3f},
		 {"arm_lower_l", {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 3.0f},
		 {"arm_upper_r", {0.0f, 0.0f, 0.0f}, {-11.2f, 0.0f, 0.0f}, 3.3f},
		 {"arm_lower_r", {0.0f, 0.0f, 0.0f}, {-10.0f, 0.0f, -0.5f}, 3.0f}}};

	bool visibility_transform_point(const visibility_bone_transform& transform, vec3 local, vec3& world)
	{
		const float x = transform.rotation[0];
		const float y = transform.rotation[1];
		const float z = transform.rotation[2];
		const float w = transform.rotation[3];
		const float norm = x * x + y * y + z * z + w * w;
		if (!std::isfinite(transform.position.x) || !std::isfinite(transform.position.y) || !std::isfinite(transform.position.z)
			|| !std::isfinite(norm) || norm < 0.25f || norm > 4.0f)
		{
			return false;
		}
		const float inverse_length = 1.0f / std::sqrt(norm);
		const float qx = x * inverse_length;
		const float qy = y * inverse_length;
		const float qz = z * inverse_length;
		const float qw = w * inverse_length;
		const vec3 twice_cross {2.0f * (qy * local.z - qz * local.y), 2.0f * (qz * local.x - qx * local.z), 2.0f * (qx * local.y - qy * local.x)};
		world = {transform.position.x + local.x + qw * twice_cross.x + qy * twice_cross.z - qz * twice_cross.y,
				 transform.position.y + local.y + qw * twice_cross.y + qz * twice_cross.x - qx * twice_cross.z,
				 transform.position.z + local.z + qw * twice_cross.z + qx * twice_cross.y - qy * twice_cross.x};
		return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
	}

	bool valid_visibility_capsule(const visibility_capsule& capsule)
	{
		const auto finite = [](vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		return finite(capsule.start) && finite(capsule.end) && std::isfinite(capsule.radius) && capsule.radius > 0.0f && capsule.radius <= 32.0f;
	}

	uint32_t visibility_hull_capsules(vec3 origin, vec3 mins, vec3 maxs, std::array<visibility_capsule, k_visibility_capsule_count>& capsules)
	{
		const float width_x = maxs.x - mins.x;
		const float width_y = maxs.y - mins.y;
		const float height = maxs.z - mins.z;
		if (!std::isfinite(width_x) || !std::isfinite(width_y) || !std::isfinite(height) || width_x <= 0.0f || width_y <= 0.0f
			|| height <= 0.0f)
		{
			return 0;
		}
		const float grid = static_cast<float>(k_visibility_hull_capsule_grid);
		const float cell_x = width_x / grid;
		const float cell_y = width_y / grid;
		// Half the cell diagonal reaches every point of the cell from its centre, so
		// each vertical capsule covers its column of the hull; 0.5 units absorbs
		// rounding. The top cap adds the radius above the head. The bottom cap is
		// raised to end just above the feet: below them it went through thin floors
		// and revealed the player to anyone underneath. Only the lowest band of the
		// hull's outer corners is left uncovered, where a player has no body.
		const float radius = 0.5f * std::sqrt(cell_x * cell_x + cell_y * cell_y) + 0.5f;
		const float bottom = origin.z + mins.z + radius + k_hull_floor_clearance;
		const float top = origin.z + maxs.z;
		if (bottom >= top)
		{
			return 0;
		}
		uint32_t count = 0;
		for (uint32_t column = 0; column < k_visibility_hull_capsule_grid; ++column)
		{
			for (uint32_t row = 0; row < k_visibility_hull_capsule_grid; ++row)
			{
				const float x = origin.x + mins.x + (static_cast<float>(column) + 0.5f) * cell_x;
				const float y = origin.y + mins.y + (static_cast<float>(row) + 0.5f) * cell_y;
				visibility_capsule& capsule = capsules[count];
				capsule = {{x, y, bottom}, {x, y, top}, radius};
				if (!valid_visibility_capsule(capsule))
				{
					return 0;
				}
				++count;
			}
		}
		return count;
	}

	float visibility_shoulder_offset_units(float rtt_seconds, const visibility_tuning& tuning, bool movement_intent)
	{
		const float configured_base = std::max(0.0f, tuning.shoulder_base_units);
		const float base = movement_intent ? configured_base : configured_base * 0.5f;
		const float rtt_ms = std::max(0.0f, rtt_seconds) * 1000.0f;
		const float stepped_ms = std::floor(rtt_ms / k_ping_step_ms) * k_ping_step_ms;
		const float wanted = base + stepped_ms * std::max(0.0f, tuning.shoulder_rtt_scale);
		// A non-positive maximum deliberately means that the RTT table has no cap.
		if (tuning.max_shoulder_units <= 0.0f)
		{
			return std::max(base, wanted);
		}
		return std::clamp(wanted, base, std::max(base, tuning.max_shoulder_units));
	}

	vec3 visibility_clip_destination(const bvh8_data& data, vec3 origin, vec3 destination, std::span<const visibility_occluder> occluders)
	{
		const auto blocked_to = [&](vec3 point) { return segment_blocked(data, origin, point).blocked || occluders_block_segment(occluders, origin, point); };
		if (distance_sq(origin, destination) <= k_same_point_epsilon_sq || !blocked_to(destination))
		{
			return destination;
		}
		vec3 clear = origin;
		vec3 blocked = destination;
		for (uint32_t step = 0; step < k_wall_clip_steps; ++step)
		{
			const vec3 middle = scale(add(clear, blocked), 0.5f);
			if (blocked_to(middle))
			{
				blocked = middle;
			}
			else
			{
				clear = middle;
			}
		}
		return clear;
	}

	weapon_muzzle_class weapon_muzzle_class_from_item_definition(uint16_t item_definition)
	{
		switch (item_definition)
		{
			case 1:
			case 2:
			case 3:
			case 4:
			case 30:
			case 32:
			case 36:
			case 61:
			case 63:
			case 64:
				return weapon_muzzle_class::pistol;
			case 17:
			case 19:
			case 23:
			case 24:
			case 26:
			case 33:
			case 34:
				return weapon_muzzle_class::smg;
			case 9:
			case 11:
			case 38:
			case 40:
				return weapon_muzzle_class::sniper;
			case 7:
			case 8:
			case 10:
			case 13:
			case 14:
			case 16:
			case 25:
			case 27:
			case 28:
			case 29:
			case 35:
			case 39:
			case 60:
				return weapon_muzzle_class::rifle;
			default:
				return weapon_muzzle_class::none;
		}
	}

	float weapon_muzzle_length(weapon_muzzle_class value)
	{
		switch (value)
		{
			case weapon_muzzle_class::pistol:
				return 18.0f;
			case weapon_muzzle_class::smg:
				return 28.0f;
			case weapon_muzzle_class::rifle:
				return 36.0f;
			case weapon_muzzle_class::sniper:
				return 52.0f;
			default:
				return 0.0f;
		}
	}

	visibility_origin_points visibility_origins(const bvh8_data& data, const visibility_player& player, const visibility_tuning& tuning,
												std::span<const visibility_occluder> occluders)
	{
		visibility_origin_points origins;
		const vec3 forward = eye_forward(player.eye_yaw_degrees);
		const vec3 right_axis = eye_right(player.eye_yaw_degrees);
		const float forward_input = static_cast<float>((player.movement_buttons & k_visibility_button_forward) != 0)
									- static_cast<float>((player.movement_buttons & k_visibility_button_back) != 0);
		const float side_input = static_cast<float>((player.movement_buttons & k_visibility_button_right) != 0)
								 - static_cast<float>((player.movement_buttons & k_visibility_button_left) != 0);
		const bool left_button = (player.movement_buttons & k_visibility_button_left) != 0;
		const bool right_button = (player.movement_buttons & k_visibility_button_right) != 0;
		const float left_offset = visibility_shoulder_offset_units(player.rtt_seconds, tuning, left_button);
		const float right_offset = visibility_shoulder_offset_units(player.rtt_seconds, tuning, right_button);
		const vec3 left = subtract(player.eye, scale(right_axis, left_offset));
		const vec3 right = add(player.eye, scale(right_axis, right_offset));
		const vec3 vertical {0.0f, 0.0f, k_vertical_origin_offset};
		add_origin(origins, player.eye, visibility_origin_role::eye);
		add_origin(origins, safe_origin(data, player.eye, left, occluders), visibility_origin_role::left_shoulder);
		add_origin(origins, safe_origin(data, player.eye, right, occluders), visibility_origin_role::right_shoulder);
		add_origin(origins, safe_origin(data, player.eye, add(player.eye, vertical), occluders), visibility_origin_role::above);
		// The origin sits on the floor surface and can round to just under it, where
		// rays pass below a thin floor to the level beneath; lift it and require a
		// clear line from the eye like the other origins.
		add_origin(origins, safe_origin(data, player.eye, add(player.origin, {0.0f, 0.0f, k_feet_origin_lift}), occluders), visibility_origin_role::feet);

		// Pure A/D already has the matching ping-scaled shoulder origin.
		if (forward_input != 0.0f)
		{
			vec3 direction = add(scale(forward, forward_input), scale(right_axis, side_input));
			const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
			const float movement_offset = visibility_shoulder_offset_units(player.rtt_seconds, tuning, true);
			direction = scale(direction, movement_offset / length);
			add_origin(origins, visibility_clip_destination(data, player.eye, add(player.eye, direction), occluders), visibility_origin_role::movement);
		}
		return origins;
	}

	bool visibility_muzzle_point(const visibility_player& player, vec3& point)
	{
		const float length = weapon_muzzle_length(player.muzzle_class);
		if (length <= 0.0f)
		{
			return false;
		}
		point = local_to_world(player, {length, 0.0f, k_muzzle_z});
		return true;
	}

	std::array<vec3, k_visibility_aabb_point_count> visibility_aabb_points(const visibility_player& player, float horizontal_padding)
	{
		const float padding = std::isfinite(horizontal_padding) ? std::clamp(horizontal_padding, 0.0f, 64.0f) : 32.0f;
		float minus_x = padding;
		float plus_x = padding;
		float minus_y = padding;
		float plus_y = padding;
		if (player.has_velocity && std::isfinite(player.velocity.x) && std::isfinite(player.velocity.y))
		{
			// Where the keys push him, at running speed.
			const vec3 forward = eye_forward(player.eye_yaw_degrees);
			const vec3 right = eye_right(player.eye_yaw_degrees);
			const float forward_input = static_cast<float>((player.movement_buttons & k_visibility_button_forward) != 0)
										- static_cast<float>((player.movement_buttons & k_visibility_button_back) != 0);
			const float side_input = static_cast<float>((player.movement_buttons & k_visibility_button_right) != 0)
									 - static_cast<float>((player.movement_buttons & k_visibility_button_left) != 0);
			vec3 intent = add(scale(forward, forward_input), scale(right, side_input));
			const float intent_length = std::sqrt(intent.x * intent.x + intent.y * intent.y);
			intent = intent_length > 0.0f ? scale(intent, k_run_speed / intent_length) : vec3 {};
			const float idle = std::min(k_idle_bounds_padding, padding);
			const auto reach = [&](float velocity, float intended)
			{ return std::clamp(idle + std::max({0.0f, velocity, intended}) * k_padding_lookahead_seconds, idle, padding); };
			plus_x = reach(player.velocity.x, intent.x);
			minus_x = reach(-player.velocity.x, -intent.x);
			plus_y = reach(player.velocity.y, intent.y);
			minus_y = reach(-player.velocity.y, -intent.y);
		}
		const vec3 minimum {player.origin.x + player.mins.x - minus_x, player.origin.y + player.mins.y - minus_y,
							player.origin.z + player.mins.z + k_bottom_bounds_lift};
		const vec3 maximum {player.origin.x + player.maxs.x + plus_x, player.origin.y + player.maxs.y + plus_y,
							player.origin.z + player.maxs.z + k_top_bounds_padding};
		return {{{minimum.x, minimum.y, minimum.z},
				 {maximum.x, minimum.y, minimum.z},
				 {minimum.x, maximum.y, minimum.z},
				 {maximum.x, maximum.y, minimum.z},
				 {minimum.x, minimum.y, maximum.z},
				 {maximum.x, minimum.y, maximum.z},
				 {minimum.x, maximum.y, maximum.z},
				 {maximum.x, maximum.y, maximum.z}}};
	}

	visibility_target_points visibility_clipped_target_points(const bvh8_data& data, const visibility_player& player,
															  std::span<const visibility_occluder> occluders, float horizontal_padding)
	{
		visibility_target_points points;
		const vec3 centre {player.origin.x, player.origin.y, player.origin.z + 0.5f * (player.mins.z + player.maxs.z)};
		const std::array<vec3, k_visibility_aabb_point_count> corners = visibility_aabb_points(player, horizontal_padding);
		for (uint32_t index = 0; index < corners.size(); ++index)
		{
			points.aabb[index] = visibility_clip_destination(data, centre, corners[index], occluders);
		}
		vec3 muzzle;
		points.has_muzzle = visibility_muzzle_point(player, muzzle);
		if (points.has_muzzle)
		{
			points.muzzle = visibility_clip_destination(data, player.eye, muzzle, occluders);
		}
		return points;
	}

} // namespace cs2glaz
