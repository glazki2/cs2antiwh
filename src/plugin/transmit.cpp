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

		// Selects how a hidden entity is withheld (see transmit_mode). Not part of
		// cs2glaz.cfg; it resets to 2 on restart. On CS2 1.41.8 the second list
		// sends its entities to the client, so mode 0 (the CE behaviour) leaks.
		CConVar<int> cs2glaz_transmit_mode("cs2glaz_transmit_mode", FCVAR_NONE,
										  "0 clear+mark second list (leaks on CS2 1.41.8), 1 clear only, 2 clear both lists (default), "
										  "3 observe only, 4 clear both plus lists +16/+24 proven by cs2glaz_probe, 5 clear both plus union lists",
										  2, true, 0, true, 5);

		// Diagnostic only: withholds every enemy even in plain view, and drops every
		// enemy from the radar, to show what still reaches a cheat through other
		// channels. It breaks normal play, is not saved in cs2glaz.cfg, and resets
		// to 0 on restart. Spawn/death safety windows and full updates still apply.
		CConVar<bool> cs2glaz_hide_all_enemies("cs2glaz_hide_all_enemies", FCVAR_NONE,
											   "Diagnostic: hide every enemy even in plain view (breaks normal play; resets on restart)", false);

		// Clears one entity bit in a list the probe proved to be an entity bit
		// list, through guarded memory access so a wrong guess cannot fault.
		void clear_extended_bit(void* list_pointer, int index)
		{
			auto* word = static_cast<uint32_t*>(list_pointer) + (index >> 5);
			uint32_t value = 0;
			const uint32_t mask = uint32_t {1} << (index & 31);
			if (list_pointer != nullptr && runtime_compatibility::safe_read(word, &value, sizeof(value)) && (value & mask) != 0)
			{
				value &= ~mask;
				runtime_compatibility::safe_write(word, &value, sizeof(value));
			}
		}

		const char* const k_transmit_probe_names[k_transmit_probe_lists] = {"+0", "+8", "+16", "+24", "unionA", "unionB"};

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
	void plugin::withhold_group(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* second_list,
								const hidden_entity_group<CEntityHandle, max_count>& group, int recipient_slot, hide_reason reason,
								std::chrono::steady_clock::time_point now, transmit_mode mode, const std::array<void*, 2>& extended_lists)
	{
		if (primary == nullptr || second_list == nullptr)
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
			if (apply_transmit_mode(primary, second_list, index, mode) && debug)
			{
				record_hidden_entity(system, entity, index, group, recipient_slot, reason, now);
			}
			if (mode == transmit_mode::clear_union)
			{
				transmit_withheld_.Set(index);
			}
			if (mode == transmit_mode::clear_extended)
			{
				for (void* list_pointer : extended_lists)
				{
					clear_extended_bit(list_pointer, index);
				}
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
		transmit_decisions_ = {};
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

	void plugin::sample_transmit_probe(CGameEntitySystem* system, const CCheckTransmitInfo* info, int slot, const visibility_result& result,
									   const CBitVec<MAX_EDICTS>& union_a, const CBitVec<MAX_EDICTS>& union_b)
	{
		const auto guarded_bit = [](const void* list_pointer, int index, bool& readable)
		{
			const auto* word = static_cast<const uint32_t*>(list_pointer) + (index >> 5);
			uint32_t value = 0;
			readable = list_pointer != nullptr && runtime_compatibility::safe_read(word, &value, sizeof(value));
			return readable && (value & (uint32_t {1} << (index & 31))) != 0;
		};
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			const target_transmit_cache& cache = transmit_target_cache_[target];
			if (cache.pawn == nullptr
				|| !visibility_pair_enabled(static_cast<uint32_t>(slot), target, result.players[slot], result.players[target], result.filter_teammates))
			{
				continue;
			}
			const int index = entity_index(cache.pawn);
			if (!valid_networked_edict_index(index))
			{
				continue;
			}
			const size_t kind = result.visible[slot][target] ? 1u : 0u;
			++transmit_probe_.samples[kind];
			std::array<bool, k_transmit_probe_lists> bits {};
			bits[0] = info->m_pTransmitEntity->IsBitSet(index);
			bits[1] = info->m_pTransmitAlways->IsBitSet(index);
			for (size_t list = 2; list < 4; ++list)
			{
				const void* list_pointer = nullptr;
				std::memcpy(&list_pointer, reinterpret_cast<const char*>(info) + list * sizeof(void*), sizeof(list_pointer));
				bool readable = false;
				bits[list] = guarded_bit(list_pointer, index, readable);
				if (!readable)
				{
					++transmit_probe_.unreadable[list - 2];
				}
			}
			bits[4] = union_a.IsBitSet(index);
			bits[5] = union_b.IsBitSet(index);
			for (size_t list = 0; list < bits.size(); ++list)
			{
				transmit_probe_.set[kind][list] += bits[list] ? 1u : 0u;
			}
			// A member the game keeps in +8 would still be sent if only +0 were
			// cleared, arriving on the client without its player.
			const auto sample_members = [&](const auto& group, bool valid)
			{
				for (size_t member = 0; valid && member < group.count; ++member)
				{
					const int member_index = resolve_entity_index(system, group.handles[member]);
					if (member_index == index || !valid_networked_edict_index(member_index))
					{
						continue;
					}
					++transmit_probe_.members[kind];
					transmit_probe_.members_set[kind][0] += info->m_pTransmitEntity->IsBitSet(member_index) ? 1u : 0u;
					transmit_probe_.members_set[kind][1] += info->m_pTransmitAlways->IsBitSet(member_index) ? 1u : 0u;
				}
			};
			sample_members(cache.group, cache.group_valid);
			sample_members(cache.attached, cache.attached_valid);
		}
	}

	void plugin::request_transmit_dump()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		transmit_dump_pending_ = true;
		META_CONPRINTF("[CS2GLAZ] the next snapshot with a live player will be scanned (mode %d)\n", cs2glaz_transmit_mode.Get());
	}

	void plugin::dump_transmit_lists(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count, const visibility_result& result,
									 const CBitVec<MAX_EDICTS>& union_a, const CBitVec<MAX_EDICTS>& union_b)
	{
		// Scans every pointer-sized field of one live recipient's record for a
		// readable 16384-bit list and reports which pawns each list holds. It only
		// reads, through guarded memory access, and runs once per request.
		for (int i = 0; i < count; ++i)
		{
			const CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players) || transmit_target_cache_[slot].pawn == nullptr)
			{
				continue;
			}
			std::array<int, k_max_players> hidden {};
			std::array<int, k_max_players> visible {};
			size_t hidden_count = 0;
			size_t visible_count = 0;
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				const target_transmit_cache& cache = transmit_target_cache_[target];
				const int index = entity_index(cache.pawn);
				if (cache.pawn == nullptr || !valid_networked_edict_index(index)
					|| !visibility_pair_enabled(static_cast<uint32_t>(slot), target, result.players[slot], result.players[target],
												result.filter_teammates))
				{
					continue;
				}
				(result.visible[slot][target] ? visible[visible_count++] : hidden[hidden_count++]) = index;
			}
			const int own = entity_index(transmit_target_cache_[slot].pawn);
			META_CONPRINTF("[CS2GLAZ] dump: records=%d slot=%d own_pawn=%d enemies behind walls=%zu visible=%zu\n", count, slot, own,
						   hidden_count, visible_count);
			std::array<uint32_t, MAX_EDICTS / 32> words {};
			const auto describe = [&](const char* label, const uint32_t* list)
			{
				const auto bit = [&](int index)
				{ return valid_networked_edict_index(index) && (list[index >> 5] & (uint32_t {1} << (index & 31))) != 0; };
				uint32_t total = 0;
				for (size_t word = 0; word < words.size(); ++word)
				{
					total += static_cast<uint32_t>(std::popcount(list[word]));
				}
				size_t hidden_set = 0;
				size_t visible_set = 0;
				for (size_t index = 0; index < hidden_count; ++index)
				{
					hidden_set += bit(hidden[index]) ? 1u : 0u;
				}
				for (size_t index = 0; index < visible_count; ++index)
				{
					visible_set += bit(visible[index]) ? 1u : 0u;
				}
				char sample[160] {};
				size_t used = 0;
				if (total <= 16)
				{
					for (int index = 0; index < MAX_EDICTS && used + 40 < sizeof(sample); ++index)
					{
						if (!bit(index))
						{
							continue;
						}
						char name[k_max_entity_name] {};
						copy_entity_name(system->GetEntityInstance(CEntityIndex(index)), name);
						const int written = std::snprintf(sample + used, sizeof(sample) - used, " %d:%.24s", index, name);
						used += written > 0 ? static_cast<size_t>(written) : 0u;
					}
				}
				META_CONPRINTF("[CS2GLAZ]   %s bits=%u own=%d behind_walls=%zu/%zu visible=%zu/%zu%s\n", label, total, bit(own) ? 1 : 0,
							   hidden_set, hidden_count, visible_set, visible_count, sample);
			};
			for (uint32_t offset = 0; offset + sizeof(void*) <= compatibility_.recipient_slot_offset(); offset += sizeof(void*))
			{
				uintptr_t value = 0;
				std::memcpy(&value, reinterpret_cast<const char*>(info) + offset, sizeof(value));
				if (value < 0x10000u || value % alignof(uint32_t) != 0
					|| !runtime_compatibility::safe_read(reinterpret_cast<const void*>(value), words.data(), sizeof(words)))
				{
					continue;
				}
				char label[32] {};
				std::snprintf(label, sizeof(label), "+%u", offset);
				describe(label, words.data());
			}
			describe("unionA", union_a.Base());
			describe("unionB", union_b.Base());
			return;
		}
		META_CONPRINTF("[CS2GLAZ] dump: no live recipient in this snapshot\n");
	}

	bool hide_all_enemies_requested()
	{
		return cs2glaz_hide_all_enemies.Get();
	}

	void print_transmit_decisions(const char* scope, const transmit_decision_stats& stats)
	{
		const auto value = [](uint64_t count) { return static_cast<unsigned long long>(count); };
		META_CONPRINTF("[CS2GLAZ] %s recipient snapshots: filtered=%llu full_update=%llu recipient_spawning_or_dying=%llu\n", scope,
					   value(stats.filtered_snapshots), value(stats.full_update_snapshots), value(stats.changing_recipient_snapshots));
		META_CONPRINTF("[CS2GLAZ] %s enemy pairs: hidden=%llu in_view=%llu shown_because enemy_spawning_or_dying=%llu baseline=%llu "
					   "attachment=%llu weapons_unlisted=%llu\n",
					   scope, value(stats.hidden), value(stats.in_view), value(stats.changing_target), value(stats.baseline),
					   value(stats.attachment), value(stats.group));
	}

	void plugin::start_transmit_probe()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		const std::array<bool, 2> allowed = transmit_probe_.extended_allowed;
		transmit_probe_ = {};
		transmit_probe_.extended_allowed = allowed;
		transmit_probe_.calls_left = k_transmit_probe_calls;
		META_CONPRINTF("[CS2GLAZ] transmit probe started for %u snapshots (mode %d); run cs2glaz_probe again in ~10 seconds\n",
					   k_transmit_probe_calls, cs2glaz_transmit_mode.Get());
	}

	void plugin::print_transmit_probe() const
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		const transmit_probe_stats& probe = transmit_probe_;
		META_CONPRINTF("[CS2GLAZ] transmit probe: mode=%d snapshots=%llu full_updates=%llu %s\n", cs2glaz_transmit_mode.Get(),
					   static_cast<unsigned long long>(probe.calls), static_cast<unsigned long long>(probe.full_updates),
					   probe.calls_left != 0 ? "(still running)" : "(finished)");
		const char* const kinds[2] = {"behind walls", "visible"};
		for (size_t kind = 0; kind < 2; ++kind)
		{
			char line[256] {};
			int used = std::snprintf(line, sizeof(line), "[CS2GLAZ] enemy pawns %s: samples=%llu", kinds[kind],
									 static_cast<unsigned long long>(probe.samples[kind]));
			for (size_t list = 0; list < k_transmit_probe_lists && used > 0 && static_cast<size_t>(used) < sizeof(line); ++list)
			{
				used += std::snprintf(line + used, sizeof(line) - static_cast<size_t>(used), " %s=%llu", k_transmit_probe_names[list],
									  static_cast<unsigned long long>(probe.set[kind][list]));
			}
			META_CONPRINTF("%s\n", line);
			META_CONPRINTF("[CS2GLAZ] their weapons/attachments %s: samples=%llu +0=%llu +8=%llu\n", kinds[kind],
						   static_cast<unsigned long long>(probe.members[kind]), static_cast<unsigned long long>(probe.members_set[kind][0]),
						   static_cast<unsigned long long>(probe.members_set[kind][1]));
		}
		META_CONPRINTF("[CS2GLAZ] unreadable lists: +16=%llu +24=%llu; mode 4 may clear: +16=%s +24=%s\n",
					   static_cast<unsigned long long>(probe.unreadable[0]), static_cast<unsigned long long>(probe.unreadable[1]),
					   probe.extended_allowed[0] ? "yes" : "no", probe.extended_allowed[1] ? "yes" : "no");
		print_transmit_decisions("probe", probe.decisions);
	}

	void plugin::hook_check_transmit(CCheckTransmitInfo** infos, int count, CBitVec<MAX_EDICTS>& union_a, CBitVec<MAX_EDICTS>& union_b,
									 const Entity2Networkable_t**, const uint16*, int)
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
		const bool probing = transmit_probe_.calls_left != 0;
		if (probing)
		{
			++transmit_probe_.calls;
			if (--transmit_probe_.calls_left == 0)
			{
				// A list earns writes only if it carried nearly every hidden pawn and
				// was always readable.
				for (size_t list = 0; list < 2; ++list)
				{
					const uint64_t hidden = transmit_probe_.samples[0];
					transmit_probe_.extended_allowed[list] = hidden >= 64 && transmit_probe_.unreadable[list] == 0
															 && transmit_probe_.set[0][list + 2] * 10 >= hidden * 9;
				}
			}
		}
		const auto note = [&](uint64_t transmit_decision_stats::*reason)
		{
			++(transmit_decisions_.*reason);
			if (probing)
			{
				++(transmit_probe_.decisions.*reason);
			}
		};
		const transmit_mode mode = static_cast<transmit_mode>(cs2glaz_transmit_mode.Get());
		const bool hide_all = hide_all_enemies_requested();
		if (mode == transmit_mode::clear_union)
		{
			transmit_withheld_.ClearAll();
		}
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || !read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				continue;
			}
			if (probing)
			{
				++transmit_probe_.full_updates;
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
		if (transmit_dump_pending_)
		{
			transmit_dump_pending_ = false;
			dump_transmit_lists(system, infos, count, *result, union_a, union_b);
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
			// with the primary list, never set, except in the legacy mode 0.
			CBitVec<MAX_EDICTS>* const second_list = info->m_pTransmitAlways;
			if (info->m_pTransmitEntity == nullptr || second_list == nullptr)
			{
				continue;
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			if (slot < 0 || slot >= static_cast<int>(k_max_players) || !result->players[slot].valid
				|| !visibility_snapshot_fresh(result->captured, now))
			{
				continue;
			}
			if (read_checktransmit_full_update(info, compatibility_.transmit_offsets().full_update_offset))
			{
				note(&transmit_decision_stats::full_update_snapshots);
				continue;
			}
			const player_state& recipient = result->players[slot];
			if (transmit_target_cache_[slot].pawn == nullptr)
			{
				note(&transmit_decision_stats::changing_recipient_snapshots);
				continue;
			}
			note(&transmit_decision_stats::filtered_snapshots);
			if (probing)
			{
				sample_transmit_probe(system, info, slot, *result, union_a, union_b);
			}
			std::array<void*, 2> extended_lists {};
			if (mode == transmit_mode::clear_extended)
			{
				for (size_t list = 0; list < extended_lists.size(); ++list)
				{
					if (transmit_probe_.extended_allowed[list])
					{
						std::memcpy(&extended_lists[list], reinterpret_cast<const char*>(info) + (list + 2) * sizeof(void*),
									sizeof(void*));
					}
				}
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
				if (!visibility_pair_enabled(static_cast<uint32_t>(slot), target, recipient, player, result->filter_teammates))
				{
					continue;
				}
				if (cache.pawn == nullptr)
				{
					note(&transmit_decision_stats::changing_target);
					continue;
				}
				pair_guard& guard = pair_guards_[slot][target];
				const bool full_group_marked = cache.group_valid && group_fully_marked(system, info->m_pTransmitEntity, cache.group);
				if (cache.group_valid)
				{
					update_pair_visual_group(guard, cache.group_key);
				}
				if (result->visible[slot][target] && !hide_all)
				{
					note(&transmit_decision_stats::in_view);
					if (full_group_marked)
					{
						pair_note_open(guard, result->sequence);
						hidden_group_clear(stored_group);
					}
					continue;
				}
				if (!pair_allows_hiding(guard, result->sequence, mode == transmit_mode::clear_and_mark))
				{
					note(&transmit_decision_stats::baseline);
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
					hidden_group_clear(stored_group);
					continue;
				}
				if (!cache.group_valid)
				{
					note(&transmit_decision_stats::group);
					if (hidden_group_quarantined(stored_group, now))
					{
						withhold_group(system, info->m_pTransmitEntity, second_list, stored_group, slot, hide_reason::quarantine, now, mode,
									   extended_lists);
						withhold_group(system, info->m_pTransmitEntity, second_list, cache.attached, slot, hide_reason::quarantine, now, mode,
									   extended_lists);
					}
					continue;
				}
				note(&transmit_decision_stats::hidden);
				hidden_group_store(stored_group, cache.group, now, k_hidden_entity_quarantine);
				withhold_group(system, info->m_pTransmitEntity, second_list, cache.group, slot, hide_reason::current, now, mode, extended_lists);
				withhold_group(system, info->m_pTransmitEntity, second_list, cache.attached, slot, hide_reason::current, now, mode,
							   extended_lists);
			}
		}
		if (mode == transmit_mode::clear_union)
		{
			// An entity no recipient keeps after filtering (full updates and
			// SourceTV keep theirs) leaves both union lists too, in case this CS2
			// build re-adds union members to every recipient after CheckTransmit.
			const uint32_t* words = transmit_withheld_.Base();
			for (int word = 0; word < transmit_withheld_.GetNumDWords(); ++word)
			{
				for (uint32_t bits = words[word]; bits != 0; bits &= bits - 1u)
				{
					const int index = word * 32 + std::countr_zero(bits);
					bool kept = false;
					for (int i = 0; i < count && !kept; ++i)
					{
						const CCheckTransmitInfo* info = infos[i];
						kept = info != nullptr && info->m_pTransmitEntity != nullptr && info->m_pTransmitEntity->IsBitSet(index);
					}
					if (!kept)
					{
						union_a.Clear(index);
						union_b.Clear(index);
					}
				}
			}
		}
		record_timing();
	}

	void plugin::print_entities(int edict)
	{
		if (edict >= 0 && !valid_networked_edict_index(edict))
		{
			META_CONPRINTF("[CS2GLAZ] invalid edict index: %d\n", edict);
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
		META_CONPRINTF("[CS2GLAZ] entity debug recording=%s records=%zu filter=%s\n", settings::current().debug ? "on" : "off", count,
					   edict < 0 ? "all" : "edict");
		for (size_t index = 0; index < count; ++index)
		{
			const recent_hide_log::record_type& record = *matches[index];
			char recipients[256] {};
			format_recipients(record.recipients, recipients);
			const double first_age_ms = std::chrono::duration<double, std::milli>(now - record.first_seen).count();
			const double last_age_ms = std::chrono::duration<double, std::milli>(now - record.last_seen).count();
			META_CONPRINTF(
				"[CS2GLAZ] entity %d class=%s handle=0x%x source=0x%x recipients=%s reasons=%s clears=%llu first_age=%.0fms last_age=%.0fms\n",
				record.edict, record.name.data(), record.handle, record.source, recipients, transmit_reason_name(record.reasons),
				static_cast<unsigned long long>(record.clears), first_age_ms, last_age_ms);
		}
		if (count == 0)
		{
			META_CONPRINTF("[CS2GLAZ] no actual transmit clears recorded%s\n", edict < 0 ? "" : " for that edict");
		}
	}

	void plugin::clear_entity_records()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		recent_hides_.clear();
		META_CONPRINTF("[CS2GLAZ] entity debug records cleared\n");
	}

} // namespace cs2glaz
