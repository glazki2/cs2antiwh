#include "plugin.h"
#include "rtti_check.h"

// Test HUD for checking walls with a second player: every living human sees,
// four times a second, what CheckTransmit last decided for each enemy in both
// directions (does the enemy receive you, do you receive the enemy). It shows
// real transmit decisions, not a separate estimate, and is off by default.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <cstring>
#include <string>

namespace cs2glaz
{
	namespace
	{

		CConVar<bool> cs2glaz_wallcheck("cs2glaz_wallcheck", FCVAR_NONE,
										"Test HUD: show each living player whether every enemy receives them and whether they receive every enemy "
										"(resets on restart)",
										false);

		constexpr auto k_wallcheck_interval = std::chrono::milliseconds(250);
		constexpr auto k_decision_fresh = std::chrono::milliseconds(500);
		constexpr uint32_t k_wallcheck_max_lines = 6;
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

	} // namespace

	void plugin::update_wallcheck_hud(CGameEntitySystem* system)
	{
		if (!cs2glaz_wallcheck.Get() || system == nullptr || engine_ == nullptr)
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now < wallcheck_next_)
		{
			return;
		}
		wallcheck_next_ = now + k_wallcheck_interval;
		const std::shared_ptr<const visibility_result> result = worker_.result();
		if (!result)
		{
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
		const auto player_name = [&](uint32_t slot)
		{
			std::string name = "игрок " + std::to_string(slot);
			CEntityInstance* controller = system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
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
		};
		for (uint32_t me = 0; me < k_max_players; ++me)
		{
			// Bots have no network channel and never see a HUD.
			if (!result->players[me].valid || engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(me))) == nullptr)
			{
				continue;
			}
			std::string text = "cs2glaz wallcheck";
			uint32_t lines = 0;
			for (uint32_t enemy = 0; enemy < k_max_players && lines < k_wallcheck_max_lines; ++enemy)
			{
				if (!visibility_pair_enabled(me, enemy, result->players[me], result->players[enemy], result->filter_teammates))
				{
					continue;
				}
				text += "\n" + player_name(enemy) + ": ";
				if (fresh[enemy])
				{
					text += "тебя ему ";
					text += decision_text(decisions[enemy][me]);
					text += " | ";
				}
				text += "его тебе ";
				text += fresh[me] ? decision_text(decisions[me][enemy]) : "нет данных";
				++lines;
			}
			if (lines == 0)
			{
				text += "\nживых врагов нет";
			}
			send_center_text(me, text);
		}
	}

	bool plugin::send_center_text(uint32_t slot, const std::string& text)
	{
		if (text_message_broken_ || game_event_system_ == nullptr || network_messages_ == nullptr || slot >= 64)
		{
			return false;
		}
		if (text_message_ == nullptr)
		{
			text_message_ = network_messages_->FindNetworkMessagePartial("TextMsg");
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
		reflection->SetUInt32(proto, destination, k_hud_print_center);
		reflection->AddString(proto, parameters, text);
		const uint64 recipients = uint64 {1} << slot;
		game_event_system_->PostEventAbstract(CSplitScreenSlot(-1), false, 1, &recipients, text_message_, data, 0, BUF_RELIABLE);
		delete data;
		return true;
	}

} // namespace cs2glaz
