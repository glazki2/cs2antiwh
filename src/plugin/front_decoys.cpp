#include "plugin.h"

// Front decoys (experimental, cs2glaz_decoy_front, off by default): against
// aimbots. Each human player gets a phantom player (phantoms.cpp) of the
// enemy team that only he receives, standing in plain view a few degrees off
// his crosshair (decoy_logic.h, choose_front_spot): the target an aimbot that
// picks the enemy nearest the crosshair takes first, and a target a visibility
// trace through the map finds visible. He cannot see it (not rendered, no
// shadow, no collision, nothing else receives it), and nothing about it
// reaches his teammates or spectators.
//
// It jumps to another spot 12-50 degrees away every one to three seconds and
// whenever a crosshair rests on it, so an aimbot locked onto it drags the
// view after it, and the jump test (decoy_jump_update) records an aim that
// lands on the new spot after a reaction and stays. A shot along a line
// through it is recorded too (an aimbot's automatic shot, a triggerbot that
// works from the bones). A control twin with no entity is placed and jumped
// by the same rules: what an honest player does at it, with the same
// readiness, is his coincidence rate, and only reports beyond it are
// evidence (decoy_evidence with k_front_prior_rate).
//
// Against triggerbots (cs2glaz_decoy_front 2, the least proven part): most
// triggerbots fire when the game's own crosshair target (the client's trace)
// is an enemy. In this mode the body carries a real player's collision, so
// its viewer's client may find it there; the server made no physics object
// for it, so the server's bullets, which decide every hit, still pass
// through it, and front_bullet_check turns the mode off for good if one ever
// stops in it. It keeps 128 units from its viewer, so his own movement never
// meets it. What his client shows when his bullets cross it, and whether his
// crosshair trace really finds it, only a test on a real server can tell.
//
// An aimbot that checks a field only real player pawns have, or a cheat that
// recognises a phantom, skips it.

