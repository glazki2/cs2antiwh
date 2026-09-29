#include "plugin.h"
#include "rtti_check.h"

// Test HUD for checking walls with a second player: every human sees, four times
// a second in the centre (or in chat when it changes), whether each enemy is
// behind a wall by the worker's line of sight and what CheckTransmit last
// decided in both directions (do you receive the enemy, does the enemy receive
// you). The transmit part is the real decision, not a separate estimate. Off by
// default.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_wallcheck("cs2glaz_wallcheck", FCVAR_NONE,
									   "Test HUD: 0 off, 1 centre of the screen, 2 chat (only when something changes); shows each living player "
									   "whether every enemy is behind a wall and whether each side receives the other (resets on restart)",
									   0, true, 0, true, 2);

		constexpr auto k_wallcheck_interval = std::chrono::milliseconds(250);
		constexpr auto k_wallcheck_chat_interval = std::chrono::seconds(1);
		constexpr auto k_decision_fresh = std::chrono::milliseconds(500);
		constexpr uint32_t k_wallcheck_max_lines = 6;
		constexpr uint32_t k_hud_print_talk = 3;
		constexpr uint32_t k_hud_print_center = 4;
		constexpr std::string_view k_text_message_name = "CUserMessageTextMsg";

		const char* decision_text(pair_decision decision)
		{
			switch (decision)
			{
				case pair_decision::hidden:
					return "СКРЫТ";
				case pair_decision::in_view:
					return "ВИДЕН";
				case pair_decision::changing:
				case pair_decision::recipient_changing:
					return "ВИДЕН (появление/смерть)";
				case pair_decision::attachment:
					return "ВИДЕН (вложения)";
				case pair_decision::group:
					return "ВИДЕН (оружие не собрано)";
				case pair_decision::baseline:
					return "ВИДЕН (режим 0)";
				case pair_decision::full_update:
					return "ВИДЕН (полное обновление)";
				case pair_decision::none:
					break;
			}
			return "нет данных";
		}

		// Why the worker called a pair visible: the test, then the viewing origin.
		std::string reveal_text(uint8_t code)
		{
			const char* test = "?";
			switch (visibility_reveal_test(code))
			{
				case visibility_reveal::hold:
					return "удержание";
				case visibility_reveal::body:
					test = "тело";
					break;
				case visibility_reveal::corner:
					test = "угол рамки";
					break;
				case visibility_reveal::muzzle:
					test = "ствол";
					break;
				case visibility_reveal::uncertain:
					test = "не доказано";
					break;
				case visibility_reveal::none:
				case visibility_reveal::count:
					return "";
			}
			constexpr const char* origins[] = {"глаза", "плечо Л", "плечо П", "над головой", "ноги", "шаг"};
			const uint8_t origin = visibility_reveal_origin(code);
			return origin < std::size(origins) ? std::string(test) + ", " + origins[origin] : std::string(test);
		}

		// Console (English) names for cs2glaz_why and the reveal log.
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

		std::string reveal_name(uint8_t code)
		{
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

	void plugin::update_wallcheck_hud(CGameEntitySystem* system)
	{
		const int mode = cs2glaz_wallcheck.Get();
		if (mode <= 0)
		{
			wallcheck_state_ = "off (cs2glaz_wallcheck 0)";
			return;
		}
		if (system == nullptr || engine_ == nullptr)
		{
			wallcheck_state_ = "no entity system or engine";
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now < wallcheck_next_)
		{
			return;
		}
		wallcheck_next_ = now + k_wallcheck_interval;
		if (text_message_broken_)
		{
			wallcheck_state_ = "off: the TextMsg user message is missing or looks different";
			return;
		}
		const std::shared_ptr<const visibility_result> result = worker_.result();
		if (!result)
		{
			wallcheck_state_ = "waiting for the first visibility result";
			return;
		}
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
		const auto player_name = [&](uint32_t slot) { return slot_name(system, slot); };
		// Log each moment a human starts seeing an enemy, with what revealed him,
		// so a leak seen in game can be matched to a line in the server console.
		for (uint32_t me = 0; me < k_max_players; ++me)
		{
			const bool human = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(me))) != nullptr;
			for (uint32_t enemy = 0; enemy < k_max_players; ++enemy)
			{
				const bool seen = human && visibility_pair_enabled(me, enemy, result->players[me], result->players[enemy], result->filter_teammates)
								  && result->visible[me][enemy];
				if (seen && !wallcheck_seen_[me].test(enemy))
				{
					META_CONPRINTF("[CS2GLAZ] wallcheck: %s now sees %s (%s, %.0f units)\n", player_name(me).c_str(), player_name(enemy).c_str(),
								   reveal_name(result->reveal[me][enemy]).c_str(), distance_units(result->players[me].eye, result->players[enemy].origin));
				}
				wallcheck_seen_[me].set(enemy, seen);
			}
		}
		const bool hide_all = hide_all_enemies_requested();
		uint32_t recipients = 0;
		for (uint32_t me = 0; me < k_max_players; ++me)
		{
			// Bots have no network channel and never see a HUD.
			if (engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(me))) == nullptr)
			{
				continue;
			}
			std::vector<std::string> lines;
			lines.emplace_back(hide_all ? "cs2glaz wallcheck [скрыть всех: все враги СКРЫТ]" : "cs2glaz wallcheck");
			if (!result->players[me].valid)
			{
				lines.emplace_back("ты не в игре (мёртв или наблюдаешь)");
			}
			for (uint32_t enemy = 0; enemy < k_max_players && lines.size() <= k_wallcheck_max_lines && result->players[me].valid; ++enemy)
			{
				if (!visibility_pair_enabled(me, enemy, result->players[me], result->players[enemy], result->filter_teammates))
				{
					continue;
				}
				// Line of sight from the worker (walls and smoke) is shown on its
				// own, so it stays readable when hide-all withholds everyone.
				std::string line = player_name(enemy) + ": ";
				line += result->visible[me][enemy] ? "НА ВИДУ (" + reveal_text(result->reveal[me][enemy]) + ")" : std::string("ЗА СТЕНОЙ");
				line += " | тебе ";
				line += fresh[me] ? decision_text(decisions[me][enemy]) : "нет данных";
				if (fresh[enemy])
				{
					line += " | ему ";
					line += decision_text(decisions[enemy][me]);
				}
				lines.push_back(std::move(line));
			}
			if (lines.size() == 1)
			{
				lines.emplace_back("живых врагов нет");
			}
			bool sent = false;
			if (mode == 1)
			{
				std::string text;
				for (const std::string& line : lines)
				{
					text += text.empty() ? line : "\n" + line;
				}
				sent = send_text(me, k_hud_print_center, text);
			}
			else
			{
				// Chat scrolls, so it gets only what changed, at most once a second.
				std::string joined;
				for (const std::string& line : lines)
				{
					joined += line + "\n";
				}
				if (joined == wallcheck_chat_last_[me] || now < wallcheck_chat_next_[me])
				{
					++recipients;
					continue;
				}
				wallcheck_chat_last_[me] = joined;
				wallcheck_chat_next_[me] = now + k_wallcheck_chat_interval;
				sent = true;
				for (const std::string& line : lines)
				{
					sent = send_text(me, k_hud_print_talk, " " + line) && sent;
				}
			}
			if (sent)
			{
				++recipients;
			}
		}
		if (recipients != 0 && wallcheck_last_recipients_ == 0)
		{
			META_CONPRINTF("[CS2GLAZ] wallcheck HUD is being sent to %u player(s)\n", recipients);
		}
		wallcheck_last_recipients_ = recipients;
		wallcheck_state_ = text_message_broken_ ? "off: the TextMsg user message is missing or looks different"
						   : recipients != 0	? "sending"
												: "no human players connected";
	}

	void plugin::print_wallcheck_status() const
	{
		META_CONPRINTF("[CS2GLAZ] wallcheck mode=%d state=%s players=%u messages_sent=%llu text_message=%s\n", cs2glaz_wallcheck.Get(),
					   wallcheck_state_, wallcheck_last_recipients_, static_cast<unsigned long long>(wallcheck_sent_),
					   text_message_broken_ ? "broken" : text_message_ != nullptr ? "found" : "not looked up yet");
		if (!disabled_reason_.empty())
		{
			META_CONPRINTF("[CS2GLAZ] wallcheck does not run while CS2GLAZ is disabled: %s\n", disabled_reason_.c_str());
		}
		else if (!settings::current().enable)
		{
			META_CONPRINTF("[CS2GLAZ] wallcheck does not run while cs2glaz_enable is 0\n");
		}
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
		META_CONPRINTF("[CS2GLAZ] why: result age=%.0fms body=%s hide_all=%d filter_full_updates=%d hold=%dms\n", age_ms,
					   compatibility_.bones_available() ? "bones" : "hull (limited mode)", hide_all_enemies_requested() ? 1 : 0,
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
							   result->visible[me][enemy] ? "VISIBLE" : "blocked", reveal_name(result->reveal[me][enemy]).c_str(),
							   fresh[me] ? decision_name(decisions[me][enemy]) : "no data", fresh[enemy] ? decision_name(decisions[enemy][me]) : "no data");
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

	CON_COMMAND_F(cs2glaz_wallcheck_status, "Show why the cs2glaz_wallcheck test HUD is or is not shown", FCVAR_NONE)
	{
		g_plugin.print_wallcheck_status();
	}

	bool plugin::send_text(uint32_t slot, uint32_t hud_destination, const std::string& text)
	{
		if (text_message_broken_ || game_event_system_ == nullptr || network_messages_ == nullptr || slot >= 64)
		{
			return false;
		}
		if (text_message_ == nullptr)
		{
			text_message_ = network_messages_->FindNetworkMessage(k_text_message_name.data());
			if (text_message_ == nullptr)
			{
				text_message_ = network_messages_->FindNetworkMessagePartial("TextMsg");
			}
			if (text_message_ == nullptr)
			{
				text_message_broken_ = true;
				META_CONPRINTF("[CS2GLAZ] wallcheck HUD off: the TextMsg user message was not found\n");
				return false;
			}
		}
		CNetMessage* data = text_message_->AllocateMessage();
		if (data == nullptr)
		{
			return false;
		}
		google::protobuf::Message* const proto = data->ToPB<google::protobuf::Message>();
		const bool verified = rtti_names_class_at_offset(data, proto, k_text_message_name, [](const void* address, void* output, size_t size)
														 { return runtime_compatibility::safe_read(address, output, size); });
		const google::protobuf::Descriptor* descriptor = verified ? proto->GetDescriptor() : nullptr;
		const google::protobuf::Reflection* reflection = descriptor == nullptr ? nullptr : proto->GetReflection();
		const google::protobuf::FieldDescriptor* destination = reflection == nullptr ? nullptr : descriptor->FindFieldByName("dest");
		const google::protobuf::FieldDescriptor* parameters = reflection == nullptr ? nullptr : descriptor->FindFieldByName("param");
		if (destination == nullptr || parameters == nullptr || destination->is_repeated()
			|| destination->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_UINT32 || !parameters->is_repeated()
			|| parameters->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING)
		{
			text_message_broken_ = true;
			META_CONPRINTF("[CS2GLAZ] wallcheck HUD off: the TextMsg user message does not have the expected layout\n");
			delete data;
			return false;
		}
		reflection->SetUInt32(proto, destination, hud_destination);
		reflection->AddString(proto, parameters, text);
		const uint64 recipients = uint64 {1} << slot;
		game_event_system_->PostEventAbstract(CSplitScreenSlot(-1), false, 1, &recipients, text_message_, data, 0, BUF_RELIABLE);
		delete data;
		++wallcheck_sent_;
		return true;
	}

} // namespace cs2glaz
