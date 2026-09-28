#include "plugin.h"

// Reads live controllers, pawns, weapons, bounds, and visual groups on the game
// thread, then outputs plain copied visibility snapshots. Broken handles,
// lifecycle changes, or incomplete groups reset toward fail-open behavior.

#include <inetchannelinfo.h>
#include <mathlib/transform.h>
#include <tier1/utlvector.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace cs2glaz
{
	namespace
	{

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

	void copy_entity_name(CEntityInstance* entity, char (&name)[k_max_entity_name])
	{
		const char* source = entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetClassname() : nullptr;
		if (source == nullptr || source[0] == '\0')
		{
			source = "<unknown>";
		}
		std::snprintf(name, sizeof(name), "%s", source);
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

	void plugin::collect_smoke_entities(CGameEntitySystem* system, std::array<CEntityInstance*, k_max_smoke_volumes>& smokes, size_t& smoke_count,
										bool& smoke_overflow)
	{
		smoke_count = 0;
		smoke_overflow = false;
		if (system == nullptr)
		{
			return;
		}
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
			if (compatibility_.smoke_available() && classname != nullptr && std::strcmp(classname, "smokegrenade_projectile") == 0
				&& field<bool>(entity, compatibility_.fields().did_smoke_effect))
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
		}
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
			return true;
		}
		snapshot.volumes.reserve(count);
		for (size_t index = 0; index < count; ++index)
		{
			CEntityInstance* entity = entities[index];
			if (entity == nullptr)
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
		value.smokes = std::make_shared<smoke_snapshot>(std::move(snapshot));
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
		const auto collect_handle = [&](CEntityHandle handle)
		{
			if (!handle.IsValid())
			{
				return true;
			}
			if (!valid_networked_edict_index(resolve_entity_index(system, handle)))
			{
				return false;
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
		group.source = entity_handle(pawn_entity);
		if (!group.source.IsValid() || !collect_handle(group.source)
			|| !collect_handle(field<CEntityHandle>(services, compatibility_.fields().active_weapon))
			|| !collect_handle(field<CEntityHandle>(services, compatibility_.fields().last_weapon))
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
		value.smoke_enabled = settings::current().smoke_occlusion;
		value.smoke_available = compatibility_.smoke_available();
		if (value.smoke_enabled && value.smoke_available)
		{
			std::array<CEntityInstance*, k_max_smoke_volumes> smoke_entities {};
			size_t smoke_count = 0;
			bool smoke_overflow = false;
			collect_smoke_entities(system, smoke_entities, smoke_count, smoke_overflow);
			if (!capture_smokes(smoke_entities, smoke_count, smoke_overflow, game_time, value))
			{
				value.smoke_available = false;
				value.smokes.reset();
			}
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
			player.eye_yaw_degrees = field<qangle>(pawn_entity, compatibility_.fields().eye_angles).y;
			if (void* movement = field<void*>(pawn_entity, compatibility_.fields().movement_services); movement != nullptr)
			{
				void* buttons = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(movement) + compatibility_.fields().movement_buttons);
				player.movement_buttons = field<uint64_t>(buttons, compatibility_.fields().button_states);
			}
			player.muzzle_class = active_weapon_muzzle_class(system, pawn_entity);
			if (INetChannelInfo* channel = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(slot))); channel != nullptr)
			{
				player.rtt_seconds = channel->GetEngineLatency();
			}
			if (!valid_player_numbers(player))
			{
				continue;
			}
			player.rtt_seconds = std::max(0.0f, player.rtt_seconds);
			player.valid = true;
			value.players[slot] = player;
			animated_pawns[slot] = pawn_entity;
		}
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
