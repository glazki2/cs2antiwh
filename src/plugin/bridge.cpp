#include "plugin.h"

// The bridge with CSVILKA, a separate anti-cheat plugin (anticheat_bridge.h).
// CS2GLAZ hands it its decoy evidence, so CSVILKA can weigh a wallhack together
// with its own aim, movement and client checks and punish through its own
// rules; CSVILKA hands back the players it detected something on, and CS2GLAZ
// watches them first with decoys. Either plugin works alone; the bridge is
// found through Metamod whenever both are loaded, in any order.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cs2glaz
{
	namespace
	{

		constexpr auto k_partner_search_interval = std::chrono::seconds(10);
		constexpr float k_max_suspect_seconds = 24.0f * 60.0f * 60.0f;
		constexpr float k_default_suspect_minutes = 30.0f;
		constexpr size_t k_max_suspects = 1024;

		bool individual_xuid(uint64_t xuid)
		{
			return (xuid >> 52u) == 0x011u && (xuid & 0xffffffffu) != 0;
		}

		// Printable ASCII only, a few dozen characters: it ends up in the console
		// and the log.
		std::string clean_reason(const char* text)
		{
			std::string reason;
			for (const char* character = text; character != nullptr && *character != '\0' && reason.size() < 48; ++character)
			{
				const auto value = static_cast<unsigned char>(*character);
				reason.push_back(value >= 0x20 && value < 0x7f && value != '"' ? static_cast<char>(value) : '?');
			}
			return reason.empty() ? std::string("unspecified") : reason;
		}

		class bridge final : public anticheat_bridge::cs2glaz
		{
		public:
			void mark_suspect(std::uint64_t steam_id64, float seconds, const char* reason) override
			{
				g_plugin.mark_suspect(steam_id64, seconds, reason, "CSVILKA");
			}

			bool filtering_active() override
			{
				return g_plugin.filtering_active_now();
			}

			bool get_decoy_evidence(std::uint64_t steam_id64, anticheat_bridge::decoy_evidence& evidence) override
			{
				return g_plugin.decoy_evidence_for(steam_id64, evidence);
			}
		};

		bridge g_bridge;

	} // namespace

	void* plugin::OnMetamodQuery(const char* iface, int* ret)
	{
		if (iface != nullptr && std::strcmp(iface, CS2GLAZ_BRIDGE_INTERFACE) == 0)
		{
			if (ret != nullptr)
			{
				*ret = META_IFACE_OK;
			}
			return static_cast<anticheat_bridge::cs2glaz*>(&g_bridge);
		}
		if (ret != nullptr)
		{
			*ret = META_IFACE_FAILED;
		}
		return nullptr;
	}

	void plugin::OnPluginLoad(PluginId)
	{
		// A plugin that just loaded may be CSVILKA: look again on the next need.
		csvilka_next_search_ = {};
	}

	void plugin::OnPluginUnload(PluginId id)
	{
		// Metamod reports an unload after it happened, on this thread, before any
		// later call could reach the partner: forget it here.
		if (csvilka_ != nullptr && id == csvilka_id_)
		{
			csvilka_ = nullptr;
			csvilka_id_ = 0;
			csvilka_next_search_ = {};
			META_CONPRINTF("[CS2GLAZ] CSVILKA unloaded; decoy evidence stays in CS2GLAZ only\n");
		}
	}

	anticheat_bridge::csvilka* plugin::csvilka_bridge(bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (csvilka_ != nullptr || api_ == nullptr || (!force && now < csvilka_next_search_))
		{
			return csvilka_;
		}
		csvilka_next_search_ = now + k_partner_search_interval;
		int status = META_IFACE_FAILED;
		PluginId id = 0;
		void* found = api_->MetaFactory(CSVILKA_BRIDGE_INTERFACE, &status, &id);
		if (found != nullptr && status == META_IFACE_OK)
		{
			csvilka_ = static_cast<anticheat_bridge::csvilka*>(found);
			csvilka_id_ = id;
			META_CONPRINTF("[CS2GLAZ] connected to CSVILKA (plugin %d): decoy evidence goes to its confirmation rules, its detections mark "
						   "players for decoys\n",
						   static_cast<int>(id));
		}
		return csvilka_;
	}

	bool plugin::filtering_active_now() const
	{
		return settings::current().enable && disabled_reason_.empty();
	}

	bool plugin::decoy_evidence_for(uint64_t xuid, anticheat_bridge::decoy_evidence& evidence) const
	{
		const auto found = decoy_players_.find(xuid);
		if (found == decoy_players_.end())
		{
			return false;
		}
		const decoy_player_record& record = found->second;
		const decoy_exposure exposure = record.exposure();
		const decoy_exposure front = record.front_exposure();
		const decoy_exposure crosshair = record.crosshair_exposure();
		// Front decoys (front_decoys.cpp) and crosshair ghosts count with the
		// others: their own controls and expectation, summed.
		evidence = {record.aims + record.jumps + record.front_jumps, // a followed jump is an aim that followed
					record.shots + record.front_shots + record.crosshair_shots,
					record.control_aims + record.control_shots + record.control_jumps + record.front_control_jumps + record.front_control_shots
						+ record.crosshair_control_shots,
					exposure.real_seconds + front.real_seconds + crosshair.real_seconds,
					exposure.control_seconds + front.control_seconds + crosshair.control_seconds,
					decoy_expected_reports(exposure, decoy_server_) + decoy_expected_reports(front, front_server_, k_front_prior_rate)
						+ decoy_expected_reports(crosshair, crosshair_server_, k_crosshair_prior_rate),
					decoy_player_evidence(record)};
		return true;
	}

	void plugin::mark_suspect(uint64_t xuid, float seconds, const char* reason, const char* source)
	{
		if (!individual_xuid(xuid) || !std::isfinite(seconds) || seconds <= 0.0f)
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const auto until = now + std::chrono::milliseconds(static_cast<int64_t>(std::min(seconds, k_max_suspect_seconds) * 1000.0f));
		auto found = suspects_.find(xuid);
		if (found == suspects_.end())
		{
			if (suspects_.size() >= k_max_suspects)
			{
				std::erase_if(suspects_, [&](const auto& entry) { return entry.second.expires <= now; });
				if (suspects_.size() >= k_max_suspects)
				{
					return;
				}
			}
			found = suspects_.emplace(xuid, suspect_entry {}).first;
		}
		suspect_entry& entry = found->second;
		const bool fresh = entry.expires <= now;
		entry.expires = std::max(entry.expires, until);
		entry.reason = clean_reason(reason);
		entry.source = clean_reason(source);
		++suspects_marked_;
		if (fresh)
		{
			const double minutes = std::chrono::duration<double>(entry.expires - now).count() / 60.0;
			META_CONPRINTF("[CS2GLAZ] decoys: watching %llu first for %.0f min (%s: %s)\n", static_cast<unsigned long long>(xuid), minutes,
						   entry.source.c_str(), entry.reason.c_str());
		}
	}

	bool plugin::is_suspect(uint32_t slot, std::chrono::steady_clock::time_point now) const
	{
		if (engine_ == nullptr || slot >= k_max_players || suspects_.empty())
		{
			return false;
		}
		const auto found = suspects_.find(engine_->GetClientXUID(CPlayerSlot(static_cast<int>(slot))));
		return found != suspects_.end() && found->second.expires > now;
	}

	void plugin::suspect_command(const CCommand& args)
	{
		if (args.ArgC() < 2)
		{
			csvilka_bridge(true);
			print_bridge_status();
			META_CONPRINTF("[CS2GLAZ] usage: cs2glaz_suspect <steamid64 | slot | name part> [minutes, default %.0f; 0 stops watching]\n",
						   k_default_suspect_minutes);
			return;
		}
		const std::string target = args.Arg(1);
		float minutes = k_default_suspect_minutes;
		if (args.ArgC() > 2)
		{
			char* end = nullptr;
			minutes = std::strtof(args.Arg(2), &end);
			if (end == args.Arg(2) || !std::isfinite(minutes) || minutes < 0.0f)
			{
				META_CONPRINTF("[CS2GLAZ] cs2glaz_suspect: minutes must be a number of 0 or more\n");
				return;
			}
		}
		// A SteamID64, a player slot, or part of a connected human's name.
		uint64_t xuid = 0;
		const bool digits = !target.empty() && std::all_of(target.begin(), target.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
		if (digits && target.size() >= 15)
		{
			xuid = std::strtoull(target.c_str(), nullptr, 10);
		}
		else if (engine_ != nullptr)
		{
			CGameEntitySystem* system = entity_system();
			std::vector<uint32_t> matches;
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				if (!human_player(slot))
				{
					continue;
				}
				if (digits ? std::strtoul(target.c_str(), nullptr, 10) == slot : slot_name(system, slot).find(target) != std::string::npos)
				{
					matches.push_back(slot);
				}
			}
			if (matches.size() != 1)
			{
				META_CONPRINTF("[CS2GLAZ] cs2glaz_suspect: %s\n", matches.empty() ? "no connected human matches" : "more than one player matches");
				return;
			}
			xuid = engine_->GetClientXUID(CPlayerSlot(static_cast<int>(matches.front())));
		}
		if (!individual_xuid(xuid))
		{
			META_CONPRINTF("[CS2GLAZ] cs2glaz_suspect: not a player's SteamID64\n");
			return;
		}
		if (minutes == 0.0f)
		{
			META_CONPRINTF("[CS2GLAZ] decoys: %s %llu\n", suspects_.erase(xuid) != 0 ? "stopped watching" : "was not watching",
						   static_cast<unsigned long long>(xuid));
			return;
		}
		suspects_.erase(xuid); // a new period from now, as asked
		mark_suspect(xuid, minutes * 60.0f, "marked by an administrator", "console");
		if (decoy_mode() == 0)
		{
			META_CONPRINTF("[CS2GLAZ] decoys are off (cs2glaz_decoys 0): turn them on for the watch to matter\n");
		}
	}

	void plugin::print_bridge_status() const
	{
		const auto now = std::chrono::steady_clock::now();
		uint32_t watched = 0;
		for (const auto& [xuid, entry] : suspects_)
		{
			watched += entry.expires > now ? 1u : 0u;
		}
		META_CONPRINTF("[CS2GLAZ] CSVILKA: %s; decoy evidence reports sent=%llu; players watched first=%u (marks received=%llu)\n",
					   csvilka_ != nullptr ? "connected" : "not found (CS2GLAZ works alone)", static_cast<unsigned long long>(csvilka_reports_),
					   watched, static_cast<unsigned long long>(suspects_marked_));
		uint32_t shown = 0;
		for (const auto& [xuid, entry] : suspects_)
		{
			if (entry.expires <= now || shown >= 10)
			{
				continue;
			}
			++shown;
			META_CONPRINTF("[CS2GLAZ] watched: %llu for %.0f more min (%s: %s)\n", static_cast<unsigned long long>(xuid),
						   std::chrono::duration<double>(entry.expires - now).count() / 60.0, entry.source.c_str(), entry.reason.c_str());
		}
	}

	CON_COMMAND_F(cs2glaz_suspect,
				  "Watch a player first with decoys: cs2glaz_suspect <steamid64 | slot | name part> [minutes, 0 stops]; no arguments lists them",
				  FCVAR_NONE)
	{
		g_plugin.suspect_command(args);
	}

} // namespace cs2glaz