#include <algorithm>
#include <cmath>
#include <vector>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_decoy_front("cs2glaz_decoy_front", FCVAR_NONE,
										 "Experimental front decoys against aimbots: 1 keeps an invisible phantom player a few degrees off every human player's "
										 "crosshair, sent to him only, jumping away whenever a crosshair rests on it; an aim that follows its jumps or a shot at it "
										 "is a decoy report; 2 also gives its body a player's collision on his client, for triggerbots that read the game's "
										 "crosshair target (least proven); 0 off (needs cs2glaz_decoys 1 or 3 and cs2glaz_decoy_phantoms of at least the "
										 "player count; test servers first; resets on restart)",
										 0, true, 0, true, 2);

		using teleport_fn = void (*)(CEntityInstance*, const Vector*, const QAngle*, const Vector*);

		// New phantoms for front decoys, one at a time: every round restart takes
		// their bodies, and twenty players need theirs back before the freeze ends.
		constexpr auto k_front_create_interval = std::chrono::milliseconds(300);
		constexpr auto k_front_retry = std::chrono::milliseconds(250);
		constexpr auto k_front_create_failed_retry = std::chrono::seconds(2);

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
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

		float milliseconds(std::chrono::steady_clock::duration duration)
		{
			return std::chrono::duration<float, std::milli>(duration).count();
		}

	} // namespace

	bool plugin::front_enabled() const
	{
		return cs2glaz_decoy_front.Get() != 0 && decoy_mode() != 0 && phantom_limit() > 0;
	}

	bool plugin::front_hittable() const
	{
		// Only with bullet impacts heard: they are how a body that stops a bullet
		// on the server is noticed.
		return front_enabled() && cs2glaz_decoy_front.Get() == 2 && front_hittable_error_.empty() && bullet_impact_listening_
			   && compatibility_.collision_attribute_available();
	}

	void plugin::front_bullet_check(vec3 eye, vec3 impact)
	{
		if (!front_hittable_error_.empty())
		{
			return;
		}
		for (uint32_t viewer = 0; viewer < k_max_players; ++viewer)
		{
			const decoy_slot& real = front_decoys_[viewer].real;
			const phantom_player* phantom = real.id != 0 ? front_phantom(viewer) : nullptr;
			if (phantom == nullptr || !phantom->hittable || !front_body_stopped_bullet(data_, real.origin, eye, impact))
			{
				continue;
			}
			// The server's bullet stopped in a body nobody can see: it would take
			// honest players' shots. Every hittable body goes (update_phantoms).
			front_hittable_error_ = "a front decoy's body stopped a bullet on the server";
			META_CONPRINTF("[CS2GLAZ] front decoys against triggerbots turned off: a bullet stopped in a front decoy's body on the server "
						   "(at %.0f %.0f %.0f); cs2glaz_decoy_front 2 works like 1 until the plugin reloads\n",
						   impact.x, impact.y, impact.z);
			return;
		}
	}

	phantom_player* plugin::front_phantom(uint32_t viewer)
	{
		if (viewer >= k_max_players)
		{
			return nullptr;
		}
		const auto found = std::find_if(phantoms_.begin(), phantoms_.end(),
										[&](const phantom_player& phantom) { return phantom.index != 0 && phantom.front && phantom.viewer == viewer; });
		return found == phantoms_.end() ? nullptr : &*found;
	}

	void plugin::stop_front_decoys(uint32_t viewer, std::chrono::steady_clock::time_point now)
	{
		if (viewer >= k_max_players)
		{
			return;
		}
		front_decoy& front = front_decoys_[viewer];
		if (front.real.id != 0)
		{
			// Its body may have reached him: not sent again before the quarantine.
			front.stopped_at = now;
		}
		front.real = {};
		front.control = {};
	}

	void plugin::update_front_decoys(CGameEntitySystem* system, const visibility_snapshot& value, float elapsed_ms,
									 std::chrono::steady_clock::time_point now)
	{
		std::string reason;
		const bool available = system != nullptr && front_enabled() && decoy_functions_.ready && phantoms_available(reason);
		std::array<front_delivery, k_max_phantoms> delivery;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			delivery = front_delivery_;
		}
		const int mode = decoy_mode();
		const bool drawn = decoy_entity_mode() == 2;
		const bool hittable = available && front_hittable();
		const schema_offsets& fields = compatibility_.fields();
		const auto quarantine = std::chrono::duration<float, std::milli>(k_decoy_reuse_quarantine_ms);
		const auto pause = [&]()
		{
			const float ms = k_decoy_jump_window_ms + k_front_pause_min_ms
							 + static_cast<float>(decoy_random(decoy_seed_) % static_cast<uint32_t>(k_front_pause_spread_ms));
			return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float, std::milli>(ms));
		};
		// Watched players first for new phantoms when the limit is short.
		std::array<uint32_t, k_max_players> order {};
		uint32_t ordered = 0;
		for (uint32_t pass = 0; pass < 2; ++pass)
		{
			for (uint32_t slot = 0; slot < k_max_players; ++slot)
			{
				const bool watched = value.players[slot].valid && is_suspect(slot, now);
				if (watched == (pass == 0))
				{
					order[ordered++] = slot;
				}
			}
		}
		for (const uint32_t viewer_slot : order)
		{
			front_decoy& front = front_decoys_[viewer_slot];
			const player_state& viewer = value.players[viewer_slot];
			const bool eligible = available && viewer.valid && human_player(viewer_slot) && (viewer.team == k_team_t || viewer.team == k_team_ct)
								  && (mode != 3 || is_suspect(viewer_slot, now));
			if (!eligible)
			{
				// Dead, gone, a spectator or not watched: its body is withheld (the
				// phantom stays while he is on a team, update_phantoms).
				stop_front_decoys(viewer_slot, now);
				continue;
			}
			std::vector<vec3> others; // every other living player: aims and shots at them prove nothing
			std::vector<uint32_t> enemies;
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				const player_state& other = value.players[target];
				if (target == viewer_slot || !other.valid)
				{
					continue;
				}
				others.push_back(other.origin);
				if ((other.team == k_team_t || other.team == k_team_ct) && other.team != viewer.team && other.pawn_entity > 0)
				{
					enemies.push_back(target);
				}
			}
			phantom_player* phantom = front_phantom(viewer_slot);
			if (phantom != nullptr)
			{
				// A phantom carries one enemy's name for good: once he is no longer
				// on its team (left, switched), a new phantom carries another.
				CEntityInstance* target = system->GetEntityInstance(CEntityIndex(static_cast<int>(phantom->target + 1u)));
				if (target == nullptr || field<uint8_t>(target, fields.team) != phantom->team)
				{
					remove_phantom(*phantom, true);
					phantom = nullptr;
				}
			}
			if (phantom == nullptr)
			{
				const auto existing =
					std::count_if(phantoms_.begin(), phantoms_.end(), [](const phantom_player& candidate) { return candidate.index != 0; });
				if (enemies.empty() || now < front.retry_at || now < phantom_next_create_ || existing >= phantom_limit())
				{
					continue;
				}
				const uint32_t target = enemies[decoy_random(decoy_seed_) % enemies.size()];
				CEntityInstance* pawn = system->GetEntityInstance(CEntityIndex(value.players[target].pawn_entity));
				const std::string model = entity_model_name(pawn);
				vec3 spot;
				const front_spot_query query {viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others, {}, nullptr, decoy_random(decoy_seed_),
											viewer.origin};
				if (pawn == nullptr || model.empty() || !choose_front_spot(data_, decoy_spots_, query, spot))
				{
					front.retry_at = now + k_front_retry;
					++decoy_counters_.front_no_spot;
					continue;
				}
				decoy_slot placement;
				placement.target = target;
				placement.origin = spot;
				placement.yaw = yaw_towards(spot, viewer.eye);
				phantom_next_create_ = now + k_front_create_interval;
				phantom = create_phantom(system, viewer_slot, placement, model, hittable ? pawn : nullptr, now);
				if (phantom == nullptr)
				{
					front.retry_at = now + k_front_create_failed_retry;
					continue;
				}
				phantom->front = true;
				if (hittable && !phantom->hittable)
				{
					// The enemy's collision did not look like a living player's: the
					// mode cannot work on this build. Plain bodies from now on.
					front_hittable_error_ = "a player's collision did not look like a player's";
					META_CONPRINTF("[CS2GLAZ] front decoys against triggerbots turned off: %s; cs2glaz_decoy_front 2 works like 1 until the "
								   "plugin reloads\n",
								   front_hittable_error_.c_str());
				}
				else if (phantom->hittable)
				{
					++decoy_counters_.front_hittable_created;
				}
				// A new body has never reached anyone.
				front.stopped_at = {};
			}
			CEntityInstance* body = system->GetEntityInstance(phantom->body);
			if (body == nullptr || phantom->teleport == nullptr)
			{
				continue; // the round restart took it: update_phantoms replaces the phantom
			}
			phantom->idle_since = now;
			const auto move_body = [&](const decoy_slot& slot)
			{
				const Vector origin(slot.origin.x, slot.origin.y, slot.origin.z);
				const QAngle angles(0.0f, slot.yaw, 0.0f);
				const Vector stop(0.0f, 0.0f, 0.0f);
				reinterpret_cast<teleport_fn>(phantom->teleport)(body, &origin, &angles, &stop);
			};
			const auto place = [&](decoy_slot& slot, const decoy_slot& twin, bool control)
			{
				const std::vector<vec3> taken = twin.id != 0 ? std::vector<vec3> {twin.origin} : std::vector<vec3> {};
				const front_spot_query query {viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others, taken, nullptr, decoy_random(decoy_seed_),
											viewer.origin};
				vec3 spot;
				if (!choose_front_spot(data_, decoy_spots_, query, spot))
				{
					++decoy_counters_.front_no_spot;
					return false;
				}
				slot = {};
				slot.id = ++decoy_next_id_ == 0 ? ++decoy_next_id_ : decoy_next_id_;
				slot.target = phantom->target;
				slot.front = true;
				slot.control = control;
				slot.spawned = true;
				slot.origin = spot;
				slot.yaw = yaw_towards(spot, viewer.eye);
				slot.spawned_at = now;
				slot.pause_until = now + pause();
				if (!control)
				{
					slot.handle = phantom->body;
					slot.teleport = phantom->teleport;
					move_body(slot);
				}
				return true;
			};
			// A run of its body starts in front of him, never sooner than the
			// quarantine after the last one ended (its client may still be
			// acknowledging the snapshot that took it).
			if (front.real.id == 0 && now >= front.retry_at && now - front.stopped_at >= quarantine)
			{
				if (place(front.real, front.control, false))
				{
					++decoy_counters_.front_runs;
				}
				else
				{
					front.retry_at = now + k_front_retry;
				}
			}
			if (front.real.id != 0 && front.control.id == 0 && now >= front.retry_at && !place(front.control, front.real, true))
			{
				front.retry_at = now + k_front_retry;
			}
			if (front.real.id == 0)
			{
				front.control = {};
				continue;
			}
			// Ready while the engine sends its body to him and has for a round trip
			// and a reaction; its twin is ready exactly then.
			const front_delivery& sent = delivery[static_cast<size_t>(phantom - phantoms_.data())];
			const bool delivered_now = sent.run == front.real.id && now - sent.last_sent <= k_decoy_delivery_gap;
			const auto last_reached = sent.run == front.real.id ? std::max(sent.last_sent, front.real.spawned_at) : front.real.spawned_at;
			if (now - last_reached >= k_decoy_undelivered_limit)
			{
				// The engine kept it from him (outside his PVS): a new run later.
				stop_front_decoys(viewer_slot, now);
				front.retry_at = now + k_front_retry;
				++decoy_counters_.undelivered;
				continue;
			}
			const float rtt_ms = viewer.rtt_seconds * 1000.0f;
			// Drawn (cs2glaz_decoys 2, for tests) anyone can see it: it still jumps,
			// but nothing counts.
			const bool ready = !drawn && delivered_now && decoy_delivery_ready(true, milliseconds(now - sent.first_sent), rtt_ms);
			decoy_player_record* record = decoy_record(viewer_slot);
			if (record != nullptr && record->name.empty())
			{
				record->name = slot_name(system, viewer_slot);
			}
			for (decoy_slot* slot : {&front.real, &front.control})
			{
				if (slot->id == 0)
				{
					continue;
				}
				const decoy_slot& twin = slot == &front.real ? front.control : front.real;
				const bool aim_on = aim_on_decoy(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, slot->origin)
									&& !aim_on_any_player(viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others);
				slot->ready = ready;
				if (ready && record != nullptr)
				{
					const double seconds = std::max(elapsed_ms, 0.0f) / 1000.0;
					(slot->control ? record->front_control_seconds : record->front_real_seconds) += seconds;
					(slot->control ? front_server_.control_seconds : front_server_.real_seconds) += seconds;
					record->this_map = true;
				}
				// The jump test: did the crosshair land on the spot it jumped to?
				if (slot->jumped && !slot->jump.done)
				{
					if (!ready)
					{
						slot->jump.done = true;
					}
					else if (decoy_jump_update(slot->jump, milliseconds(now - slot->jump_at), aim_on, elapsed_ms))
					{
						report_decoy(system, viewer_slot, *slot, decoy_report::jump, distance_units(viewer.eye, slot->origin));
					}
				}
				slot->aim_ms = aim_on ? slot->aim_ms + std::max(elapsed_ms, 0.0f) : 0.0f;
				const std::vector<vec3> taken = twin.id != 0 ? std::vector<vec3> {twin.origin} : std::vector<vec3> {};
				front_spot_query query {viewer.eye, viewer.eye_pitch_degrees, viewer.eye_yaw_degrees, others, taken, nullptr, decoy_random(decoy_seed_),
											viewer.origin};
				// At his side is no place for a decoy, and a hittable body must
				// never meet his movement (front_crowds_viewer, in the keep rules).
				const bool keeps = front_spot_keeps(data_, query, slot->origin);
				const bool testing = slot->jumped && !slot->jump.done;
				const bool dodge = slot->aim_ms >= k_front_dodge_ms;
				const front_move move = front_decide(keeps, testing, slot->aim_ms, now >= slot->pause_until);
				if (move == front_move::stay)
				{
					continue;
				}
				// A jump from a spot it could keep (12-50 degrees on his screen); a
				// new place near the crosshair when he turned away, came close or it
				// is out of clear view. Either is a test while it is ready.
				const vec3 from = slot->origin;
				query.from = move == front_move::jump ? &from : nullptr;
				vec3 spot;
				if (!choose_front_spot(data_, decoy_spots_, query, spot))
				{
					slot->pause_until = now + k_front_retry;
					++decoy_counters_.front_no_spot;
					if (!slot->control && phantom->hittable && front_crowds_viewer(viewer.origin, slot->origin))
					{
						// Nowhere else to go and too close to him: its body leaves his
						// client rather than meet his movement.
						stop_front_decoys(viewer_slot, now);
						front.retry_at = now + k_front_retry;
						break;
					}
					continue;
				}
				slot->origin = spot;
				slot->yaw = yaw_towards(spot, viewer.eye);
				slot->aim_ms = 0.0f;
				slot->shot_reported = false;
				slot->jumped = ready;
				slot->jump_at = now;
				slot->jump = {};
				slot->pause_until = now + pause();
				if (ready)
				{
					++decoy_counters_.front_jumps;
				}
				if (dodge)
				{
					++decoy_counters_.front_dodges;
				}
				if (!slot->control)
				{
					move_body(*slot);
				}
			}
		}
	}

	void plugin::front_shot(uint32_t shooter, vec3 eye, vec3 direction, std::span<const vec3> others)
	{
		if (shooter >= k_max_players)
		{
			return;
		}
		front_decoy& front = front_decoys_[shooter];
		for (decoy_slot* slot : {&front.real, &front.control})
		{
			if (slot->id == 0 || !slot->ready || slot->shot_reported || !direction_on_decoy(eye, direction, slot->origin)
				|| direction_on_any_player(eye, direction, others))
			{
				continue;
			}
			slot->shot_reported = true; // once per spot
			report_decoy(entity_system(), shooter, *slot, decoy_report::shot, distance_units(eye, slot->origin));
		}
	}

	double plugin::decoy_player_evidence(const decoy_player_record& record) const
	{
		return decoy_evidence(record.exposure(), decoy_server_) + decoy_evidence(record.front_exposure(), front_server_, k_front_prior_rate);
	}

	void plugin::print_front_status() const
	{
		const int setting = cs2glaz_decoy_front.Get();
		uint32_t running = 0;
		uint32_t ready = 0;
		for (const front_decoy& front : front_decoys_)
		{
			running += front.real.id != 0 ? 1u : 0u;
			ready += front.real.id != 0 && front.real.ready ? 1u : 0u;
		}
		decoy_counters counters;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			counters = decoy_counters_;
		}
		std::string reason;
		const char* state = setting == 0								  ? "(off)"
							: decoy_mode() == 0							  ? "(waiting for cs2glaz_decoys)"
							: phantom_limit() == 0						  ? "(waiting for cs2glaz_decoy_phantoms)"
							: !decoy_functions_resolved_ || decoy_functions_.ready
								? (!phantoms_available(reason) ? "(phantoms unavailable)"
								   : decoy_entity_mode() == 2  ? "drawn for a test (nothing counts)"
															   : "ready")
								: "(decoys unavailable)";
		const auto value = [](uint64_t number) { return static_cast<unsigned long long>(number); };
		META_CONPRINTF("[CS2GLAZ] front decoys: %d %s running=%u ready=%u runs=%llu jumps=%llu (after a resting crosshair %llu) no_spot=%llu; "
					   "followed=%llu shots=%llu at real ones over %.0f s; followed=%llu shots=%llu at control twins over %.0f s (an honest "
					   "player's rate: %.2f per minute)\n",
					   setting, state, running, ready, value(counters.front_runs), value(counters.front_jumps), value(counters.front_dodges),
					   value(counters.front_no_spot), value(counters.front_follows), value(counters.front_shots), front_server_.real_seconds,
					   value(counters.front_control_follows), value(counters.front_control_shots), front_server_.control_seconds,
					   60.0 * decoy_server_rate(front_server_, k_front_prior_rate));
		if (setting == 2)
		{
			// Against triggerbots: bodies with a player's collision for their viewer.
			const char* hittable = !front_hittable_error_.empty()						  ? front_hittable_error_.c_str()
								   : !bullet_impact_listening_							  ? "waiting for bullet_impact"
								   : !compatibility_.collision_attribute_available()	  ? "the schema lacks collision attributes"
								   : front_hittable()									  ? "on"
																						  : "waiting";
			META_CONPRINTF("[CS2GLAZ] front decoys against triggerbots: %s; bodies with a player's collision made=%llu\n", hittable,
						   value(counters.front_hittable_created));
		}
	}

} // namespace cs2glaz
