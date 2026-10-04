#include "plugin.h"

// Ghost players (experimental, cs2glaz_decoy_ghosts, off by default).
//
// Most cheats draw players only: an external ESP walks the controllers in
// entity slots 1-64 and follows each one's m_hPlayerPawn. A prop decoy is
// never one of them, so such a cheat never shows it. A ghost is a fake client
// (IVEngineServer2::CreateFakeClient) on the enemy team of one viewer: its
// controller goes to that viewer only, its pawn only while it stands in for
// one of his decoys behind a wall, and nobody else ever receives anything of
// it. On the server it is kept harmless every update: it takes no damage, is
// not solid, is touched by no trace and does not move by itself; it gives the
// bomb away at once, and it kills itself as soon as nobody else on its team is
// alive, so it never decides a round. The game events about it are not
// broadcast, radar entries about it are dropped, and dead players watching it
// are moved to another target.
//
// Anything unexpected (it cannot join its team, takes damage, disappears)
// kicks it and turns ghosts off until the plugin reloads; prop decoys and wall
// hiding are not affected.

#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoy_ghosts("cs2glaz_decoy_ghosts", FCVAR_NONE,
										  "Experimental ghost players for player-only ESPs: up to this many fake players (one per enemy team), each sent to "
										  "one viewer only and standing in for one of his decoys; 0 off (needs cs2glaz_decoys; test servers; resets on "
										  "restart)",
										  0, true, 0, true, static_cast<int>(k_max_ghosts));

		constexpr auto k_ghost_create_cooldown = std::chrono::seconds(5);
		constexpr auto k_ghost_join_interval = std::chrono::seconds(1);
		constexpr uint32_t k_ghost_join_attempts = 10;
		constexpr auto k_ghost_unused_kick = std::chrono::seconds(30);
		constexpr auto k_ghost_event_linger = std::chrono::seconds(5);
		constexpr auto k_ghost_bomb_limit = std::chrono::seconds(2);
		constexpr uint32_t k_ghost_lost_limit = 3;
		constexpr uint8_t k_move_type_none = 0;
		constexpr uint8_t k_solid_none = 0;
		constexpr uint8_t k_solid_flag_not_solid = 0x04;

		// Plausible names; a ghost never takes a real player's.
		constexpr const char* k_ghost_names[] = {"Kirill",	 "maks1m",	 "Den4ik",	 "ShadowFox", "n1ghtowl", "Artyom",	 "Lexa",	"ZeRo",
												 "frostbyte", "DimaPro",	 "Vlad",	 "sanya228",  "Nikita",	  "kolya",	 "Rustam", "ghostline",
												 "Egor",	  "TimuR",	 "arsen1y",	 "Pasha",	  "Mishanya", "kozak",	 "Lynx",	"Ilya"};

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		uint8_t enemy_team(uint8_t team)
		{
			return team == k_team_t ? k_team_ct : (team == k_team_ct ? k_team_t : 0);
		}

	} // namespace

	// Every step of a ghost that calls into the game, written and flushed to
	// addons/cs2glaz/logs/ghost_trace.log before the call: after a crash its
	// last line is the step that crashed. Ghost steps are rare (seconds apart).
	void plugin::ghost_trace(const char* format, ...) const
	{
		if (api_ == nullptr || format == nullptr)
		{
			return;
		}
		char text[256];
		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		const std::filesystem::path directory = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2glaz" / "logs";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		const std::string path = (directory / "ghost_trace.log").string();
		FILE* file = std::fopen(path.c_str(), ghost_trace_started_ ? "a" : "w");
		if (file == nullptr)
		{
			return;
		}
		ghost_trace_started_ = true;
		const std::time_t now = std::time(nullptr);
		std::tm parts {};
#if defined(_WIN32)
		gmtime_s(&parts, &now);
#else
		gmtime_r(&now, &parts);
#endif
		char stamp[32] {};
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%SZ", &parts);
		std::fprintf(file, "%s %s\n", stamp, text);
		std::fflush(file);
#if !defined(_WIN32)
		fsync(fileno(file));
#endif
		std::fclose(file);
	}

	int plugin::ghost_limit() const
	{
		return std::clamp(cs2glaz_decoy_ghosts.Get(), 0, static_cast<int>(k_max_ghosts));
	}

	bool plugin::ghosts_available(std::string& reason) const
	{
		if (!ghost_error_.empty())
		{
			reason = ghost_error_;
			return false;
		}
		if (!compatibility_.valid() || !compatibility_.ghost_schema_available() || !compatibility_.collision_attribute_available()
			|| !compatibility_.decoy_schema_available())
		{
			reason = "the schema lacks m_bTakesDamage, m_MoveType, m_bPawnIsAlive or the collision attributes";
			return false;
		}
		if (engine_ == nullptr || cvar_ == nullptr)
		{
			reason = "the engine or cvar interface is missing";
			return false;
		}
		if (!decoy_functions_.ready || decoy_functions_.teleport_vtable_index == 0)
		{
			reason = "decoys are not ready";
			return false;
		}
		if (!game_event_fire_hooked_)
		{
			// Its connect, team and death events would reach every player.
			reason = "the game event manager was not found, so events about a ghost cannot be kept off players' screens";
			return false;
		}
		return true;
	}

	bool plugin::ghost_pawn_alive(CGameEntitySystem* system, CEntityHandle pawn) const
	{
		CEntityInstance* entity = system != nullptr && pawn.IsValid() ? system->GetEntityInstance(pawn) : nullptr;
		return entity != nullptr && field<uint8_t>(entity, compatibility_.fields().life_state) == k_life_alive
			   && field<int32_t>(entity, compatibility_.fields().health) > 0;
	}

	// Runs a console command as the ghost, exactly as if its client had sent it.
	void plugin::ghost_command(int slot, const char* command)
	{
		if (cvar_ == nullptr || slot < 0 || command == nullptr)
		{
			return;
		}
		CCommand args;
		if (!args.Tokenize(command) || args.ArgC() < 1)
		{
			return;
		}
		const ConCommandRef handle = cvar_->FindConCommand(args.Arg(0));
		if (!handle.IsValidRef())
		{
			return;
		}
		ghost_trace("command slot %d: %s", slot, command);
		cvar_->DispatchConCommand(handle, CCommandContext(CT_NO_TARGET, CPlayerSlot(slot)), args);
		ghost_trace("command done");
	}

	void plugin::kick_ghost(ghost_player& ghost, const char* reason)
	{
		if (ghost.slot >= 0 && engine_ != nullptr)
		{
			// Its disconnect event is still kept from clients for a few seconds.
			ghost_kicked_at_[static_cast<size_t>(ghost.slot)] = std::chrono::steady_clock::now();
			ghost_trace("kick slot %d: %s", ghost.slot, reason);
			engine_->KickClient(CPlayerSlot(ghost.slot), reason, NETWORK_DISCONNECT_KICKED);
			ghost_trace("kick done");
			++ghost_counters_.kicked;
		}
		// A crosshair ghost's turn ends with it.
		end_crosshair_turn(nullptr, static_cast<size_t>(&ghost - ghosts_.data()), std::chrono::steady_clock::now());
		if (ghost.driving_id != 0)
		{
			for (auto& row : decoys_)
			{
				for (decoy_slot& slot : row)
				{
					if (slot.id == ghost.driving_id && slot.ghost)
					{
						slot = {};
					}
				}
			}
		}
		ghost = {};
	}

	void plugin::kick_all_ghosts(const char* reason)
	{
		bool any = false;
		for (ghost_player& ghost : ghosts_)
		{
			any = any || ghost.slot >= 0;
			kick_ghost(ghost, reason);
		}
		if (any)
		{
			publish_ghost_transmit();
		}
	}

	void plugin::update_ghosts(CGameEntitySystem* system, const visibility_snapshot& value, std::chrono::steady_clock::time_point now)
	{
		// Slots kicked a while ago stop hiding their events.
		uint64_t lingering = 0;
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			if (ghost_kicked_at_[slot] != std::chrono::steady_clock::time_point {} && now - ghost_kicked_at_[slot] < k_ghost_event_linger)
			{
				lingering |= uint64_t {1} << slot;
			}
		}
		std::string reason;
		// Crosshair ghosts (crosshair_ghosts.cpp) take both ghosts, one per team,
		// shared by every player of the other team in turns.
		const bool shared = crosshair_enabled();
		const int wanted = decoy_mode() == 0 ? 0 : shared ? static_cast<int>(k_max_ghosts) : std::clamp(cs2glaz_decoy_ghosts.Get(), 0, static_cast<int>(k_max_ghosts));
		if (shared != ghosts_shared_)
		{
			ghost_trace("crosshair ghosts %s (decoys mode %d)", shared ? "on" : "off", decoy_mode());
			// A ghost made for one use is never handed to the other.
			for (size_t index = 0; index < ghosts_.size(); ++index)
			{
				end_crosshair_turn(system, index, now);
			}
			kick_all_ghosts("cs2glaz ghost mode changed");
			ghosts_shared_ = shared;
		}
		if (wanted == 0 || system == nullptr || !ghosts_available(reason))
		{
			for (size_t index = 0; index < ghosts_.size(); ++index)
			{
				end_crosshair_turn(system, index, now);
			}
			kick_all_ghosts("cs2glaz ghosts off");
			ghost_slots_.store(0);
			ghost_event_slots_.store(lingering);
			return;
		}
		const schema_offsets& fields = compatibility_.fields();
		const auto fail = [&](ghost_player& ghost, const std::string& why)
		{
			ghost_error_ = why;
			META_CONPRINTF("[CS2GLAZ] ghosts turned off: %s; they stay off until the plugin reloads\n", why.c_str());
			kick_ghost(ghost, "cs2glaz ghost failed");
		};
		// Viewers that may have a ghost, watched players first; in mode 3 only them.
		fixed_list<uint32_t, k_max_players> viewers;
		for (int pass = 0; pass < 2; ++pass)
		{
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				const player_state& player = value.players[slot];
				if (!player.valid || !human_player(slot) || ghost_capture_slot(slot))
				{
					continue;
				}
				const bool watched = is_suspect(slot, now);
				if (watched == (pass == 0) && (watched || decoy_mode() != 3))
				{
					viewers.push_back(slot);
				}
			}
		}
		// Alive, spawning included (every pawn spawns at once at a round start).
		const auto alive_on_team = [&](uint8_t team)
		{
			uint32_t alive = 0;
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				const lifecycle_key key = player_lifecycle(slot, system, nullptr);
				alive += !ghost_capture_slot(slot) && key.alive && key.team == team ? 1u : 0u;
			}
			return alive;
		};
		for (size_t index = 0; index < ghosts_.size(); ++index)
		{
			ghost_player& ghost = ghosts_[index];
			if (ghost.slot < 0)
			{
				continue;
			}
			CEntityInstance* controller = system->GetEntityInstance(CEntityIndex(ghost.slot + 1));
			if (controller == nullptr)
			{
				if (now - ghost.created_at > std::chrono::seconds(3))
				{
					// Gone without us (an idle kick, another plugin): made again later,
					// unless it keeps happening.
					if (++ghost_lost_ >= k_ghost_lost_limit)
					{
						fail(ghost, "ghost players keep disappearing (kicked by the game or another plugin?)");
					}
					else
					{
						META_CONPRINTF("[CS2GLAZ] ghost \"%s\" disappeared; a new one may be made in a few seconds\n", ghost.name.c_str());
						kick_ghost(ghost, "cs2glaz ghost lost");
					}
				}
				continue;
			}
			// Its viewer stays its viewer while he is connected, on the other team
			// and (mode 3) watched, alive or dead: its controller keeps going to
			// him instead of leaving and coming back with every death. A crosshair
			// ghost's viewer is the one of its current turn.
			CEntityInstance* viewer_controller =
				ghost.viewer < k_max_players ? system->GetEntityInstance(CEntityIndex(static_cast<int>(ghost.viewer + 1u))) : nullptr;
			const bool viewer_ok = viewer_controller != nullptr && human_player(ghost.viewer)
								   && enemy_team(field<uint8_t>(viewer_controller, fields.team)) == ghost.team
								   && (decoy_mode() != 3 || is_suspect(ghost.viewer, now));
			if (!shared && !viewer_ok && ghost.viewer != k_max_players)
			{
				ghost.viewer = k_max_players;
				ghost.unused_since = now;
			}
			const uint8_t team = field<uint8_t>(controller, fields.team);
			if (team != ghost.team)
			{
				if (now - ghost.last_join_attempt >= k_ghost_join_interval)
				{
					if (ghost.join_attempts >= k_ghost_join_attempts)
					{
						fail(ghost, "a ghost player could not join its team (team full, mp_limitteams, or the game refused a fake client)");
						continue;
					}
					++ghost.join_attempts;
					ghost.last_join_attempt = now;
					char command[32];
					std::snprintf(command, sizeof(command), "jointeam %u", static_cast<unsigned>(ghost.team));
					ghost_command(ghost.slot, command);
					++ghost_counters_.join_commands;
				}
				continue;
			}
			ghost.join_attempts = 0;
			CEntityInstance* body = pawn(controller);
			const bool alive = body != nullptr && field<uint8_t>(body, fields.life_state) == k_life_alive && field<int32_t>(body, fields.health) > 0;
			const CEntityHandle handle = alive ? entity_handle(body) : CEntityHandle();
			const int32_t health = alive ? field<int32_t>(body, fields.health) : 0;
			// Kept harmless since the last update and still lost health: damage
			// reaches it, so it could absorb real players' shots or grenades.
			if (alive && ghost.harmless && handle == ghost.pawn && health < ghost.health)
			{
				fail(ghost, "a ghost player took damage although it should not");
				continue;
			}
			ghost.pawn = handle;
			ghost.health = health;
			ghost.harmless = false;
			if (!alive)
			{
				continue;
			}
			if (handle != ghost.traced_pawn)
			{
				ghost_trace("slot %d alive on team %u: making it harmless%s", ghost.slot, static_cast<unsigned>(team), shared ? " (crosshair ghost)" : "");
				ghost.traced_pawn = handle;
			}
			// Harmless: no damage, not solid, no trace touches it, it does not move.
			// A crosshair ghost keeps a player's collision, so its viewer's client
			// finds it under his crosshair as it finds any enemy (what triggerbots
			// read); it is shown only where nobody else can see or reach it.
			field<bool>(body, fields.takes_damage) = false;
			field<uint8_t>(body, fields.move_type) = k_move_type_none;
			field<uint8_t>(body, fields.actual_move_type) = k_move_type_none;
			if (!shared)
			{
				void* collision = reinterpret_cast<std::byte*>(body) + fields.model_collision;
				field<uint8_t>(collision, fields.solid_type) = k_solid_none;
				field<uint8_t>(collision, fields.solid_flags) |= k_solid_flag_not_solid;
				void* attribute = reinterpret_cast<std::byte*>(collision) + fields.collision_attribute;
				field<uint64_t>(attribute, fields.interacts_as) = 0;
				field<uint64_t>(attribute, fields.interacts_with) = 0;
			}
			if (compatibility_.shadow_strength_available())
			{
				field<float>(body, fields.shadow_strength) = 0.0f;
			}
			if (decoy_entity_mode() == 1)
			{
				field<uint8_t>(body, fields.render_mode) = compatibility_.render_none_value();
				field<uint8_t>(body, fields.render_color + 3) = 0;
			}
			ghost.harmless = true;
			// The bomb goes to a real player: take it out and drop it where it stands
			// (still at its spawn: a ghost carrying it is never used as a decoy).
			ghost.has_bomb = false;
			void* services = field<void*>(body, fields.weapon_services);
			auto* weapons = services == nullptr ? nullptr : reinterpret_cast<CUtlVector<CEntityHandle>*>(reinterpret_cast<uintptr_t>(services) + fields.weapons);
			for (int item = 0; weapons != nullptr && item < weapons->Count() && item < static_cast<int>(k_max_weapons); ++item)
			{
				CEntityInstance* weapon = system->GetEntityInstance((*weapons)[item]);
				const char* name = weapon == nullptr || weapon->m_pEntity == nullptr ? nullptr : weapon->m_pEntity->GetClassname();
				if (name != nullptr && std::strcmp(name, "weapon_c4") == 0)
				{
					ghost.has_bomb = true;
					if (ghost.bomb_since == std::chrono::steady_clock::time_point {})
					{
						ghost.bomb_since = now;
					}
					CEntityInstance* active = system->GetEntityInstance(field<CEntityHandle>(services, fields.active_weapon));
					if (active == weapon)
					{
						ghost_command(ghost.slot, "drop");
						++ghost_counters_.bomb_drops;
					}
					else
					{
						ghost_command(ghost.slot, "use weapon_c4");
					}
					break;
				}
			}
			if (!ghost.has_bomb)
			{
				ghost.bomb_since = {};
			}
			else if (now - ghost.bomb_since > k_ghost_bomb_limit)
			{
				// It could not drop it: leaving the server drops it for the team.
				META_CONPRINTF("[CS2GLAZ] ghost \"%s\" could not drop the bomb; it leaves so the bomb drops\n", ghost.name.c_str());
				kick_ghost(ghost, "cs2glaz ghost had the bomb");
				continue;
			}
			// Nobody else on its team alive: it must not keep the round going.
			if (alive_on_team(ghost.team) == 0)
			{
				// A crosshair ghost dies below the map, so the pistol it drops falls
				// out of the world instead of onto it.
				vec3 above;
				vec3 below;
				if (shared && crosshair_park_spots(above, below))
				{
					ghost_teleport(system, ghost, below, 0.0f);
				}
				ghost_command(ghost.slot, "kill");
				++ghost_counters_.suicides;
				ghost.pawn = CEntityHandle();
				ghost.harmless = false;
			}
		}
		// Makes a ghost on this team, for this viewer (k_max_players: none yet).
		const auto create = [&](uint8_t team, uint32_t viewer)
		{
			const auto on_team = std::count_if(ghosts_.begin(), ghosts_.end(), [&](const ghost_player& ghost) { return ghost.slot >= 0 && ghost.team == team; });
			const auto existing = std::count_if(ghosts_.begin(), ghosts_.end(), [](const ghost_player& ghost) { return ghost.slot >= 0; });
			auto free = std::find_if(ghosts_.begin(), ghosts_.end(), [](const ghost_player& ghost) { return ghost.slot < 0; });
			if (on_team != 0 || existing >= wanted || free == ghosts_.end() || now < ghost_next_create_)
			{
				return;
			}
			// Keep a slot free for a real player.
			INetworkGameServer* server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
			CGlobalVars* globals = server == nullptr ? nullptr : server->GetGlobals();
			const int max_clients = globals == nullptr ? 0 : std::min(globals->maxClients, static_cast<int>(k_max_players));
			int occupied = 0;
			for (int slot = 0; slot < max_clients; ++slot)
			{
				occupied += system->GetEntityInstance(CEntityIndex(slot + 1)) != nullptr ? 1 : 0;
			}
			ghost_next_create_ = now + k_ghost_create_cooldown;
			if (max_clients - occupied < 2)
			{
				return;
			}
			ghost_name_seed_ = ghost_name_seed_ * 1664525u + 1013904223u;
			const char* name = k_ghost_names[(ghost_name_seed_ >> 16) % std::size(k_ghost_names)];
			ghost_creating_ = true;
			ghost_trace("create fake client \"%s\" for team %u", name, static_cast<unsigned>(team));
			const CPlayerSlot created = engine_->CreateFakeClient(name);
			ghost_trace("created slot %d", created.Get());
			ghost_creating_ = false;
			if (created.Get() < 0 || created.Get() >= static_cast<int>(k_max_players))
			{
				META_CONPRINTF("[CS2GLAZ] ghost: the engine did not create a fake client (no free slot?)\n");
				return;
			}
			ghost_player& ghost = *free;
			ghost = {};
			ghost.slot = created.Get();
			ghost.viewer = viewer;
			ghost.team = team;
			ghost.name = name;
			ghost.created_at = now;
			ghost_kicked_at_[static_cast<size_t>(ghost.slot)] = {};
			// From now on its events stay off clients and its entities off snapshots.
			ghost_slots_.fetch_or(uint64_t {1} << ghost.slot);
			ghost_event_slots_.fetch_or(uint64_t {1} << ghost.slot);
			++ghost_counters_.created;
			if (viewer < k_max_players)
			{
				META_CONPRINTF("[CS2GLAZ] ghost \"%s\" (slot %d) created for viewer slot %u on team %u\n", name, ghost.slot, viewer, static_cast<unsigned>(team));
			}
			else
			{
				META_CONPRINTF("[CS2GLAZ] crosshair ghost \"%s\" (slot %d) created on team %u\n", name, ghost.slot, static_cast<unsigned>(team));
			}
		};
		if (shared)
		{
			// One per team while the other team has a player to show it to; a
			// ghost with nobody to show it to leaves after a while.
			for (const uint8_t team : {k_team_t, k_team_ct})
			{
				const bool needed = std::any_of(viewers.begin(), viewers.end(), [&](uint32_t viewer) { return enemy_team(value.players[viewer].team) == team; });
				for (ghost_player& ghost : ghosts_)
				{
					if (ghost.slot >= 0 && ghost.team == team)
					{
						ghost.unused_since = needed ? std::chrono::steady_clock::time_point {} : (ghost.unused_since == std::chrono::steady_clock::time_point {} ? now : ghost.unused_since);
					}
				}
				if (needed)
				{
					create(team, k_max_players);
				}
			}
		}
		else
		{
			// Assign free ghosts and create missing ones: one per enemy team.
			for (const uint32_t viewer : viewers)
			{
				const uint8_t team = enemy_team(value.players[viewer].team);
				if (team == 0 || std::any_of(ghosts_.begin(), ghosts_.end(), [&](const ghost_player& ghost) { return ghost.slot >= 0 && ghost.viewer == viewer; }))
				{
					continue;
				}
				auto idle = std::find_if(ghosts_.begin(), ghosts_.end(),
										 [&](const ghost_player& ghost) { return ghost.slot >= 0 && ghost.team == team && ghost.viewer == k_max_players; });
				if (idle != ghosts_.end())
				{
					idle->viewer = viewer;
					idle->unused_since = {};
					continue;
				}
				create(team, viewer);
			}
		}
		// Ghosts nobody needs for a while leave.
		for (size_t index = 0; index < ghosts_.size(); ++index)
		{
			ghost_player& ghost = ghosts_[index];
			if (ghost.slot >= 0 && ghost.viewer == k_max_players && ghost.unused_since != std::chrono::steady_clock::time_point {}
				&& now - ghost.unused_since >= k_ghost_unused_kick)
			{
				end_crosshair_turn(system, index, now);
				kick_ghost(ghost, "cs2glaz ghost unused");
			}
		}
		// Dead players watching a ghost are moved on (their client does not have it).
		if (compatibility_.observer_available())
		{
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				if (ghost_capture_slot(slot))
				{
					continue;
				}
				CEntityInstance* controller = system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1)));
				CEntityInstance* observer = controller == nullptr ? nullptr : system->GetEntityInstance(field<CEntityHandle>(controller, fields.observer_pawn));
				void* services = observer == nullptr ? nullptr : field<void*>(observer, fields.observer_services);
				if (services == nullptr)
				{
					continue;
				}
				CEntityHandle& target = field<CEntityHandle>(services, fields.observer_target);
				if (std::any_of(ghosts_.begin(), ghosts_.end(), [&](const ghost_player& ghost) { return ghost.pawn.IsValid() && target == ghost.pawn; }))
				{
					target = CEntityHandle();
					++ghost_counters_.observers_moved;
				}
			}
		}
		uint64_t slots = 0;
		for (size_t index = 0; index < ghosts_.size(); ++index)
		{
			const ghost_player& ghost = ghosts_[index];
			slots |= ghost.slot >= 0 ? uint64_t {1} << ghost.slot : 0;
			ghost_pawn_index_[index].store(ghost.pawn.IsValid() ? entity_index(system->GetEntityInstance(ghost.pawn)) : -1);
		}
		ghost_slots_.store(slots);
		ghost_event_slots_.store(slots | lingering);
	}

	bool plugin::ghost_take(CGameEntitySystem* system, uint32_t viewer, decoy_slot& slot, const std::string& model,
							std::chrono::steady_clock::time_point now)
	{
		if (system == nullptr || slot.control)
		{
			return false;
		}
		if (ghosts_shared_)
		{
			// Both ghosts are crosshair ghosts: hidden decoys use phantoms.
			return phantom_take(system, viewer, slot, model, now);
		}
		for (ghost_player& ghost : ghosts_)
		{
			if (ghost.slot < 0 || ghost.viewer != viewer || ghost.driving_id != 0 || !ghost.harmless || ghost.has_bomb || !ghost.pawn.IsValid()
				|| slot.target >= k_max_players || now - ghost.released_at < std::chrono::duration<float, std::milli>(k_decoy_reuse_quarantine_ms))
			{
				continue;
			}
			CEntityInstance* body = system->GetEntityInstance(ghost.pawn);
			void** vtable = nullptr;
			void* teleport = nullptr;
			if (body == nullptr || !runtime_compatibility::safe_read(body, &vtable, sizeof(vtable)) || vtable == nullptr
				|| !runtime_compatibility::safe_read(vtable + decoy_functions_.teleport_vtable_index, &teleport, sizeof(teleport))
				|| !compatibility_.address_in_server_module(teleport))
			{
				continue;
			}
			const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
			const QAngle angles(0.0f, slot.yaw, 0.0f);
			const Vector stop(0.0f, 0.0f, 0.0f);
			reinterpret_cast<void (*)(CEntityInstance*, const Vector*, const QAngle*, const Vector*)>(teleport)(body, &origin, &angles, &stop);
			slot.handle = ghost.pawn;
			slot.teleport = teleport;
			slot.model.clear();
			slot.spawned = true;
			slot.ghost = true;
			ghost.driving_id = slot.id;
			++ghost_counters_.runs;
			return true;
		}
		// No fake client free for him: a phantom, which takes no player slot.
		return phantom_take(system, viewer, slot, model, now);
	}

	void plugin::ghost_release(uint32_t decoy_id, std::chrono::steady_clock::time_point now)
	{
		for (ghost_player& ghost : ghosts_)
		{
			if (decoy_id != 0 && ghost.driving_id == decoy_id)
			{
				ghost.driving_id = 0;
				ghost.released_at = now;
			}
		}
		for (phantom_player& phantom : phantoms_)
		{
			if (decoy_id != 0 && phantom.driving_id == decoy_id)
			{
				phantom.driving_id = 0;
				phantom.released_at = now;
				phantom.idle_since = now;
			}
		}
	}

	void plugin::publish_ghost_transmit()
	{
		CGameEntitySystem* system = entity_system();
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		for (size_t index = 0; index < ghosts_.size(); ++index)
		{
			const ghost_player& ghost = ghosts_[index];
			ghost_transmit_entry& entry = ghost_transmit_[index];
			entry = {};
			if (ghost.slot < 0)
			{
				continue;
			}
			entry.controller = ghost.slot + 1;
			entry.viewer = ghost.viewer;
			entry.pawn = ghost.pawn;
			entry.shared = ghosts_shared_;
			const crosshair_turn& turn = crosshair_turns_[index];
			entry.presenting = ghosts_shared_ && turn.id != 0 && !turn.control && turn.viewer == ghost.viewer && ghost.pawn.IsValid();
			entry.turn = entry.presenting ? turn.id : 0;
			CEntityInstance* controller = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(ghost.slot + 1));
			if (controller != nullptr)
			{
				if (!entry.pawn.IsValid())
				{
					// Dead: its pawn is still an entity, and still never sent.
					entry.pawn = field<CEntityHandle>(controller, compatibility_.fields().player_pawn);
				}
				if (compatibility_.observer_available())
				{
					entry.observer = field<CEntityHandle>(controller, compatibility_.fields().observer_pawn);
				}
			}
		}
	}

	// After withhold_decoys: a ghost's controller goes to its viewer only; its
	// pawn reaches him only through the decoy slot it stands in for (already
	// decided), and its items, observer pawn and everything attached never go.
	void plugin::withhold_ghosts(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count)
	{
		if (system == nullptr || ghost_slots_.load(std::memory_order_relaxed) == 0)
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		for (size_t entry_index = 0; entry_index < ghost_transmit_.size(); ++entry_index)
		{
			const ghost_transmit_entry& entry = ghost_transmit_[entry_index];
			if (entry.controller <= 0)
			{
				continue;
			}
			CEntityInstance* body = entry.pawn.IsValid() ? system->GetEntityInstance(entry.pawn) : nullptr;
			const int pawn_index = entity_index(body);
			visual_entity_group group;
			attached_entity_group attached;
			const bool group_valid = body != nullptr && collect_player_visual_group(system, body, group);
			const bool attached_valid = group_valid && collect_attached_entities(system, body, group, attached);
			const int observer_index = entry.observer.IsValid() ? entity_index(system->GetEntityInstance(entry.observer)) : -1;
			for (int i = 0; i < count; ++i)
			{
				CCheckTransmitInfo* info = infos[i];
				if (info == nullptr || info->m_pTransmitEntity == nullptr || info->m_pTransmitAlways == nullptr)
				{
					continue;
				}
				int slot = -1;
				std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
				// A crosshair ghost reaches its viewer only while it is shown to him.
				const bool viewer = slot >= 0 && static_cast<uint32_t>(slot) == entry.viewer && (!entry.shared || entry.presenting);
				if (!viewer)
				{
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, entry.controller, withhold_reason::ghost);
					if (valid_networked_edict_index(pawn_index))
					{
						withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, pawn_index, withhold_reason::ghost);
					}
				}
				else if (entry.shared)
				{
					// The engine packed its pawn for him: the turn's ghost reaches him.
					if (valid_networked_edict_index(pawn_index) && info->m_pTransmitEntity->IsBitSet(pawn_index))
					{
						front_delivery& sent = crosshair_delivery_[entry_index];
						if (sent.run != entry.turn)
						{
							sent = {};
							sent.run = entry.turn;
							sent.first_sent = now;
						}
						sent.last_sent = now;
					}
				}
				else if (valid_networked_edict_index(pawn_index) && entry.pawn.IsValid()
						 && !std::any_of(decoy_transmit_[entry.viewer].begin(), decoy_transmit_[entry.viewer].end(),
										 [&](const decoy_transmit_entry& decoy) { return decoy.id != 0 && decoy.handle == entry.pawn; }))
				{
					// Not standing in for a decoy now: not sent to its viewer either.
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, pawn_index, withhold_reason::ghost);
				}
				if (valid_networked_edict_index(observer_index) && observer_index != pawn_index)
				{
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, observer_index, withhold_reason::ghost);
				}
				for (size_t member = 0; group_valid && member < group.count; ++member)
				{
					const int index = resolve_entity_index(system, group.handles[member]);
					if (valid_networked_edict_index(index) && index != pawn_index)
					{
						withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, index, withhold_reason::ghost);
					}
				}
				for (size_t member = 0; attached_valid && member < attached.count; ++member)
				{
					const int index = resolve_entity_index(system, attached.handles[member]);
					if (valid_networked_edict_index(index))
					{
						withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, index, withhold_reason::ghost);
					}
				}
			}
		}
	}

	bool plugin::ghost_event(IGameEvent* event) const
	{
		if (ghost_creating_)
		{
			return true;
		}
		if (event == nullptr || ghost_event_slots_.load(std::memory_order_relaxed) == 0)
		{
			return false;
		}
		for (const char* key : {"userid", "attacker", "assister"})
		{
			if (ghost_slot(event->GetPlayerSlot(game_event_key(key)).Get()))
			{
				return true;
			}
		}
		return false;
	}

	KHook::Return<bool> plugin::khook_fire_event(IGameEventManager2* manager, IGameEvent* event, bool dont_broadcast)
	{
		if (dont_broadcast || manager == nullptr || !ghost_event(event))
		{
			return {KHook::Action::Ignore, false};
		}
		// Fired for the server as usual, but not sent to any client: the whole
		// hook chain runs again with dont_broadcast set (as CounterStrikeSharp
		// does). Calling the original here and superseding crashed a server
		// (0.15.1): the original frees the event, and the other plugins hooking
		// FireEvent after this one were then handed the freed event.
		++ghost_counters_.events_hidden;
		return KHook::Recall(&IGameEventManager2::FireEvent, KHook::Return<bool> {KHook::Action::Ignore, false}, manager, event, true);
	}

	bool plugin::ghost_radar_entity(int index) const
	{
		if (index <= 0)
		{
			return false;
		}
		for (const std::atomic<int>& pawn : ghost_pawn_index_)
		{
			if (pawn.load(std::memory_order_relaxed) == index)
			{
				return true;
			}
		}
		return false;
	}

	void plugin::print_ghost_status() const
	{
		const int wanted = cs2glaz_decoy_ghosts.Get();
		std::string reason;
		const bool available = ghosts_available(reason);
		const auto value = [](uint64_t number) { return static_cast<unsigned long long>(number); };
		META_CONPRINTF("[CS2GLAZ] ghost players: wanted=%d %s created=%llu kicked=%llu runs=%llu join_commands=%llu suicides=%llu bomb_drops=%llu "
					   "observers_moved=%llu events_hidden=%llu radar_entries_dropped=%llu\n",
					   wanted, wanted == 0 ? "(off)" : (available ? "ready" : ("unavailable: " + reason).c_str()), value(ghost_counters_.created),
					   value(ghost_counters_.kicked), value(ghost_counters_.runs), value(ghost_counters_.join_commands), value(ghost_counters_.suicides),
					   value(ghost_counters_.bomb_drops), value(ghost_counters_.observers_moved), value(ghost_counters_.events_hidden),
					   value(ghost_counters_.radar_entries));
		for (const ghost_player& ghost : ghosts_)
		{
			if (ghost.slot < 0)
			{
				continue;
			}
			META_CONPRINTF("[CS2GLAZ] ghost \"%s\" slot %d team %u viewer %s%s%s%s\n", ghost.name.c_str(), ghost.slot, static_cast<unsigned>(ghost.team),
						   ghost.viewer < k_max_players ? std::to_string(ghost.viewer).c_str() : "none", ghost.pawn.IsValid() ? " alive" : " dead or joining",
						   ghost.driving_id != 0 ? " standing in for a decoy" : "", ghost.has_bomb ? " (dropping the bomb)" : "");
		}
		print_phantom_status();
	}

} // namespace cs2glaz
