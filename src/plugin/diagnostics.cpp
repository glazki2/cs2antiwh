#include "plugin.h"

// Administrator diagnostics in the server console only (nothing is sent to
// players): cs2glaz_why explains, for each enemy, the line of sight, what
// opened it and the last CheckTransmit decision in both directions.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace cs2glaz
{
	namespace
	{

		constexpr auto k_decision_fresh = std::chrono::milliseconds(500);

		// Console names for cs2glaz_why.
		const char* decision_name(pair_decision decision)
		{
			switch (decision)
			{
				case pair_decision::hidden:
					return "hidden";
				case pair_decision::in_view:
					return "sent (in view)";
				case pair_decision::changing:
					return "sent (enemy spawning/dying)";
				case pair_decision::recipient_changing:
					return "sent (you spawning/dying)";
				case pair_decision::attachment:
					return "sent (attachment)";
				case pair_decision::group:
					return "sent (weapons not listed)";
				case pair_decision::baseline:
					return "sent (mode 0 baseline)";
				case pair_decision::full_update:
					return "sent (full update)";
				case pair_decision::none:
					break;
			}
			return "no data";
		}

		std::string reveal_name(uint8_t code, uint8_t held = 0)
		{
			if (visibility_reveal_test(code) == visibility_reveal::hold && visibility_reveal_test(held) != visibility_reveal::none
				&& visibility_reveal_test(held) != visibility_reveal::hold)
			{
				return "hold after " + reveal_name(held);
			}
			constexpr const char* tests[] = {"hidden", "hold", "body", "bounds corner", "muzzle", "unproven"};
			constexpr const char* origins[] = {"eye", "left shoulder", "right shoulder", "above head", "feet", "movement"};
			const auto test = static_cast<size_t>(visibility_reveal_test(code));
			const uint8_t origin = visibility_reveal_origin(code);
			std::string text = test < std::size(tests) ? tests[test] : "?";
			if (origin < std::size(origins))
			{
				text += " from ";
				text += origins[origin];
			}
			return text;
		}

		float distance_units(vec3 a, vec3 b)
		{
			const float x = a.x - b.x;
			const float y = a.y - b.y;
			const float z = a.z - b.z;
			return std::sqrt(x * x + y * y + z * z);
		}

	} // namespace

	std::string plugin::slot_name(CGameEntitySystem* system, uint32_t slot) const
	{
		std::string name = "игрок " + std::to_string(slot);
		CEntityInstance* controller = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
		if (controller == nullptr || !compatibility_.player_name_available())
		{
			return name;
		}
		char text[33] {};
		std::memcpy(text, reinterpret_cast<const char*>(controller) + compatibility_.fields().player_name, sizeof(text) - 1);
		for (char& character : text)
		{
			if (character != '\0' && static_cast<unsigned char>(character) < 0x20)
			{
				character = ' ';
			}
		}
		return text[0] == '\0' ? name : std::string(text);
	}

	// Re-runs every test from every viewing origin now, without smoke, to show
	// exactly which one sees the enemy.
	void plugin::print_why_probe(const player_state& viewer, const player_state& enemy, const visibility_result& result) const
	{
		if (data_.nodes.empty() || enemy.capsule_count == 0 || enemy.capsule_count > k_visibility_capsule_count)
		{
			return;
		}
		const runtime_configuration& configuration = settings::current();
		const visibility_tuning tuning {configuration.shoulder_base_units, configuration.shoulder_rtt_scale, configuration.max_shoulder_units,
										configuration.bounds_padding_units};
		const visibility_player from = visibility_sample(viewer);
		const visibility_player to = visibility_sample(enemy);
		const visibility_origin_points origins = visibility_origins(data_, from, tuning, result.occluders);
		const visibility_target_points points = visibility_clipped_target_points(data_, to, result.occluders, tuning.bounds_padding_units);
		constexpr const char* roles[] = {"eye", "left shoulder", "right shoulder", "above head", "feet", "movement"};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
		std::string text;
		for (uint32_t index = 0; index < origins.count; ++index)
		{
			const vec3 origin = origins.points[index];
			const capsule_query_result body =
				capsule_visible_from_origin(data_, origin, std::span<const visibility_capsule>(to.capsules.data(), to.capsule_count), nullptr, 0.0f,
											deadline, nullptr, nullptr, nullptr, result.occluders);
			uint32_t open_corners = 0;
			for (const vec3& corner : points.aabb)
			{
				open_corners += !segment_blocked(data_, origin, corner).blocked && !occluders_block_segment(result.occluders, origin, corner);
			}
			const bool muzzle = points.has_muzzle && !segment_blocked(data_, origin, points.muzzle).blocked
								&& !occluders_block_segment(result.occluders, origin, points.muzzle);
			const auto role = static_cast<size_t>(origins.roles[index]);
			char line[160];
			std::snprintf(line, sizeof(line), "%s%s(%.0f %.0f %.0f): body %s, corners %u/8, muzzle %s", text.empty() ? "" : "; ",
						  role < std::size(roles) ? roles[role] : "?", origin.x, origin.y, origin.z,
						  body == capsule_query_result::blocked	  ? "blocked"
						  : body == capsule_query_result::visible ? "OPEN"
																  : "unproven",
						  open_corners, !points.has_muzzle ? "-" : (muzzle ? "OPEN" : "blocked"));
			text += line;
		}
		META_CONPRINTF("[CS2GLAZ] why:     now, without smoke: %s\n", text.c_str());
	}

	void plugin::print_why(const std::string& filter)
	{
		CGameEntitySystem* system = entity_system();
		const std::shared_ptr<const visibility_result> result = worker_.result();
		if (system == nullptr || engine_ == nullptr || !result)
		{
			META_CONPRINTF("[CS2GLAZ] why: no visibility result yet (%s)\n", disabled_reason_.empty() ? "waiting for the worker" : disabled_reason_.c_str());
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		std::array<std::array<pair_decision, k_max_players>, k_max_players> decisions {};
		std::array<bool, k_max_players> fresh {};
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			decisions = pair_decisions_;
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				fresh[slot] = recipient_decided_at_[slot] != std::chrono::steady_clock::time_point {} && now - recipient_decided_at_[slot] <= k_decision_fresh;
			}
		}
		const runtime_configuration& configuration = settings::current();
		const visibility_tuning tuning {configuration.shoulder_base_units, configuration.shoulder_rtt_scale, configuration.max_shoulder_units};
		const float age_ms = std::chrono::duration<float, std::milli>(now - result->captured).count();
		META_CONPRINTF("[CS2GLAZ] why: result age=%.0fms body=%s filter_full_updates=%d hold=%dms\n", age_ms,
					   compatibility_.bones_available() ? "bones" : "hull (limited mode)",
					   cs2glaz_filter_full_updates_value() ? 1 : 0, configuration.visibility_hold_ms);
		uint32_t shown = 0;
		for (uint32_t me = 0; me < k_max_players; ++me)
		{
			const player_state& player = result->players[me];
			const bool human = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(me))) != nullptr;
			const std::string name = slot_name(system, me);
			if (!player.valid || (filter.empty() ? !human : (name.find(filter) == std::string::npos && filter != std::to_string(me))))
			{
				continue;
			}
			++shown;
			META_CONPRINTF("[CS2GLAZ] why: %s (slot %u%s) at %.0f %.0f %.0f rtt=%.0fms shoulders idle=%.0f moving=%.0f capsules=%u\n", name.c_str(), me,
						   human ? "" : ", bot", player.origin.x, player.origin.y, player.origin.z, player.rtt_seconds * 1000.0f,
						   visibility_shoulder_offset_units(player.rtt_seconds, tuning, false), visibility_shoulder_offset_units(player.rtt_seconds, tuning, true),
						   player.capsule_count);
			for (uint32_t enemy = 0; enemy < k_max_players; ++enemy)
			{
				if (!visibility_pair_enabled(me, enemy, player, result->players[enemy], result->filter_teammates))
				{
					continue;
				}
				META_CONPRINTF("[CS2GLAZ] why:   %s: %.0f units, line of sight %s (%s), you receive him: %s, he receives you: %s\n",
							   slot_name(system, enemy).c_str(), distance_units(player.eye, result->players[enemy].origin),
							   result->visible[me][enemy] ? "VISIBLE" : "blocked", reveal_name(result->reveal[me][enemy], result->held_reveal[me][enemy]).c_str(),
							   fresh[me] ? decision_name(decisions[me][enemy]) : "no data", fresh[enemy] ? decision_name(decisions[enemy][me]) : "no data");
				print_why_probe(player, result->players[enemy], *result);
			}
		}
		if (shown == 0)
		{
			META_CONPRINTF("[CS2GLAZ] why: no living %s matches; use cs2glaz_why <name part or slot>\n", filter.empty() ? "human" : "player");
		}
	}

	CON_COMMAND_F(cs2glaz_why, "Explain the line of sight and transmit decision for every enemy of living humans, or of one player: cs2glaz_why [name or slot]",
				  FCVAR_NONE)
	{
		g_plugin.print_why(args.ArgC() > 1 ? std::string(args.ArgS()) : std::string());
	}

} // namespace cs2glaz
