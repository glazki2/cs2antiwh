#include "plugin.h"

// Turns a fresh visibility result into paired primary/don't-transmit updates
// for verified enemy visual groups. CheckTransmit holds the plugin transmit-state
// lock, skips full updates, allocates nothing, and fails open on uncertain state.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

namespace cs2fow
{
	namespace
	{

		visual_group_key make_current_visual_group_key(const visual_entity_group& group)
		{
			std::array<uint32_t, k_max_hidden_player_entities> values {};
			for (size_t index = 0; index < group.count; ++index)
			{
				values[index] = static_cast<uint32_t>(group.handles[index].ToInt());
			}
			return make_visual_group_key(values, group.count);
		}

		uint8_t hide_reason_mask(hide_reason reason)
		{
			return reason == hide_reason::quarantine ? k_transmit_reason_quarantine : k_transmit_reason_current;
		}

		const char* transmit_reason_name(uint8_t reasons)
		{
			if (reasons == (k_transmit_reason_current | k_transmit_reason_quarantine))
			{
				return "current+quarantine";
			}
			return (reasons & k_transmit_reason_quarantine) != 0 ? "quarantine" : "current";
		}

		void format_recipients(uint64_t mask, char (&text)[256])
		{
			text[0] = '\0';
			size_t used = 0;
			for (int slot = 0; slot < 64; ++slot)
			{
				if ((mask & (uint64_t {1} << slot)) == 0)
				{
					continue;
				}
				const int written = std::snprintf(text + used, sizeof(text) - used, "%s%d", used == 0 ? "" : ",", slot);
				if (written < 0 || static_cast<size_t>(written) >= sizeof(text) - used)
				{
					break;
				}
				used += static_cast<size_t>(written);
			}
			if (used == 0)
			{
				std::snprintf(text, sizeof(text), "-");
			}
		}

	} // namespace

	bool plugin::group_fully_marked(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* bits, const visual_entity_group& group) const
	{
		if (bits == nullptr || group.count == 0)
		{
			return false;
		}
		return hidden_group_all_of(group,
								   [&](CEntityHandle handle)
								   {
									   const int index = resolve_entity_index(system, handle);
									   return valid_networked_edict_index(index) && bits->IsBitSet(index);
								   });
	}

	template<size_t max_count>
	void plugin::record_hidden_entity(CGameEntitySystem* system, size_t member_index, int edict, const hidden_entity_group<CEntityHandle, max_count>& group,
									  int recipient_slot, hide_reason reason, std::chrono::steady_clock::time_point now)
	{
		const CEntityHandle handle = group.handles[member_index];
		char name[k_max_entity_name] {};
		copy_entity_name(system == nullptr ? nullptr : system->GetEntityInstance(handle), name);
		recent_hides_.record({edict, static_cast<uint32_t>(handle.ToInt()), static_cast<uint32_t>(group.source.ToInt()), recipient_slot,
							  hide_reason_mask(reason), now},
							 name);
	}

	template<size_t max_count>
	void plugin::withhold_group(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* dont_transmit,
								const hidden_entity_group<CEntityHandle, max_count>& group, int recipient_slot, hide_reason reason,
								std::chrono::steady_clock::time_point now)
	{
		if (primary == nullptr || dont_transmit == nullptr)
		{
			return;
		}
		const bool debug = settings::current().debug;
		for (size_t entity = 0; entity < group.count; ++entity)
		{
			const CEntityHandle handle = group.handles[entity];
			const int index = resolve_entity_index(system, handle);
			if (!valid_networked_edict_index(index))
			{
				continue;
			}
			if (withhold_transmit_bit(primary, dont_transmit, index) && debug)
			{
				record_hidden_entity(system, entity, index, group, recipient_slot, reason, now);
			}
		}
	}

