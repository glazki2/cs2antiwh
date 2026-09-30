#include "plugin.h"

// Turns a fresh visibility result into primary/second-list clears for verified
// enemy visual groups. CheckTransmit holds the plugin transmit-state lock, skips
// full updates, allocates nothing, and fails open on uncertain state.

#include <algorithm>
#include <bit>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

namespace cs2glaz
{
	namespace
	{

		// Dead players who may only watch their own team (mp_forcecamera 1 or 2)
		// get only the enemies a living teammate sees and whoever they watch; a
		// dead cheater used to receive every enemy and could call them out.
		CConVar<bool> cs2glaz_filter_dead("cs2glaz_filter_dead", FCVAR_NONE,
										  "Dead players get only the enemies their living team sees (needs mp_forcecamera 1 or 2; resets on restart)",
										  true);

		// A full update is a snapshot the client rebuilds its entities from; what it
		// lacks the client simply does not create, as with ordinary PVS culling.
		// A client can force one (starting a demo recording, or a forged message
		// from a cheat), so sending every enemy in it handed a wallhack everyone on
		// demand. 0 restores the old unfiltered full updates.
		CConVar<bool> cs2glaz_filter_full_updates("cs2glaz_filter_full_updates", FCVAR_NONE,
												  "Filter enemies in client full updates too (0 sends everyone in them, the old behaviour)", true);

		visual_group_key make_current_visual_group_key(const visual_entity_group& group)
		{
			std::array<uint32_t, k_max_hidden_player_entities> values {};
			for (size_t index = 0; index < group.count; ++index)
			{
				values[index] = static_cast<uint32_t>(group.handles[index].ToInt());
			}
			return make_visual_group_key(values, group.count);
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

	// An enemy who dies while hidden from a recipient stays hidden from him
	// (with what hangs on his body) until he respawns or the pawn is gone. The
	// recipient could not see him die, so nothing he could see goes missing;
	// weapons dropped on the ground are separate entities and are not held.
	void plugin::withhold_dead_hidden(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count)
	{
		if (system == nullptr)
		{
			return;
		}
		const auto dead_pawn = [&](CEntityHandle handle) -> CEntityInstance*
		{
			CEntityInstance* pawn = handle.IsValid() ? system->GetEntityInstance(handle) : nullptr;
			if (pawn == nullptr)
			{
				return nullptr;
			}
			uint8_t life_state = k_life_alive;
			std::memcpy(&life_state, reinterpret_cast<const std::byte*>(pawn) + compatibility_.fields().life_state, sizeof(life_state));
			return life_state == k_life_alive ? nullptr : pawn;
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
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				CEntityHandle& kept = dead_hidden_pawns_[slot][target];
				const visual_entity_group& stored = hidden_groups_[slot][target];
				CEntityInstance* pawn = kept.IsValid() ? dead_pawn(kept) : nullptr;
				// The previous decision for the pair (this tick's comes later) says
				// whether he was hidden from this recipient when he died.
				if (pawn == nullptr && pair_decisions_[slot][target] == pair_decision::hidden && stored.count != 0
					&& (pawn = dead_pawn(stored.source)) != nullptr)
				{
					kept = stored.source;
				}
				if (pawn == nullptr)
				{
					kept = CEntityHandle();
					continue;
				}
				++transmit_decisions_.dead_hidden;
				visual_entity_group group;
				attached_entity_group attached;
				if (collect_player_visual_group(system, pawn, group))
				{
					withhold_group(system, info->m_pTransmitEntity, info->m_pTransmitAlways, group);
					if (collect_attached_entities(system, pawn, group, attached))
					{
						withhold_group(system, info->m_pTransmitEntity, info->m_pTransmitAlways, attached);
					}
				}
				else
				{
					const int index = resolve_entity_index(system, kept);
					if (valid_networked_edict_index(index))
					{
						apply_transmit_mode(info->m_pTransmitEntity, info->m_pTransmitAlways, index, transmit_mode::clear_both);
					}
				}
			}
		}
	}

