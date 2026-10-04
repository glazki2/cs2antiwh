#include "plugin.h"

// Crosshair ghosts (experimental, cs2glaz_decoy_crosshair, off by default):
// against triggerbots and aimbots that skip anything but real players.
//
// The two ghost players (ghosts.cpp) become one per team, and each is shown in
// turns to the players of the other team, one at a time: for one turn its pawn
// stands in the open with its head on that player's crosshair
// (choose_crosshair_spot), and only he receives it, controller and pawn, until
// the turn ends half a second or so later. It is a real player of the enemy
// team, so a cheat treats it as one: a triggerbot fires the moment it is under
// the crosshair, an aimbot is on it already, an ESP draws it. He cannot see it
// (not rendered, no shadow), nobody else can see the spot, and no other player
// is near his crosshair line, so a shot at it right after it reached him is
// a report (crosshair_shot). Control turns send nothing and count the same
// shots at the same kind of spot: his honest coincidences, which the reports
// are weighed against (decoy_evidence with k_crosshair_prior_rate).
//
// Between turns a ghost is parked high above the map, sent to nobody. It takes
// no damage and does not move. It keeps a player's collision so his client
// finds it under the crosshair as it finds any enemy (what triggerbots read);
// that is why it only ever stands where nobody else can see it, never where a
// weapon or item lies (it would pick it up), never while he holds anything
// but a gun (a grenade would bounce off it), and why bullets of anyone else
// stopping in it turn the mode off (crosshair_bullet_check). Given the bomb,
// it hangs above a living teammate until it has dropped it there; it dies
// below the map, so its pistol falls out of the world.

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoy_crosshair("cs2glaz_decoy_crosshair", FCVAR_NONE,
											 "Experimental crosshair ghosts against triggerbots and aimbots: 1 makes two fake players, one per team, each shown "
											 "in turns to one player of the other team with its head on his crosshair, invisible and sent to him only; a shot "
											 "at it right after it reached him is a decoy report; 0 off (needs cs2glaz_decoys 1 or 3 and two free player "
											 "slots; uses the ghost players instead of cs2glaz_decoy_ghosts; test servers first)",
											 0, true, 0, true, 1);

		// A turn: the ghost reaches him, his shot can come back within his round
		// trip plus this, then the ghost leaves him.
		constexpr float k_crosshair_window_ms = 350.0f;
		// The engine had it in his list within this (plus his round trip), or the turn ends.
		constexpr float k_crosshair_delivery_limit_ms = 250.0f;
		constexpr auto k_crosshair_gap = std::chrono::milliseconds(300);		// between a ghost's turns
		constexpr auto k_crosshair_retry = std::chrono::milliseconds(200);		// nobody was due or fitting
		constexpr uint32_t k_crosshair_viewer_gap_ms = 2500;					// between one player's turns, plus up to as much again
		constexpr auto k_crosshair_quiet = std::chrono::milliseconds(600);		// no turn while he is shooting
		constexpr uint32_t k_crosshair_control_one_in = 3;
		// Players whose crosshair is checked for a spot per ghost and update: each
		// check traces the map from every other player's eye.
		constexpr uint32_t k_crosshair_candidates_per_update = 2;
		constexpr uint32_t k_crosshair_foreign_bullet_limit = 3;
		constexpr float k_park_offset = 1024.0f;
		constexpr float k_bomb_drop_height = 160.0f; // above a teammate: clear of his head when he jumps
		constexpr float k_item_clearance = 96.0f;	  // no weapon or item lying this close to its feet

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		uint8_t enemy_team(uint8_t team)
		{
			return team == k_team_t ? k_team_ct : (team == k_team_ct ? k_team_t : 0);
		}

		float milliseconds(std::chrono::steady_clock::duration duration)
		{
			return std::chrono::duration<float, std::milli>(duration).count();
		}

		float yaw_towards(vec3 from, vec3 to)
		{
			return std::atan2(to.y - from.y, to.x - from.x) * 57.29578f;
		}

		float distance_units(vec3 a, vec3 b)
		{
			const float x = a.x - b.x;
			const float y = a.y - b.y;
			const float z = a.z - b.z;
			return std::sqrt(x * x + y * y + z * z);
		}

		std::chrono::steady_clock::duration duration_ms(float value)
		{
			return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float, std::milli>(value));
		}

	} // namespace

	bool plugin::crosshair_enabled() const
	{
		return cs2glaz_decoy_crosshair.Get() != 0 && decoy_mode() != 0 && crosshair_error_.empty();
	}

	bool plugin::ghost_teleport(CGameEntitySystem* system, ghost_player& ghost, vec3 feet, float yaw)
	{
		CEntityInstance* body = system != nullptr && ghost.pawn.IsValid() ? system->GetEntityInstance(ghost.pawn) : nullptr;
		if (body == nullptr || !std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z) || !std::isfinite(yaw))
		{
			return false;
		}
		if (ghost.teleport == nullptr || ghost.teleport_pawn != ghost.pawn)
		{
			// Read once per pawn: each guarded read is a system call.
			void** vtable = nullptr;
			void* teleport = nullptr;
			if (!decoy_functions_.ready || decoy_functions_.teleport_vtable_index == 0 || !runtime_compatibility::safe_read(body, &vtable, sizeof(vtable))
				|| vtable == nullptr || !runtime_compatibility::safe_read(vtable + decoy_functions_.teleport_vtable_index, &teleport, sizeof(teleport))
				|| !compatibility_.address_in_server_module(teleport))
			{
				return false;
			}
			ghost.teleport = teleport;
			ghost.teleport_pawn = ghost.pawn;
		}
		const Vector origin(feet.x, feet.y, feet.z);
		const QAngle angles(0.0f, yaw, 0.0f);
		const Vector stop(0.0f, 0.0f, 0.0f);
		reinterpret_cast<void (*)(CEntityInstance*, const Vector*, const QAngle*, const Vector*)>(ghost.teleport)(body, &origin, &angles, &stop);
		return true;
	}

	bool plugin::crosshair_park_spots(vec3& above, vec3& below) const
	{
		const auto& header = data_.header;
		if (header.triangle_count == 0)
		{
			return false;
		}
		const float x = (header.world_min[0] + header.world_max[0]) * 0.5f;
		const float y = (header.world_min[1] + header.world_max[1]) * 0.5f;
		above = {x, y, std::min(header.world_max[2] + k_park_offset, 14000.0f)};
		below = {x, y, std::max(header.world_min[2] - k_park_offset, -14000.0f)};
		return std::isfinite(x) && std::isfinite(y) && std::isfinite(above.z) && std::isfinite(below.z);
	}

	bool plugin::collect_lying_items(CGameEntitySystem* system, fixed_list<vec3, k_max_lying_items>& items) const
	{
		if (system == nullptr)
		{
			return false;
		}
		const schema_offsets& fields = compatibility_.fields();
		CEntityIdentity* identity = system->m_EntityList.m_pFirstActiveEntity;
		for (uint32_t scanned = 0; identity != nullptr && scanned < k_entity_scan_hard_limit; identity = identity->m_pNext, ++scanned)
		{
			const char* name = identity->m_designerName.String();
			CEntityInstance* entity = identity->m_pInstance;
			if (name == nullptr || entity == nullptr || (std::strncmp(name, "weapon_", 7) != 0 && std::strncmp(name, "item_", 5) != 0))
			{
				continue;
			}
			// Players' own weapons are with them, and no player is near a spot.
			void* body_component = field<void*>(entity, fields.body_component);
			void* scene_node = body_component == nullptr ? nullptr : field<void*>(body_component, fields.scene_node);
			if (scene_node == nullptr)
			{
				continue;
			}
			if (items.size() == k_max_lying_items)
			{
				return false; // more than can be checked: no spot is safe
			}
			const Vector origin = field<Vector>(scene_node, fields.abs_origin);
			items.push_back({origin.x, origin.y, origin.z});
		}
		return true;
	}

	void plugin::end_crosshair_turn(CGameEntitySystem*, size_t index, std::chrono::steady_clock::time_point now)
	{
		if (index >= crosshair_turns_.size())
		{
			return;
		}
		crosshair_turn& turn = crosshair_turns_[index];
		ghost_player& ghost = ghosts_[index];
		if (turn.id != 0)
		{
			// The part of the window he had counts: real and control alike.
			if (turn.counts && turn.opened != std::chrono::steady_clock::time_point {} && now > turn.opened && turn.viewer < k_max_players)
			{
				const double seconds = std::chrono::duration<double>(std::min(now, turn.closes) - turn.opened).count();
				decoy_player_record* record = decoy_record(turn.viewer);
				if (record != nullptr && seconds > 0.0)
				{
					(turn.control ? record->crosshair_control_seconds : record->crosshair_real_seconds) += seconds;
					(turn.control ? crosshair_server_.control_seconds : crosshair_server_.real_seconds) += seconds;
					record->this_map = true;
				}
			}
			crosshair_next_turn_[index] = now + k_crosshair_gap;
		}
		turn = {};
		if (ghosts_shared_ && ghost.slot >= 0)
		{
			ghost.viewer = k_max_players;
		}
	}

	// Runs before update_decoys publishes the ghosts' transmit state.
	void plugin::update_crosshair_ghosts(CGameEntitySystem* system, const visibility_snapshot& value, std::chrono::steady_clock::time_point now)
	{
		if (!ghosts_shared_ || system == nullptr)
		{
			for (size_t index = 0; index < crosshair_turns_.size(); ++index)
			{
				end_crosshair_turn(system, index, now);
			}
			return;
		}
		std::array<front_delivery, k_max_ghosts> delivery;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			delivery = crosshair_delivery_;
		}
		const bool drawn = decoy_entity_mode() == 2;
		vec3 park;
		vec3 below;
		const bool park_known = crosshair_park_spots(park, below);
		const schema_offsets& fields = compatibility_.fields();
		const auto origin_of = [&](const ghost_player& ghost, vec3& origin)
		{
			CEntityInstance* body = system->GetEntityInstance(ghost.pawn);
			void* body_component = body == nullptr ? nullptr : field<void*>(body, fields.body_component);
			void* scene_node = body_component == nullptr ? nullptr : field<void*>(body_component, fields.scene_node);
			if (scene_node == nullptr)
			{
				return false;
			}
			const Vector value = field<Vector>(scene_node, fields.abs_origin);
			origin = {value.x, value.y, value.z};
			return true;
		};
		const auto within = [](vec3 a, vec3 b, float distance)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return dx * dx + dy * dy + dz * dz < distance * distance;
		};
		// Weapons and items lying on the map, read once per update when a spot is
		// being checked: a ghost standing on one would pick it up.
		fixed_list<vec3, k_max_lying_items> items;
		int items_state = 0; // 0 not read, 1 read, -1 unknown
		const auto items_near = [&](vec3 feet)
		{
			if (items_state == 0)
			{
				items_state = collect_lying_items(system, items) ? 1 : -1;
			}
			return items_state < 0 || std::any_of(items.begin(), items.end(), [&](vec3 item)
												  {
													  const float dx = item.x - feet.x;
													  const float dy = item.y - feet.y;
													  return dx * dx + dy * dy < k_item_clearance * k_item_clearance && item.z > feet.z - 48.0f
															 && item.z < feet.z + k_crosshair_head_height + 32.0f;
												  });
		};
		for (size_t index = 0; index < ghosts_.size(); ++index)
		{
			ghost_player& ghost = ghosts_[index];
			crosshair_turn& turn = crosshair_turns_[index];
			if (ghost.slot >= 0 && ghost.harmless && ghost.has_bomb && ghost.pawn.IsValid())
			{
				// The bomb goes to a real player (update_ghosts drops it): it hangs
				// above a living teammate meanwhile, so the bomb lands at his feet.
				end_crosshair_turn(system, index, now);
				for (const player_state& teammate : value.players)
				{
					if (teammate.valid && teammate.team == ghost.team)
					{
						const vec3 drop {teammate.origin.x, teammate.origin.y, teammate.origin.z + k_bomb_drop_height};
						vec3 origin;
						if (!origin_of(ghost, origin) || !within(origin, drop, 32.0f))
						{
							ghost_teleport(system, ghost, drop, 0.0f);
						}
						break;
					}
				}
				continue;
			}
			const bool usable = ghost.slot >= 0 && ghost.harmless && ghost.pawn.IsValid();
			if (!usable)
			{
				if (turn.id != 0)
				{
					end_crosshair_turn(system, index, now);
				}
				continue;
			}
			if (turn.id != 0)
			{
				const player_state& viewer = value.players[turn.viewer];
				const bool viewer_ok = viewer.valid && human_player(turn.viewer) && enemy_team(viewer.team) == ghost.team;
				if (!turn.control && turn.opened == std::chrono::steady_clock::time_point {})
				{
					const front_delivery& sent = delivery[index];
					if (sent.run == turn.id)
					{
						// Reached him: his shot can come back from now on.
						turn.opened = sent.first_sent;
						turn.closes = turn.opened + duration_ms(turn.rtt_ms + k_crosshair_window_ms);
						++decoy_counters_.crosshair_delivered;
					}
					else if (now - turn.started >= duration_ms(turn.rtt_ms + k_crosshair_delivery_limit_ms))
					{
						// The engine kept it from him (outside his PVS): no turn.
						++decoy_counters_.crosshair_undelivered;
						turn.counts = false;
						end_crosshair_turn(system, index, now);
						continue;
					}
				}
				if (!viewer_ok || (turn.opened != std::chrono::steady_clock::time_point {} && now >= turn.closes))
				{
					end_crosshair_turn(system, index, now);
				}
			}
			// Without the map's walls no spot can be proven out of others' sight.
			if (turn.id == 0 && now >= crosshair_next_turn_[index] && data_.header.triangle_count != 0)
			{
				// The next player of the other team, in turns: alive, a human, not
				// shooting, (mode 3) watched, and his own turns spaced out.
				crosshair_next_turn_[index] = now + k_crosshair_retry;
				const uint32_t start = crosshair_rotation_[index];
				uint32_t candidates = 0;
				for (uint32_t step = 1; step <= k_max_players && candidates < k_crosshair_candidates_per_update; ++step)
				{
					const uint32_t slot = (start + step) % k_max_players;
					const player_state& viewer = value.players[slot];
					// A gun in his hands: a grenade he throws must not bounce off it.
					const bool holds_gun = viewer.muzzle_class != weapon_muzzle_class::none || !compatibility_.weapon_item_available();
					if (!viewer.valid || enemy_team(viewer.team) != ghost.team || !human_player(slot) || ghost_capture_slot(slot) || !holds_gun
						|| now < crosshair_viewer_next_[slot] || now - last_fire_at_[slot] < k_crosshair_quiet || (decoy_mode() == 3 && !is_suspect(slot, now)))
					{
						continue;
					}
					fixed_list<vec3, k_max_players> players;
					fixed_list<vec3, k_max_players> watchers;
					for (uint32_t other = 0; other < k_max_players; ++other)
					{
						if (other != slot && value.players[other].valid)
						{
							players.push_back(value.players[other].origin);
							watchers.push_back(value.players[other].eye);
						}
					}
					const crosshair_spot_query query {viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, players, watchers, decoy_random(decoy_seed_)};
					// The next check starts after him, whether his crosshair fits or not.
					++candidates;
					crosshair_rotation_[index] = slot;
					vec3 feet;
					if (!choose_crosshair_spot(data_, query, feet) || items_near(feet))
					{
						++decoy_counters_.crosshair_no_spot;
						continue;
					}
					crosshair_viewer_next_[slot] = now + std::chrono::milliseconds(k_crosshair_viewer_gap_ms + decoy_random(decoy_seed_) % k_crosshair_viewer_gap_ms);
					turn = {};
					turn.id = ++decoy_next_id_ == 0 ? ++decoy_next_id_ : decoy_next_id_;
					turn.viewer = slot;
					turn.control = decoy_random(decoy_seed_) % k_crosshair_control_one_in == 0;
					turn.feet = feet;
					turn.yaw = yaw_towards(feet, viewer.eye);
					turn.rtt_ms = std::clamp(std::isfinite(viewer.rtt_seconds) ? viewer.rtt_seconds * 1000.0f : 0.0f, 0.0f, 500.0f);
					turn.counts = !drawn;
					turn.started = now;
					if (turn.control)
					{
						// Nothing is sent; its window is the one a real turn would have.
						turn.opened = now + std::chrono::milliseconds(16);
						turn.closes = turn.opened + duration_ms(turn.rtt_ms + k_crosshair_window_ms);
						++decoy_counters_.crosshair_controls;
					}
					else
					{
						if (!ghost_teleport(system, ghost, feet, turn.yaw))
						{
							turn = {};
							break;
						}
						ghost.viewer = slot;
						++decoy_counters_.crosshair_turns;
						++ghost_counters_.runs;
					}
					break;
				}
			}
			// Not shown to anyone: parked high above the map, where nothing meets it.
			vec3 origin;
			if ((turn.id == 0 || turn.control) && park_known && origin_of(ghost, origin) && !within(origin, park, 64.0f))
			{
				ghost_teleport(system, ghost, park, 0.0f);
			}
		}
	}

	void plugin::crosshair_shot(uint32_t shooter, vec3 eye, vec3 direction, std::span<const vec3> others, std::chrono::steady_clock::time_point now)
	{
		if (!ghosts_shared_ || shooter >= k_max_players)
		{
			return;
		}
		for (crosshair_turn& turn : crosshair_turns_)
		{
			// From half his round trip after it reached him (sooner it cannot be
			// his reaction to it) until the window closes; once per turn.
			if (turn.id == 0 || turn.viewer != shooter || turn.reported || !turn.counts || turn.opened == std::chrono::steady_clock::time_point {}
				|| now < turn.opened + duration_ms(turn.rtt_ms * 0.5f) || now > turn.closes || !direction_on_standing_body(eye, direction, turn.feet)
				|| direction_on_any_player(eye, direction, others))
			{
				continue;
			}
			turn.reported = true;
			decoy_slot slot;
			slot.id = turn.id;
			slot.origin = turn.feet;
			slot.control = turn.control;
			slot.crosshair = true;
			slot.reaction_ms = milliseconds(now - turn.opened);
			report_decoy(entity_system(), shooter, slot, decoy_report::shot, distance_units(eye, turn.feet));
		}
	}

	void plugin::crosshair_bullet_check(uint32_t shooter, vec3 eye, vec3 impact)
	{
		if (!ghosts_shared_)
		{
			return;
		}
		for (const crosshair_turn& turn : crosshair_turns_)
		{
			// Its own viewer's bullet in it is his shot at it; anyone else's means
			// someone the spot rules missed (a wallbang): a few, and it stops.
			if (turn.id == 0 || turn.control || shooter == turn.viewer || !front_body_stopped_bullet(data_, turn.feet, eye, impact))
			{
				continue;
			}
			++decoy_counters_.crosshair_foreign_bullets;
			if (decoy_counters_.crosshair_foreign_bullets >= k_crosshair_foreign_bullet_limit && crosshair_error_.empty())
			{
				crosshair_error_ = "other players' bullets stopped in a shown crosshair ghost";
				META_CONPRINTF("[CS2GLAZ] crosshair ghosts turned off: %llu bullets of other players stopped in a shown ghost (last at %.0f %.0f %.0f); "
							   "they stay off until the plugin reloads\n",
							   static_cast<unsigned long long>(decoy_counters_.crosshair_foreign_bullets), impact.x, impact.y, impact.z);
			}
			return;
		}
	}

	void plugin::print_crosshair_status() const
	{
		const int setting = cs2glaz_decoy_crosshair.Get();
		decoy_counters counters;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			counters = decoy_counters_;
		}
		std::string reason;
		const char* state = setting == 0				   ? "(off)"
							: !crosshair_error_.empty()	   ? crosshair_error_.c_str()
							: decoy_mode() == 0			   ? "(waiting for cs2glaz_decoys)"
							: !ghosts_available(reason)	   ? "(ghost players unavailable)"
							: decoy_entity_mode() == 2	   ? "drawn for a test (nothing counts)"
														   : "ready";
		const auto value = [](uint64_t number) { return static_cast<unsigned long long>(number); };
		META_CONPRINTF("[CS2GLAZ] crosshair ghosts: %d %s turns=%llu reached=%llu kept_from_him=%llu controls=%llu no_spot=%llu; shots=%llu in %.0f s of "
					   "open windows, at controls %llu in %.0f s (an honest player's rate: %.2f per minute); other players' bullets in one: %llu\n",
					   setting, state, value(counters.crosshair_turns), value(counters.crosshair_delivered), value(counters.crosshair_undelivered),
					   value(counters.crosshair_controls), value(counters.crosshair_no_spot), value(counters.crosshair_shots), crosshair_server_.real_seconds,
					   value(counters.crosshair_control_shots), crosshair_server_.control_seconds,
					   60.0 * decoy_server_rate(crosshair_server_, k_crosshair_prior_rate), value(counters.crosshair_foreign_bullets));
		for (size_t index = 0; index < crosshair_turns_.size(); ++index)
		{
			const crosshair_turn& turn = crosshair_turns_[index];
			if (turn.id != 0)
			{
				META_CONPRINTF("[CS2GLAZ] crosshair ghost \"%s\" %s player slot %u\n", ghosts_[index].name.c_str(), turn.control ? "control turn for" : "shown to",
							   turn.viewer);
			}
		}
	}

} // namespace cs2glaz
