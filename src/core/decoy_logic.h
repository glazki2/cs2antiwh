#pragma once

// Decoys: per viewer, a fake enemy placed where the viewer cannot see it and
// sent to that viewer only. An honest player never receives one while it could
// be seen; a wallhack that draws it shows an enemy that is not there, and
// aiming or shooting at it through the wall is logged for moderators.
//
// Spots come from places players have stood on this map (so a decoy is on a
// floor, never inside a wall), away from every real enemy of the viewer (so a
// decoy never hints where one is), behind the map geometry from the viewer.

#include "bvh8.h"
#include "capsule_visibility.h"
#include "dynamic_occluders.h"
#include "visibility_sampling.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace cs2glaz
{

	// One per hidden enemy in a 5 on 5 match.
	inline constexpr uint32_t k_max_decoys_per_viewer = 5;
	inline constexpr uint32_t k_max_decoy_spots = 4096;
	inline constexpr vec3 k_decoy_mins {-16.0f, -16.0f, 0.0f};
	inline constexpr vec3 k_decoy_maxs {16.0f, 16.0f, 72.0f};
	inline constexpr float k_decoy_center_height = 40.0f;
	inline constexpr float k_decoy_min_distance = 256.0f;
	inline constexpr float k_decoy_max_distance = 2048.0f;
	inline constexpr float k_decoy_enemy_clearance = 400.0f;
	inline constexpr float k_decoy_player_clearance = 96.0f;
	inline constexpr float k_decoy_spacing = 128.0f;
	// A walking decoy's legs: between recorded floor spots this far apart.
	inline constexpr float k_decoy_step_min = 64.0f;
	inline constexpr float k_decoy_step_max = 320.0f;

	// One candidate or live decoy as the worker sees it; id 0 is an empty slot.
	struct decoy_probe
	{
		uint32_t id {};
		vec3 origin;
	};

	using decoy_probe_set = std::array<decoy_probe, k_max_decoys_per_viewer>;

	// Floor points players have stood on, one per 48x48x64 cell, oldest replaced
	// first once full.
	class decoy_spot_history
	{
	public:
		void clear();
		void record(vec3 feet);
		std::span<const vec3> points() const
		{
			return spots_;
		}

	private:
		std::vector<vec3> spots_;
		std::vector<uint64_t> keys_;
		std::unordered_set<uint64_t> occupied_;
		uint32_t next_ {};
	};

	// A wallhack draws only what is on its user's screen, so a new decoy goes in
	// front of the viewer when it can: within this many degrees of his view yaw.
	inline constexpr float k_decoy_view_half_angle = 60.0f;

	struct decoy_spot_query
	{
		vec3 viewer_eye;
		std::span<const vec3> enemies;	// the viewer's living enemies (feet)
		std::span<const vec3> players;	// every living player (feet)
		std::span<const vec3> taken;	// the viewer's other decoys
		uint32_t seed {};
		float view_yaw_degrees {std::numeric_limits<float>::quiet_NaN()}; // NaN: no preference
	};

	// Tries up to 32 random spots and keeps, among those whose body centre the
	// map geometry or an occluder hides from the viewer's eye, the closest one in
	// front of the viewer (k_decoy_view_half_angle), else the closest one. The
	// worker proves it hidden from every viewing origin before it is sent.
	bool choose_decoy_spot(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3& spot);

	// The next point for a walking decoy standing at from: a recorded floor
	// spot 64-320 units away on a straight path clear of the map and occluders
	// at knee and chest height, with floor under it and a slope a player can
	// walk, meeting every rule of choose_decoy_spot; its end and middle are
	// hidden from the viewer's eye. query.taken must not include this decoy.
	bool choose_decoy_step(const bvh8_data& data, std::span<const visibility_occluder> occluders, const decoy_spot_history& history,
						   const decoy_spot_query& query, vec3 from, vec3& next);

	enum class decoy_proof : uint8_t
	{
		hidden,	  // hidden from every viewing origin
		seen,	  // some origin may see part of it
		unproven, // the deadline came first, or the input was bad
	};

	// Whether a decoy standing at origin is hidden from every viewing origin:
	// the exact body test, then padded bounds corners. Smoke does not count.
	// Only a hidden decoy may be sent; an unproven one is not sent either, but
	// it is not proof that the viewer could see it.
	decoy_proof prove_decoy_hidden(const bvh8_data& data, const visibility_origin_points& origins, vec3 origin,
								   std::span<const visibility_occluder> occluders, std::chrono::steady_clock::time_point deadline);

	inline bool decoy_hidden_from_origins(const bvh8_data& data, const visibility_origin_points& origins, vec3 origin,
										  std::span<const visibility_occluder> occluders, std::chrono::steady_clock::time_point deadline)
	{
		return prove_decoy_hidden(data, origins, origin, occluders, deadline) == decoy_proof::hidden;
	}

	vec3 view_forward(float pitch_degrees, float yaw_degrees);

	// Whether a direction from the eye points at a decoy's body: within its
	// angular size plus a small tolerance. The direction need not be unit
	// length: a view (view_forward) or the line to where a bullet hit.
	bool direction_on_decoy(vec3 eye, vec3 direction, vec3 decoy_origin);

	inline bool aim_on_decoy(vec3 eye, float pitch_degrees, float yaw_degrees, vec3 decoy_origin)
	{
		return std::isfinite(pitch_degrees) && std::isfinite(yaw_degrees) && direction_on_decoy(eye, view_forward(pitch_degrees, yaw_degrees), decoy_origin);
	}

	// How far the aim followed a decoy between two samples that were both on
	// it, in degrees: the smaller of how much the direction to it turned (the
	// viewer or the decoy moved) and how much the view turned. A decoy walking
	// through a still crosshair, a crosshair sweeping over a still decoy, or
	// walking straight at it follows nothing.
	float decoy_follow_degrees(vec3 eye_before, vec3 forward_before, vec3 decoy_before, vec3 eye_now, vec3 forward_now, vec3 decoy_now);

	// Whether a direction points at any of these players' bodies (feet origins,
	// the same body as a decoy): an aim or shot at a decoy that is also at a
	// player, an enemy he hears or a teammate he follows, proves nothing.
	bool direction_on_any_player(vec3 eye, vec3 direction, std::span<const vec3> players);

	inline bool aim_on_any_player(vec3 eye, float pitch_degrees, float yaw_degrees, std::span<const vec3> players)
	{
		return std::isfinite(pitch_degrees) && std::isfinite(yaw_degrees)
			   && direction_on_any_player(eye, view_forward(pitch_degrees, yaw_degrees), players);
	}

	// How long a decoy must have reached its viewer's client before his aim or
	// shot counts: the round trip (the decoy travels to him, his view back) plus
	// a reaction. A crosshair already on the spot before then must leave it and
	// come back.
	inline constexpr float k_decoy_reaction_ms = 150.0f;

	inline bool decoy_delivery_ready(bool delivered_now, float delivered_ms, float rtt_ms)
	{
		return delivered_now && std::isfinite(delivered_ms) && delivered_ms >= k_decoy_reaction_ms + std::clamp(std::isfinite(rtt_ms) ? rtt_ms : 0.0f, 0.0f, 500.0f);
	}

	// Each decoy reaches its viewer's client in one unbroken run at most. Once
	// it has been sent, a moment without a fresh proof (the worker out of time,
	// a late result) does not take it away for up to k_decoy_latch_ms; when it
	// does go (proven seen, its enemy in view, the viewer gone, the latch
	// over), the run is over for good and the decoy is retired. A client that
	// lost an entity a moment ago may still be acknowledging the snapshot that
	// took it, and sending it back then is exactly what CS2's delta encoding
	// handles worst; an entity that leaves once never comes back that way.
	inline constexpr float k_decoy_latch_ms = 250.0f;

	struct decoy_delivery_state
	{
		bool proven_hidden {};	// a fresh result for this decoy proves it hidden
		bool proven_exposed {}; // a fresh result for this decoy proves it seen
		bool viewer_gone {};	// a fresh result has the viewer dead or gone
		bool target_visible {}; // a fresh result has its enemy in the viewer's sight
		bool running {};		// it has been sent and its run has not ended
		bool ended {};			// its run is over
		float unproven_ms {};	// since the last fresh proof while running
	};

	inline bool decoy_may_deliver(const decoy_delivery_state& state)
	{
		if (state.ended || state.viewer_gone || state.target_visible || state.proven_exposed)
		{
			return false;
		}
		return state.proven_hidden || (state.running && std::isfinite(state.unproven_ms) && state.unproven_ms <= k_decoy_latch_ms);
	}

	// A parked decoy entity is used again only after it has been withheld from
	// everyone this long.
	inline constexpr float k_decoy_reuse_quarantine_ms = 1000.0f;

	// One decoy in k_decoy_control_one_in is a control: a twin created, placed,
	// walked and proven hidden like the others, but CheckTransmit withholds it
	// from its viewer too, so no client ever has it and no cheat can see it.
	// It is "ready" exactly when a real decoy would be (the engine would have
	// sent it: same PVS), so a player's reports at controls per second of
	// readiness are his honest coincidence rate.
	inline constexpr uint32_t k_decoy_control_one_in = 3;

	// Reports and seconds of readiness (summed over decoys) at real decoys and
	// at controls, for one player or the whole server.
	struct decoy_exposure
	{
		uint64_t real_reports {};
		uint64_t control_reports {};
		double real_seconds {};
		double control_seconds {};
	};

	// Before the server has data: one coincidence per 500 decoy-seconds, as if
	// seen over 600 control seconds; a player's own controls weigh like 60.
	inline constexpr double k_decoy_prior_rate = 0.002;
	inline constexpr double k_decoy_server_prior_seconds = 600.0;
	inline constexpr double k_decoy_player_prior_seconds = 60.0;

	// The server's honest coincidence rate, reports per decoy-second: every
	// player's reports at control twins over their readiness, drawn towards
	// the prior while there is little of it.
	inline double decoy_server_rate(const decoy_exposure& server, double prior_rate = k_decoy_prior_rate)
	{
		return (static_cast<double>(server.control_reports) + prior_rate * k_decoy_server_prior_seconds)
			   / (std::max(server.control_seconds, 0.0) + k_decoy_server_prior_seconds);
	}

	// Real reports an honest player would be expected to have: his coincidence
	// rate (his own controls, drawn towards the server's rate when he has little
	// control time) times his real decoy readiness.
	inline double decoy_expected_reports(const decoy_exposure& player, const decoy_exposure& server, double prior_rate = k_decoy_prior_rate)
	{
		const double player_rate = (static_cast<double>(player.control_reports) + decoy_server_rate(server, prior_rate) * k_decoy_player_prior_seconds)
								   / (std::max(player.control_seconds, 0.0) + k_decoy_player_prior_seconds);
		return player_rate * std::max(player.real_seconds, 0.0);
	}

	// Evidence against a player: real reports beyond what an honest player
	// with the same readiness could plausibly have, the expected count plus
	// three standard deviations (Poisson). An honest player stays at 0 however
	// long he plays; a wallhack sees only the real decoys.
	inline double decoy_evidence(const decoy_exposure& player, const decoy_exposure& server, double prior_rate = k_decoy_prior_rate)
	{
		const double expected = decoy_expected_reports(player, server, prior_rate);
		const double excess = static_cast<double>(player.real_reports) - expected - 3.0 * std::sqrt(expected);
		return excess > 0.0 ? excess : 0.0;
	}

	// Whether a player's evidence reached the kick threshold since his last
	// kick: a player who comes back is kicked again only after as much new
	// evidence. 0 or less never kicks.
	inline bool decoy_kick_due(double evidence, int threshold, double evidence_at_last_kick)
	{
		return threshold > 0 && std::isfinite(evidence) && evidence >= evidence_at_last_kick + static_cast<double>(threshold);
	}

	// The jump test. A ready decoy is teleported once to another hidden spot,
	// k_decoy_jump_min_degrees to k_decoy_jump_max_degrees away as seen from
	// its viewer (on his screen if he looks its way). An honest player cannot
	// see either spot; a wallhack draws the box jumping. A crosshair that was
	// not on the new spot and lands on it after a reaction, within the window,
	// and stays there, followed the jump. Control twins jump the same way.
	inline constexpr float k_decoy_jump_min_degrees = 12.0f;
	inline constexpr float k_decoy_jump_max_degrees = 50.0f;
	inline constexpr float k_decoy_jump_reaction_ms = 120.0f;
	inline constexpr float k_decoy_jump_window_ms = 900.0f;
	inline constexpr float k_decoy_jump_hold_ms = 150.0f;
	// Ready this long before it jumps, so a wallhack had it on screen.
	inline constexpr float k_decoy_jump_after_ready_ms = 1000.0f;

	// Degrees between the directions from the eye to two decoys' centres.
	inline float decoy_angle_between(vec3 eye, vec3 first_origin, vec3 second_origin)
	{
		const vec3 a {first_origin.x - eye.x, first_origin.y - eye.y, first_origin.z + k_decoy_center_height - eye.z};
		const vec3 b {second_origin.x - eye.x, second_origin.y - eye.y, second_origin.z + k_decoy_center_height - eye.z};
		const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) * std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
		if (!(length > 1e-3f) || !std::isfinite(length))
		{
			return 0.0f;
		}
		const float cosine = std::clamp((a.x * b.x + a.y * b.y + a.z * b.z) / length, -1.0f, 1.0f);
		return std::acos(cosine) * 57.29578f;
	}

	inline bool decoy_jump_distance_ok(vec3 eye, vec3 from, vec3 to)
	{
		const float degrees = decoy_angle_between(eye, from, to);
		return degrees >= k_decoy_jump_min_degrees && degrees <= k_decoy_jump_max_degrees;
	}

	// One update of the jump test. Returns true when the jump was followed;
	// done is set once the test is over either way.
	struct decoy_jump_state
	{
		float on_ms {}; // continuously on the new spot since the reaction time
		bool done {};
	};

	inline bool decoy_jump_update(decoy_jump_state& state, float since_jump_ms, bool aim_on_new_spot, float elapsed_ms)
	{
		if (state.done)
		{
			return false;
		}
		if (!std::isfinite(since_jump_ms) || since_jump_ms > k_decoy_jump_window_ms)
		{
			state.done = true;
			return false;
		}
		if (!aim_on_new_spot)
		{
			state.on_ms = 0.0f;
			return false;
		}
		if (since_jump_ms < k_decoy_jump_reaction_ms)
		{
			// Already there before anyone could react: proves nothing.
			state.done = true;
			return false;
		}
		state.on_ms += std::isfinite(elapsed_ms) ? std::max(elapsed_ms, 0.0f) : 0.0f;
		if (state.on_ms >= k_decoy_jump_hold_ms)
		{
			state.done = true;
			return true;
		}
		return false;
	}

	// Front decoys (cs2glaz_decoy_front): per viewer, an invisible phantom
	// player standing in plain view a few degrees off his crosshair, the
	// target an aimbot picks first (the nearest to the crosshair inside its
	// field of view). An honest player sees nothing there: it is not rendered,
	// casts no shadow, has no collision and nobody else receives it. It jumps
	// to another spot every few seconds and whenever a crosshair rests on it,
	// so an aimbot locked onto it follows it around (the jump test, with a
	// control twin nobody receives placed and jumped by the same rules).
	// Spots are recorded floor points in clear view of the eye (an aimbot's
	// visibility trace passes), away from every player.
	inline constexpr float k_front_min_distance = 160.0f;
	inline constexpr float k_front_max_distance = 1200.0f;
	// Off the crosshair by this much at the body centre, and never on it
	// (direction_on_decoy): an honest crosshair is not already there.
	inline constexpr float k_front_min_degrees = 3.0f;
	inline constexpr float k_front_max_degrees = 20.0f;
	// Further off than this (the viewer turned away), nearer or further than
	// these, or out of clear view: placed again.
	inline constexpr float k_front_keep_degrees = 35.0f;
	inline constexpr float k_front_keep_min_distance = 96.0f;
	inline constexpr float k_front_keep_max_distance = 1600.0f;
	inline constexpr float k_front_player_clearance = 128.0f;
	inline constexpr float k_front_head_height = 64.0f;
	// A crosshair resting on it this long makes it jump (an aimbot that locked
	// on follows; an honest crosshair that happened to be there does not).
	inline constexpr float k_front_dodge_ms = 300.0f;
	// Between jumps: the jump window, then a random pause up to this.
	inline constexpr float k_front_pause_min_ms = 400.0f;
	inline constexpr float k_front_pause_spread_ms = 2000.0f;
	// Honest coincidences at front decoys before the server has data: one per
	// 100 decoy-seconds (they stand near the crosshair, so more than behind
	// walls).
	inline constexpr double k_front_prior_rate = 0.01;

	struct front_spot_query
	{
		vec3 viewer_eye;
		float view_pitch_degrees {};
		float view_yaw_degrees {};
		std::span<const vec3> players; // every other living player (feet)
		std::span<const vec3> taken;   // the viewer's other front decoy
		const vec3* from {};		   // a jump from here: also decoy_jump_distance_ok
		uint32_t seed {};
		// The viewer's feet: a decoy never stands at his side (front_crowds_viewer).
		// NaN: not checked.
		vec3 viewer_feet {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
	};

	// A body this close to the viewer's feet across and in height would be in
	// his way (k_front_hittable_clearance, defined below with the bodies).
	bool front_crowds_viewer(vec3 viewer_feet, vec3 origin);

	// Degrees between the view and the line from the eye to a body's centre.
	float front_offset_degrees(vec3 eye, float pitch_degrees, float yaw_degrees, vec3 origin);

	// The rules without traces: distance, offset, not on the crosshair, away
	// from players and from the line to any of them, not at the viewer's side,
	// apart on screen from the taken spots, and a jump's distance from
	// query.from.
	bool front_spot_rules(const front_spot_query& query, vec3 candidate);

	// Among recorded floor spots meeting front_spot_rules whose centre and head
	// the map leaves in clear view of the eye, one near the crosshair (the
	// smallest offset after a random spread). False when none qualifies.
	bool choose_front_spot(const bvh8_data& data, const decoy_spot_history& history, const front_spot_query& query, vec3& spot);

	// Whether a front decoy standing at origin may stay: within the keep
	// limits, away from players, not at the viewer's side, and in clear view
	// of the eye.
	bool front_spot_keeps(const bvh8_data& data, const front_spot_query& query, vec3 origin);

	enum class front_move : uint8_t
	{
		stay,
		jump,	 // 12-50 degrees from where it stands (front_spot_query::from)
		replace, // anywhere near the crosshair: it may not stay
	};

	// What a front decoy does this update: placed again when it may not stay
	// (even during a jump test, which then ends); otherwise it stands while its
	// jump test runs, and jumps when a crosshair has rested on it for
	// k_front_dodge_ms or its pause is over.
	inline front_move front_decide(bool keeps, bool testing, float aim_ms, bool pause_over)
	{
		if (!keeps)
		{
			return front_move::replace;
		}
		if (testing)
		{
			return front_move::stay;
		}
		return aim_ms >= k_front_dodge_ms || pause_over ? front_move::jump : front_move::stay;
	}

	// Front decoys against triggerbots (cs2glaz_decoy_front 2): the body
	// carries a real player's collision for its viewer's client, whose
	// crosshair trace (what most triggerbots read) then finds it. The server
	// never made a physics object for it, so its own bullets pass through. If
	// one ever does not, this sees it: a bullet impact inside the body's box,
	// above its knees (a weapon on the floor or a chicken there is not it),
	// where the map has nothing within 8 units along the bullet. Half the box
	// width, its height, and the knees:
	inline constexpr float k_front_body_half_width = 20.0f;
	inline constexpr float k_front_body_height = 80.0f;
	inline constexpr float k_front_body_floor_margin = 32.0f;
	// A body that is hit by its viewer's client must stay out of his way.
	inline constexpr float k_front_hittable_clearance = 128.0f;

	bool front_body_stopped_bullet(const bvh8_data& data, vec3 body_origin, vec3 eye, vec3 impact);

	// Crosshair ghosts (crosshair_ghosts.cpp): one fake player per team, shown
	// in turns to one enemy at a time, standing in the open with its head on his
	// crosshair. A cheat takes it for a player (it is one): a triggerbot fires at
	// once, an aimbot is already on it. He cannot see it, and nobody else can see
	// the spot, so an honest shot at it within the short window is a
	// coincidence, measured on control turns where nothing is sent.
	inline constexpr float k_crosshair_head_height = 64.0f; // feet to the middle of a standing player's head
	inline constexpr float k_crosshair_min_distance = 200.0f;
	inline constexpr float k_crosshair_max_distance = 900.0f;
	inline constexpr float k_crosshair_max_pitch = 35.0f;		  // looking further up or down: no turn
	inline constexpr float k_crosshair_player_degrees = 10.0f;	  // no other player this close to the crosshair line
	inline constexpr float k_crosshair_player_clearance = 300.0f; // nor this close to the ghost
	inline constexpr float k_crosshair_wall_margin = 32.0f;		  // its head stays this far in front of what the crosshair points at
	inline constexpr float k_crosshair_body_radius = 16.0f;
	// Honest shots at the empty crosshair spot per second of open window, until
	// the server's control turns say more (decoy_server_rate).
	inline constexpr double k_crosshair_prior_rate = 0.02;

	struct crosshair_spot_query
	{
		vec3 viewer_eye;
		float view_pitch_degrees {};
		float view_yaw_degrees {};
		std::span<const vec3> players;	// every other living player (feet)
		std::span<const vec3> watchers; // every other living player (eyes): none may see the spot
		uint32_t seed {};
	};

	// Where a ghost's feet go so its head is on the viewer's crosshair: in clear
	// view from his eye, between the minimum distance and what his crosshair
	// points at, its body clear of the map, no other player near the crosshair
	// line or the spot, and no other player with a line of sight to it (nobody
	// else can see it or shoot through it).
	bool choose_crosshair_spot(const bvh8_data& data, const crosshair_spot_query& query, vec3& feet);

	// Whether a shot (a direction from the eye, any length) passes through a
	// standing ghost's body, head included.
	bool direction_on_standing_body(vec3 eye, vec3 direction, vec3 feet);

	// Blind hits: gun damage to an enemy whose pawn CS2GLAZ had not sent to the
	// attacker for k_blind_hit_unsent_ms. Honest players get some (spraying a
	// smoke, wallbanging a common spot, a teammate's call); a sound ESP or a
	// radar hack gets many more. A player's blind hits are weighed against the
	// server's share of blind hits among all gun hits, like decoy evidence.
	inline constexpr double k_blind_hit_unsent_ms = 1500.0;
	inline constexpr double k_blind_prior_share = 0.05;
	inline constexpr double k_blind_server_prior_hits = 500.0;

	inline double blind_hit_server_share(uint64_t server_blind, uint64_t server_hits)
	{
		return (static_cast<double>(server_blind) + k_blind_prior_share * k_blind_server_prior_hits)
			   / (static_cast<double>(server_hits) + k_blind_server_prior_hits);
	}

	inline double blind_hit_evidence(uint64_t blind, uint64_t hits, double server_share)
	{
		const double expected = static_cast<double>(hits) * std::clamp(std::isfinite(server_share) ? server_share : 1.0, 0.0, 1.0);
		const double excess = static_cast<double>(blind) - expected - 3.0 * std::sqrt(expected);
		return excess > 0.0 ? excess : 0.0;
	}

	// Small deterministic generator for spots and lifetimes.
	inline uint32_t decoy_random(uint32_t& state)
	{
		state ^= state << 13u;
		state ^= state >> 17u;
		state ^= state << 5u;
		return state;
	}

} // namespace cs2glaz
