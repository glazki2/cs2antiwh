#include "plugin.h"

// The transmit journal (transmit_journal.h) on the plugin side. CheckTransmit
// notes every entity CS2GLAZ clears for a recipient; at the end of the pass
// each recipient's lists are compared with his previous snapshot, so the
// journal holds only what changed for his client because of CS2GLAZ: taken
// from it, or sent to it again. A client that crashes with "CopyExistingEntity:
// missing client entity N" times out on the server about 15 seconds later;
// the journal of the last minute is printed then, and cs2glaz_entity N shows
// everything known about N.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cs2glaz
{
	namespace
	{

		constexpr int k_disconnect_timed_out = 29; // NETWORK_DISCONNECT_TIMEDOUT
		constexpr double k_summary_seconds = 60.0;
		constexpr size_t k_summary_lines = 14;
		constexpr size_t k_entity_lines = 24;

		const char* event_text(journal_event_kind kind)
		{
			switch (kind)
			{
				case journal_event_kind::withheld:
					return "withheld";
				case journal_event_kind::sent_again:
					return "sent again";
				case journal_event_kind::full_update:
					return "full update";
			}
			return "?";
		}

		std::string event_detail(const journal_event& event, double now)
		{
			char text[160] {};
			if (event.kind == journal_event_kind::full_update)
			{
				std::snprintf(text, sizeof(text), "full update %.1f s ago (his client rebuilt every entity)", now - event.seconds);
				return text;
			}
			if (event.kind == journal_event_kind::sent_again && event.withheld_ms >= 0.0)
			{
				std::snprintf(text, sizeof(text), "#%u %s serial %u (%s): sent again %.1f s ago after %.0f ms withheld", event.index, event.classname,
							  event.serial, withhold_reason_name(event.reason), now - event.seconds, event.withheld_ms);
				return text;
			}
			std::snprintf(text, sizeof(text), "#%u %s serial %u (%s): %s %.1f s ago", event.index, event.classname, event.serial,
						  withhold_reason_name(event.reason), event_text(event.kind), now - event.seconds);
			return text;
		}

	} // namespace

	double plugin::journal_now() const
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - journal_epoch_).count();
	}

	void plugin::finish_transmit_journal(CCheckTransmitInfo** infos, int count)
	{
		CGameEntitySystem* system = entity_system();
		const double now = journal_now();
		std::array<bool, k_max_players> seen {};
		const auto describe = [&](uint16_t index, uint16_t& serial, char* classname)
		{
			constexpr size_t size = sizeof(journal_event::classname);
			CEntityInstance* entity = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(index)));
			if (entity == nullptr)
			{
				std::snprintf(classname, size, "%s", "(none)");
				return;
			}
			serial = static_cast<uint16_t>(entity_handle(entity).GetSerialNumber());
			const char* name = entity->m_pEntity == nullptr ? nullptr : entity->m_pEntity->GetClassname();
			std::snprintf(classname, size, "%s", name == nullptr || name[0] == '\0' ? "?" : name);
		};
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || info->m_pTransmitEntity == nullptr || info->m_pTransmitAlways == nullptr)
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players))
			{
				continue;
			}
			seen[static_cast<size_t>(slot)] = true;
			recipient_journal& journal = journals_[static_cast<size_t>(slot)];
			const uint64_t xuid = engine_ == nullptr ? 0 : engine_->GetClientXUID(CPlayerSlot(slot));
			if (xuid != journal.xuid || (journal.name[0] == '\0' && journal.count == 0))
			{
				// Another player in this slot: his history starts here.
				journal_reset(journal);
				journal.xuid = xuid;
				std::snprintf(journal.name, sizeof(journal.name), "%s", slot_name(system, static_cast<uint32_t>(slot)).c_str());
			}
			const bool full_update = read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset);
			journal_finish(journal, info->m_pTransmitEntity->Base(), info->m_pTransmitAlways->Base(), full_update, now, describe, quick_resends_);
		}
		// Not in this pass (gone, or not receiving): his next snapshot starts a
		// fresh comparison.
		for (size_t slot = 0; slot < k_max_players; ++slot)
		{
			if (!seen[slot] && journals_[slot].primed)
			{
				journal_unprime(journals_[slot]);
			}
		}
	}

	void plugin::print_journal_summary(uint32_t slot, double from_seconds, double to_seconds) const
	{
		const recipient_journal& journal = journals_[slot];
		const double now = journal_now();
		// The newest event of each index, and how many quick re-sends it had.
		struct line
		{
			const journal_event* event;
			uint32_t quick;
		};
		std::vector<line> lines;
		size_t full_updates = 0;
		for (size_t newest = 0; newest < journal.count; ++newest)
		{
			const journal_event& event = journal_event_at(journal, newest);
			if (event.seconds < from_seconds || event.seconds > to_seconds)
			{
				continue;
			}
			if (event.kind == journal_event_kind::full_update)
			{
				if (full_updates++ == 0 && lines.size() < k_summary_lines)
				{
					lines.push_back({&event, 0});
				}
				continue;
			}
			auto found = std::find_if(lines.begin(), lines.end(), [&](const line& item)
									  { return item.event->kind != journal_event_kind::full_update && item.event->index == event.index; });
			const bool quick = event.kind == journal_event_kind::sent_again && event.withheld_ms >= 0.0 && event.withheld_ms < k_quick_resend_ms;
			if (found != lines.end())
			{
				found->quick += quick ? 1u : 0u;
			}
			else if (lines.size() < k_summary_lines)
			{
				lines.push_back({&event, quick ? 1u : 0u});
			}
		}
		META_CONPRINTF("[CS2GLAZ] journal of \"%s\" %llu: what CS2GLAZ took from his client or sent to it again, %.0f s, newest first (%zu events kept)\n",
					   journal.name, static_cast<unsigned long long>(journal.xuid), to_seconds - from_seconds, journal.count);
		if (lines.empty())
		{
			META_CONPRINTF("[CS2GLAZ]   nothing: CS2GLAZ changed nothing in his snapshots then\n");
			return;
		}
		for (const line& item : lines)
		{
			if (item.quick != 0)
			{
				META_CONPRINTF("[CS2GLAZ]   %s; sent again within %.0f ms %u times\n", event_detail(*item.event, now).c_str(), k_quick_resend_ms, item.quick);
			}
			else
			{
				META_CONPRINTF("[CS2GLAZ]   %s\n", event_detail(*item.event, now).c_str());
			}
		}
	}

	void plugin::journal_disconnect(IGameEvent* event)
	{
		if (event == nullptr || event->GetInt(game_event_key("reason"), -1) != k_disconnect_timed_out)
		{
			return;
		}
		const int slot = event->GetPlayerSlot(game_event_key("userid")).Get();
		if (slot < 0 || slot >= static_cast<int>(k_max_players))
		{
			return;
		}
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		const recipient_journal& journal = journals_[static_cast<size_t>(slot)];
		if (journal.count == 0)
		{
			return;
		}
		const double now = journal_now();
		META_CONPRINTF("[CS2GLAZ] \"%s\" timed out. A game that crashed looks like this to the server; if his game showed \"CopyExistingEntity: "
					   "missing client entity N\", look for #N below or run cs2glaz_entity N before the map changes.\n",
					   journal.name);
		print_journal_summary(static_cast<uint32_t>(slot), now - k_summary_seconds, now);
	}

	void print_quick_resends(const char* scope, const quick_resend_counts& counts)
	{
		META_CONPRINTF("[CS2GLAZ] %s entities sent again within %.0f ms of being withheld: players=%llu items=%llu decoys=%llu other=%llu\n", scope,
					   k_quick_resend_ms, static_cast<unsigned long long>(counts.players), static_cast<unsigned long long>(counts.items),
					   static_cast<unsigned long long>(counts.decoys), static_cast<unsigned long long>(counts.other));
	}

	void plugin::entity_command(const CCommand& args)
	{
		if (args.ArgC() < 2)
		{
			META_CONPRINTF("[CS2GLAZ] usage: cs2glaz_entity <index> - what entity N is and what CS2GLAZ did with it for each player (the number in "
						   "\"CopyExistingEntity: missing client entity N\")\n");
			return;
		}
		char* end = nullptr;
		const long value = std::strtol(args.Arg(1), &end, 10);
		if (end == args.Arg(1) || *end != '\0' || !valid_networked_edict_index(static_cast<int>(value)))
		{
			META_CONPRINTF("[CS2GLAZ] cs2glaz_entity: the index must be a networked entity number, 1 to %d\n", MAX_EDICTS - 1);
			return;
		}
		const int index = static_cast<int>(value);
		CGameEntitySystem* system = entity_system();
		CEntityInstance* entity = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(index));
		if (entity == nullptr)
		{
			META_CONPRINTF("[CS2GLAZ] entity %d: none now\n", index);
		}
		else
		{
			const CEntityHandle handle = entity_handle(entity);
			const char* name = entity->m_pEntity == nullptr ? nullptr : entity->m_pEntity->GetClassname();
			std::string role;
			for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
			{
				for (const decoy_slot& slot : decoys_[viewer])
				{
					if (slot.id != 0 && slot.spawned && slot.handle == handle)
					{
						role = std::string(slot.control ? "a control decoy of \"" : "a decoy of \"") + slot_name(system, viewer) + "\"";
					}
				}
			}
			if (std::any_of(decoy_pool_.begin(), decoy_pool_.end(), [&](const parked_decoy& parked) { return parked.handle == handle; }))
			{
				role = "a parked decoy";
			}
			if (std::find(decoy_graveyard_.begin(), decoy_graveyard_.end(), handle) != decoy_graveyard_.end())
			{
				role = "a removed decoy";
			}
			{
				std::lock_guard<std::mutex> lock(transmit_state_mutex_);
				for (uint32_t target = 0; target < k_max_players && role.empty(); ++target)
				{
					const target_transmit_cache& cache = transmit_target_cache_[target];
					if (cache.group_valid && hidden_group_contains(cache.group, handle))
					{
						role = (cache.group.source == handle ? "the pawn of \"" : "an item of \"") + slot_name(system, target) + "\"";
					}
					else if (cache.attached_valid && hidden_group_contains(cache.attached, handle))
					{
						role = "attached to \"" + slot_name(system, target) + "\"";
					}
				}
			}
			META_CONPRINTF("[CS2GLAZ] entity %d: %s serial %d%s%s\n", index, name == nullptr ? "?" : name, handle.GetSerialNumber(),
						   role.empty() ? "" : ", ", role.c_str());
		}
		struct found_event
		{
			const journal_event* event;
			uint32_t slot;
		};
		std::vector<found_event> found;
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		const double now = journal_now();
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			const recipient_journal& journal = journals_[slot];
			for (size_t newest = 0; newest < journal.count; ++newest)
			{
				const journal_event& event = journal_event_at(journal, newest);
				if (event.kind != journal_event_kind::full_update && event.index == index)
				{
					found.push_back({&event, slot});
				}
			}
		}
		std::sort(found.begin(), found.end(), [](const found_event& left, const found_event& right) { return left.event->seconds > right.event->seconds; });
		if (found.empty())
		{
			META_CONPRINTF("[CS2GLAZ] journal: CS2GLAZ has not taken entity %d from any player's client, or not recently (the journal keeps the last %zu "
						   "changes per player, this map)\n",
						   index, k_journal_events);
			return;
		}
		META_CONPRINTF("[CS2GLAZ] journal for entity %d, newest first:\n", index);
		for (size_t line = 0; line < found.size() && line < k_entity_lines; ++line)
		{
			const recipient_journal& journal = journals_[found[line].slot];
			META_CONPRINTF("[CS2GLAZ]   \"%s\": %s\n", journal.name, event_detail(*found[line].event, now).c_str());
		}
	}

	CON_COMMAND_F(cs2glaz_entity, "What entity N is and what CS2GLAZ withheld or sent again of it, per player: cs2glaz_entity <index>", FCVAR_NONE)
	{
		g_plugin.entity_command(args);
	}

} // namespace cs2glaz