	void plugin::reset_transmit_state(bool clear_debug_records)
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		for (lifecycle_guard& guard : lifecycle_)
		{
			guard = {};
		}
		for (auto& row : pair_guards_)
		{
			for (pair_guard& guard : row)
			{
				guard = {};
			}
		}
		for (auto& row : hidden_groups_)
		{
			for (visual_entity_group& group : row)
			{
				hidden_group_clear(group);
			}
		}
		he_clearance_history_.clear();
		player_bone_cache_.fill({});
		capture_timing_ = {};
		bone_timing_ = {};
		transmit_timing_ = {};
		capsule_players_ = 0;
		capsule_failed_players_ = 0;
		if (clear_debug_records)
		{
			recent_hides_.clear();
		}
	}

	bool plugin::checktransmit_layout_plausible(CCheckTransmitInfo** infos, int count) const
	{
		// Structural facts every real recipient list satisfies. A private layout
		// change breaks at least one of them long before it could hide the wrong
		// player: no slot appears twice, the full-update flag is a boolean, and the
		// two entity lists are distinct.
		uint64_t seen = 0;
		for (int i = 0; i < count; ++i)
		{
			const CCheckTransmitInfo* info = infos[i];
			if (info == nullptr)
			{
				continue;
			}
			uint8_t full_update = 0;
			std::memcpy(&full_update, reinterpret_cast<const char*>(info) + compatibility_.transmit_offsets().full_update_offset, sizeof(full_update));
			if (full_update > 1u || (info->m_pTransmitEntity != nullptr && info->m_pTransmitEntity == info->m_pTransmitAlways))
			{
				return false;
			}
			// Out-of-range slots are skipped later (never filtered), as before.
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players))
			{
				continue;
			}
			if ((seen & (uint64_t {1} << slot)) != 0)
			{
				return false;
			}
			seen |= uint64_t {1} << slot;
		}
		return true;
	}

	bool plugin::checktransmit_lists_readable(CCheckTransmitInfo** infos, int count) const
	{
		// Once per map, before anything is read directly: prove with guarded reads
		// that the recipient array, each recipient record, and both entity lists are
		// mapped memory. A CS2 update that changed these layouts fails here instead
		// of faulting the server.
		const auto readable = [](const void* address, size_t size)
		{
			if (address == nullptr)
			{
				return true;
			}
			const auto* first = static_cast<const std::byte*>(address);
			uint32_t word = 0;
			return runtime_compatibility::safe_read(first, &word, sizeof(word))
				   && runtime_compatibility::safe_read(first + size - sizeof(word), &word, sizeof(word));
		};
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = nullptr;
			if (!runtime_compatibility::safe_read(infos + i, &info, sizeof(info)))
			{
				return false;
			}
			if (info == nullptr)
			{
				continue;
			}
			static_assert(offsetof(CCheckTransmitInfo, m_pTransmitEntity) == 0 && offsetof(CCheckTransmitInfo, m_pTransmitAlways) == sizeof(void*));
			CBitVec<MAX_EDICTS>* lists[2] {};
			const size_t record_size = std::max<size_t>(compatibility_.recipient_slot_offset() + sizeof(int),
														compatibility_.transmit_offsets().full_update_offset + sizeof(bool));
			if (!readable(info, record_size) || !runtime_compatibility::safe_read(info, lists, sizeof(lists))
				|| !readable(lists[0], sizeof(CBitVec<MAX_EDICTS>)) || !readable(lists[1], sizeof(CBitVec<MAX_EDICTS>)))
			{
				return false;
			}
		}
		return true;
	}

	bool plugin::checktransmit_recipients_consistent(CCheckTransmitInfo** infos, int count) const
	{
		// A live player always receives their own pawn. If a recipient's list lacks
		// it, the slot or list offsets are wrong, and filtering would hide players
		// from the wrong people; stop before any list is changed.
		for (int i = 0; i < count; ++i)
		{
			const CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || info->m_pTransmitEntity == nullptr || info->m_pTransmitAlways == nullptr
				|| read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players))
			{
				continue;
			}
			const int own_pawn = entity_index(transmit_target_cache_[slot].pawn);
			if (valid_networked_edict_index(own_pawn) && !info->m_pTransmitEntity->IsBitSet(own_pawn))
			{
				return false;
			}
		}
		return true;
	}

	void plugin::hook_check_transmit(CCheckTransmitInfo** infos, int count, CBitVec<MAX_EDICTS>&, CBitVec<MAX_EDICTS>&, const Entity2Networkable_t**,
									 const uint16*, int)
	{
		if (!settings::current().enable || !disabled_reason_.empty() || infos == nullptr || count <= 0 || count > static_cast<int>(k_max_players)
			|| transmit_layout_invalid_.load(std::memory_order_relaxed))
		{
			return;
		}
		if (!transmit_lists_verified_)
		{
			if (!checktransmit_lists_readable(infos, count))
			{
				transmit_layout_invalid_.store(true);
				return;
			}
			transmit_lists_verified_ = true;
		}
		if (!checktransmit_layout_plausible(infos, count))
		{
			transmit_layout_invalid_.store(true);
			return;
		}
		const auto timing_started = std::chrono::steady_clock::now();
		const auto record_timing = [&]
		{ transmit_timing_.record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - timing_started).count()); };
		const std::shared_ptr<const visibility_result> result = worker_.result();
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || !read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players))
			{
				continue;
			}
			for (visual_entity_group& group : hidden_groups_[slot])
			{
				hidden_group_clear(group);
			}
		}
		if (!result || !visibility_snapshot_fresh(result->captured, now))
		{
			record_timing();
			return;
		}
		CGameEntitySystem* system = entity_system();
		if (system == nullptr)
		{
			record_timing();
			return;
		}
		const auto current_player_pawn = [&](uint32_t slot, const player_state& saved)
		{
			if (!saved.valid)
			{
				return static_cast<CEntityInstance*>(nullptr);
			}
			live_player live;
			const lifecycle_key key = player_lifecycle(slot, system, &live);
			if (live.pawn == nullptr || !lifecycle_allows_hiding(lifecycle_[slot], now) || lifecycle_changed(lifecycle_[slot].key, key)
				|| key.pawn_entity != saved.pawn_entity || key.team != saved.team)
			{
				return static_cast<CEntityInstance*>(nullptr);
			}
			return live.pawn;
		};
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			target_transmit_cache& cache = transmit_target_cache_[target];
			cache.pawn = current_player_pawn(target, result->players[target]);
			cache.group_valid = cache.pawn != nullptr && collect_player_visual_group(system, cache.pawn, cache.group);
			if (cache.group_valid)
			{
				cache.group_key = make_current_visual_group_key(cache.group);
			}
			cache.attached_valid = cache.pawn != nullptr && collect_attached_entities(system, cache.pawn, cache.group, cache.attached);
		}
		if (!checktransmit_recipients_consistent(infos, count))
		{
			transmit_layout_invalid_.store(true);
			record_timing();
			return;
		}
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr)
			{
				continue;
			}
			// The SDK keeps its old name; current CS2 uses this as the explicit don't-transmit list.
			CBitVec<MAX_EDICTS>* const dont_transmit = info->m_pTransmitAlways;
			if (info->m_pTransmitEntity == nullptr || dont_transmit == nullptr)
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players)
				|| read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset) || !result->players[slot].valid
				|| !visibility_snapshot_fresh(result->captured, now))
			{
				continue;
			}
			const player_state& recipient = result->players[slot];
			if (transmit_target_cache_[slot].pawn == nullptr)
			{
				continue;
			}
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				const player_state& player = result->players[target];
				const target_transmit_cache& cache = transmit_target_cache_[target];
				visual_entity_group& stored_group = hidden_groups_[slot][target];
				if (stored_group.count != 0 && now >= stored_group.quarantine_until)
				{
					hidden_group_clear(stored_group);
				}
				if (!visibility_pair_enabled(static_cast<uint32_t>(slot), target, recipient, player, result->filter_teammates)
					|| cache.pawn == nullptr)
				{
					continue;
				}
				pair_guard& guard = pair_guards_[slot][target];
				const bool full_group_marked = cache.group_valid && group_fully_marked(system, info->m_pTransmitEntity, cache.group);
				if (cache.group_valid)
				{
					update_pair_visual_group(guard, cache.group_key);
				}
				if (result->visible[slot][target])
				{
					if (full_group_marked)
					{
						pair_note_open(guard, result->sequence);
						hidden_group_clear(stored_group);
					}
					continue;
				}
				if (!pair_allows_hiding(guard, result->sequence))
				{
					if (full_group_marked)
					{
						pair_note_open(guard, result->sequence);
						hidden_group_clear(stored_group);
					}
					continue;
				}
				if (!cache.attached_valid)
				{
					// Something is attached that cannot be hidden with the player;
					// withholding the player alone would orphan it on the client.
					hidden_group_clear(stored_group);
					continue;
				}
				if (!cache.group_valid)
				{
					if (hidden_group_quarantined(stored_group, now))
					{
						withhold_group(system, info->m_pTransmitEntity, dont_transmit, stored_group, slot, hide_reason::quarantine, now);
						withhold_group(system, info->m_pTransmitEntity, dont_transmit, cache.attached, slot, hide_reason::quarantine, now);
					}
					continue;
				}
				hidden_group_store(stored_group, cache.group, now, k_hidden_entity_quarantine);
				withhold_group(system, info->m_pTransmitEntity, dont_transmit, cache.group, slot, hide_reason::current, now);
				withhold_group(system, info->m_pTransmitEntity, dont_transmit, cache.attached, slot, hide_reason::current, now);
			}
		}
		record_timing();
	}

	void plugin::print_entities(int edict)
	{
		if (edict >= 0 && !valid_networked_edict_index(edict))
		{
			META_CONPRINTF("[CS2FOW] invalid edict index: %d\n", edict);
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		std::array<const recent_hide_log::record_type*, k_max_recent_hide_records> matches {};
		size_t count = 0;
		for (const recent_hide_log::record_type& record : recent_hides_.records())
		{
			if (!record.valid || (edict >= 0 && record.edict != edict))
			{
				continue;
			}
			matches[count++] = &record;
		}
		std::sort(matches.begin(), matches.begin() + count, [](const recent_hide_log::record_type* left, const recent_hide_log::record_type* right)
				  { return left->last_seen > right->last_seen; });
		META_CONPRINTF("[CS2FOW] entity debug recording=%s records=%zu filter=%s\n", settings::current().debug ? "on" : "off", count,
					   edict < 0 ? "all" : "edict");
		for (size_t index = 0; index < count; ++index)
		{
			const recent_hide_log::record_type& record = *matches[index];
			char recipients[256] {};
			format_recipients(record.recipients, recipients);
			const double first_age_ms = std::chrono::duration<double, std::milli>(now - record.first_seen).count();
			const double last_age_ms = std::chrono::duration<double, std::milli>(now - record.last_seen).count();
			META_CONPRINTF(
				"[CS2FOW] entity %d class=%s handle=0x%x source=0x%x recipients=%s reasons=%s clears=%llu first_age=%.0fms last_age=%.0fms\n",
				record.edict, record.name.data(), record.handle, record.source, recipients, transmit_reason_name(record.reasons),
				static_cast<unsigned long long>(record.clears), first_age_ms, last_age_ms);
		}
		if (count == 0)
		{
			META_CONPRINTF("[CS2FOW] no actual transmit clears recorded%s\n", edict < 0 ? "" : " for that edict");
		}
	}

	void plugin::clear_entity_records()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		recent_hides_.clear();
		META_CONPRINTF("[CS2FOW] entity debug records cleared\n");
	}

} // namespace cs2fow
