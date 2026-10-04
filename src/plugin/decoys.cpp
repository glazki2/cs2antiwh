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
									"Experimental decoys for wallhack users: 0 off, 1 invisible, 2 model drawn behind walls (testing), 3 invisible and only "
									"for watched players (CSVILKA detections, cs2glaz_suspect); resets on restart",
									0, true, 0, true, 3);
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
		return std::clamp(cs2glaz_decoys.Get(), 0, 3);
	}

	int plugin::decoy_entity_mode() const
	{
		const int mode = decoy_mode();
		return mode == 0 ? 0 : (mode == 2 ? 2 : 1);
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

	bool plugin::read_signatures(std::unordered_map<std::string, std::string>& values, std::string& error) const
	{
		values.clear();
		if (api_ == nullptr)
		{
			error = "Metamod is unavailable";
			return false;
		}
		const std::filesystem::path path = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2glaz" / "gamedata" / "cs2glaz.signatures.txt";
		std::ifstream stream(path);
		if (!stream)
		{
			error = "cannot read " + path.string();
			return false;
		}
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
		return true;
	}

	const std::byte* plugin::find_unique_server_pattern(const std::unordered_map<std::string, std::string>& values, const char* key,
														 std::string& error) const
	{
		byte_pattern pattern;
		const auto value = values.find(key);
		if (value == values.end() || !parse_byte_pattern(value->second, pattern))
		{
			error = std::string(key) + ": no valid pattern";
			return nullptr;
		}
		const std::vector<std::span<const std::byte>> ranges = compatibility_.server_code_ranges();
		if (ranges.empty())
		{
			error = "cannot find the server binary's code";
			return nullptr;
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
			error = std::string(key) + (count == 0 ? ": not found" : ": found more than once");
			return nullptr;
		}
		return first;
	}

	void plugin::resolve_decoy_functions()
	{
		decoy_functions_ = find_decoy_functions();
		decoy_functions_resolved_ = true;
	}

	decoy_functions plugin::find_decoy_functions() const
	{
		decoy_functions functions;
		if (!compatibility_.valid() || api_ == nullptr)
		{
			functions.error = "CS2GLAZ is not active on this server build";
			return functions;
		}
		if (!compatibility_.decoy_schema_available())
		{
			functions.error = "missing schema fields (model name, collision or render mode)";
			return functions;
		}
		std::unordered_map<std::string, std::string> values;
		if (!read_signatures(values, functions.error))
		{
			return functions;
		}
		const auto find = [&](const char* key, void*& output)
		{
			std::string error;
			const std::byte* found = find_unique_server_pattern(values, key, error);
			if (found == nullptr)
			{
				functions.error += std::string(functions.error.empty() ? "" : ", ") + error;
				return;
			}
			output = const_cast<std::byte*>(found);
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
		return functions;
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
		const auto now = std::chrono::steady_clock::now();
		const auto quarantine = std::chrono::duration<float, std::milli>(k_decoy_reuse_quarantine_ms);
		while (entity == nullptr)
		{
			// The longest parked first, and only once no client can still be
			// taking it in (k_decoy_reuse_quarantine_ms).
			const auto ready = std::find_if(decoy_pool_.begin(), decoy_pool_.end(), [&](const parked_decoy& parked) { return now - parked.parked_at >= quarantine; });
			if (ready == decoy_pool_.end())
			{
				break;
			}
			parked_decoy parked = std::move(*ready);
			decoy_pool_.erase(ready);
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
		if (slot.ghost)
		{
			// A ghost is not a prop: it stays a player, withheld until its next run.
			ghost_release(slot.id, std::chrono::steady_clock::now());
			slot = {};
			return;
		}
		CEntityInstance* entity = slot.spawned && system != nullptr && slot.handle.IsValid() ? system->GetEntityInstance(slot.handle) : nullptr;
		if (entity != nullptr)
		{
			// Parked for the next decoy while there is room; withheld from everyone
			// meanwhile.
			const int mode = decoy_entity_mode();
			if (decoy_functions_.ready && mode != 0 && slot.teleport != nullptr && decoy_pool_.size() < k_decoy_pool_size
				&& decoy_graveyard_.size() + decoy_pool_.size() < decoy_graveyard_transmit_.size())
			{
				decoy_pool_.push_back({slot.handle, slot.teleport, slot.model, mode, std::chrono::steady_clock::now()});
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
					if (slot.ghost)
					{
						ghost_release(slot.id, std::chrono::steady_clock::now());
					}
					else if (system != nullptr && slot.spawned && slot.handle.IsValid())
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
			kick_all_ghosts("cs2glaz decoys off");
			ghost_slots_.store(0);
			remove_all_phantoms(true);
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
			kick_all_ghosts("cs2glaz decoys unavailable");
			ghost_slots_.store(0);
			remove_all_phantoms(true);
			if (decoys_live_.load())
			{
				remove_all_decoys(true);
			}
			return;
		}
		update_ghosts(system, value, now);
		update_phantoms(system, value, now);
		if (!weapon_fire_listening_ && game_events_ != nullptr)
		{
			weapon_fire_listening_ = game_events_->AddListener(this, "weapon_fire", true);
		}
		if (!player_hurt_listening_ && game_events_ != nullptr)
		{
			player_hurt_listening_ = game_events_->AddListener(this, "player_hurt", true);
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
		fixed_list<vec3, k_max_players> living;
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
		// Watched players (CSVILKA detections, cs2glaz_suspect) come first for the
		// shared spawn and candidate budgets and pick new decoys on every update;
		// in mode 3 they are the only ones with decoys.
		const int entity_mode = decoy_entity_mode();
		const bool watched_only = mode == 3;
		csvilka_bridge();
		std::array<bool, k_max_players> watched {};
		std::array<uint32_t, k_max_players> order {};
		uint32_t ordered = 0;
		for (uint32_t pass = 0; pass < 2; ++pass)
		{
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				if (pass == 0)
				{
					watched[slot] = value.players[slot].valid && is_suspect(slot, now);
				}
				if (watched[slot] == (pass == 0))
				{
					order[ordered++] = slot;
				}
			}
		}
		for (const uint32_t viewer_slot : order)
		{
			const player_state& viewer = value.players[viewer_slot];
			const bool eligible = viewer.valid && human_player(viewer_slot) && (!watched_only || watched[viewer_slot]);
			fixed_list<vec3, k_max_players> enemies;
			fixed_list<vec3, k_max_players> others; // every other living player, for aims a real player explains
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
					// Removed by the game (round restart cleanup), or a ghost kicked.
					if (slot.ghost)
					{
						ghost_release(slot.id, now);
					}
					slot = {};
					continue;
				}
				if (slot.ghost && !ghost_pawn_alive(system, slot.handle))
				{
					drop = true; // the ghost died (its team had nobody else alive)
				}
				else if (!slot.ghost && entity != nullptr && field<int32_t>(entity, compatibility_.fields().health) < k_decoy_health)
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
					// A ghost player stands in for a real decoy when its viewer has one
					// ready: a cheat that draws only players sees it.
					if (ghost_take(system, viewer_slot, slot, model, now))
					{
						slot.spawned_at = now;
						slot.pause_until = now + std::chrono::milliseconds(k_pause_min_ms + decoy_random(decoy_seed_) % k_pause_spread_ms);
					}
					else if (!spawn_decoy(system, slot, model, entity_mode))
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
				if (!drop && slot.spawned && sent.id == slot.id && sent.ended)
				{
					// Its one run reached its end: it never goes to this client again.
					drop = true;
					++decoy_counters_.runs_ended;
				}
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
							report_decoy(system, viewer_slot, slot, decoy_report::aim, distance_units(viewer.eye, slot.origin));
						}
					}
					else
					{
						slot.aim_ms = 0.0f;
						slot.aim_turn = 0.0f;
					}
				}
				// The jump test: did the crosshair land on the spot it jumped to?
				if (slot.jumped && !slot.jump.done)
				{
					const float since = std::chrono::duration<float, std::milli>(now - slot.jump_at).count();
					const bool on_new = slot.ready && aim_on
										&& !aim_on_any_player(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others);
					if (!slot.ready || drop)
					{
						slot.jump.done = true; // no longer on his client: the test proves nothing
					}
					else if (decoy_jump_update(slot.jump, since, on_new, elapsed_ms))
					{
						slot.aim_reported = true; // one report for this decoy's following
						report_decoy(system, viewer_slot, slot, decoy_report::jump, distance_units(viewer.eye, slot.origin));
					}
				}
				if (!drop && slot.spawned && entity != nullptr && slot.teleport != nullptr)
				{
					fixed_list<vec3, k_max_decoys_per_viewer> taken;
					for (const decoy_slot& other : decoys_[viewer_slot])
					{
						if (&other != &slot && other.id != 0)
						{
							taken.push_back(other.origin);
						}
					}
					const float ready_ms = std::chrono::duration<float, std::milli>(now - delivered_since).count();
					const bool jump_due = slot.ready && !slot.jumped && !slot.moving && !aim_on && sent.id == slot.id
										  && ready_ms >= k_decoy_reaction_ms + std::clamp(rtt_ms, 0.0f, 500.0f) + k_decoy_jump_after_ready_ms
										  && decoy_random(decoy_seed_) % 32u == 0;
					if (!(jump_due && jump_decoy(entity, slot, viewer, enemies, living, taken, value, now)))
					{
						walk_decoy(entity, slot, viewer, enemies, living, taken, now, elapsed_ms, value);
					}
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
			if (eligible && (slow_tick || watched[viewer_slot]) && fresh && result->players[viewer_slot].valid)
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
					fixed_list<vec3, k_max_decoys_per_viewer> taken;
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
		if (decoy_functions_.ready)
		{
			update_front_decoys(system, value, elapsed_ms, now);
			update_crosshair_ghosts(system, value, now);
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
			kick_all_ghosts("cs2glaz decoys turned off");
			ghost_slots_.store(0);
			remove_all_phantoms(true);
			remove_all_decoys(true);
			return;
		}
		publish_decoy_transmit();
		publish_ghost_transmit();
		publish_phantom_transmit();
	}

	bool plugin::jump_decoy(CEntityInstance* entity, decoy_slot& slot, const player_state& viewer, std::span<const vec3> enemies,
							std::span<const vec3> living, std::span<const vec3> taken, const visibility_snapshot& value,
							std::chrono::steady_clock::time_point now)
	{
		// Another hidden spot, on the same part of his screen but clearly apart,
		// not where his crosshair already is.
		for (int attempt = 0; attempt < 4; ++attempt)
		{
			vec3 spot;
			const decoy_spot_query query {viewer.eye, enemies, living, taken, decoy_random(decoy_seed_), viewer.eye_yaw_degrees};
			if (!choose_decoy_spot(data_, value.occluders, decoy_spots_, query, spot) || !decoy_jump_distance_ok(viewer.eye, slot.origin, spot)
				|| aim_on_decoy(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, spot))
			{
				continue;
			}
			slot.origin = spot;
			slot.goal = spot;
			slot.moving = false;
			// It stands still after the jump, so following it means finding it.
			slot.pause_until = now + std::chrono::milliseconds(static_cast<int>(k_decoy_jump_window_ms) + decoy_random(decoy_seed_) % 600u);
			slot.jumped = true;
			slot.jump_at = now;
			slot.jump = {};
			slot.aim_ms = 0.0f;
			slot.aim_turn = 0.0f;
			slot.aim_origin = spot;
			const Vector origin(spot.x, spot.y, spot.z);
			const QAngle angles(0.0f, slot.yaw, 0.0f);
			const Vector stop(0.0f, 0.0f, 0.0f);
			reinterpret_cast<teleport_fn>(slot.teleport)(entity, &origin, &angles, &stop);
			++decoy_counters_.jumps;
			return true;
		}
		return false;
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
			int slot = -1;
			std::memcpy(&slot, reinterpret_cast<const char*>(info) + compatibility_.recipient_slot_offset(), sizeof(slot));
			for (uint32_t index = 0; index < decoy_graveyard_count_; ++index)
			{
				if (removed[index] >= 0)
				{
					withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, removed[index], withhold_reason::decoy_parked);
				}
			}
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
					const withhold_reason reason = entry.control ? withhold_reason::decoy_control : withhold_reason::decoy;
					if (slot != static_cast<int>(viewer) || entry.target >= k_max_players)
					{
						// Another viewer's decoy: nobody else ever receives it.
						withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, edict, reason);
						continue;
					}
					decoy_delivery& sent = decoy_delivery_[viewer][index];
					if (sent.id != entry.id)
					{
						sent = {};
						sent.id = entry.id;
					}
					const bool same = fresh && result->decoys[viewer][index].id == entry.id;
					decoy_delivery_state state;
					state.proven_hidden = same && result->decoy_hidden[viewer][index];
					state.proven_exposed = same && result->decoy_proven[viewer][index] && !result->decoy_hidden[viewer][index];
					state.viewer_gone = fresh && !result->players[viewer].valid;
					state.target_visible = fresh && result->visible[viewer][entry.target];
					state.running = sent.first_sent != std::chrono::steady_clock::time_point {} && !sent.ended;
					state.ended = sent.ended;
					state.unproven_ms = std::chrono::duration<float, std::milli>(now - sent.last_proven).count();
					const bool allowed = decoy_may_deliver(state);
					if (allowed && info->m_pTransmitEntity->IsBitSet(edict))
					{
						// The engine packed it for its viewer: a real decoy reaches his
						// client now, a control twin would have (same PVS), and both
						// count as delivered from here.
						if (!entry.control)
						{
							++decoy_counters_.ticks_sent;
							decoy_counters_.latched_ticks += state.proven_hidden ? 0u : 1u;
						}
						if (sent.first_sent == std::chrono::steady_clock::time_point {})
						{
							sent.first_sent = now;
						}
						sent.last_sent = now;
						if (state.proven_hidden)
						{
							sent.last_proven = now;
						}
					}
					else
					{
						if (allowed && !entry.control)
						{
							++decoy_counters_.ticks_outside_pvs;
						}
						// A run that stops here is over: the decoy never reaches this
						// client again and is retired on the next update.
						sent.ended = sent.ended || state.running;
					}
					// Bits are only ever cleared. Setting one for an entity the engine
					// did not pack this frame makes the client fail with "CopyExistingEntity:
					// missing client entity" and crash. A control twin is cleared even
					// for its viewer: no client ever has it.
					if (!allowed || entry.control)
					{
						withhold_entity(slot, info->m_pTransmitEntity, info->m_pTransmitAlways, edict, reason);
					}
				}
			}
		}
	}

	void plugin::note_pawns_sent(CCheckTransmitInfo** infos, int count, const visibility_result* result)
	{
		const double now = journal_now();
		if (result == nullptr || !visibility_snapshot_fresh(result->captured, std::chrono::steady_clock::now()) || !settings::current().enable
			|| !disabled_reason_.empty())
		{
			// No hiding now (failing open, or off): every enemy went to everyone.
			everything_sent_at_.store(now);
			return;
		}
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
				const player_state& player = result->players[target];
				const int index = player.pawn_entity;
				if (player.valid && valid_networked_edict_index(index)
					&& (info->m_pTransmitEntity->IsBitSet(index) || info->m_pTransmitAlways->IsBitSet(index)))
				{
					pawn_sent_at_[static_cast<size_t>(slot)][target] = now;
				}
			}
		}
	}

	// player_hurt: a human's gun hit on an enemy whose pawn CS2GLAZ had not
	// sent him for k_blind_hit_unsent_ms (blind_hit_evidence).
	void plugin::blind_hit_event(IGameEvent* event)
	{
		if (event == nullptr || decoy_mode() == 0 || !gun(event->GetString(game_event_key("weapon"), "")))
		{
			return;
		}
		const int attacker = event->GetPlayerSlot(game_event_key("attacker")).Get();
		const int victim = event->GetPlayerSlot(game_event_key("userid")).Get();
		if (attacker < 0 || victim < 0 || attacker >= static_cast<int>(k_max_players) || victim >= static_cast<int>(k_max_players) || attacker == victim
			|| ghost_slot(attacker) || ghost_slot(victim) || !human_player(static_cast<uint32_t>(attacker)))
		{
			return;
		}
		CGameEntitySystem* system = entity_system();
		const lifecycle_key shooter = player_lifecycle(static_cast<uint32_t>(attacker), system, nullptr);
		const lifecycle_key target = player_lifecycle(static_cast<uint32_t>(victim), system, nullptr);
		if ((shooter.team != k_team_t && shooter.team != k_team_ct) || (target.team != k_team_t && target.team != k_team_ct)
			|| shooter.team == target.team)
		{
			return;
		}
		decoy_player_record* record = decoy_record(static_cast<uint32_t>(attacker));
		if (record == nullptr)
		{
			return;
		}
		const double now = journal_now();
		double sent_at = 0.0;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			sent_at = pawn_sent_at_[static_cast<size_t>(attacker)][static_cast<size_t>(victim)];
		}
		const bool blind = (now - std::max(sent_at, everything_sent_at_.load())) * 1000.0 > k_blind_hit_unsent_ms;
		++record->gun_hits;
		++decoy_counters_.gun_hits;
		if (!blind)
		{
			return;
		}
		++record->blind_hits;
		++decoy_counters_.blind_hits;
		record->name = slot_name(system, static_cast<uint32_t>(attacker));
		record->this_map = true;
		const uint64_t xuid = engine_->GetClientXUID(CPlayerSlot(attacker));
		write_decoy_log(xuid, "blind_hit", -1, *record);
		const double share = blind_hit_server_share(decoy_counters_.blind_hits, decoy_counters_.gun_hits);
		const double evidence = blind_hit_evidence(record->blind_hits, record->gun_hits, share);
		if (evidence >= 1.0 && std::floor(evidence) > record->blind_evidence_marked)
		{
			record->blind_evidence_marked = std::floor(evidence);
			META_CONPRINTF("[CS2GLAZ] blind hits: \"%s\" %llu hit enemies CS2GLAZ had not sent him %u times of %u gun hits (an honest player: about "
						   "%.1f); evidence %.1f - weak alone (a sound ESP or radar hack?), his decoys come first now\n",
						   record->name.c_str(), static_cast<unsigned long long>(xuid), record->blind_hits, record->gun_hits,
						   static_cast<double>(record->gun_hits) * share, evidence);
			write_decoy_log(xuid, "blind_hit_evidence", -1, *record);
			mark_suspect(xuid, 30.0f * 60.0f, "blind hits", "cs2glaz");
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
		last_fire_at_[static_cast<size_t>(shooter)] = std::chrono::steady_clock::now();
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
			// Anyone's bullet, a bot's too: a hittable front body must never stop one.
			front_bullet_check(view.eye, impact);
			crosshair_bullet_check(static_cast<uint32_t>(shooter), view.eye, impact);
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
		fixed_list<vec3, k_max_players> others;
		for (uint32_t player = 0; player < k_max_players; ++player)
		{
			if (player != shooter && result->players[player].valid)
			{
				others.push_back(result->players[player].origin);
			}
		}
		front_shot(shooter, eye, direction, others);
		crosshair_shot(shooter, eye, direction, others, now);
		for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
		{
			decoy_slot& slot = decoys_[shooter][index];
			if (!slot.spawned || !slot.ready || slot.shot_reported || result->decoys[shooter][index].id != slot.id || !result->decoy_hidden[shooter][index]
				|| !direction_on_decoy(eye, direction, slot.origin) || direction_on_any_player(eye, direction, others))
			{
				continue;
			}
			slot.shot_reported = true;
			report_decoy(system, shooter, slot, decoy_report::shot, distance_units(eye, slot.origin));
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
				std::erase_if(decoy_players_, [](const auto& entry) { return !entry.second.anything(); });
				if (decoy_players_.size() >= k_max_decoy_players)
				{
					return nullptr;
				}
			}
			found = decoy_players_.emplace(xuid, decoy_player_record {}).first;
		}
		return &found->second;
	}

	void plugin::report_decoy(CGameEntitySystem* system, uint32_t viewer, const decoy_slot& slot, decoy_report kind, float distance)
	{
		decoy_player_record* record = decoy_record(viewer);
		if (record == nullptr)
		{
			return;
		}
		const uint64_t xuid = engine_->GetClientXUID(CPlayerSlot(static_cast<int>(viewer)));
		record->name = slot_name(system, viewer);
		record->this_map = true;
		const int reaction = slot.reaction_ms >= 0.0f ? static_cast<int>(slot.reaction_ms + 0.5f) : -1;
		if (slot.crosshair)
		{
			// A crosshair ghost (crosshair_ghosts.cpp): only shots are reported.
			if (slot.control)
			{
				++record->crosshair_control_shots;
				++decoy_counters_.crosshair_control_shots;
				++crosshair_server_.control_reports;
				write_decoy_log(xuid, "control_crosshair_shot", static_cast<int>(distance), *record, reaction);
				return;
			}
			++record->crosshair_shots;
			++decoy_counters_.crosshair_shots;
			++crosshair_server_.real_reports;
		}
		else if (slot.front)
		{
			// A front decoy (front_decoys.cpp): only jumps and shots are reported.
			const bool shot = kind == decoy_report::shot;
			if (slot.control)
			{
				shot ? ++record->front_control_shots : ++record->front_control_jumps;
				shot ? ++decoy_counters_.front_control_shots : ++decoy_counters_.front_control_follows;
				++front_server_.control_reports;
				write_decoy_log(xuid, shot ? "control_front_shot" : "control_front_jump", static_cast<int>(distance), *record);
				return;
			}
			shot ? ++record->front_shots : ++record->front_jumps;
			shot ? ++decoy_counters_.front_shots : ++decoy_counters_.front_follows;
			++front_server_.real_reports;
		}
		else if (slot.control)
		{
			// No client ever had it: an honest coincidence, the baseline the real
			// reports are weighed against. Logged for the record only.
			switch (kind)
			{
				case decoy_report::aim:
					++record->control_aims;
					++decoy_counters_.control_aims;
					break;
				case decoy_report::shot:
					++record->control_shots;
					++decoy_counters_.control_shots;
					break;
				case decoy_report::jump:
					++record->control_jumps;
					++decoy_counters_.control_jump_follows;
					break;
			}
			++decoy_server_.control_reports;
			write_decoy_log(xuid, kind == decoy_report::shot ? "control_shot" : (kind == decoy_report::jump ? "control_jump" : "control_aim"),
							static_cast<int>(distance), *record);
			return;
		}
		else
		{
			switch (kind)
			{
				case decoy_report::aim:
					++record->aims;
					++decoy_counters_.aims;
					break;
				case decoy_report::shot:
					++record->shots;
					++decoy_counters_.shots;
					break;
				case decoy_report::jump:
					++record->jumps;
					++decoy_counters_.jump_follows;
					break;
			}
			++decoy_server_.real_reports;
		}
		const bool shot = kind == decoy_report::shot;
		// Front decoys and decoys behind walls are weighed against their own
		// controls; the evidence is the sum.
		const decoy_exposure exposure = slot.crosshair ? record->crosshair_exposure() : slot.front ? record->front_exposure() : record->exposure();
		const double expected = slot.crosshair ? decoy_expected_reports(exposure, crosshair_server_, k_crosshair_prior_rate)
								: slot.front   ? decoy_expected_reports(exposure, front_server_, k_front_prior_rate)
											   : decoy_expected_reports(exposure, decoy_server_);
		const double evidence = decoy_player_evidence(*record);
		// An aimbot locked onto a front decoy follows it every second or so:
		// every report goes to the log, the console gets one every few seconds.
		const auto now = std::chrono::steady_clock::now();
		if (!slot.front || now - record->front_printed_at >= std::chrono::seconds(5))
		{
			if (slot.front)
			{
				record->front_printed_at = now;
			}
			const match_moment moment = current_moment();
			char where[64] {};
			if (moment.round > 0)
			{
				std::snprintf(where, sizeof(where), " at round %d%s", moment.round, moment.warmup ? " (warmup)" : "");
				if (moment.round_seconds >= 0.0f)
				{
					const size_t used = std::strlen(where);
					std::snprintf(where + used, sizeof(where) - used, ", %d:%02d into it", static_cast<int>(moment.round_seconds) / 60,
								  static_cast<int>(moment.round_seconds) % 60);
				}
			}
			char what[96] {};
			if (slot.crosshair)
			{
				std::snprintf(what, sizeof(what), "an invisible ghost on his crosshair %d ms after it reached him (a triggerbot or aimbot?)", reaction);
			}
			META_CONPRINTF("[CS2GLAZ] decoy: \"%s\" %llu %s %s (%.0f units). Since load: %llu at real ones in %.0f s of them, "
						   "%llu at controls in %.0f s; an honest player would have about %.1f; evidence %.1f - suspect, check the demo%s\n",
						   record->name.c_str(), static_cast<unsigned long long>(xuid),
						   shot ? "shot at" : (kind == decoy_report::jump ? "followed the jump of" : "aimed at"),
						   slot.crosshair ? what : slot.front ? "an invisible front decoy (an aimbot or triggerbot?)" : "a decoy through a wall", distance,
						   static_cast<unsigned long long>(exposure.real_reports), exposure.real_seconds,
						   static_cast<unsigned long long>(exposure.control_reports), exposure.control_seconds, expected, evidence, where);
		}
		write_decoy_log(xuid,
						slot.crosshair ? "crosshair_shot"
						: slot.front   ? (shot ? "front_shot" : "front_jump")
									   : (shot ? "shot" : (kind == decoy_report::jump ? "jump" : "aim")),
						static_cast<int>(distance), *record, reaction);
		// CSVILKA weighs it together with its own checks and punishes by its own
		// rules: every whole point of new evidence goes to it.
		if (evidence >= 1.0 && std::floor(evidence) > record->evidence_reported)
		{
			if (anticheat_bridge::csvilka* partner = csvilka_bridge(); partner != nullptr)
			{
				anticheat_bridge::decoy_evidence shared {};
				decoy_evidence_for(xuid, shared);
				record->evidence_reported = std::floor(evidence);
				++csvilka_reports_;
				write_decoy_log(xuid, "reported_to_csvilka", -1, *record);
				partner->report_decoy_evidence(static_cast<int>(viewer), xuid, shared);
			}
		}
		const int threshold = cs2glaz_decoy_kick.Get();
		if (!human_player(viewer) || !decoy_kick_due(evidence, threshold, record->evidence_at_kick))
		{
			return;
		}
		if (anticheat_bridge::csvilka* partner = csvilka_bridge(); partner != nullptr && partner->is_whitelisted(xuid))
		{
			// Never punished, by either plugin; counted like a kick so the line
			// comes once per threshold.
			record->evidence_at_kick = evidence;
			META_CONPRINTF("[CS2GLAZ] decoy: \"%s\" %llu reached decoy evidence %.1f but is on CSVILKA's whitelist: not kicked\n",
						   record->name.c_str(), static_cast<unsigned long long>(xuid), evidence);
			write_decoy_log(xuid, "whitelisted", -1, *record);
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

	plugin::match_moment plugin::current_moment() const
	{
		match_moment moment;
		INetworkGameServer* server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
		CGlobalVars* globals = server == nullptr ? nullptr : server->GetGlobals();
		if (globals == nullptr)
		{
			return moment;
		}
		moment.tick = globals->tickcount;
		CGameEntitySystem* system = entity_system();
		if (system == nullptr || !compatibility_.round_schema_available())
		{
			return moment;
		}
		CEntityInstance* proxy = game_rules_proxy_.IsValid() ? system->GetEntityInstance(game_rules_proxy_) : nullptr;
		if (proxy == nullptr)
		{
			// One walk of the entity list per map: the proxy lives as long as the map.
			CEntityIdentity* identity = system->m_EntityList.m_pFirstActiveEntity;
			for (uint32_t scanned = 0; identity != nullptr && scanned < k_entity_scan_hard_limit; identity = identity->m_pNext, ++scanned)
			{
				const char* name = identity->m_designerName.String();
				if (name != nullptr && std::strcmp(name, "cs_gamerules") == 0 && identity->m_pInstance != nullptr)
				{
					proxy = identity->m_pInstance;
					game_rules_proxy_ = entity_handle(proxy);
					break;
				}
			}
		}
		const schema_offsets& fields = compatibility_.fields();
		const std::byte* rules = nullptr;
		int32_t rounds = 0;
		float started = 0.0f;
		bool warmup = false;
		if (proxy == nullptr
			|| !runtime_compatibility::safe_read(reinterpret_cast<const std::byte*>(proxy) + fields.game_rules, &rules, sizeof(rules)) || rules == nullptr
			|| !runtime_compatibility::safe_read(rules + fields.total_rounds_played, &rounds, sizeof(rounds))
			|| !runtime_compatibility::safe_read(rules + fields.round_start_time, &started, sizeof(started))
			|| !runtime_compatibility::safe_read(rules + fields.warmup_period, &warmup, sizeof(warmup)) || rounds < 0 || rounds > 1000)
		{
			return moment;
		}
		moment.round = rounds + 1;
		moment.warmup = warmup;
		if (std::isfinite(started) && started > 0.0f && globals->curtime >= started)
		{
			moment.round_seconds = globals->curtime - started;
		}
		return moment;
	}

	void plugin::write_decoy_log(uint64_t xuid, const char* event, int distance, const decoy_player_record& record, int reaction_ms) const
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
		const decoy_exposure front = record.front_exposure();
		const decoy_exposure crosshair = record.crosshair_exposure();
		char numbers[640] {};
		std::snprintf(numbers, sizeof(numbers),
					  "real_seconds=%.0f control_seconds=%.0f expected=%.2f front_jumps=%u front_shots=%u control_front_jumps=%u "
					  "control_front_shots=%u front_seconds=%.0f control_front_seconds=%.0f front_expected=%.2f crosshair_shots=%u "
					  "control_crosshair_shots=%u crosshair_seconds=%.1f control_crosshair_seconds=%.1f crosshair_expected=%.2f evidence=%.2f",
					  exposure.real_seconds, exposure.control_seconds, decoy_expected_reports(exposure, decoy_server_), record.front_jumps,
					  record.front_shots, record.front_control_jumps, record.front_control_shots, front.real_seconds, front.control_seconds,
					  decoy_expected_reports(front, front_server_, k_front_prior_rate), record.crosshair_shots, record.crosshair_control_shots,
					  crosshair.real_seconds, crosshair.control_seconds, decoy_expected_reports(crosshair, crosshair_server_, k_crosshair_prior_rate),
					  decoy_player_evidence(record));
		log << utc_timestamp() << " map=" << map_;
		const match_moment moment = current_moment();
		if (moment.tick >= 0)
		{
			log << " tick=" << moment.tick;
		}
		if (moment.round > 0)
		{
			char round[64] {};
			std::snprintf(round, sizeof(round), " round=%d%s", moment.round, moment.warmup ? " warmup=1" : "");
			log << round;
			if (moment.round_seconds >= 0.0f)
			{
				std::snprintf(round, sizeof(round), " round_time=%d:%02d", static_cast<int>(moment.round_seconds) / 60,
							  static_cast<int>(moment.round_seconds) % 60);
				log << round;
			}
		}
		log << " player=\"" << record.name << "\" steamid=" << xuid << " event=" << event;
		if (distance >= 0)
		{
			log << " distance=" << distance;
		}
		if (reaction_ms >= 0)
		{
			log << " reaction_ms=" << reaction_ms;
		}
		log << " aims=" << record.aims << " shots=" << record.shots << " jumps=" << record.jumps << " control_aims=" << record.control_aims
			<< " control_shots=" << record.control_shots << " control_jumps=" << record.control_jumps << " gun_hits=" << record.gun_hits
			<< " blind_hits=" << record.blind_hits << " " << numbers << "\n";
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
					   mode, mode == 0 ? "off" : mode == 1 ? "invisible" : mode == 2 ? "drawn" : "invisible, watched players only",
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
			META_CONPRINTF("[CS2GLAZ] decoy transmit ticks this map: sent=%llu (of them without a fresh proof, kept for at most %.0f ms: %llu) "
						   "outside_pvs=%llu (outside the PVS a decoy is not sent); runs ended=%llu (each decoy reaches its viewer in one run, "
						   "then is retired)\n",
						   value(counters.ticks_sent), k_decoy_latch_ms, value(counters.latched_ticks), value(counters.ticks_outside_pvs),
						   value(counters.runs_ended));
			if (weapon_fire_listening_)
			{
				META_CONPRINTF("[CS2GLAZ] decoy shots: from the view at weapon_fire%s; blind hits: %s\n",
							   bullet_impact_listening_ ? " and where bullets hit (bullet_impact)" : (bullet_impact_tried_ ? " (bullet_impact unavailable)" : ""),
							   player_hurt_listening_ ? "on" : "off (player_hurt unavailable)");
			}
			else
			{
				META_CONPRINTF("[CS2GLAZ] decoy shots and blind hits: off, game events unavailable%s%s%s\n", game_event_manager_error_.empty() ? "" : " (",
							   game_event_manager_error_.c_str(), game_event_manager_error_.empty() ? "" : ")");
			}
		}
		print_ghost_status();
		print_front_status();
		print_crosshair_status();
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
			if (record.aims + record.shots + record.jumps + record.front_jumps + record.front_shots + record.crosshair_shots != 0)
			{
				suspects.emplace_back(xuid, &record);
			}
		}
		std::sort(suspects.begin(), suspects.end(),
				  [&](const auto& left, const auto& right)
				  {
					  const double a = decoy_player_evidence(*left.second);
					  const double b = decoy_player_evidence(*right.second);
					  return a != b ? a > b : left.second->aims + left.second->shots > right.second->aims + right.second->shots;
				  });
		for (size_t index = 0; index < suspects.size() && index < 10; ++index)
		{
			const decoy_player_record& record = *suspects[index].second;
			const decoy_exposure exposure = record.exposure();
			const decoy_exposure front = record.front_exposure();
			const decoy_exposure crosshair = record.crosshair_exposure();
			META_CONPRINTF("[CS2GLAZ] decoy suspect: \"%s\" %llu aims=%u shots=%u jumps=%u in %.0f s of real decoys; control aims=%u shots=%u "
						   "jumps=%u in %.0f s; expected %.1f; front decoys: jumps=%u shots=%u in %.0f s, at controls %u and %u in %.0f s, "
						   "expected %.1f; crosshair ghosts: shots=%u in %.1f s, at controls %u in %.1f s, expected %.1f; evidence %.1f; kicks=%u\n",
						   record.name.c_str(), value(suspects[index].first), record.aims, record.shots, record.jumps, exposure.real_seconds,
						   record.control_aims, record.control_shots, record.control_jumps, exposure.control_seconds,
						   decoy_expected_reports(exposure, decoy_server_), record.front_jumps, record.front_shots, front.real_seconds,
						   record.front_control_jumps, record.front_control_shots, front.control_seconds,
						   decoy_expected_reports(front, front_server_, k_front_prior_rate), record.crosshair_shots, crosshair.real_seconds,
						   record.crosshair_control_shots, crosshair.control_seconds,
						   decoy_expected_reports(crosshair, crosshair_server_, k_crosshair_prior_rate), decoy_player_evidence(record), record.kicks);
		}
		// Blind hits: weak evidence of a sound ESP or radar hack, the channels
		// hiding enemies does not close.
		const double share = blind_hit_server_share(counters.blind_hits, counters.gun_hits);
		META_CONPRINTF("[CS2GLAZ] decoy jumps=%llu followed=%llu (at control twins %llu); blind hits since load: %llu of %llu gun hits on enemies "
					   "(server share %.1f%%)\n",
					   value(counters.jumps), value(counters.jump_follows), value(counters.control_jump_follows), value(counters.blind_hits),
					   value(counters.gun_hits), 100.0 * share);
		std::vector<std::pair<uint64_t, const decoy_player_record*>> blind;
		for (const auto& [xuid, record] : decoy_players_)
		{
			if (blind_hit_evidence(record.blind_hits, record.gun_hits, share) > 0.0)
			{
				blind.emplace_back(xuid, &record);
			}
		}
		std::sort(blind.begin(), blind.end(),
				  [&](const auto& left, const auto& right)
				  {
					  return blind_hit_evidence(left.second->blind_hits, left.second->gun_hits, share)
							 > blind_hit_evidence(right.second->blind_hits, right.second->gun_hits, share);
				  });
		for (size_t index = 0; index < blind.size() && index < 5; ++index)
		{
			const decoy_player_record& record = *blind[index].second;
			META_CONPRINTF("[CS2GLAZ] blind-hit suspect: \"%s\" %llu %u of %u gun hits on enemies not sent to him; expected %.1f; evidence %.1f\n",
						   record.name.c_str(), value(blind[index].first), record.blind_hits, record.gun_hits,
						   static_cast<double>(record.gun_hits) * share, blind_hit_evidence(record.blind_hits, record.gun_hits, share));
		}
		print_bridge_status();
	}

} // namespace cs2glaz