	// Withholds every entity of a group from one recipient: clears it in both
	// lists (on CS2 1.41.8 the second one also sends what it holds).
	template<size_t max_count>
	void plugin::withhold_group(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* second_list,
								const hidden_entity_group<CEntityHandle, max_count>& group)
	{
		if (primary == nullptr || second_list == nullptr)
		{
			return;
		}
		for (size_t entity = 0; entity < group.count; ++entity)
		{
			const int index = resolve_entity_index(system, group.handles[entity]);
			if (valid_networked_edict_index(index))
			{
				apply_transmit_mode(primary, second_list, index, transmit_mode::clear_both);
			}
		}
	}

	void plugin::reset_transmit_state()
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
		for (auto& row : dead_hidden_pawns_)
		{
			row.fill(CEntityHandle());
		}
		he_clearance_history_.clear();
		// A verified smoke layout belongs to the server binary and stays; a failed
		// check is retried on the next map.
		if (smoke_layout_state_ == smoke_layout_state::failed)
		{
			smoke_layout_state_ = smoke_layout_state::unchecked;
		}
		smoke_seen_.fill({});
		smoke_layout_failures_ = 0;
		he_tracked_count_ = 0;
		player_bone_cache_.fill({});
		capture_timing_ = {};
		bone_timing_ = {};
		transmit_timing_ = {};
		transmit_decisions_ = {};
		for (auto& row : pair_decisions_)
		{
			row.fill(pair_decision::none);
		}
		recipient_decided_at_.fill({});
		capsule_players_ = 0;
		capsule_failed_players_ = 0;
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

	bool cs2glaz_filter_full_updates_value()
	{
		return cs2glaz_filter_full_updates.Get();
	}

	bool filter_dead_players_requested()
	{
		return cs2glaz_filter_dead.Get();
	}

	void print_transmit_decisions(const char* scope, const transmit_decision_stats& stats)
	{
		const auto value = [](uint64_t count) { return static_cast<unsigned long long>(count); };
		META_CONPRINTF("[CS2GLAZ] %s recipient snapshots: filtered=%llu dead_viewers=%llu full_update=%llu (filtered %llu) "
					   "recipient_spawning_or_dying=%llu\n",
					   scope, value(stats.filtered_snapshots), value(stats.dead_viewer_snapshots), value(stats.full_update_snapshots),
					   value(stats.full_update_filtered), value(stats.changing_recipient_snapshots));
		META_CONPRINTF("[CS2GLAZ] %s enemy pairs: hidden=%llu in_view=%llu shown_because enemy_spawning_or_dying=%llu baseline=%llu "
					   "attachment=%llu weapons_unlisted=%llu dead_kept_hidden=%llu\n",
					   scope, value(stats.hidden), value(stats.in_view), value(stats.changing_target), value(stats.baseline),
					   value(stats.attachment), value(stats.group), value(stats.dead_hidden));
	}

