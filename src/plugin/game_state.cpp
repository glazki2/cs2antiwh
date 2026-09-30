#include "plugin.h"
#include "smoke_layout_check.h"

// Reads live controllers, pawns, weapons, bounds, and visual groups on the game
// thread, then outputs plain copied visibility snapshots. Broken handles,
// lifecycle changes, or incomplete groups reset toward fail-open behavior.

#include <inetchannelinfo.h>
#include <mathlib/transform.h>
#include <tier1/utlvector.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace cs2glaz
{
	namespace
	{

		constexpr float k_max_rtt_seconds = 0.5f;
		// How long after death a dead player still gets every enemy: the death
		// and freeze cameras watch the killer. With the observer target known the
		// watched player is always sent, so a short margin is enough.
		constexpr float k_dead_viewer_delay_seconds = 2.0f;
		constexpr float k_dead_viewer_blind_delay_seconds = 6.0f;

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		vec3 to_vec3(const Vector& value)
		{
			return {value.x, value.y, value.z};
		}

	} // namespace

	int entity_index(CEntityInstance* entity)
	{
		return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->m_EHandle.GetEntryIndex() : -1;
	}

	CEntityHandle entity_handle(CEntityInstance* entity)
	{
		return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetRefEHandle() : CEntityHandle {};
	}

	bool valid_networked_edict_index(int index)
	{
		return index > 0 && index < MAX_EDICTS;
	}

	int resolve_entity_index(CGameEntitySystem* system, CEntityHandle handle)
	{
		if (system == nullptr || !handle.IsValid())
		{
			return -1;
		}
		return entity_index(system->GetEntityInstance(handle));
	}

	bool plugin::validate_limited_runtime(std::string& error) const
	{
		// Limited mode trusts the entity-system offset without a verified binary, so
		// prove it points at a live C++ object from a game binary before any use.
		if (game_resource_ == nullptr || compatibility_.entity_system_offset() == 0)
		{
			error = "limited mode: game resource service is unavailable";
			return false;
		}
		void* system_pointer = nullptr;
		const auto* slot = reinterpret_cast<const std::byte*>(game_resource_) + compatibility_.entity_system_offset();
		if (!runtime_compatibility::safe_read(slot, &system_pointer, sizeof(system_pointer)) || system_pointer == nullptr)
		{
			error = "limited mode: entity system pointer is unreadable";
			return false;
		}
		void* system_vtable = nullptr;
		void* service_vtable = nullptr;
		if (!runtime_compatibility::safe_read(system_pointer, &system_vtable, sizeof(system_vtable))
			|| !runtime_compatibility::safe_read(game_resource_, &service_vtable, sizeof(service_vtable))
			|| !(compatibility_.address_in_server_module(system_vtable) || runtime_compatibility::same_module(system_vtable, service_vtable)))
		{
			error = "limited mode: entity system offset does not point at the game entity system";
			return false;
		}
		// Walk entity 0 with guarded reads only, so an entity-list layout that moved
		// in a CS2 update fails this check instead of faulting the server.
		auto* system = static_cast<CGameEntitySystem*>(system_pointer);
		CEntityIdentity* identity = nullptr;
		CEntityInstance* world = nullptr;
		CEntityHandle handle;
		CEntityIdentity* back_pointer = nullptr;
		const char* name_pointer = nullptr;
		static_assert(sizeof(CUtlSymbolLarge) == sizeof(const char*));
		if (!runtime_compatibility::safe_read(&system->m_EntityList.m_pIdentityChunks[0], &identity, sizeof(identity)) || identity == nullptr
			|| !runtime_compatibility::safe_read(&identity->m_pInstance, &world, sizeof(world)) || world == nullptr
			|| !runtime_compatibility::safe_read(&identity->m_EHandle, &handle, sizeof(handle)) || handle.GetEntryIndex() != 0
			|| !runtime_compatibility::safe_read(&world->m_pEntity, &back_pointer, sizeof(back_pointer)) || back_pointer != identity
			|| !runtime_compatibility::safe_read(&identity->m_designerName, &name_pointer, sizeof(name_pointer)) || name_pointer == nullptr)
		{
			error = "limited mode: the entity list does not have the expected layout";
			return false;
		}
		char name[16] {};
		for (size_t index = 0; index + 1 < sizeof(name); ++index)
		{
			if (!runtime_compatibility::safe_read(name_pointer + index, &name[index], 1))
			{
				error = "limited mode: the world entity name is unreadable";
				return false;
			}
			if (name[index] == '\0')
			{
				break;
			}
		}
		// CS2 registers the world as "worldent"; "worldspawn" is its map-file name.
		if (std::strcmp(name, "worldent") != 0 && std::strcmp(name, "worldspawn") != 0)
		{
			error = "limited mode: entity 0 is not the world entity";
			return false;
		}
		return true;
	}

	CGameEntitySystem* plugin::entity_system() const
	{
		if (game_resource_ == nullptr || compatibility_.entity_system_offset() == 0)
		{
			return nullptr;
		}
		return field<CGameEntitySystem*>(game_resource_, compatibility_.entity_system_offset());
	}

	CEntityInstance* plugin::controller(uint32_t slot) const
	{
		CGameEntitySystem* system = entity_system();
		return system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
	}

	CEntityInstance* plugin::pawn(CEntityInstance* controller_entity) const
	{
		if (controller_entity == nullptr)
		{
			return nullptr;
		}
		const CEntityHandle handle = field<CEntityHandle>(controller_entity, compatibility_.fields().player_pawn);
		CGameEntitySystem* system = entity_system();
		return handle.IsValid() && system != nullptr ? system->GetEntityInstance(handle) : nullptr;
	}

	lifecycle_key plugin::player_lifecycle(uint32_t slot, CGameEntitySystem* system, live_player* live) const
	{
		if (live != nullptr)
		{
			*live = {};
		}
		lifecycle_key key;
		CEntityInstance* controller_entity = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
		key.has_controller = controller_entity != nullptr;
		if (controller_entity == nullptr)
		{
			return key;
		}
		key.hltv = field<bool>(controller_entity, compatibility_.fields().is_hltv);
		if (key.hltv)
		{
			return key;
		}
		CEntityInstance* pawn_entity = pawn(controller_entity);
		CEntityInstance* pawn_controller =
			pawn_entity == nullptr ? nullptr : system->GetEntityInstance(field<CEntityHandle>(pawn_entity, compatibility_.fields().pawn_controller));
		key.pawn_entity = entity_index(pawn_entity);
		if (pawn_entity == nullptr || pawn_controller != controller_entity || !valid_networked_edict_index(key.pawn_entity))
		{
			return key;
		}
		key.team = field<uint8_t>(pawn_entity, compatibility_.fields().team);
		key.alive = field<uint8_t>(pawn_entity, compatibility_.fields().life_state) == k_life_alive
					&& field<int32_t>(pawn_entity, compatibility_.fields().health) > 0;
		key.spawning = field<bool>(pawn_entity, compatibility_.fields().is_spawning);
		key.death_flags = field<int32_t>(pawn_entity, compatibility_.fields().death_flags);
		key.has_death_info = field<bool>(pawn_entity, compatibility_.fields().has_death_info);
		key.death_time = field<float>(pawn_entity, compatibility_.fields().death_time);
		key.death_info_time = field<float>(pawn_entity, compatibility_.fields().death_info_time);
		if (live != nullptr && key.alive && !key.spawning && (key.team == k_team_t || key.team == k_team_ct))
		{
			live->pawn = pawn_entity;
			live->pawn_entity = key.pawn_entity;
			live->team = key.team;
		}
		return key;
	}

	void plugin::capture_dead_viewers(CGameEntitySystem* system, const std::array<lifecycle_key, k_max_players>& keys, float game_time,
									  visibility_snapshot& value) const
	{
		value.dead_viewer_target.fill(-1);
		// With mp_forcecamera 0 a dead player may watch enemies, who then need
		// their own teammates sent; only team-restricted spectating is filtered.
		if (!filter_dead_players_requested() || forcecamera_mode() == 0 || !std::isfinite(game_time))
		{
			return;
		}
		const bool observer = compatibility_.observer_available();
		const float delay = observer ? k_dead_viewer_delay_seconds : k_dead_viewer_blind_delay_seconds;
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			const lifecycle_key& key = keys[slot];
			if (!key.has_controller || key.hltv || key.alive || (key.team != k_team_t && key.team != k_team_ct) || value.players[slot].valid)
			{
				continue;
			}
			// The first seconds after death show the killer (death and freeze
			// cameras); without the observer target they stay unfiltered.
			const float since_death = game_time - key.death_time;
			if (!std::isfinite(since_death) || since_death < delay)
			{
				continue;
			}
			int watched = -1;
			if (observer)
			{
				CEntityInstance* controller = system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
				const CEntityHandle observer_handle = controller == nullptr ? CEntityHandle {} : field<CEntityHandle>(controller, compatibility_.fields().observer_pawn);
				CEntityInstance* observer_pawn = observer_handle.IsValid() ? system->GetEntityInstance(observer_handle) : nullptr;
				void* services = observer_pawn == nullptr ? nullptr : field<void*>(observer_pawn, compatibility_.fields().observer_services);
				const CEntityHandle target = services == nullptr ? CEntityHandle {} : field<CEntityHandle>(services, compatibility_.fields().observer_target);
				watched = target.IsValid() ? entity_index(system->GetEntityInstance(target)) : -1;
			}
			value.dead_viewer_team[slot] = key.team;
			value.dead_viewer_target[slot] = watched;
		}
	}

	weapon_muzzle_class plugin::active_weapon_muzzle_class(CGameEntitySystem* system, CEntityInstance* pawn_entity) const
	{
		if (system == nullptr || pawn_entity == nullptr || !compatibility_.weapon_item_available())
		{
			return weapon_muzzle_class::none;
		}
		void* services = field<void*>(pawn_entity, compatibility_.fields().weapon_services);
		if (services == nullptr)
		{
			return weapon_muzzle_class::none;
		}
		const CEntityHandle active_weapon = field<CEntityHandle>(services, compatibility_.fields().active_weapon);
		CEntityInstance* weapon = active_weapon.IsValid() ? system->GetEntityInstance(active_weapon) : nullptr;
		if (weapon == nullptr)
		{
			return weapon_muzzle_class::none;
		}
		void* attribute_manager = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(weapon) + compatibility_.fields().attribute_manager);
		void* item = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(attribute_manager) + compatibility_.fields().item);
		const uint16_t definition = field<uint16_t>(item, compatibility_.fields().item_definition_index);
		return weapon_muzzle_class_from_item_definition(definition);
	}

	void plugin::collect_smoke_entities(CGameEntitySystem* system, float game_time, bool include_candidates,
										std::array<CEntityInstance*, k_max_smoke_volumes>& smokes, size_t& smoke_count, bool& smoke_overflow)
	{
		smoke_count = 0;
		smoke_overflow = false;
		if (system == nullptr)
		{
			return;
		}
		const bool want_smokes = compatibility_.smoke_available() || include_candidates;
		// Limited mode has no HE event listener; follow the HE projectiles instead.
		const bool track_he = !he_event_available_ && compatibility_.smoke_available() && std::isfinite(game_time);
		std::array<tracked_grenade, 32> grenades {};
		uint32_t grenade_count = 0;
		bool grenade_overflow = false;
		CEntityIdentity* identity = system->m_EntityList.m_pFirstActiveEntity;
		for (uint32_t scanned = 0; identity != nullptr && scanned < k_entity_scan_hard_limit; identity = identity->m_pNext, ++scanned)
		{
			CEntityInstance* entity = identity->m_pInstance;
			const int edict = entity_index(entity);
			if (!valid_networked_edict_index(edict))
			{
				continue;
			}
			const char* classname = entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetClassname() : nullptr;
			if (classname == nullptr)
			{
				continue;
			}
			if (want_smokes && std::strcmp(classname, "smokegrenade_projectile") == 0 && field<bool>(entity, compatibility_.fields().did_smoke_effect))
			{
				if (smoke_count < smokes.size())
				{
					smokes[smoke_count++] = entity;
				}
				else
				{
					smoke_overflow = true;
				}
			}
			else if (track_he && std::strcmp(classname, "hegrenade_projectile") == 0)
			{
				void* body_component = field<void*>(entity, compatibility_.fields().body_component);
				void* scene_node = body_component == nullptr ? nullptr : field<void*>(body_component, compatibility_.fields().scene_node);
				if (scene_node == nullptr || grenade_count >= grenades.size())
				{
					grenade_overflow = true;
					continue;
				}
				grenades[grenade_count++] = {static_cast<uint32_t>(entity_handle(entity).ToInt()),
											 to_vec3(field<Vector>(scene_node, compatibility_.fields().abs_origin))};
			}
		}
		if (!track_he || grenade_overflow)
		{
			he_tracked_count_ = 0;
			return;
		}
		for (uint32_t previous = 0; previous < he_tracked_count_; ++previous)
		{
			const bool still_flying = std::any_of(grenades.begin(), grenades.begin() + grenade_count,
												  [&](const tracked_grenade& grenade) { return grenade.handle == he_tracked_[previous].handle; });
			if (!still_flying)
			{
				std::lock_guard<std::mutex> lock(transmit_state_mutex_);
				if (he_clearance_history_.record(he_tracked_[previous].position, game_time))
				{
					++he_tracked_detonations_;
				}
			}
		}
		he_tracked_ = grenades;
		he_tracked_count_ = grenade_count;
	}

	bool plugin::smoke_layout_matches(const CEntityInstance* smoke, uint32_t volume_offset, vec3 detonation, float game_time) const
	{
		const smoke_private_layout& layout = compatibility_.smoke_layout();
		const auto* volume = reinterpret_cast<const std::byte*>(smoke) + volume_offset;
		Vector center;
		smoke_volume_header header;
		if (!runtime_compatibility::safe_read(volume + layout.center, &center, sizeof(center))
			|| !runtime_compatibility::safe_read(volume + layout.start_time, &header.start_time, sizeof(header.start_time))
			|| !runtime_compatibility::safe_read(volume + layout.frame, &header.frame, sizeof(header.frame))
			|| !runtime_compatibility::safe_read(volume + layout.storage, &header.storage, sizeof(header.storage)))
		{
			return false;
		}
		header.center = to_vec3(center);
		if (!smoke_header_plausible(header, detonation, game_time))
		{
			return false;
		}
		std::vector<uint8_t> mask(k_smoke_mask_bytes);
		std::vector<std::byte> density(k_smoke_storage_frame_stride);
		return runtime_compatibility::safe_read(header.storage + k_smoke_storage_mask_offset, mask.data(), mask.size())
			   && runtime_compatibility::safe_read(header.storage + k_smoke_storage_density_offset
													   + static_cast<size_t>(header.frame) * k_smoke_storage_frame_stride,
												   density.data(), density.size())
			   && smoke_voxels_plausible(mask.data(), density.data());
	}

	void plugin::verify_runtime_smoke_layout(const std::array<CEntityInstance*, k_max_smoke_volumes>& smokes, size_t count, float game_time)
	{
		if (smoke_layout_state_ != smoke_layout_state::unchecked || !compatibility_.smoke_layout_candidate() || !std::isfinite(game_time))
		{
			return;
		}
		for (size_t index = 0; index < count && smoke_layout_state_ == smoke_layout_state::unchecked; ++index)
		{
			CEntityInstance* smoke = smokes[index];
			const uint32_t handle = static_cast<uint32_t>(entity_handle(smoke).ToInt());
			smoke_seen* seen = nullptr;
			for (smoke_seen& entry : smoke_seen_)
			{
				if (entry.handle == handle)
				{
					seen = &entry;
					break;
				}
			}
			if (seen == nullptr)
			{
				// Remember when the smoke first appeared; judge it once it has spread.
				auto slot = std::find_if(smoke_seen_.begin(), smoke_seen_.end(), [](const smoke_seen& entry) { return entry.handle == 0; });
				if (slot == smoke_seen_.end())
				{
					slot = std::min_element(smoke_seen_.begin(), smoke_seen_.end(),
											[](const smoke_seen& left, const smoke_seen& right) { return left.first_seen < right.first_seen; });
				}
				*slot = {handle, game_time, false};
				continue;
			}
			if (seen->judged || game_time - seen->first_seen < k_smoke_check_min_age + 0.5f)
			{
				continue;
			}
			seen->judged = true;
			++smoke_layout_judged_;
			const vec3 detonation = to_vec3(field<Vector>(smoke, compatibility_.fields().smoke_detonation_pos));
			const uint32_t base = compatibility_.smoke_layout().volume;
			uint32_t matches = 0;
			uint32_t matched_offset = 0;
			if (smoke_layout_matches(smoke, base, detonation, game_time))
			{
				matches = 1;
				matched_offset = base;
			}
			else
			{
				// A new build may have moved the volume inside the entity; its own
				// layout must still match exactly one nearby offset.
				for (int64_t shift = -1024; shift <= 1024 && matches < 2; shift += 8)
				{
					const int64_t offset = static_cast<int64_t>(base) + shift;
					if (shift == 0 || offset < 64)
					{
						continue;
					}
					if (smoke_layout_matches(smoke, static_cast<uint32_t>(offset), detonation, game_time))
					{
						++matches;
						matched_offset = static_cast<uint32_t>(offset);
					}
				}
			}
			if (matches == 1)
			{
				smoke_layout_shift_ = static_cast<int64_t>(matched_offset) - static_cast<int64_t>(base);
				compatibility_.accept_runtime_smoke_layout(matched_offset);
				smoke_layout_state_ = smoke_layout_state::verified;
				META_CONPRINTF("[CS2GLAZ] smoke layout verified on a live smoke (volume offset %u, %+lld from gamedata); smoke occlusion is on\n",
							   matched_offset, static_cast<long long>(smoke_layout_shift_));
			}
			else if (++smoke_layout_failures_ >= 3)
			{
				smoke_layout_state_ = smoke_layout_state::failed;
				META_CONPRINTF("[CS2GLAZ] smoke layout not recognised on %u live smokes; smoke occlusion stays off for this map\n",
							   smoke_layout_failures_);
			}
		}
	}

	bool plugin::smoke_header_readable(const CEntityInstance* smoke) const
	{
		// Re-checked for every smoke before the direct copy in limited mode: the
		// verified layout must still describe this entity and its storage.
		const smoke_private_layout& layout = compatibility_.smoke_layout();
		const auto* volume = reinterpret_cast<const std::byte*>(smoke) + layout.volume;
		Vector center;
		int32_t frame = -1;
		const std::byte* storage = nullptr;
		uint8_t probe = 0;
		const vec3 detonation = to_vec3(field<Vector>(const_cast<CEntityInstance*>(smoke), compatibility_.fields().smoke_detonation_pos));
		if (!runtime_compatibility::safe_read(volume + layout.center, &center, sizeof(center))
			|| !runtime_compatibility::safe_read(volume + layout.frame, &frame, sizeof(frame))
			|| !runtime_compatibility::safe_read(volume + layout.storage, &storage, sizeof(storage)) || (frame != 0 && frame != 1)
			|| storage == nullptr)
		{
			return false;
		}
		const float dx = center.x - detonation.x;
		const float dy = center.y - detonation.y;
		const float dz = center.z - detonation.z;
		const std::byte* last_cell = storage + k_smoke_storage_density_offset + static_cast<size_t>(frame) * k_smoke_storage_frame_stride
									 + static_cast<size_t>(k_smoke_cell_count - 1) * k_smoke_storage_cell_stride;
		return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz)
			   && dx * dx + dy * dy + dz * dz <= k_smoke_check_center_tolerance * k_smoke_check_center_tolerance
			   && runtime_compatibility::safe_read(storage + k_smoke_storage_mask_offset, &probe, sizeof(probe))
			   && runtime_compatibility::safe_read(last_cell, &probe, sizeof(probe));
	}

	const char* plugin::smoke_layout_summary() const
	{
		if (!compatibility_.limited())
		{
			return compatibility_.smoke_available() ? "smoke" : "smoke off";
		}
		switch (smoke_layout_state_)
		{
			case smoke_layout_state::verified:
				return "smoke verified on a live smoke";
			case smoke_layout_state::failed:
				return "smoke off: layout not recognised";
			case smoke_layout_state::unchecked:
				break;
		}
		return compatibility_.smoke_layout_candidate() ? "smoke checked on the first live smoke" : "smoke off";
	}

	void plugin::print_smoke_layout() const
	{
		const char* state = smoke_layout_state_ == smoke_layout_state::verified ? "verified"
							: smoke_layout_state_ == smoke_layout_state::failed ? "failed"
																				: "unchecked";
		META_CONPRINTF("[CS2GLAZ] smoke layout limited=%d candidate=%d state=%s judged=%u failures=%u volume_offset=%u shift=%lld "
					   "he_tracking=%d he_tracked_detonations=%llu\n",
					   compatibility_.limited() ? 1 : 0, compatibility_.smoke_layout_candidate() ? 1 : 0, state, smoke_layout_judged_,
					   smoke_layout_failures_, compatibility_.smoke_layout().volume, static_cast<long long>(smoke_layout_shift_),
					   !he_event_available_ && compatibility_.smoke_available() ? 1 : 0, static_cast<unsigned long long>(he_tracked_detonations_));
	}

	bool plugin::capture_smokes(const std::array<CEntityInstance*, k_max_smoke_volumes>& entities, size_t count, bool overflow, float game_time,
								visibility_snapshot& value)
	{
		if (overflow || !std::isfinite(game_time))
		{
			return false;
		}
		smoke_snapshot snapshot;
		snapshot.he_clear_radius_units = settings::current().he_clear_radius_units;
		snapshot.he_clear_seconds = settings::current().he_clear_seconds;
		if (snapshot.he_clear_radius_units > 0.0f && snapshot.he_clear_seconds > 0.0f)
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			for (uint32_t index = 0; index < he_clearance_history_.count; ++index)
			{
				const live_he_clearance& clearance = he_clearance_history_.records[index];
				const float age = game_time - clearance.detonation_time;
				if (age >= 0.0f && age < snapshot.he_clear_seconds)
				{
					snapshot.he_clearances[snapshot.he_clearance_count++] = {clearance.center, age, clearance.detonation_time};
				}
			}
		}
		if (count == 0 && snapshot.he_clearance_count == 0)
		{
			value.smokes.reset();
			smoke_cache_.reset();
			return true;
		}
		std::vector<std::pair<const void*, float>> key;
		key.reserve(count);
		for (size_t index = 0; index < count; ++index)
		{
			CEntityInstance* entity = entities[index];
			if (entity == nullptr || (compatibility_.limited() && !smoke_header_readable(entity)))
			{
				return false;
			}
			auto* volume = reinterpret_cast<std::byte*>(entity) + compatibility_.smoke_layout().volume;
			key.emplace_back(entity, field<float>(volume, compatibility_.smoke_layout().start_time));
		}
		const auto now = value.captured;
		if (smoke_cache_ != nullptr && key == smoke_cache_key_ && snapshot.he_clearance_count == smoke_cache_he_count_
			&& now - smoke_cache_copied_ < k_smoke_copy_interval)
		{
			value.smokes = smoke_cache_;
			return true;
		}
		snapshot.copied = now;
		snapshot.volumes.reserve(count);
		for (size_t index = 0; index < count; ++index)
		{
			CEntityInstance* entity = entities[index];
			if (entity == nullptr)
			{
				return false;
			}
			if (compatibility_.limited() && !smoke_header_readable(entity))
			{
				return false;
			}
			auto* volume = reinterpret_cast<std::byte*>(entity) + compatibility_.smoke_layout().volume;
			const vec3 center = to_vec3(field<Vector>(volume, compatibility_.smoke_layout().center));
			const float start_time = field<float>(volume, compatibility_.smoke_layout().start_time);
			const auto* storage = field<std::byte*>(volume, compatibility_.smoke_layout().storage);
			snapshot.volumes.emplace_back();
			if (!copy_stable_smoke_frame(storage, center, game_time - start_time, snapshot.volumes.back(),
										 [&] { return field<int32_t>(volume, compatibility_.smoke_layout().frame); }))
			{
				return false;
			}
			snapshot.volumes.back().start_time = start_time;
		}
		smoke_cache_ = std::make_shared<smoke_snapshot>(std::move(snapshot));
		smoke_cache_copied_ = now;
		smoke_cache_key_ = std::move(key);
		smoke_cache_he_count_ = smoke_cache_->he_clearance_count;
		value.smokes = smoke_cache_;
		return true;
	}

	bool plugin::collect_player_visual_group(CGameEntitySystem* system, CEntityInstance* pawn_entity, visual_entity_group& group) const
	{
		hidden_group_clear(group);
		if (system == nullptr || pawn_entity == nullptr)
		{
			return false;
		}
		void* services = field<void*>(pawn_entity, compatibility_.fields().weapon_services);
		if (services == nullptr)
		{
			return false;
		}
		// A handle whose entity is gone (a thrown grenade, a removed weapon) or was
		// never networked has nothing to withhold, so it is skipped. Treating it as
		// uncertain used to reveal the player for as long as it stayed stale.
		const auto collect_handle = [&](CEntityHandle handle)
		{
			if (!handle.IsValid() || !valid_networked_edict_index(resolve_entity_index(system, handle)))
			{
				return true;
			}
			return hidden_group_append_unique(group, handle);
		};
		const auto collect_vector = [&](void* base, uint32_t offset, int max_count)
		{
			auto* handles = reinterpret_cast<CUtlVector<CEntityHandle>*>(reinterpret_cast<uintptr_t>(base) + offset);
			const int count = handles->Count();
			if (count < 0 || count > max_count)
			{
				return false;
			}
			for (int item = 0; item < count; ++item)
			{
				if (!collect_handle((*handles)[item]))
				{
					return false;
				}
			}
			return true;
		};
		// The pawn itself must resolve. The previous weapon (m_hLastWeapon) is not
		// collected: while it is still owned it is in the weapons list, and once
		// dropped it lies in the world or belongs to another player, who must not
		// lose it whenever this player is hidden.
		group.source = entity_handle(pawn_entity);
		if (!group.source.IsValid() || !valid_networked_edict_index(resolve_entity_index(system, group.source))
			|| !collect_handle(group.source) || !collect_handle(field<CEntityHandle>(services, compatibility_.fields().active_weapon))
			|| !collect_vector(services, compatibility_.fields().weapons, static_cast<int>(k_max_weapons))
			|| !collect_vector(pawn_entity, compatibility_.fields().wearables, static_cast<int>(k_max_wearables)))
		{
			hidden_group_clear(group);
			return false;
		}
		void* hostage_services = field<void*>(pawn_entity, compatibility_.fields().hostage_services);
		if (hostage_services != nullptr && !collect_handle(field<CEntityHandle>(hostage_services, compatibility_.fields().carried_hostage_prop)))
		{
			hidden_group_clear(group);
			return false;
		}
		return group.count != 0;
	}

	bool plugin::collect_attached_entities(CGameEntitySystem* system, CEntityInstance* pawn_entity, const visual_entity_group& owned,
										   attached_entity_group& attached) const
	{
		hidden_group_clear(attached);
		if (!compatibility_.scene_hierarchy_available())
		{
			return true;
		}
		if (system == nullptr || pawn_entity == nullptr)
		{
			return false;
		}
		void* body_component = field<void*>(pawn_entity, compatibility_.fields().body_component);
		void* root = body_component == nullptr ? nullptr : field<void*>(body_component, compatibility_.fields().scene_node);
		if (root == nullptr)
		{
			return false;
		}
		attached.source = entity_handle(pawn_entity);
		const auto next_sibling = [&](void* node) { return field<void*>(node, compatibility_.fields().scene_node_next_sibling); };
		const auto child_of = [&](void* node) { return field<void*>(node, compatibility_.fields().scene_node_child); };
		const auto visit = [&](void* node)
		{
			CEntityInstance* owner = field<CEntityInstance*>(node, compatibility_.fields().scene_node_owner);
			if (owner == nullptr || owner == pawn_entity)
			{
				return true;
			}
			const CEntityHandle handle = entity_handle(owner);
			if (!handle.IsValid() || system->GetEntityInstance(handle) != owner)
			{
				return false;
			}
			if (!valid_networked_edict_index(entity_index(owner)) || hidden_group_contains(owned, handle))
			{
				return true;
			}
			// Never withhold another player riding on this one: that player may be
			// the recipient, who must always receive their own pawn.
			const char* classname = owner->m_pEntity->GetClassname();
			if (classname == nullptr || std::strcmp(classname, "player") == 0)
			{
				return false;
			}
			return hidden_group_append_unique(attached, handle);
		};
		// Anything the walk cannot account for reveals the player instead.
		return walk_scene_descendants<k_max_scene_nodes_walked>(child_of(root), next_sibling, child_of, visit);
	}

	bool plugin::capture_animated_capsules(CEntityInstance* pawn, uint32_t slot, player_state& player, std::chrono::steady_clock::time_point now)
	{
		if (pawn == nullptr || slot >= player_bone_cache_.size() || compatibility_.lookup_bone() == nullptr
			|| compatibility_.get_bone_transform() == nullptr)
		{
			return false;
		}
		player_bone_cache& cache = player_bone_cache_[slot];
		if (cache.pawn != pawn || (!cache.valid && now >= cache.retry_after))
		{
			cache = {};
			cache.pawn = pawn;
			cache.retry_after = now + std::chrono::seconds(1);
			cache.valid = true;
			const auto lookup = reinterpret_cast<int32_t (*)(void*, const char*)>(compatibility_.lookup_bone());
			for (size_t capsule = 0; capsule < cache.indices.size(); ++capsule)
			{
				cache.indices[capsule] = lookup(pawn, k_visibility_capsule_bindings[capsule].bone);
				if (cache.indices[capsule] < 0)
				{
					cache.valid = false;
				}
			}
		}
		if (!cache.valid)
		{
			return false;
		}

		std::array<visibility_capsule, k_visibility_capsule_count> capsules;
		constexpr float k_max_capsule_endpoint_distance_sq = 128.0f * 128.0f;
		for (size_t capsule = 0; capsule < capsules.size(); ++capsule)
		{
			CTransform transform;
			const float invalid = std::numeric_limits<float>::quiet_NaN();
			transform.m_vPosition.Init(invalid, invalid, invalid);
			transform.m_orientation.Init(invalid, invalid, invalid, invalid);
#if defined(_WIN32)
			reinterpret_cast<void (*)(void*, CTransform*, int32_t)>(compatibility_.get_bone_transform())(pawn, &transform, cache.indices[capsule]);
#else
			reinterpret_cast<void (*)(CTransform*, void*, int32_t)>(compatibility_.get_bone_transform())(&transform, pawn, cache.indices[capsule]);
#endif
			const visibility_bone_transform copied {
				to_vec3(transform.m_vPosition),
				{transform.m_orientation.x, transform.m_orientation.y, transform.m_orientation.z, transform.m_orientation.w}};
			const visibility_capsule_binding& binding = k_visibility_capsule_bindings[capsule];
			visibility_capsule& output = capsules[capsule];
			output.radius = binding.radius;
			if (!visibility_transform_point(copied, binding.local_start, output.start)
				|| !visibility_transform_point(copied, binding.local_end, output.end) || !valid_visibility_capsule(output))
			{
				return false;
			}
			for (vec3 endpoint : {output.start, output.end})
			{
				const float x = endpoint.x - player.origin.x;
				const float y = endpoint.y - player.origin.y;
				const float z = endpoint.z - player.origin.z;
				if (x * x + y * y + z * z > k_max_capsule_endpoint_distance_sq)
				{
					return false;
				}
			}
		}
		player.capsules = capsules;
		player.capsule_count = static_cast<uint32_t>(capsules.size());
		return true;
	}

	bool plugin::capture(visibility_snapshot& value, float game_time)
	{
		CGameEntitySystem* system = entity_system();
		if (system == nullptr)
		{
			return false;
		}
		value.sequence = ++snapshot_sequence_;
		value.captured = std::chrono::steady_clock::now();
		const auto now = value.captured;
		std::array<lifecycle_key, k_max_players> keys;
		std::array<bool, k_max_players> stable_slots {};
		std::array<CEntityInstance*, k_max_players> animated_pawns {};
		value.filter_teammates = visibility_teammate_filter_enabled(settings::current().filter_teammates, teammates_are_enemies());
		capture_occluders(system, value, now);
		value.smoke_enabled = settings::current().smoke_occlusion;
		const bool smoke_check_pending = !compatibility_.smoke_available() && compatibility_.smoke_layout_candidate()
										 && smoke_layout_state_ == smoke_layout_state::unchecked;
		if (value.smoke_enabled && (compatibility_.smoke_available() || smoke_check_pending))
		{
			std::array<CEntityInstance*, k_max_smoke_volumes> smoke_entities {};
			size_t smoke_count = 0;
			bool smoke_overflow = false;
			collect_smoke_entities(system, game_time, smoke_check_pending, smoke_entities, smoke_count, smoke_overflow);
			if (smoke_check_pending)
			{
				verify_runtime_smoke_layout(smoke_entities, smoke_count, game_time);
			}
			value.smoke_available = compatibility_.smoke_available();
			if (value.smoke_available && !capture_smokes(smoke_entities, smoke_count, smoke_overflow, game_time, value))
			{
				value.smoke_available = false;
				value.smokes.reset();
			}
		}
		else
		{
			value.smoke_available = compatibility_.smoke_available();
		}
		std::unique_lock<std::mutex> lock(transmit_state_mutex_);
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			live_player live;
			const lifecycle_key key = player_lifecycle(slot, system, &live);
			keys[slot] = key;
			const bool stable = live.pawn != nullptr;
			stable_slots[slot] = stable;
			update_lifecycle_guard(lifecycle_[slot], key, stable, now, k_lifecycle_fail_open);
			if (!stable || !lifecycle_allows_hiding(lifecycle_[slot], now))
			{
				continue;
			}
			CEntityInstance* pawn_entity = live.pawn;
			void* body_component = field<void*>(pawn_entity, compatibility_.fields().body_component);
			void* scene_node = body_component == nullptr ? nullptr : field<void*>(body_component, compatibility_.fields().scene_node);
			void* collision = field<void*>(pawn_entity, compatibility_.fields().collision);
			if (scene_node == nullptr || collision == nullptr)
			{
				continue;
			}
			player_state player;
			player.team = live.team;
			player.pawn_entity = live.pawn_entity;
			player.origin = to_vec3(field<Vector>(scene_node, compatibility_.fields().abs_origin));
			player.mins = to_vec3(field<Vector>(collision, compatibility_.fields().mins));
			player.maxs = to_vec3(field<Vector>(collision, compatibility_.fields().maxs));
			void* view = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pawn_entity) + compatibility_.fields().view_offset);
			player.eye = {player.origin.x + field<float>(view, compatibility_.fields().view_x),
						  player.origin.y + field<float>(view, compatibility_.fields().view_y),
						  player.origin.z + field<float>(view, compatibility_.fields().view_z)};
			const qangle eye_angles = field<qangle>(pawn_entity, compatibility_.fields().eye_angles);
			player.eye_yaw_degrees = eye_angles.y;
			player.eye_pitch_degrees = std::isfinite(eye_angles.x) ? eye_angles.x : 0.0f;
			if (void* movement = field<void*>(pawn_entity, compatibility_.fields().movement_services); movement != nullptr)
			{
				void* buttons = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(movement) + compatibility_.fields().movement_buttons);
				player.movement_buttons = field<uint64_t>(buttons, compatibility_.fields().button_states);
			}
			player.muzzle_class = active_weapon_muzzle_class(system, pawn_entity);
			if (compatibility_.velocity_available())
			{
				const vec3 velocity = to_vec3(field<Vector>(pawn_entity, compatibility_.fields().abs_velocity));
				player.has_velocity = std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z);
				player.velocity = player.has_velocity ? velocity : vec3 {};
			}
			if (INetChannelInfo* channel = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(slot))); channel != nullptr)
			{
				player.rtt_seconds = channel->GetEngineLatency();
			}
			if (!valid_player_numbers(player))
			{
				continue;
			}
			// The latency comes from an engine virtual; a value past half a second is
			// not a real round trip and only widens the viewing origins.
			player.rtt_seconds = std::isfinite(player.rtt_seconds) ? std::clamp(player.rtt_seconds, 0.0f, k_max_rtt_seconds) : 0.0f;
			player.valid = true;
			value.players[slot] = player;
			animated_pawns[slot] = pawn_entity;
		}
		capture_dead_viewers(system, keys, game_time, value);
		const auto bones_started = std::chrono::steady_clock::now();
		uint32_t capsule_players = 0;
		uint32_t capsule_failed_players = 0;
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			if (!value.players[slot].valid)
			{
				player_bone_cache_[slot] = {};
				continue;
			}
			if (!compatibility_.bones_available())
			{
				player_state& player = value.players[slot];
				player.capsule_count = visibility_hull_capsules(player.origin, player.mins, player.maxs, player.capsules);
				player.capsule_count != 0 ? ++capsule_players : ++capsule_failed_players;
				continue;
			}
			capture_animated_capsules(animated_pawns[slot], slot, value.players[slot], now) ? ++capsule_players : ++capsule_failed_players;
		}
		bone_timing_.record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bones_started).count());
		capsule_players_ = capsule_players;
		capsule_failed_players_ = capsule_failed_players;
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				if (update_pair_guard(pair_guards_[recipient][target], keys[recipient], stable_slots[recipient], keys[target], stable_slots[target])
					&& hidden_groups_[recipient][target].count != 0)
				{
					hidden_group_clear(hidden_groups_[recipient][target]);
				}
			}
		}
		return true;
	}

} // namespace cs2glaz
