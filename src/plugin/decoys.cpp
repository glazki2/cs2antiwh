#include "plugin.h"

// Experimental decoys (cs2glaz_decoys, off by default). For each human viewer
// and each enemy hidden from him, a prop_dynamic with that enemy's model is
// placed on a floor spot the viewer cannot see, away from every real enemy,
// and sent to that viewer only, only while the worker proves it hidden from
// all of his viewing origins. It has no collision, no animation, no sound and
// no radar entry; in mode 1 it is not rendered at all. Aiming at it for half a
// second or shooting at it through the wall is logged for moderators.
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
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoys("cs2glaz_decoys", FCVAR_NONE,
									"Experimental decoys for wallhack users: 0 off, 1 invisible, 2 model drawn behind walls (testing); resets on restart",
									0, true, 0, true, 2);

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

	bool plugin::spawn_decoy(CGameEntitySystem* system, decoy_slot& slot, const std::string& model, int mode)
	{
		decoy_functions& functions = decoy_functions_;
		const auto fail = [&](CEntityInstance* entity, const char* reason)
		{
			if (entity != nullptr)
			{
				const CEntityHandle handle = entity_handle(entity);
				reinterpret_cast<remove_entity_fn>(functions.remove_entity)(entity);
				if (handle.IsValid() && decoy_graveyard_.size() < decoy_graveyard_transmit_.size())
				{
					decoy_graveyard_.push_back(handle);
				}
			}
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
		CEntityInstance* entity = reinterpret_cast<create_entity_fn>(functions.create_entity_by_name)("prop_dynamic", -1);
		if (entity == nullptr)
		{
			return false;
		}
		const schema_offsets& fields = compatibility_.fields();
		void** vtable = nullptr;
		void* teleport = nullptr;
		if (!runtime_compatibility::safe_read(entity, &vtable, sizeof(vtable)) || vtable == nullptr
			|| !runtime_compatibility::safe_read(vtable + functions.teleport_vtable_index, &teleport, sizeof(teleport))
			|| !compatibility_.address_in_server_module(teleport))
		{
			return fail(entity, "the Teleport vtable index does not point into the server");
		}
		// Not solid, touched by no trace, (mode 1) not rendered, before and after
		// the model and the spawn; nothing has been sent yet, so the first send
		// carries these values. The model is set before spawning (a prop spawned
		// without one logs "has no model name"); being not solid, it gets no
		// physics object, which is checked right after the spawn.
		void* collision = reinterpret_cast<std::byte*>(entity) + fields.model_collision;
		const auto apply = [&]
		{
			field<uint8_t>(collision, fields.solid_type) = k_solid_none;
			field<uint8_t>(collision, fields.solid_flags) |= k_solid_flag_not_solid;
			if (compatibility_.collision_attribute_available())
			{
				void* attribute = reinterpret_cast<std::byte*>(collision) + fields.collision_attribute;
				field<uint64_t>(attribute, fields.interacts_as) = 0;
				field<uint64_t>(attribute, fields.interacts_with) = 0;
			}
			field<int32_t>(entity, fields.health) = k_decoy_health;
			if (mode == 1)
			{
				field<uint8_t>(entity, fields.render_mode) = compatibility_.render_none_value();
				field<uint8_t>(entity, fields.render_color + 3) = 0; // alpha
			}
		};
		apply();
		reinterpret_cast<set_model_fn>(functions.set_model)(entity, model.c_str());
		apply();
		reinterpret_cast<dispatch_spawn_fn>(functions.dispatch_spawn)(entity, nullptr);
		if (field<uint8_t>(collision, fields.solid_type) != k_solid_none || (field<uint8_t>(collision, fields.solid_flags) & k_solid_flag_not_solid) == 0)
		{
			return fail(entity, "a spawned decoy was solid");
		}
		apply();
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
		slot.spawned = true;
		++decoy_counters_.spawned;
		return true;
	}

	void plugin::remove_decoy(CGameEntitySystem* system, decoy_slot& slot)
	{
		if (slot.spawned && system != nullptr && decoy_functions_.remove_entity != nullptr && slot.handle.IsValid())
		{
			if (CEntityInstance* entity = system->GetEntityInstance(slot.handle); entity != nullptr)
			{
				reinterpret_cast<remove_entity_fn>(decoy_functions_.remove_entity)(entity);
				if (decoy_graveyard_.size() < decoy_graveyard_transmit_.size())
				{
					decoy_graveyard_.push_back(slot.handle);
				}
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
					remove_decoy(system, slot);
				}
			}
		}
		publish_decoy_transmit();
	}

	void plugin::prune_decoy_graveyard(CGameEntitySystem* system)
	{
		if (system == nullptr)
		{
			decoy_graveyard_.clear();
			return;
		}
		std::erase_if(decoy_graveyard_, [&](CEntityHandle handle) { return system->GetEntityInstance(handle) == nullptr; });
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
				entry = slot.spawned ? decoy_transmit_entry {slot.handle, slot.id, slot.target} : decoy_transmit_entry {};
				live = live || slot.spawned;
			}
		}
		decoy_graveyard_count_ = static_cast<uint32_t>(std::min(decoy_graveyard_.size(), decoy_graveyard_transmit_.size()));
		std::copy_n(decoy_graveyard_.begin(), decoy_graveyard_count_, decoy_graveyard_transmit_.begin());
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
		uint32_t spawn_budget = k_spawns_per_update;
		uint32_t candidate_budget = k_candidates_per_update;
		for (uint32_t viewer_slot = 0; viewer_slot < k_max_players; ++viewer_slot)
		{
			const player_state& viewer = value.players[viewer_slot];
			const bool eligible = viewer.valid && human_player(viewer_slot);
			std::vector<vec3> enemies;
			for (uint32_t target = 0; eligible && target < k_max_players; ++target)
			{
				if (enemies_of(viewer, viewer_slot, target))
				{
					enemies.push_back(value.players[target].origin);
				}
			}
			const auto too_close = [&](vec3 point)
			{
				const auto near = [&](vec3 other, float distance)
				{
					const float x = point.x - other.x;
					const float y = point.y - other.y;
					const float z = point.z - other.z;
					return x * x + y * y + z * z < distance * distance;
				};
				return near(viewer.origin, k_decoy_viewer_drop)
					   || std::any_of(enemies.begin(), enemies.end(), [&](vec3 enemy) { return near(enemy, k_decoy_enemy_drop); });
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
				if (checked && result->visible[viewer_slot][slot.target])
				{
					drop = true; // the real enemy is in view now
				}
				else if (checked && !result->decoy_hidden[viewer_slot][index])
				{
					drop = true;
					if (slot.spawned)
					{
						++decoy_counters_.exposed;
					}
				}
				else if (checked && !slot.spawned && spawn_budget > 0)
				{
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
						slot.pause_until = now + std::chrono::milliseconds(k_pause_min_ms + decoy_random(decoy_seed_) % k_pause_spread_ms);
					}
				}
				if (!drop && checked && slot.spawned)
				{
					if (aim_on_decoy(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, slot.origin))
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
					const decoy_spot_query query {viewer.eye, enemies, living, taken, decoy_random(decoy_seed_)};
					if (!choose_decoy_spot(data_, value.occluders, decoy_spots_, query, spot))
					{
						continue;
					}
					decoy_slot& slot = *free;
					slot = {};
					slot.id = ++decoy_next_id_ == 0 ? ++decoy_next_id_ : decoy_next_id_;
					slot.target = target;
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
					// Bits are only ever cleared. Setting one for an entity the engine
					// did not pack this frame makes the client fail with "CopyExistingEntity:
					// missing client entity" and crash.
					if (!allowed)
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
		if (shooter < 0 || shooter >= static_cast<int>(k_max_players) || !last_view_[shooter].valid)
		{
			return;
		}
		const std::shared_ptr<const visibility_result> result = worker_.result();
		const auto now = std::chrono::steady_clock::now();
		if (result == nullptr || !visibility_snapshot_fresh(result->captured, now))
		{
			return;
		}
		CGameEntitySystem* system = entity_system();
		const view_sample& view = last_view_[shooter];
		for (uint32_t index = 0; index < k_max_decoys_per_viewer; ++index)
		{
			decoy_slot& slot = decoys_[shooter][index];
			if (!slot.spawned || slot.shot_reported || result->decoys[shooter][index].id != slot.id || !result->decoy_hidden[shooter][index]
				|| !aim_on_decoy(view.eye, view.pitch, view.yaw, slot.origin))
			{
				continue;
			}
			slot.shot_reported = true;
			report_decoy(system, static_cast<uint32_t>(shooter), slot, true, distance_units(view.eye, slot.origin));
		}
	}

	void plugin::report_decoy(CGameEntitySystem* system, uint32_t viewer, const decoy_slot&, bool shot, float distance)
	{
		const uint64_t xuid = engine_ == nullptr ? 0 : engine_->GetClientXUID(CPlayerSlot(static_cast<int>(viewer)));
		decoy_player_record& record = decoy_records_[viewer];
		if (record.xuid != xuid)
		{
			record = {xuid, 0, 0};
		}
		shot ? ++record.shots : ++record.aims;
		shot ? ++decoy_counters_.shots : ++decoy_counters_.aims;
		const std::string name = slot_name(system, viewer);
		const char* what = shot ? "shot at" : "aimed at";
		META_CONPRINTF("[CS2GLAZ] decoy: \"%s\" %llu %s a decoy through a wall (%.0f units; this map aims=%u shots=%u) - suspect, check the demo\n",
					   name.c_str(), static_cast<unsigned long long>(xuid), what, distance, record.aims, record.shots);
		if (api_ == nullptr)
		{
			return;
		}
		std::error_code error;
		const std::filesystem::path directory = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2glaz" / "logs";
		std::filesystem::create_directories(directory, error);
		std::ofstream log(directory / "decoys.log", std::ios::app);
		if (log)
		{
			log << utc_timestamp() << " map=" << map_ << " player=\"" << name << "\" steamid=" << xuid << " event=" << (shot ? "shot" : "aim")
				<< " distance=" << static_cast<int>(distance) << " aims=" << record.aims << " shots=" << record.shots << "\n";
		}
	}

	void plugin::print_decoy_status() const
	{
		const int mode = decoy_mode();
		uint32_t live = 0;
		uint32_t pending = 0;
		for (const auto& row : decoys_)
		{
			for (const decoy_slot& slot : row)
			{
				slot.spawned ? ++live : (slot.id != 0 ? ++pending : 0u);
			}
		}
		const decoy_counters& counters = decoy_counters_;
		META_CONPRINTF("[CS2GLAZ] decoys mode=%d (%s) %s live=%u pending=%u spots=%zu created=%llu exposed=%llu failed=%llu aims=%llu shots=%llu\n",
					   mode, mode == 0 ? "off" : mode == 1 ? "invisible" : "drawn",
					   !decoy_functions_resolved_ ? "not checked yet" : decoy_functions_.ready ? "ready" : "unavailable", live, pending,
					   decoy_spots_.points().size(), static_cast<unsigned long long>(counters.spawned),
					   static_cast<unsigned long long>(counters.exposed), static_cast<unsigned long long>(counters.spawn_failures),
					   static_cast<unsigned long long>(counters.aims), static_cast<unsigned long long>(counters.shots));
		if (decoy_functions_resolved_ && !decoy_functions_.ready)
		{
			META_CONPRINTF("[CS2GLAZ] decoys: %s\n", decoy_functions_.error.c_str());
		}
		CGameEntitySystem* system = entity_system();
		for (uint32_t slot = 0; slot < k_max_players; ++slot)
		{
			const decoy_player_record& record = decoy_records_[slot];
			if (record.aims + record.shots != 0)
			{
				META_CONPRINTF("[CS2GLAZ] decoy suspect: \"%s\" %llu aims=%u shots=%u\n", slot_name(system, slot).c_str(),
							   static_cast<unsigned long long>(record.xuid), record.aims, record.shots);
			}
		}
	}

} // namespace cs2glaz