	void plugin::hook_check_transmit(CCheckTransmitInfo** infos, int count, CBitVec<MAX_EDICTS>& /*union_a*/, CBitVec<MAX_EDICTS>& /*union_b*/,
									 const Entity2Networkable_t**, const uint16*, int)
	{
		// Decoys are withheld even while filtering is off, until the game thread
		// removes them.
		const bool filtering = settings::current().enable && disabled_reason_.empty();
		if ((!filtering && !decoys_live_.load()) || infos == nullptr || count <= 0 || count > static_cast<int>(k_max_players)
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
		withhold_decoys(entity_system(), infos, count, result.get(), now);
		if (!filtering)
		{
			record_timing();
			return;
		}
		const auto note = [&](uint64_t transmit_decision_stats::*reason) { ++(transmit_decisions_.*reason); };
		const bool filter_full_updates = cs2glaz_filter_full_updates.Get();
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || !read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				continue;
			}
			// A filtered full update leaves hidden entities off the client, so the
			// stored groups stay true; an unfiltered one sends them all.
			if (filter_full_updates)
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
		CGameEntitySystem* system = entity_system();
		withhold_dead_hidden(system, infos, count);
		if (!result || !visibility_snapshot_fresh(result->captured, now) || system == nullptr)
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
			// CS2 1.41.8 sends what is set here (a probe never saw an enemy pawn in
			// it, and setting hidden pawns there leaked them live), so it is cleared
			// with the primary list, never set.
			CBitVec<MAX_EDICTS>* const second_list = info->m_pTransmitAlways;
			if (info->m_pTransmitEntity == nullptr || second_list == nullptr)
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players) || !visibility_snapshot_fresh(result->captured, now))
			{
				continue;
			}
			// A living recipient uses his own sight; a dead one watching his team
			// uses the team's sight.
			const bool alive_viewer = result->players[slot].valid;
			std::array<bool, k_max_players> team_sight {};
			const bool dead_viewer = !alive_viewer && dead_viewer_sight(*result, static_cast<uint32_t>(slot), team_sight);
			if (!alive_viewer && !dead_viewer)
			{
				continue;
			}
			if (read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				note(&transmit_decision_stats::full_update_snapshots);
				if (!filter_full_updates)
				{
					recipient_decided_at_[slot] = now;
					pair_decisions_[slot].fill(pair_decision::full_update);
					continue;
				}
				note(&transmit_decision_stats::full_update_filtered);
			}
			const player_state& recipient = result->players[slot];
			if (alive_viewer && transmit_target_cache_[slot].pawn == nullptr)
			{
				note(&transmit_decision_stats::changing_recipient_snapshots);
				recipient_decided_at_[slot] = now;
				pair_decisions_[slot].fill(pair_decision::recipient_changing);
				continue;
			}
			note(dead_viewer ? &transmit_decision_stats::dead_viewer_snapshots : &transmit_decision_stats::filtered_snapshots);
			recipient_decided_at_[slot] = now;
			std::array<pair_decision, k_max_players>& decisions = pair_decisions_[slot];
			decisions.fill(pair_decision::none);
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				const player_state& player = result->players[target];
				const target_transmit_cache& cache = transmit_target_cache_[target];
				visual_entity_group& stored_group = hidden_groups_[slot][target];
				if (stored_group.count != 0 && now >= stored_group.quarantine_until)
				{
					hidden_group_clear(stored_group);
				}
				const bool enemy = dead_viewer ? player.valid && player.team != result->dead_viewer_team[slot]
											   : visibility_pair_enabled(static_cast<uint32_t>(slot), target, recipient, player, result->filter_teammates);
				if (!enemy)
				{
					continue;
				}
				if (cache.pawn == nullptr)
				{
					note(&transmit_decision_stats::changing_target);
					decisions[target] = pair_decision::changing;
					continue;
				}
				pair_guard& guard = pair_guards_[slot][target];
				const bool full_group_marked = cache.group_valid && group_fully_marked(system, info->m_pTransmitEntity, cache.group);
				if (cache.group_valid)
				{
					update_pair_visual_group(guard, cache.group_key);
				}
				if ((dead_viewer ? team_sight[target] : result->visible[slot][target]))
				{
					note(&transmit_decision_stats::in_view);
					decisions[target] = pair_decision::in_view;
					if (full_group_marked)
					{
						pair_note_open(guard, result->sequence);
						hidden_group_clear(stored_group);
					}
					continue;
				}
				if (!pair_allows_hiding(guard, result->sequence, false))
				{
					note(&transmit_decision_stats::baseline);
					decisions[target] = pair_decision::baseline;
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
					note(&transmit_decision_stats::attachment);
					decisions[target] = pair_decision::attachment;
					hidden_group_clear(stored_group);
					continue;
				}
				if (!cache.group_valid)
				{
					note(&transmit_decision_stats::group);
					decisions[target] = pair_decision::group;
					if (hidden_group_quarantined(stored_group, now))
					{
						decisions[target] = pair_decision::hidden;
						withhold_group(system, info->m_pTransmitEntity, second_list, stored_group);
						withhold_group(system, info->m_pTransmitEntity, second_list, cache.attached);
					}
					continue;
				}
				note(&transmit_decision_stats::hidden);
				decisions[target] = pair_decision::hidden;
				hidden_group_store(stored_group, cache.group, now, k_hidden_entity_quarantine);
				withhold_group(system, info->m_pTransmitEntity, second_list, cache.group);
				withhold_group(system, info->m_pTransmitEntity, second_list, cache.attached);
			}
		}
		record_timing();
	}

} // namespace cs2glaz
