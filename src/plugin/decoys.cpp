#include "plugin.h"

// Experimental decoys (cs2glaz_decoys, off by default). For each human viewer
// and each enemy hidden from him, a prop_dynamic with that enemy's model is
// placed on a floor spot the viewer cannot see, away from every real enemy,
// and sent to that viewer only, only while the worker proves it hidden from
// all of his viewing origins. It has no collision, no animation, no sound and
// no radar entry; in mode 1 it is not rendered at all. Aiming at it for half a
// second or shooting at it through the wall is logged for moderators, only
// while it actually reaches the viewer's client (and a reaction after), never
// for an aim that was already there or is also on a real player. One decoy in
// three is a control twin nobody receives (withheld from its viewer too):
// reports at twins per second of readiness measure the viewer's honest
// coincidences, and only real reports beyond them count as evidence.
//
// Creating entities needs server functions that limited mode does not know,
// so they are found by byte pattern (addons/cs2glaz/gamedata/
// cs2glaz.signatures.txt). If any pattern is missing or ambiguous, a field is
// missing, or a spawned decoy turns out solid or visible, decoys stay off;
// wall hiding is never affected.

#include "signature_scan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoys("cs2glaz_decoys", FCVAR_NONE,
									"Experimental decoys for wallhack users: 0 off, 1 invisible, 2 model drawn behind walls (testing); resets on restart",
									0, true, 0, true, 2);
		CConVar<int> cs2glaz_decoy_kick("cs2glaz_decoy_kick", FCVAR_NONE,
										"Kick a player once his decoy evidence (real reports beyond an honest player's, judged by control twins) reaches this; 0 only logs",
										0, true, 0, true, 100);

		using create_entity_fn = CEntityInstance* (*)(const char*, int);
		using dispatch_spawn_fn = void (*)(CEntityInstance*, void*);
		using remove_entity_fn = void (*)(CEntityInstance*);
		using set_model_fn = void (*)(CEntityInstance*, const char*);
		using teleport_fn = void (*)(CEntityInstance*, const Vector*, const QAngle*, const Vector*);

		constexpr auto k_decoy_slow_tick = std::chrono::milliseconds(250);
		constexpr float k_aim_report_ms = 500.0f;
		constexpr uint32_t k_spawns_per_update = 4;
		constexpr uint32_t k_candidates_per_update = 6;
		constexpr uint32_t k_max_teleport_vtable_index = 1024;
		constexpr uint8_t k_solid_none = 0;
		constexpr uint8_t k_solid_flag_not_solid = 0x04;
		// A decoy's health: a hit lowers it without killing it, which is how a
		// decoy that traces still reach is noticed.
		constexpr int32_t k_decoy_health = 1000000;
		// Parked decoys kept for reuse.
		constexpr size_t k_decoy_pool_size = 16;
		// A reported aim: on the decoy for k_aim_report_ms while the aim followed
		// it (see decoy_follow_degrees) by at least this much. Holding a fixed
		// angle it walks through, sweeping across it, or walking straight at it
		// does not count; keeping the crosshair on it while it or the viewer
		// moves does.
		constexpr float k_aim_min_tracking_degrees = 3.0f;
		// Walking decoys: player walking to running speed, pauses between legs.
		constexpr float k_walk_speed_min = 130.0f;
		constexpr uint32_t k_walk_speed_spread = 121;
		constexpr uint32_t k_pause_min_ms = 400;
		constexpr uint32_t k_pause_spread_ms = 2100;
		// Dropped when a real enemy or its viewer comes this close.
		constexpr float k_decoy_enemy_drop = 200.0f;
		constexpr float k_decoy_viewer_drop = 128.0f;
		// Shown to a kicked player; it names no decoy, so it teaches a cheat nothing.
		constexpr const char* k_decoy_kick_message = "Kicked by the server anti-cheat";


#if defined(_WIN32)
		constexpr std::string_view k_platform_suffix = "_windows";
#else
		constexpr std::string_view k_platform_suffix = "_linux";
