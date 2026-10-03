#include "plugin.h"

// Phantom players (experimental, cs2glaz_decoy_phantoms, off by default).
//
// A ghost (ghosts.cpp) is a fake client and takes a player slot. A phantom
// takes none: it is a player controller entity created in an entity slot
// above the server's player count (-maxplayers 32 leaves entity slots 33-64
// to nobody), with no client behind it, so the game's player loops never see
// it. External ESPs walk entity slots 1-64 and follow each controller's pawn
// handle: a phantom's points at a decoy prop with the hidden enemy's model,
// health 100 and his team, and the controller carries his name. Only its
// viewer receives the controller, and the prop only while it stands in for
// one of his decoys (delivery, walking, jumps and reports as for any decoy)
// or, with cs2glaz_decoy_front 1, while it runs as his front decoy
// (front_decoys.cpp; every phantom is a front one then).
//
// Nothing in the game expects a controller without a client, so this is the
// least proven part of CS2GLAZ: test it on your own server with your own
// clients first. The controller is created once with every field set before
// it is first sent and never changed afterwards (a different enemy to imitate
// means a new phantom); it is removed with its prop. Anything unexpected turns
// phantoms off until the plugin reloads.

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoy_phantoms("cs2glaz_decoy_phantoms", FCVAR_NONE,
											"Experimental phantom players for player-only ESPs: up to this many controllers without a client (no "
											"player slot taken), one per viewer, carrying the hidden enemy's name and standing in for his decoys "
											"(or, with cs2glaz_decoy_front 1, standing in front of him); 0 off (needs cs2glaz_decoys and -maxplayers "
											"below 64; test servers; resets on restart)",
											0, true, 0, true, static_cast<int>(k_max_phantoms));

		using create_entity_fn = CEntityInstance* (*)(const char*, int);
		using dispatch_spawn_fn = void (*)(CEntityInstance*, void*);
		using remove_entity_fn = void (*)(CEntityInstance*);
		using set_model_fn = void (*)(CEntityInstance*, const char*);
		using teleport_fn = void (*)(CEntityInstance*, const Vector*, const QAngle*, const Vector*);

		constexpr uint8_t k_solid_none = 0;
		constexpr uint8_t k_solid_flag_not_solid = 0x04;
		constexpr int32_t k_phantom_health = 100;
		constexpr int32_t k_connected = 0; // PlayerConnectedState::PlayerConnected
		constexpr size_t k_player_name_bytes = 128;
		constexpr auto k_phantom_create_interval = std::chrono::seconds(2);
		constexpr auto k_phantom_retarget_after = std::chrono::seconds(5);
		constexpr auto k_phantom_idle_remove = std::chrono::seconds(30);

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		uint8_t enemy_of(uint8_t team)
		{
			return team == k_team_t ? k_team_ct : (team == k_team_ct ? k_team_t : 0);
		}

	} // namespace

	int plugin::phantom_limit() const
	{
		return decoy_mode() == 0 ? 0 : std::clamp(cs2glaz_decoy_phantoms.Get(), 0, static_cast<int>(k_max_phantoms));
	}

	bool plugin::phantoms_available(std::string& reason) const
	{
		if (!phantom_error_.empty())
		{
			reason = phantom_error_;
			return false;
		}
		if (!compatibility_.valid() || !compatibility_.phantom_schema_available() || !compatibility_.collision_attribute_available()
			|| !compatibility_.decoy_schema_available())
		{
			reason = "the schema lacks a controller field (m_iszPlayerName, m_iPawnHealth, m_iConnected, m_hPawn) or the collision attributes";
			return false;
		}
		if (!decoy_functions_.ready || decoy_functions_.teleport_vtable_index == 0)
		{
			reason = "decoys are not ready";
			return false;
		}
		INetworkGameServer* server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
		CGlobalVars* globals = server == nullptr ? nullptr : server->GetGlobals();
		if (globals == nullptr || globals->maxClients <= 0 || globals->maxClients >= static_cast<int>(k_max_players))
		{
			reason = "no entity slot above the player count (start the server with -maxplayers below 64)";
			return false;
		}
		return true;
	}

	phantom_player* plugin::create_phantom(CGameEntitySystem* system, uint32_t viewer, const decoy_slot& slot, const std::string& model,
										   CEntityInstance* collision_source, std::chrono::steady_clock::time_point now)
	{
		const auto free = std::find_if(phantoms_.begin(), phantoms_.end(), [](const phantom_player& phantom) { return phantom.index == 0; });
		if (system == nullptr || free == phantoms_.end() || model.empty() || slot.target >= k_max_players)
		{
			return nullptr;
		}
		const schema_offsets& fields = compatibility_.fields();
		CEntityInstance* target_controller = system->GetEntityInstance(CEntityIndex(static_cast<int>(slot.target + 1u)));
		CEntityInstance* viewer_controller = system->GetEntityInstance(CEntityIndex(static_cast<int>(viewer + 1u)));
		if (target_controller == nullptr || viewer_controller == nullptr)
		{
			return nullptr;
		}
		const uint8_t team = enemy_of(field<uint8_t>(viewer_controller, fields.team));
		if (team == 0)
		{
			return nullptr;
		}
		// The highest free entity slot above the player count.
		INetworkGameServer* server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
		CGlobalVars* globals = server == nullptr ? nullptr : server->GetGlobals();
		const int max_clients = globals == nullptr ? static_cast<int>(k_max_players) : globals->maxClients;
		int index = 0;
		for (int candidate = static_cast<int>(k_max_players); candidate > max_clients && index == 0; --candidate)
		{
			if (system->GetEntityInstance(CEntityIndex(candidate)) == nullptr
				&& std::none_of(phantoms_.begin(), phantoms_.end(), [&](const phantom_player& phantom) { return phantom.index == candidate; }))
			{
				index = candidate;
			}
		}
		if (index == 0)
		{
			return nullptr;
		}
		const decoy_functions& functions = decoy_functions_;
		const auto fail = [&](const char* why, CEntityInstance* body, CEntityInstance* controller)
		{
			if (controller != nullptr)
			{
				discard_decoy_entity(controller);
			}
			if (body != nullptr)
			{
				discard_decoy_entity(body);
			}
			phantom_error_ = why;
			META_CONPRINTF("[CS2GLAZ] phantoms turned off: %s; they stay off until the plugin reloads\n", why);
			return static_cast<phantom_player*>(nullptr);
		};
		// The body: a decoy prop with the enemy's model, his team and health 100,
		// harmless like every decoy.
		const auto apply_body = [&](CEntityInstance* entity)
		{
			void* collision = reinterpret_cast<std::byte*>(entity) + fields.model_collision;
			field<uint8_t>(collision, fields.solid_type) = k_solid_none;
			field<uint8_t>(collision, fields.solid_flags) |= k_solid_flag_not_solid;
			void* attribute = reinterpret_cast<std::byte*>(collision) + fields.collision_attribute;
			field<uint64_t>(attribute, fields.interacts_as) = 0;
			field<uint64_t>(attribute, fields.interacts_with) = 0;
			field<int32_t>(entity, fields.health) = k_phantom_health;
			field<bool>(entity, fields.takes_damage) = false;
			field<uint8_t>(entity, fields.team) = team;
			if (compatibility_.shadow_strength_available())
			{
				field<float>(entity, fields.shadow_strength) = 0.0f;
			}
			if (decoy_entity_mode() == 1)
			{
				field<uint8_t>(entity, fields.render_mode) = compatibility_.render_none_value();
				field<uint8_t>(entity, fields.render_color + 3) = 0;
			}
		};
		CEntityInstance* body = reinterpret_cast<create_entity_fn>(functions.create_entity_by_name)("prop_dynamic", -1);
		if (body == nullptr)
		{
			return nullptr;
		}
		void** vtable = nullptr;
		void* teleport = nullptr;
		if (!runtime_compatibility::safe_read(body, &vtable, sizeof(vtable)) || vtable == nullptr
			|| !runtime_compatibility::safe_read(vtable + functions.teleport_vtable_index, &teleport, sizeof(teleport))
			|| !compatibility_.address_in_server_module(teleport))
		{
			return fail("the Teleport vtable index does not point into the server", body, nullptr);
		}
		apply_body(body);
		reinterpret_cast<dispatch_spawn_fn>(functions.dispatch_spawn)(body, nullptr);
		apply_body(body);
		reinterpret_cast<set_model_fn>(functions.set_model)(body, model.c_str());
		void* collision = reinterpret_cast<std::byte*>(body) + fields.model_collision;
		if (field<uint8_t>(collision, fields.solid_type) != k_solid_none || (field<uint8_t>(collision, fields.solid_flags) & k_solid_flag_not_solid) == 0)
		{
			return fail("a phantom's body was solid", body, nullptr);
		}
		apply_body(body);
		const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
		const QAngle angles(0.0f, slot.yaw, 0.0f);
		const Vector stop(0.0f, 0.0f, 0.0f);
		reinterpret_cast<teleport_fn>(teleport)(body, &origin, &angles, &stop);
		// Against triggerbots (cs2glaz_decoy_front 2): a real player's collision,
		// written before the body is first sent, so its viewer's client (whose
		// crosshair trace most triggerbots read) takes it for a player's. The
		// server made no physics object for it (it was spawned before its model)
		// and gets none from these fields, so its own traces still pass through;
		// front_bullet_check turns this off if one ever does not.
		bool hittable = false;
		if (collision_source != nullptr)
		{
			void* from = reinterpret_cast<std::byte*>(collision_source) + fields.model_collision;
			void* to = reinterpret_cast<std::byte*>(body) + fields.model_collision;
			const uint8_t solid_type = field<uint8_t>(from, fields.solid_type);
			const uint8_t solid_flags = field<uint8_t>(from, fields.solid_flags);
			const Vector mins = field<Vector>(from, fields.mins);
			const Vector maxs = field<Vector>(from, fields.maxs);
			// Only a living player's plain collision: solid, a sane box.
			if (solid_type != k_solid_none && (solid_flags & k_solid_flag_not_solid) == 0 && mins.x < maxs.x && mins.y < maxs.y && mins.z < maxs.z
				&& maxs.x - mins.x <= 64.0f && maxs.y - mins.y <= 64.0f && maxs.z - mins.z <= 96.0f)
			{
				void* attribute_from = reinterpret_cast<std::byte*>(from) + fields.collision_attribute;
				void* attribute_to = reinterpret_cast<std::byte*>(to) + fields.collision_attribute;
				field<Vector>(to, fields.mins) = mins;
				field<Vector>(to, fields.maxs) = maxs;
				field<uint64_t>(attribute_to, fields.interacts_as) = field<uint64_t>(attribute_from, fields.interacts_as);
				field<uint64_t>(attribute_to, fields.interacts_with) = field<uint64_t>(attribute_from, fields.interacts_with);
				field<uint8_t>(to, fields.solid_flags) = solid_flags;
				field<uint8_t>(to, fields.solid_type) = solid_type;
				hittable = true;
			}
		}
		const CEntityHandle body_handle = entity_handle(body);
		// The controller, in the chosen entity slot, with everything set before it
		// is first sent: it is never changed afterwards.
		char name[k_player_name_bytes] {};
		std::memcpy(name, reinterpret_cast<const char*>(target_controller) + fields.player_name, sizeof(name) - 1);
		const auto apply_controller = [&](CEntityInstance* entity)
		{
			field<uint8_t>(entity, fields.team) = team;
			std::memcpy(reinterpret_cast<char*>(entity) + fields.player_name, name, sizeof(name));
			field<int32_t>(entity, fields.connected) = k_connected;
			field<bool>(entity, fields.pawn_is_alive) = true;
			field<uint32_t>(entity, fields.pawn_health) = static_cast<uint32_t>(k_phantom_health);
			field<CEntityHandle>(entity, fields.player_pawn) = body_handle;
			field<CEntityHandle>(entity, fields.controller_pawn) = body_handle;
			field<bool>(entity, fields.is_hltv) = false;
		};
		CEntityInstance* controller = reinterpret_cast<create_entity_fn>(functions.create_entity_by_name)("cs_player_controller", index);
		if (controller == nullptr)
		{
			return fail("the game did not create a player controller without a client", body, nullptr);
		}
		if (entity_index(controller) != index)
		{
			return fail("the game put a phantom's controller in another entity slot", body, controller);
		}
		apply_controller(controller);
		reinterpret_cast<dispatch_spawn_fn>(functions.dispatch_spawn)(controller, nullptr);
		apply_controller(controller);
		phantom_player& phantom = *free;
		phantom = {};
		phantom.index = index;
		phantom.controller = entity_handle(controller);
		phantom.body = body_handle;
		phantom.teleport = teleport;
		phantom.viewer = viewer;
		phantom.target = slot.target;
		phantom.team = team;
		phantom.hittable = hittable;
		phantom.entity_mode = decoy_entity_mode();
		phantom.created_at = now;
		phantom.idle_since = now;
		phantom_slots_.fetch_or(uint64_t {1} << (index - 1));
		++ghost_counters_.phantoms_created;
		META_CONPRINTF("[CS2GLAZ] phantom \"%s\" (entity %d) created for viewer slot %u on team %u\n", name, index, viewer, static_cast<unsigned>(team));
		return &phantom;
	}

	bool plugin::phantom_take(CGameEntitySystem* system, uint32_t viewer, decoy_slot& slot, const std::string& model,
							  std::chrono::steady_clock::time_point now)
	{
		std::string reason;
		// Front decoys take every phantom: hidden decoys are props (or ghosts) then.
		if (system == nullptr || slot.control || phantom_limit() == 0 || front_enabled() || !phantoms_available(reason))
		{
			return false;
		}
		phantom_player* phantom = nullptr;
		for (phantom_player& candidate : phantoms_)
		{
			if (candidate.index == 0 || candidate.viewer != viewer)
			{
				continue;
			}
			if (candidate.driving_id != 0 || now - candidate.released_at < std::chrono::duration<float, std::milli>(k_decoy_reuse_quarantine_ms))
			{
				return false; // his phantom is busy: this decoy is a prop
			}
			if (candidate.target == slot.target && system->GetEntityInstance(candidate.body) != nullptr)
			{
				phantom = &candidate;
			}
			else if (now - candidate.created_at >= k_phantom_retarget_after)
			{
				// Another enemy to imitate: its name cannot change, so a new phantom.
				remove_phantom(candidate, true);
			}
			else
			{
				return false;
			}
			break;
		}
		if (phantom == nullptr)
		{
			const auto existing = std::count_if(phantoms_.begin(), phantoms_.end(), [](const phantom_player& candidate) { return candidate.index != 0; });
			if (existing >= phantom_limit() || now < phantom_next_create_)
			{
				return false;
			}
			phantom_next_create_ = now + k_phantom_create_interval;
			phantom = create_phantom(system, viewer, slot, model, nullptr, now);
			if (phantom == nullptr)
			{
				return false;
			}
		}
		CEntityInstance* body = system->GetEntityInstance(phantom->body);
		if (body == nullptr || phantom->teleport == nullptr)
		{
			return false;
		}
		const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
		const QAngle angles(0.0f, slot.yaw, 0.0f);
		const Vector stop(0.0f, 0.0f, 0.0f);
		reinterpret_cast<teleport_fn>(phantom->teleport)(body, &origin, &angles, &stop);
		slot.handle = phantom->body;
		slot.teleport = phantom->teleport;
		slot.model = model;
		slot.spawned = true;
		slot.ghost = true;
		phantom->driving_id = slot.id;
		++ghost_counters_.phantom_runs;
		return true;
	}

	void plugin::remove_phantom(phantom_player& phantom, bool remove_entities)
	{
		if (phantom.index == 0)
		{
			return;
		}
		CGameEntitySystem* system = remove_entities ? entity_system() : nullptr;
		if (system != nullptr)
		{
			// Withheld from everyone until the game deletes them (the graveyard).
			discard_decoy_entity(system->GetEntityInstance(phantom.controller));
			discard_decoy_entity(system->GetEntityInstance(phantom.body));
		}
		if (phantom.front)
		{
			stop_front_decoys(phantom.viewer, std::chrono::steady_clock::now());
		}
		if (phantom.driving_id != 0)
		{
			for (auto& row : decoys_)
			{
				for (decoy_slot& slot : row)
				{
					if (slot.id == phantom.driving_id && slot.ghost)
					{
						slot = {};
					}
				}
			}
		}
		phantom_slots_.fetch_and(~(uint64_t {1} << (phantom.index - 1)));
		phantom = {};
		++ghost_counters_.phantoms_removed;
	}

	void plugin::remove_all_phantoms(bool remove_entities)
	{
		bool any = false;
		for (phantom_player& phantom : phantoms_)
		{
			any = any || phantom.index != 0;
			remove_phantom(phantom, remove_entities);
		}
		phantom_slots_.store(0);
		const auto now = std::chrono::steady_clock::now();
		for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
		{
			stop_front_decoys(viewer, now);
		}
		if (any)
		{
			publish_phantom_transmit();
			publish_decoy_transmit();
		}
	}

	void plugin::update_phantoms(CGameEntitySystem* system, const visibility_snapshot& value, std::chrono::steady_clock::time_point now)
	{
		std::string reason;
		if (phantom_limit() == 0 || system == nullptr || !phantoms_available(reason))
		{
			remove_all_phantoms(system != nullptr);
			return;
		}
		const schema_offsets& fields = compatibility_.fields();
		for (phantom_player& phantom : phantoms_)
		{
			if (phantom.index == 0)
			{
				continue;
			}
			CEntityInstance* controller = system->GetEntityInstance(phantom.controller);
			CEntityInstance* body = system->GetEntityInstance(phantom.body);
			CEntityInstance* viewer = system->GetEntityInstance(CEntityIndex(static_cast<int>(phantom.viewer + 1u)));
			// A front phantom stays while its viewer is on a team, dead or alive
			// (its body goes to him only while he is alive), and while its body's
			// collision matches cs2glaz_decoy_front; a stand-in only while he is
			// alive, and never once front decoys take the phantoms. Either goes
			// when cs2glaz_decoys changes between drawn and not drawn: its body
			// was made for the other.
			const bool viewer_ok = viewer != nullptr && human_player(phantom.viewer) && enemy_of(field<uint8_t>(viewer, fields.team)) == phantom.team
								   && (phantom.front || value.players[phantom.viewer].valid) && phantom.front == front_enabled()
								   && (!phantom.front || phantom.hittable == front_hittable()) && phantom.entity_mode == decoy_entity_mode()
								   && (decoy_mode() != 3 || is_suspect(phantom.viewer, now));
			if (controller == nullptr || body == nullptr)
			{
				// The round restart took its prop (or something took either): its
				// controller would point at nothing.
				remove_phantom(phantom, true);
				continue;
			}
			if (!viewer_ok || (phantom.driving_id == 0 && now - phantom.idle_since >= k_phantom_idle_remove && !phantom.front))
			{
				remove_phantom(phantom, true);
			}
		}
	}

	void plugin::publish_phantom_transmit()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		for (size_t index = 0; index < phantoms_.size(); ++index)
		{
			const phantom_player& phantom = phantoms_[index];
			const uint32_t front_run =
				phantom.index != 0 && phantom.front && phantom.viewer < k_max_players ? front_decoys_[phantom.viewer].real.id : 0u;
			phantom_transmit_[index] =
				phantom.index == 0 ? phantom_transmit_entry {} : phantom_transmit_entry {phantom.index, phantom.viewer, phantom.body, front_run};
		}
	}

	// A phantom's controller goes to its viewer only; its body reaches him only
	// through the decoy slot it stands in for (withhold_decoys) or while it runs
	// as his front decoy, and nobody else.
	void plugin::withhold_phantoms(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count)
	{
		if (system == nullptr || phantom_slots_.load(std::memory_order_relaxed) == 0)
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		// Each recipient's slot, read once instead of once per phantom.
		std::array<int, k_max_players> slots;
		const int recipients = std::min(count, static_cast<int>(k_max_players));
		for (int i = 0; i < recipients; ++i)
		{
			slots[i] = -1;
			const CCheckTransmitInfo* info = infos[i];
			if (info != nullptr && info->m_pTransmitEntity != nullptr && info->m_pTransmitAlways != nullptr)
			{
				std::memcpy(&slots[i], reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(int));
			}
		}
		for (size_t entry_index = 0; entry_index < phantom_transmit_.size(); ++entry_index)
		{
			const phantom_transmit_entry& entry = phantom_transmit_[entry_index];
			if (entry.controller <= 0)
			{
				continue;
			}
			const int body = entity_index(entry.body.IsValid() ? system->GetEntityInstance(entry.body) : nullptr);
			const bool standing_in = entry.viewer < k_max_players
									 && (entry.front_run != 0
										 || std::any_of(decoy_transmit_[entry.viewer].begin(), decoy_transmit_[entry.viewer].end(),
														[&](const decoy_transmit_entry& decoy) { return decoy.id != 0 && decoy.handle == entry.body; }));
			for (int i = 0; i < recipients; ++i)
			{
				CCheckTransmitInfo* info = infos[i];
				if (info == nullptr || info->m_pTransmitEntity == nullptr || info->m_pTransmitAlways == nullptr)
				{
					continue;
				}
				const int slot = slots[i];
				const bool viewer = slot >= 0 && static_cast<uint32_t>(slot) == entry.viewer;
				if (!viewer)
				{
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, entry.controller, withhold_reason::ghost);
				}
				if (valid_networked_edict_index(body) && (!viewer || !standing_in))
				{
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, body, withhold_reason::ghost);
				}
				else if (valid_networked_edict_index(body) && entry.front_run != 0 && info->m_pTransmitEntity->IsBitSet(body))
				{
					// The engine packed its body for him: his front decoy reaches him.
					front_delivery& sent = front_delivery_[entry_index];
					if (sent.run != entry.front_run)
					{
						sent = {};
						sent.run = entry.front_run;
						sent.first_sent = now;
					}
					sent.last_sent = now;
				}
			}
		}
	}

	void plugin::print_phantom_status() const
	{
		std::string reason;
		const int limit = cs2glaz_decoy_phantoms.Get();
		const bool available = phantoms_available(reason);
		const auto value = [](uint64_t number) { return static_cast<unsigned long long>(number); };
		META_CONPRINTF("[CS2GLAZ] phantom players: limit=%d %s created=%llu removed=%llu runs=%llu\n", limit,
					   limit == 0 ? "(off)" : (available ? "ready" : ("unavailable: " + reason).c_str()), value(ghost_counters_.phantoms_created),
					   value(ghost_counters_.phantoms_removed), value(ghost_counters_.phantom_runs));
		for (const phantom_player& phantom : phantoms_)
		{
			if (phantom.index != 0)
			{
				META_CONPRINTF("[CS2GLAZ] phantom entity %d team %u viewer %u imitating slot %u%s\n", phantom.index, static_cast<unsigned>(phantom.team),
							   phantom.viewer, phantom.target,
							   phantom.front ? " in front of him" : (phantom.driving_id != 0 ? " standing in for a decoy" : ""));
			}
		}
	}

} // namespace cs2glaz