#endif

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		std::string trim(std::string text)
		{
			const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
			text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), space));
			text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(), text.end());
			return text;
		}

		bool gun(const char* weapon)
		{
			if (weapon == nullptr || weapon[0] == '\0')
			{
				return false;
			}
			const std::string_view name(weapon);
			for (std::string_view word : {"knife", "bayonet", "grenade", "flashbang", "molotov", "decoy", "c4", "healthshot", "fists", "melee"})
			{
				if (name.find(word) != std::string_view::npos)
				{
					return false;
				}
			}
			return true;
		}

		float distance_units(vec3 a, vec3 b)
		{
			const float x = a.x - b.x;
			const float y = a.y - b.y;
			const float z = a.z - b.z;
			return std::sqrt(x * x + y * y + z * z);
		}

		std::string utc_timestamp()
		{
			const std::time_t now = std::time(nullptr);
			std::tm parts {};
#if defined(_WIN32)
			gmtime_s(&parts, &now);
#else
			gmtime_r(&now, &parts);
#endif
			char text[32] {};
			std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%SZ", &parts);
			return text;
		}

	} // namespace

	int plugin::decoy_mode() const
	{
		return std::clamp(cs2glaz_decoys.Get(), 0, 2);
	}

	bool plugin::human_player(uint32_t slot) const
	{
		if (engine_ == nullptr || slot >= k_max_players)
		{
			return false;
		}
		// Bots and SourceTV have no individual SteamID64 (0x0110000100000000 + account).
		const uint64_t xuid = engine_->GetClientXUID(CPlayerSlot(static_cast<int>(slot)));
		return (xuid >> 52u) == 0x011u && (xuid & 0xffffffffu) != 0;
	}

	void plugin::resolve_decoy_functions()
	{
		decoy_functions_ = {};
		decoy_functions_resolved_ = true;
		decoy_functions& functions = decoy_functions_;
		if (!compatibility_.valid() || api_ == nullptr)
		{
			functions.error = "CS2GLAZ is not active on this server build";
			return;
		}
		if (!compatibility_.decoy_schema_available())
		{
			functions.error = "missing schema fields (model name, collision or render mode)";
			return;
		}
		const std::filesystem::path path = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2glaz" / "gamedata" / "cs2glaz.signatures.txt";
		std::ifstream stream(path);
		if (!stream)
		{
			functions.error = "cannot read " + path.string();
			return;
		}
		std::unordered_map<std::string, std::string> values;
		std::string line;
		while (std::getline(stream, line))
		{
			line = trim(line);
			const size_t equals = line.find('=');
			if (line.empty() || line.rfind("//", 0) == 0 || equals == std::string::npos)
			{
				continue;
			}
			std::string key = trim(line.substr(0, equals));
			if (key.size() > k_platform_suffix.size() && key.compare(key.size() - k_platform_suffix.size(), k_platform_suffix.size(), k_platform_suffix) == 0)
			{
				key.resize(key.size() - k_platform_suffix.size());
				values[key] = trim(line.substr(equals + 1));
			}
		}
		const std::vector<std::span<const std::byte>> ranges = compatibility_.server_code_ranges();
		if (ranges.empty())
		{
			functions.error = "cannot find the server binary's code";
			return;
		}
		const auto find = [&](const char* key, void*& output)
		{
			byte_pattern pattern;
			const auto value = values.find(key);
			if (value == values.end() || !parse_byte_pattern(value->second, pattern))
			{
				functions.error += std::string(functions.error.empty() ? "" : ", ") + key + ": no valid pattern";
				return;
			}
			uint32_t count = 0;
			const std::byte* first = nullptr;
			for (const std::span<const std::byte>& range : ranges)
			{
				const pattern_matches matches = find_byte_pattern(range, pattern);
				if (matches.count != 0 && first == nullptr)
				{
					first = matches.first;
				}
				count += matches.count;
			}
			if (count != 1)
			{
				functions.error += std::string(functions.error.empty() ? "" : ", ") + key + (count == 0 ? ": not found" : ": found more than once");
				return;
			}
			output = const_cast<std::byte*>(first);
		};
		find("create_entity_by_name", functions.create_entity_by_name);
		find("dispatch_spawn", functions.dispatch_spawn);
		find("remove_entity", functions.remove_entity);
		find("set_model", functions.set_model);
		const auto teleport = values.find("teleport_vtable_index");
		char* end = nullptr;
		const unsigned long index = teleport == values.end() ? 0 : std::strtoul(teleport->second.c_str(), &end, 10);
		if (teleport == values.end() || end == teleport->second.c_str() || *end != '\0' || index == 0 || index > k_max_teleport_vtable_index)
		{
			functions.error += std::string(functions.error.empty() ? "" : ", ") + "teleport_vtable_index: missing or out of range";
		}
		else
		{
			functions.teleport_vtable_index = static_cast<uint32_t>(index);
		}
		functions.ready = functions.error.empty();
	}

	void plugin::discard_decoy_entity(CEntityInstance* entity)
	{
		if (entity == nullptr || decoy_functions_.remove_entity == nullptr)
		{
			return;
		}
		const CEntityHandle handle = entity_handle(entity);
		reinterpret_cast<remove_entity_fn>(decoy_functions_.remove_entity)(entity);
		if (handle.IsValid() && decoy_graveyard_.size() + decoy_pool_.size() < decoy_graveyard_transmit_.size())
		{
			decoy_graveyard_.push_back(handle);
		}
	}

	bool plugin::spawn_decoy(CGameEntitySystem* system, decoy_slot& slot, const std::string& model, int mode)
	{
		decoy_functions& functions = decoy_functions_;
		const auto fail = [&](CEntityInstance* entity, const char* reason)
		{
			discard_decoy_entity(entity);
			if (reason != nullptr)
			{
				// A decoy that could collide or be seen must never exist: stop for good.
				functions.ready = false;
				functions.error = reason;
				META_CONPRINTF("[CS2GLAZ] decoys turned off: %s\n", reason);
			}
			return false;
		};
		if (system == nullptr || model.empty() || model.size() > 255 || !functions.ready)
		{
			return false;
		}
		const schema_offsets& fields = compatibility_.fields();
		// Not solid, touched by no trace, (mode 1) not rendered, before and after
		// the spawn and the model; nothing has been sent yet, so the first send
		// carries these values.
		const auto apply = [&](CEntityInstance* entity)
		{
			void* collision = reinterpret_cast<std::byte*>(entity) + fields.model_collision;
			field<uint8_t>(collision, fields.solid_type) = k_solid_none;
			field<uint8_t>(collision, fields.solid_flags) |= k_solid_flag_not_solid;
			if (compatibility_.collision_attribute_available())
			{
				void* attribute = reinterpret_cast<std::byte*>(collision) + fields.collision_attribute;
				field<uint64_t>(attribute, fields.interacts_as) = 0;
				field<uint64_t>(attribute, fields.interacts_with) = 0;
			}
			field<int32_t>(entity, fields.health) = k_decoy_health;
			if (compatibility_.shadow_strength_available())
			{
				// No shadow either, which a drawn model (mode 2) could cast past its wall.
				field<float>(entity, fields.shadow_strength) = 0.0f;
			}
			if (mode == 1)
			{
				field<uint8_t>(entity, fields.render_mode) = compatibility_.render_none_value();
				field<uint8_t>(entity, fields.render_color + 3) = 0; // alpha
			}
		};
		CEntityInstance* entity = nullptr;
		void* teleport = nullptr;
		bool reused = false;
		while (entity == nullptr && !decoy_pool_.empty())
		{
			parked_decoy parked = std::move(decoy_pool_.back());
			decoy_pool_.pop_back();
			CEntityInstance* candidate = system->GetEntityInstance(parked.handle);
			if (candidate == nullptr)
			{
				continue;
			}
			if (field<int32_t>(candidate, fields.health) < k_decoy_health)
			{
				return fail(candidate, "a decoy was hit by a shot or a knife");
			}
			if (parked.mode != mode)
			{
				discard_decoy_entity(candidate);
				continue;
			}
			entity = candidate;
			teleport = parked.teleport;
			reused = true;
			apply(entity);
			if (parked.model != model)
			{
				reinterpret_cast<set_model_fn>(functions.set_model)(entity, model.c_str());
			}
		}
		if (entity == nullptr)
		{
			entity = reinterpret_cast<create_entity_fn>(functions.create_entity_by_name)("prop_dynamic", -1);
			if (entity == nullptr)
			{
				return false;
			}
			void** vtable = nullptr;
			if (!runtime_compatibility::safe_read(entity, &vtable, sizeof(vtable)) || vtable == nullptr
				|| !runtime_compatibility::safe_read(vtable + functions.teleport_vtable_index, &teleport, sizeof(teleport))
				|| !compatibility_.address_in_server_module(teleport))
			{
				return fail(entity, "the Teleport vtable index does not point into the server");
			}
			// Spawned before its model, so the prop never gets a physics object
			// (spawning with the model made it solid on 1.41.8); this logs "has
			// no model name" once per new prop, which reuse keeps rare.
			apply(entity);
			reinterpret_cast<dispatch_spawn_fn>(functions.dispatch_spawn)(entity, nullptr);
			apply(entity);
			reinterpret_cast<set_model_fn>(functions.set_model)(entity, model.c_str());
		}
		// Checked straight after the model is set, before anything is applied
		// again: a model that brought collision back must never be used.
		void* collision = reinterpret_cast<std::byte*>(entity) + fields.model_collision;
		if (field<uint8_t>(collision, fields.solid_type) != k_solid_none || (field<uint8_t>(collision, fields.solid_flags) & k_solid_flag_not_solid) == 0)
		{
			return fail(entity, "a spawned decoy was solid");
		}
		apply(entity);
		if (mode == 1 && field<uint8_t>(entity, fields.render_mode) != compatibility_.render_none_value())
		{
			return fail(entity, "a spawned decoy was rendered");
		}
		const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
		const QAngle angles(0.0f, slot.yaw, 0.0f);
		reinterpret_cast<teleport_fn>(teleport)(entity, &origin, &angles, nullptr);
		slot.handle = entity_handle(entity);
		if (!slot.handle.IsValid())
		{
			return fail(entity, nullptr);
		}
		slot.teleport = teleport;
		slot.model = model;
		slot.spawned = true;
		reused ? ++decoy_counters_.reused : ++decoy_counters_.spawned;
		return true;
	}

	void plugin::remove_decoy(CGameEntitySystem* system, decoy_slot& slot)
	{
		CEntityInstance* entity = slot.spawned && system != nullptr && slot.handle.IsValid() ? system->GetEntityInstance(slot.handle) : nullptr;
		if (entity != nullptr)
		{
			// Parked for the next decoy while there is room; withheld from everyone
			// meanwhile.
			const int mode = decoy_mode();
			if (decoy_functions_.ready && mode != 0 && slot.teleport != nullptr && decoy_pool_.size() < k_decoy_pool_size
				&& decoy_graveyard_.size() + decoy_pool_.size() < decoy_graveyard_transmit_.size())
			{
				decoy_pool_.push_back({slot.handle, slot.teleport, slot.model, mode});
			}
			else
			{
				discard_decoy_entity(entity);
			}
		}
		slot = {};
	}

	void plugin::remove_all_decoys(bool remove_entities)
	{
		CGameEntitySystem* system = remove_entities ? entity_system() : nullptr;
		if (system == nullptr)
		{
			decoy_graveyard_.clear();
		}
		for (auto& viewer : decoys_)
		{
			for (decoy_slot& slot : viewer)
			{
				if (slot.id != 0)
				{
					if (system != nullptr && slot.spawned && slot.handle.IsValid())
					{
						discard_decoy_entity(system->GetEntityInstance(slot.handle));
					}
					slot = {};
				}
			}
		}
		std::vector<parked_decoy> pool = std::move(decoy_pool_);
		decoy_pool_.clear();
		for (const parked_decoy& parked : pool)
		{
			if (system != nullptr)
			{
				discard_decoy_entity(system->GetEntityInstance(parked.handle));
			}
		}
		publish_decoy_transmit();
	}

	void plugin::prune_decoy_graveyard(CGameEntitySystem* system)
	{
		if (system == nullptr)
		{
			decoy_graveyard_.clear();
			decoy_pool_.clear();
			return;
		}
		std::erase_if(decoy_graveyard_, [&](CEntityHandle handle) { return system->GetEntityInstance(handle) == nullptr; });
		std::erase_if(decoy_pool_, [&](const parked_decoy& parked) { return system->GetEntityInstance(parked.handle) == nullptr; });
	}

	void plugin::publish_decoy_transmit()
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		bool live = false;
		for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
		{
			for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
			{
				const decoy_slot& slot = decoys_[viewer][index];
				decoy_transmit_entry& entry = decoy_transmit_[viewer][index];
				entry = slot.spawned ? decoy_transmit_entry {slot.handle, slot.id, slot.target, slot.control} : decoy_transmit_entry {};
				live = live || slot.spawned;
			}
		}
		uint32_t count = 0;
		for (const parked_decoy& parked : decoy_pool_)
		{
			if (count < decoy_graveyard_transmit_.size())
			{
				decoy_graveyard_transmit_[count++] = parked.handle;
			}
		}
		for (const CEntityHandle& handle : decoy_graveyard_)
		{
			if (count < decoy_graveyard_transmit_.size())
			{
				decoy_graveyard_transmit_[count++] = handle;
			}
		}
		decoy_graveyard_count_ = count;
		decoys_live_.store(live || decoy_graveyard_count_ != 0);
	}

	void plugin::update_decoys(CGameEntitySystem* system, visibility_snapshot& value, std::chrono::steady_clock::time_point now)
	{
		const std::array<view_sample, k_max_players> previous_view = last_view_;
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			const player_state& player = value.players[slot];
			last_view_[slot] = player.valid ? view_sample {true, player.eye, player.eye_pitch_degrees, player.eye_yaw_degrees} : view_sample {};
		}
		prune_decoy_graveyard(system);
		const int mode = decoy_mode();
		if (mode == 0 || !compatibility_.valid())
		{
			if (decoys_live_.load() || std::any_of(decoys_.begin(), decoys_.end(), [](const auto& row)
													{ return std::any_of(row.begin(), row.end(), [](const decoy_slot& slot) { return slot.id != 0; }); }))
			{
				remove_all_decoys(true);
			}
			return;
		}
		if (!decoy_functions_resolved_)
		{
			resolve_decoy_functions();
			if (decoy_functions_.ready)
			{
				META_CONPRINTF("[CS2GLAZ] decoys ready (experimental)\n");
			}
			else
			{
				META_CONPRINTF("[CS2GLAZ] decoys unavailable: %s\n", decoy_functions_.error.c_str());
			}
		}
		if (!decoy_functions_.ready)
		{
			if (decoys_live_.load())
			{
				remove_all_decoys(true);
			}
			return;
		}
		if (!weapon_fire_listening_ && game_events_ != nullptr)
		{
			weapon_fire_listening_ = game_events_->AddListener(this, "weapon_fire", true);
		}
		if (!bullet_impact_tried_ && weapon_fire_listening_ && game_events_ != nullptr)
		{
			// Tried once (game events are loaded by now): an unknown event must not
			// be retried every tick.
			bullet_impact_tried_ = true;
			bullet_impact_listening_ = game_events_->AddListener(this, "bullet_impact", true);
		}
		const bool slow_tick = now >= decoy_spots_next_;
		if (slow_tick)
		{
			decoy_spots_next_ = now + k_decoy_slow_tick;
			for (const player_state& player : value.players)
			{
				if (player.valid && (!player.has_velocity || std::fabs(player.velocity.z) < 5.0f))
				{
					decoy_spots_.record(player.origin);
				}
			}
		}
		const float elapsed_ms = decoy_last_update_ == std::chrono::steady_clock::time_point {}
									 ? 0.0f
									 : std::clamp(std::chrono::duration<float, std::milli>(now - decoy_last_update_).count(), 0.0f, 200.0f);
		decoy_last_update_ = now;
		const std::shared_ptr<const visibility_result> result = worker_.result();
		const bool fresh = result != nullptr && visibility_snapshot_fresh(result->captured, now);
		const auto enemies_of = [&](const player_state& viewer, uint32_t viewer_slot, uint32_t target)
		{
			const player_state& other = value.players[target];
			return other.valid && target != viewer_slot && (value.filter_teammates || other.team != viewer.team);
		};
		std::vector<vec3> living;
		for (const player_state& player : value.players)
		{
			if (player.valid)
			{
				living.push_back(player.origin);
			}
		}
		std::array<std::array<decoy_delivery, k_max_decoys_per_viewer>, k_max_players> delivery;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			delivery = decoy_delivery_;
		}
		uint32_t spawn_budget = k_spawns_per_update;
		uint32_t candidate_budget = k_candidates_per_update;
		for (uint32_t viewer_slot = 0; viewer_slot < k_max_players; ++viewer_slot)
		{
			const player_state& viewer = value.players[viewer_slot];
			const bool eligible = viewer.valid && human_player(viewer_slot);
			std::vector<vec3> enemies;
			std::vector<vec3> others; // every other living player, for aims a real player explains
			for (uint32_t target = 0; eligible && target < k_max_players; ++target)
			{
				if (enemies_of(viewer, viewer_slot, target))
				{
					enemies.push_back(value.players[target].origin);
				}
				if (target != viewer_slot && value.players[target].valid)
				{
					others.push_back(value.players[target].origin);
				}
			}
			const float rtt_ms = viewer.rtt_seconds * 1000.0f;
			decoy_player_record* record = eligible ? decoy_record(viewer_slot) : nullptr;
			if (record != nullptr && record->name.empty())
			{
				record->name = slot_name(system, viewer_slot);
			}
			const auto too_close = [&](vec3 point)
			{
				const auto within = [&](vec3 other, float distance)
				{
					const float x = point.x - other.x;
					const float y = point.y - other.y;
					const float z = point.z - other.z;
					return x * x + y * y + z * z < distance * distance;
				};
				return within(viewer.origin, k_decoy_viewer_drop)
					   || std::any_of(enemies.begin(), enemies.end(), [&](vec3 enemy) { return within(enemy, k_decoy_enemy_drop); });
			};
			for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
			{
				decoy_slot& slot = decoys_[viewer_slot][index];
				if (slot.id == 0)
				{
					continue;
				}
				bool drop = !eligible || !enemies_of(viewer, viewer_slot, slot.target) || now >= slot.expires || too_close(slot.origin);
				CEntityInstance* entity = slot.spawned ? system->GetEntityInstance(slot.handle) : nullptr;
				if (slot.spawned && entity == nullptr)
				{
					// Removed by the game (round restart cleanup).
					slot = {};
					continue;
				}
				if (entity != nullptr && field<int32_t>(entity, compatibility_.fields().health) < k_decoy_health)
				{
					// A shot or a knife reached it: it would absorb honest players' shots.
					decoy_functions_.ready = false;
					decoy_functions_.error = "a decoy was hit by a shot or a knife";
					META_CONPRINTF("[CS2GLAZ] decoys turned off: a decoy was hit by a shot or a knife; they stay off until the plugin reloads\n");
					break;
				}
				const bool checked = !drop && fresh && result->decoys[viewer_slot][index].id == slot.id;
				// A worker out of time proves nothing either way: the decoy is not
				// sent this tick (CheckTransmit needs it proven hidden), but it is
				// not taken as seen.
				const bool proven = checked && result->decoy_proven[viewer_slot][index];
				if (checked && result->visible[viewer_slot][slot.target])
				{
					drop = true; // the real enemy is in view now
				}
				else if (proven && !result->decoy_hidden[viewer_slot][index])
				{
					drop = true;
					if (slot.spawned)
					{
						++decoy_counters_.exposed;
					}
				}
				else if (proven && !slot.spawned && spawn_budget > 0)
				{
					// Controls are created too: twins of the real ones in every way
					// but delivery, so they meet the same PVS and the same checks.
					--spawn_budget;
					const int pawn_index = value.players[slot.target].pawn_entity;
					CEntityInstance* pawn = pawn_index > 0 ? system->GetEntityInstance(CEntityIndex(pawn_index)) : nullptr;
					const std::string model = entity_model_name(pawn);
					if (!spawn_decoy(system, slot, model, mode))
					{
						++decoy_counters_.spawn_failures;
						drop = true;
					}
					else
					{
						// A new decoy stands a moment before it starts walking.
						slot.spawned_at = now;
						slot.pause_until = now + std::chrono::milliseconds(k_pause_min_ms + decoy_random(decoy_seed_) % k_pause_spread_ms);
						if (slot.control)
						{
							++decoy_counters_.controls;
						}
					}
				}
				// Whether it has reached the viewer long enough to be acted on, by
				// what CheckTransmit saw the engine do: outside his PVS a decoy is
				// not sent, and a wallhack cannot see it. A control twin counts as
				// reaching him when the engine would have sent it.
				const decoy_delivery& sent = delivery[viewer_slot][index];
				const bool delivered_now = sent.id == slot.id && now - sent.last_sent <= k_decoy_delivery_gap;
				const auto delivered_since = sent.first_sent;
				if (!drop && slot.spawned)
				{
					const auto last_reached = sent.id == slot.id ? std::max(sent.last_sent, slot.spawned_at) : slot.spawned_at;
					if (now - last_reached >= k_decoy_undelivered_limit)
					{
						// Outside his PVS it reaches nobody: try a spot that is not.
						drop = true;
						++decoy_counters_.undelivered;
					}
				}
				const bool aim_on = aim_on_decoy(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, slot.origin);
				const bool ready = !drop && checked && slot.spawned
								   && decoy_delivery_ready(delivered_now, std::chrono::duration<float, std::milli>(now - delivered_since).count(), rtt_ms);
				if (!ready)
				{
					// Not on his screen yet (as far as any cheat could know): a
					// crosshair already there was put there without it.
					slot.aimed_before_ready = aim_on;
				}
				else if (!aim_on)
				{
					slot.aimed_before_ready = false;
				}
				slot.ready = ready && !slot.aimed_before_ready;
				if (slot.ready && record != nullptr)
				{
					// Seconds of readiness: what an honest player's coincidences
					// are counted against (decoy_expected_reports).
					const double seconds = std::max(elapsed_ms, 0.0f) / 1000.0;
					(slot.control ? record->control_seconds : record->real_seconds) += seconds;
					(slot.control ? decoy_server_.control_seconds : decoy_server_.real_seconds) += seconds;
					record->this_map = true;
				}
				if (!slot.ready)
				{
					slot.aim_ms = 0.0f;
					slot.aim_turn = 0.0f;
				}
				else
				{
					if (aim_on && !aim_on_any_player(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others))
					{
						const view_sample& before = previous_view[viewer_slot];
						if (slot.aim_ms > 0.0f && before.valid)
						{
							slot.aim_turn += decoy_follow_degrees(before.eye, view_forward(before.pitch, before.yaw), slot.aim_origin, viewer.eye,
																  view_forward(viewer.eye_pitch_degrees, viewer.eye_yaw_degrees), slot.origin);
						}
						slot.aim_origin = slot.origin;
						slot.aim_ms += std::max(elapsed_ms, 0.001f);
						if (slot.aim_ms >= k_aim_report_ms && slot.aim_turn >= k_aim_min_tracking_degrees && !slot.aim_reported)
						{
							slot.aim_reported = true;
							report_decoy(system, viewer_slot, slot, false, distance_units(viewer.eye, slot.origin));
						}
					}
					else
					{
						slot.aim_ms = 0.0f;
						slot.aim_turn = 0.0f;
					}
				}
				if (!drop && slot.spawned && entity != nullptr && slot.teleport != nullptr)
				{
					std::vector<vec3> taken;
					for (const decoy_slot& other : decoys_[viewer_slot])
					{
						if (&other != &slot && other.id != 0)
						{
							taken.push_back(other.origin);
						}
					}
					walk_decoy(entity, slot, viewer, enemies, living, taken, now, elapsed_ms, value);
				}
				if (drop)
				{
					remove_decoy(system, slot);
				}
			}
			if (!decoy_functions_.ready)
			{
				break;
			}
			if (eligible && slow_tick && fresh && result->players[viewer_slot].valid)
			{
				for (uint32_t target = 0; target < k_max_players && candidate_budget > 0; ++target)
				{
					auto& row = decoys_[viewer_slot];
					if (!enemies_of(viewer, viewer_slot, target) || !result->players[target].valid || result->visible[viewer_slot][target]
						|| std::any_of(row.begin(), row.end(), [&](const decoy_slot& slot) { return slot.id != 0 && slot.target == target; }))
					{
						continue;
					}
					const auto free = std::find_if(row.begin(), row.end(), [](const decoy_slot& slot) { return slot.id == 0; });
					if (free == row.end())
					{
						break;
					}
					std::vector<vec3> taken;
					for (const decoy_slot& slot : row)
					{
						if (slot.id != 0)
						{
							taken.push_back(slot.origin);
						}
					}
					--candidate_budget;
					vec3 spot;
					const decoy_spot_query query {viewer.eye, enemies, living, taken, decoy_random(decoy_seed_), viewer.eye_yaw_degrees};
					if (!choose_decoy_spot(data_, value.occluders, decoy_spots_, query, spot))
					{
						continue;
					}
					decoy_slot& slot = *free;
					slot = {};
					slot.id = ++decoy_next_id_ == 0 ? ++decoy_next_id_ : decoy_next_id_;
					slot.target = target;
					slot.control = decoy_random(decoy_seed_) % k_decoy_control_one_in == 0;
					slot.origin = spot;
					slot.yaw = static_cast<float>(decoy_random(decoy_seed_) % 360u);
					slot.expires = now + std::chrono::milliseconds(8000 + decoy_random(decoy_seed_) % 7000u);
					++decoy_counters_.candidates;
				}
			}
		}
		for (uint32_t viewer_slot = 0; viewer_slot < k_max_players; ++viewer_slot)
		{
			for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
			{
				const decoy_slot& slot = decoys_[viewer_slot][index];
				value.decoys[viewer_slot][index] = slot.id != 0 ? decoy_probe {slot.id, slot.origin} : decoy_probe {};
			}
		}
		if (!decoy_functions_.ready)
		{
			remove_all_decoys(true);
			return;
		}
		publish_decoy_transmit();
	}

	void plugin::walk_decoy(CEntityInstance* entity, decoy_slot& slot, const player_state& viewer, std::span<const vec3> enemies,
							std::span<const vec3> living, std::span<const vec3> taken, std::chrono::steady_clock::time_point now, float elapsed_ms,
							const visibility_snapshot& value)
	{
		if (!slot.moving)
		{
			if (now < slot.pause_until)
			{
				return;
			}
			vec3 goal;
			const decoy_spot_query query {viewer.eye, enemies, living, taken, decoy_random(decoy_seed_)};
			if (!choose_decoy_step(data_, value.occluders, decoy_spots_, query, slot.origin, goal))
			{
				slot.pause_until = now + std::chrono::milliseconds(k_pause_min_ms);
				return;
			}
			slot.goal = goal;
			slot.speed = k_walk_speed_min + static_cast<float>(decoy_random(decoy_seed_) % k_walk_speed_spread);
			slot.moving = true;
		}
		const float dx = slot.goal.x - slot.origin.x;
		const float dy = slot.goal.y - slot.origin.y;
		const float dz = slot.goal.z - slot.origin.z;
		const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float step = slot.speed * elapsed_ms / 1000.0f;
		if (length <= step || length < 1.0f)
		{
			slot.origin = slot.goal;
			slot.moving = false;
			slot.pause_until = now + std::chrono::milliseconds(k_pause_min_ms + decoy_random(decoy_seed_) % k_pause_spread_ms);
		}
		else
		{
			slot.origin = {slot.origin.x + dx / length * step, slot.origin.y + dy / length * step, slot.origin.z + dz / length * step};
		}
		if (dx * dx + dy * dy > 1.0f)
		{
			slot.yaw = std::atan2(dy, dx) * 57.29578f;
		}
		const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
		const QAngle angles(0.0f, slot.yaw, 0.0f);
		reinterpret_cast<teleport_fn>(slot.teleport)(entity, &origin, &angles, nullptr);
	}

	void plugin::withhold_decoys(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count, const visibility_result* result,
								 std::chrono::steady_clock::time_point now)
	{
		if (!decoys_live_.load() || system == nullptr)
		{
			return;
		}
		const bool fresh = result != nullptr && visibility_snapshot_fresh(result->captured, now);
		std::array<std::array<int, k_max_decoys_per_viewer>, k_max_players> indices {};
		for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
		{
			for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
			{
				const decoy_transmit_entry& entry = decoy_transmit_[viewer][index];
				CEntityInstance* entity = entry.id != 0 && entry.handle.IsValid() ? system->GetEntityInstance(entry.handle) : nullptr;
				const int edict = entity_index(entity);
				indices[viewer][index] = valid_networked_edict_index(edict) ? edict : -1;
			}
		}
		std::array<int, k_max_players * k_max_decoys_per_viewer> removed {};
		for (uint32_t index = 0; index < decoy_graveyard_count_; ++index)
		{
			const int edict = entity_index(system->GetEntityInstance(decoy_graveyard_transmit_[index]));
			removed[index] = valid_networked_edict_index(edict) ? edict : -1;
		}
		for (int i = 0; i < count; ++i)
		{
			CCheckTransmitInfo* info = infos[i];
			if (info == nullptr || info->m_pTransmitEntity == nullptr || info->m_pTransmitAlways == nullptr)
			{
				continue;
			}
			for (uint32_t index = 0; index < decoy_graveyard_count_; ++index)
			{
				if (removed[index] >= 0)
				{
					apply_transmit_mode(info->m_pTransmitEntity, info->m_pTransmitAlways, removed[index], transmit_mode::clear_both);
				}
			}
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
			{
				for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
				{
					const int edict = indices[viewer][index];
					if (edict < 0)
					{
						continue;
					}
					const decoy_transmit_entry& entry = decoy_transmit_[viewer][index];
					const bool allowed = slot == static_cast<int>(viewer) && fresh && result->decoys[viewer][index].id == entry.id
										 && result->decoy_hidden[viewer][index] && result->players[viewer].valid && entry.target < k_max_players
										 && !result->visible[viewer][entry.target];
					if (allowed && info->m_pTransmitEntity->IsBitSet(edict))
					{
						// The engine packed it for its viewer: a real decoy reaches his
						// client now, a control twin would have (same PVS), and both
						// count as delivered from here.
						if (!entry.control)
						{
							++decoy_counters_.ticks_sent;
						}
						decoy_delivery& sent = decoy_delivery_[viewer][index];
						if (sent.id != entry.id || now - sent.last_sent > k_decoy_delivery_gap)
						{
							sent.id = entry.id;
							sent.first_sent = now;
						}
						sent.last_sent = now;
					}
					else if (allowed && !entry.control)
					{
						++decoy_counters_.ticks_outside_pvs;
					}
					// Bits are only ever cleared. Setting one for an entity the engine
					// did not pack this frame makes the client fail with "CopyExistingEntity:
					// missing client entity" and crash. A control twin is cleared even
					// for its viewer: no client ever has it.
					if (!allowed || entry.control)
					{
						apply_transmit_mode(info->m_pTransmitEntity, info->m_pTransmitAlways, edict, transmit_mode::clear_both);
					}
				}
			}
		}
	}

	void plugin::decoy_weapon_fire(IGameEvent* event)
	{
		if (event == nullptr || decoy_mode() == 0 || !decoy_functions_.ready || !gun(event->GetString(game_event_key("weapon"), "")))
		{
			return;
		}
		const int shooter = event->GetPlayerSlot(game_event_key("userid")).Get();
		if (shooter < 0 || shooter >= static_cast<int>(k_max_players))
		{
			return;
		}
		// The view as the shot is fired: this event comes while the shooting
		// command runs, and the last capture is a tick older.
		view_sample view;
		if (!live_view(static_cast<uint32_t>(shooter), view))
		{
			view = last_view_[shooter];
		}
		if (view.valid)
		{
			decoy_shot(static_cast<uint32_t>(shooter), view.eye, view_forward(view.pitch, view.yaw));
		}
	}

	void plugin::decoy_bullet_impact(IGameEvent* event)
	{
		if (event == nullptr || decoy_mode() == 0 || !decoy_functions_.ready)
		{
			return;
		}
		const int shooter = event->GetPlayerSlot(game_event_key("userid")).Get();
		if (shooter < 0 || shooter >= static_cast<int>(k_max_players))
		{
			return;
		}
		// Where the bullet went, not where the screen looked: an aimbot that
		// turns only the shot (silent aim) and subtick shots show up here.
		const float missing = std::numeric_limits<float>::quiet_NaN();
		const vec3 impact {event->GetFloat(game_event_key("x"), missing), event->GetFloat(game_event_key("y"), missing),
						   event->GetFloat(game_event_key("z"), missing)};
		view_sample view;
		if (!live_view(static_cast<uint32_t>(shooter), view))
		{
			view = last_view_[shooter];
		}
		if (view.valid && std::isfinite(impact.x) && std::isfinite(impact.y) && std::isfinite(impact.z))
		{
			decoy_shot(static_cast<uint32_t>(shooter), view.eye, {impact.x - view.eye.x, impact.y - view.eye.y, impact.z - view.eye.z});
		}
	}

	void plugin::decoy_shot(uint32_t shooter, vec3 eye, vec3 direction)
	{
		const std::shared_ptr<const visibility_result> result = worker_.result();
		const auto now = std::chrono::steady_clock::now();
		if (shooter >= k_max_players || result == nullptr || !visibility_snapshot_fresh(result->captured, now))
		{
			return;
		}
		CGameEntitySystem* system = entity_system();
		std::vector<vec3> others;
		for (uint32_t player = 0; player < k_max_players; ++player)
		{
			if (player != shooter && result->players[player].valid)
			{
				others.push_back(result->players[player].origin);
			}
		}
		for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
		{
			decoy_slot& slot = decoys_[shooter][index];
			if (!slot.spawned || !slot.ready || slot.shot_reported || result->decoys[shooter][index].id != slot.id || !result->decoy_hidden[shooter][index]
				|| !direction_on_decoy(eye, direction, slot.origin) || direction_on_any_player(eye, direction, others))
			{
				continue;
			}
			slot.shot_reported = true;
			report_decoy(system, shooter, slot, true, distance_units(eye, slot.origin));
		}
	}

	decoy_player_record* plugin::decoy_record(uint32_t viewer)
	{
		if (engine_ == nullptr || viewer >= k_max_players)
		{
			return nullptr;
		}
		// Kept by SteamID64, so a record follows its player across maps and
		// reconnects; bots and SourceTV have none.
		const uint64_t xuid = engine_->GetClientXUID(CPlayerSlot(static_cast<int>(viewer)));
		if ((xuid >> 52u) != 0x011u || (xuid & 0xffffffffu) == 0)
		{
			return nullptr;
		}
		auto found = decoy_players_.find(xuid);
		if (found == decoy_players_.end())
		{
			if (decoy_players_.size() >= k_max_decoy_players)
			{
				// Players with nothing against them go first.
				std::erase_if(decoy_players_, [](const auto& entry) { return entry.second.aims + entry.second.shots == 0; });
				if (decoy_players_.size() >= k_max_decoy_players)
				{
					return nullptr;
				}
			}
			found = decoy_players_.emplace(xuid, decoy_player_record {}).first;
		}
		return &found->second;
	}

	void plugin::report_decoy(CGameEntitySystem* system, uint32_t viewer, const decoy_slot& slot, bool shot, float distance)
	{
		decoy_player_record* record = decoy_record(viewer);
		if (record == nullptr)
		{
			return;
		}
		const uint64_t xuid = engine_->GetClientXUID(CPlayerSlot(static_cast<int>(viewer)));
		record->name = slot_name(system, viewer);
		record->this_map = true;
		if (slot.control)
		{
			// No client ever had it: an honest coincidence, the baseline the real
			// reports are weighed against. Logged for the record only.
			shot ? ++record->control_shots : ++record->control_aims;
			shot ? ++decoy_counters_.control_shots : ++decoy_counters_.control_aims;
			++decoy_server_.control_reports;
			write_decoy_log(xuid, shot ? "control_shot" : "control_aim", static_cast<int>(distance), *record);
			return;
		}
		shot ? ++record->shots : ++record->aims;
		shot ? ++decoy_counters_.shots : ++decoy_counters_.aims;
		++decoy_server_.real_reports;
		const decoy_exposure exposure = record->exposure();
		const double expected = decoy_expected_reports(exposure, decoy_server_);
		const double evidence = decoy_evidence(exposure, decoy_server_);
		META_CONPRINTF("[CS2GLAZ] decoy: \"%s\" %llu %s a decoy through a wall (%.0f units). Since load: %llu at real decoys in %.0f s of them, "
					   "%llu at controls in %.0f s; an honest player would have about %.1f; evidence %.1f - suspect, check the demo\n",
					   record->name.c_str(), static_cast<unsigned long long>(xuid), shot ? "shot at" : "aimed at", distance,
					   static_cast<unsigned long long>(exposure.real_reports), exposure.real_seconds,
					   static_cast<unsigned long long>(exposure.control_reports), exposure.control_seconds, expected, evidence);
		write_decoy_log(xuid, shot ? "shot" : "aim", static_cast<int>(distance), *record);
		const int threshold = cs2glaz_decoy_kick.Get();
		if (!human_player(viewer) || !decoy_kick_due(evidence, threshold, record->evidence_at_kick))
		{
			return;
		}
		// Queued as a console command, so the client leaves at a safe point of the
		// frame, never inside this event or the transmit hook.
		const int userid = engine_->GetPlayerUserId(CPlayerSlot(static_cast<int>(viewer))).Get();
		if (userid < 0)
		{
			return;
		}
		record->evidence_at_kick = evidence;
		++record->kicks;
		++decoy_counters_.kicks;
		char command[128] {};
		std::snprintf(command, sizeof(command), "kickid %d \"%s\"\n", userid, k_decoy_kick_message);
		engine_->ServerCommand(command);
		META_CONPRINTF("[CS2GLAZ] decoy: kicked \"%s\" %llu at decoy evidence %.1f (cs2glaz_decoy_kick %d)\n", record->name.c_str(),
					   static_cast<unsigned long long>(xuid), evidence, threshold);
		write_decoy_log(xuid, "kick", static_cast<int>(distance), *record);
	}

	void plugin::write_decoy_log(uint64_t xuid, const char* event, int distance, const decoy_player_record& record) const
	{
		if (api_ == nullptr)
		{
			return;
		}
		std::error_code error;
		const std::filesystem::path directory = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2glaz" / "logs";
		std::filesystem::create_directories(directory, error);
		std::ofstream log(directory / "decoys.log", std::ios::app);
		if (!log)
		{
			return;
		}
		const decoy_exposure exposure = record.exposure();
		char numbers[192] {};
		std::snprintf(numbers, sizeof(numbers), "real_seconds=%.0f control_seconds=%.0f expected=%.2f evidence=%.2f", exposure.real_seconds,
					  exposure.control_seconds, decoy_expected_reports(exposure, decoy_server_), decoy_evidence(exposure, decoy_server_));
		log << utc_timestamp() << " map=" << map_ << " player=\"" << record.name << "\" steamid=" << xuid << " event=" << event;
		if (distance >= 0)
		{
			log << " distance=" << distance;
		}
		log << " aims=" << record.aims << " shots=" << record.shots << " control_aims=" << record.control_aims << " control_shots=" << record.control_shots
			<< " " << numbers << "\n";
	}

	void plugin::write_decoy_map_summary()
	{
		// One line per player the decoys met on this map, with his record since
		// load: readiness included, which the event lines alone do not show.
		for (auto& [xuid, record] : decoy_players_)
		{
			if (record.this_map)
			{
				write_decoy_log(xuid, "map_summary", -1, record);
				record.this_map = false;
			}
		}
	}

	void plugin::print_decoy_status() const
	{
		const int mode = decoy_mode();
		uint32_t live = 0;
		uint32_t controls = 0;
		uint32_t pending = 0;
		for (const auto& row : decoys_)
		{
			for (const decoy_slot& slot : row)
			{
				!slot.spawned ? (slot.id != 0 ? ++pending : 0u) : slot.control ? ++controls : ++live;
			}
		}
		decoy_counters counters;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			counters = decoy_counters_;
		}
		const auto value = [](uint64_t number) { return static_cast<unsigned long long>(number); };
		META_CONPRINTF("[CS2GLAZ] decoys mode=%d (%s) %s live=%u controls=%u pending=%u parked=%zu spots=%zu created=%llu reused=%llu exposed=%llu "
					   "undelivered=%llu failed=%llu\n",
					   mode, mode == 0 ? "off" : mode == 1 ? "invisible" : "drawn",
					   !decoy_functions_resolved_ ? "not checked yet" : decoy_functions_.ready ? "ready" : "unavailable", live, controls, pending,
					   decoy_pool_.size(), decoy_spots_.points().size(), value(counters.spawned), value(counters.reused), value(counters.exposed),
					   value(counters.undelivered), value(counters.spawn_failures));
		if (decoy_functions_resolved_ && !decoy_functions_.ready)
		{
			META_CONPRINTF("[CS2GLAZ] decoys: %s\n", decoy_functions_.error.c_str());
		}
		if (mode != 0)
		{
			// A decoy reaches its viewer only while the engine counts it in his PVS.
			META_CONPRINTF("[CS2GLAZ] decoy transmit ticks this map: sent=%llu outside_pvs=%llu (outside the PVS a decoy is not sent); shots from "
						   "the view at weapon_fire%s\n",
						   value(counters.ticks_sent), value(counters.ticks_outside_pvs),
						   bullet_impact_listening_ ? " and bullet_impact" : (bullet_impact_tried_ ? " (bullet_impact unavailable)" : ""));
		}
		// The server's honest coincidences: what every player's real reports are
		// weighed against until his own controls say more.
		META_CONPRINTF("[CS2GLAZ] decoy reports since load: aims=%llu shots=%llu at real decoys over %.0f s; aims=%llu shots=%llu at control twins "
					   "over %.0f s (an honest player's rate: %.2f per minute of decoys); kicks=%llu (kick at evidence %d, 0 = log only)\n",
					   value(counters.aims), value(counters.shots), decoy_server_.real_seconds, value(counters.control_aims), value(counters.control_shots),
					   decoy_server_.control_seconds,
					   60.0 * decoy_server_rate(decoy_server_), value(counters.kicks),
					   cs2glaz_decoy_kick.Get());
		std::vector<std::pair<uint64_t, const decoy_player_record*>> suspects;
		for (const auto& [xuid, record] : decoy_players_)
		{
			if (record.aims + record.shots != 0)
			{
				suspects.emplace_back(xuid, &record);
			}
		}
		std::sort(suspects.begin(), suspects.end(),
				  [&](const auto& left, const auto& right)
				  {
					  const double a = decoy_evidence(left.second->exposure(), decoy_server_);
					  const double b = decoy_evidence(right.second->exposure(), decoy_server_);
					  return a != b ? a > b : left.second->aims + left.second->shots > right.second->aims + right.second->shots;
				  });
		for (size_t index = 0; index < suspects.size() && index < 10; ++index)
		{
			const decoy_player_record& record = *suspects[index].second;
			const decoy_exposure exposure = record.exposure();
			META_CONPRINTF("[CS2GLAZ] decoy suspect: \"%s\" %llu aims=%u shots=%u in %.0f s of real decoys; control aims=%u shots=%u in %.0f s; "
						   "expected %.1f; evidence %.1f; kicks=%u\n",
						   record.name.c_str(), value(suspects[index].first), record.aims, record.shots, exposure.real_seconds, record.control_aims,
						   record.control_shots, exposure.control_seconds, decoy_expected_reports(exposure, decoy_server_),
						   decoy_evidence(exposure, decoy_server_), record.kicks);
		}
	}

} // namespace cs2glaz
