#pragma once

// Declares the one CS2GLAZ plugin object and the fixed runtime state shared by
// its modules. Live engine objects stay with game-thread/CheckTransmit callers;
// the worker receives copied snapshots, and uncertain state must fail open.

#include "anticheat_bridge.h"
#include "automatic_baker.h"
#include "lifecycle_guard.h"
#include "map_source.h"
#include "runtime_compatibility.h"
#include "settings.h"
#include "smoke_layout_check.h"
#include "transmit_journal.h"
#include "transmit_masks.h"
#include "updater.h"
#include "visibility_worker.h"

#include <ISmmPlugin.h>
#include <eiface.h>
#include <engine/igameeventsystem.h>
#include <networksystem/inetworkmessages.h>
#include <entity2/entitysystem.h>
#include <filesystem.h>
#include <igameevents.h>
#include <iserver.h>
#include <tier1/convar.h>

#include <array>
#include <bitset>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>

PLUGIN_GLOBALVARS();

namespace cs2glaz
{

	inline constexpr uint32_t k_max_weapons = 64;
	inline constexpr uint32_t k_max_wearables = 32;
	inline constexpr uint32_t k_entity_scan_hard_limit = MAX_TOTAL_ENTITIES;
	inline constexpr uint32_t k_max_hidden_player_entities = 1 + 2 + k_max_weapons + k_max_wearables + 1;
	// Other networked entities attached below a player's scene node. More than
	// this, or a hierarchy deeper than the walk budget, reveals the player.
	inline constexpr uint32_t k_max_attached_entities = 32;
	inline constexpr uint32_t k_max_scene_nodes_walked = 256;
	inline constexpr uint8_t k_life_alive = 0;
	inline constexpr uint8_t k_team_t = 2;
	inline constexpr uint8_t k_team_ct = 3;
	inline constexpr auto k_lifecycle_fail_open = std::chrono::milliseconds(1000);
	inline constexpr auto k_smoke_copy_interval = std::chrono::milliseconds(100);
	inline constexpr auto k_grenade_scan_interval = std::chrono::milliseconds(100);
	// A decoy that reached its viewer less recently than this is not reaching him.
	inline constexpr auto k_decoy_delivery_gap = std::chrono::milliseconds(100);
	// A real decoy that has not reached its viewer for this long (outside his
	// PVS) is moved to another spot.
	inline constexpr auto k_decoy_undelivered_limit = std::chrono::milliseconds(1500);
	// Players kept in the since-load decoy records.
	inline constexpr size_t k_max_decoy_players = 4096;
	inline constexpr size_t k_max_grenade_candidates = 128;
	inline constexpr auto k_hidden_entity_quarantine = std::chrono::milliseconds(3000);
	inline constexpr uint32_t k_limited_validation_attempts = 256;
	static_assert(MAX_EDICTS == 16384);

	class game_resource_service
	{
	};

	struct live_player
	{
		CEntityInstance* pawn {};
		int pawn_entity {-1};
		uint8_t team {};
	};

	using visual_entity_group = hidden_entity_group<CEntityHandle, k_max_hidden_player_entities>;
	using attached_entity_group = hidden_entity_group<CEntityHandle, k_max_attached_entities>;
	static_assert(k_max_hidden_player_entities <= k_pair_visual_group_key_max);

	struct target_transmit_cache
	{
		CEntityInstance* pawn {};
		visual_entity_group group;
		visual_group_key group_key;
		bool group_valid {};
		attached_entity_group attached;
		bool attached_valid {};
	};

	struct player_bone_cache
	{
		CEntityInstance* pawn {};
		std::array<int32_t, k_visibility_capsule_count> indices {};
		std::chrono::steady_clock::time_point retry_after {};
		bool valid {};
	};

	// Why CheckTransmit withheld an enemy or let it through, so a wallhack that
	// flickers can be traced to a reason. Recipient snapshots and enemy pairs are
	// counted separately.
	struct transmit_decision_stats
	{
		uint64_t filtered_snapshots {};
		uint64_t full_update_snapshots {};		 // client full updates
		uint64_t full_update_filtered {};		 // of those, filtered (cs2glaz_filter_full_updates 1)
		uint64_t dead_viewer_snapshots {};		 // dead recipients filtered by their team's sight
		uint64_t changing_recipient_snapshots {}; // recipient spawned, died or changed team <1 s ago
		uint64_t hidden {};						 // walls block the pair; enemy withheld
		uint64_t in_view {};					 // rays or the reveal hold say visible
		uint64_t changing_target {};			 // enemy spawned, died or changed team <1 s ago
		uint64_t baseline {};					 // waiting for a sent baseline
		uint64_t attachment {};					 // something attached cannot be hidden with the enemy
		uint64_t group {};						 // the enemy's weapons/wearables could not be listed
		uint64_t dead_hidden {};				 // an enemy who died while hidden, kept hidden until he respawns
	};

	// Radar messages (CCSUsrMsg_ProcessSpottedEntityUpdate) carry the position and
	// yaw of spotted enemies that are outside a recipient's snapshot, so hiding a
	// player makes the server announce him there instead; these count what the
	// filter kept and dropped.
	struct radar_filter_stats
	{
		std::atomic<uint64_t> messages {};
		std::atomic<uint64_t> filtered_messages {};
		std::atomic<uint64_t> skipped_messages {}; // several recipients, stale visibility, or filtering off
		std::atomic<uint64_t> kept_entries {};
		std::atomic<uint64_t> dropped_entries {};
		std::atomic<uint64_t> unmatched_entries {}; // kept: not a live player's pawn index
	};

	// What CheckTransmit last decided for one recipient/enemy pair; shown by the
	// cs2glaz_why.
	enum class pair_decision : uint8_t
	{
		none,
		hidden,
		in_view,
		changing,
		attachment,
		group,
		baseline,
		full_update,
		recipient_changing,
	};

	struct runtime_timing_stats
	{
		double latest_ms {};
		double total_ms {};
		double maximum_ms {};
		uint64_t calls {};

		void record(double milliseconds)
		{
			latest_ms = milliseconds;
			total_ms += milliseconds;
			if (milliseconds > maximum_ms)
			{
				maximum_ms = milliseconds;
			}
			++calls;
		}

		double average_ms() const
		{
			return calls == 0 ? 0.0 : total_ms / static_cast<double>(calls);
		}
	};

	struct qangle
	{
		float x {};
		float y {};
		float z {};
	};

	int entity_index(CEntityInstance* entity);
	CEntityHandle entity_handle(CEntityInstance* entity);
	bool valid_networked_edict_index(int index);
	int resolve_entity_index(CGameEntitySystem* system, CEntityHandle handle);
	void print_transmit_decisions(const char* scope, const transmit_decision_stats& stats);
	void print_quick_resends(const char* scope, const quick_resend_counts& counts);
	// cs2glaz_filter_dead: dead players only get what their living team sees.
	bool filter_dead_players_requested();
	bool cs2glaz_filter_full_updates_value();
	GameEventKeySymbol_t game_event_key(const char* name);

	// Experimental decoys (cs2glaz_decoys, decoys.cpp): server functions found by
	// byte pattern, used only when every one is found exactly once.
	struct decoy_functions
	{
		void* create_entity_by_name {};
		void* dispatch_spawn {};
		void* remove_entity {};
		void* set_model {};
		uint32_t teleport_vtable_index {};
		bool ready {};
		std::string error;
	};

	struct decoy_slot
	{
		uint32_t id {}; // 0 = empty
		uint32_t target {};
		vec3 origin;
		float yaw {};
		CEntityHandle handle;
		bool spawned {};
		// A control twin (k_decoy_control_one_in): created and walked like the
		// others but withheld from its viewer too; reports at it are his honest
		// coincidences.
		bool control {};
		// Driven by a ghost player's pawn (ghosts.cpp) instead of a prop.
		bool ghost {};
		std::chrono::steady_clock::time_point spawned_at;
		std::chrono::steady_clock::time_point expires;
		float aim_ms {};
		float aim_turn {}; // degrees the aim followed it while on it
		vec3 aim_origin;   // where it stood at the previous on-target sample
		bool aim_reported {};
		bool shot_reported {};
		// The jump test (decoy_jump_update): once per decoy.
		bool jumped {};
		std::chrono::steady_clock::time_point jump_at;
		decoy_jump_state jump;
		// Reports count only while it has reached the viewer's client long enough
		// (decoy_delivery_ready); a crosshair already on it before then must
		// leave it first.
		bool ready {};
		bool aimed_before_ready {};
		// Walking: legs between recorded floor spots, with pauses.
		void* teleport {}; // this entity's Teleport, checked when spawned
		std::string model;
		vec3 goal;
		float speed {};
		bool moving {};
		std::chrono::steady_clock::time_point pause_until;
	};

	// A spawned decoy kept for reuse: withheld from everyone like a removed one.
	// Each new prop logs "has no model name" once (it is spawned before its
	// model so it never gets a physics object); a reused one logs nothing.
	struct parked_decoy
	{
		CEntityHandle handle;
		void* teleport {};
		std::string model;
		int mode {};
		std::chrono::steady_clock::time_point parked_at; // reused only k_decoy_reuse_quarantine_ms later
	};

	// What CheckTransmit needs about a live decoy; guarded by the transmit lock.
	struct decoy_transmit_entry
	{
		CEntityHandle handle;
		uint32_t id {};
		uint32_t target {};
		bool control {}; // withheld from everyone, its viewer included
	};

	// When a decoy reached its viewer: CheckTransmit left it in his list and
	// the engine had it there (inside his PVS). It reaches him in one run at
	// most (decoy_may_deliver); when the run ends, the decoy is retired.
	// Guarded by the transmit lock.
	struct decoy_delivery
	{
		uint32_t id {};
		std::chrono::steady_clock::time_point first_sent;
		std::chrono::steady_clock::time_point last_sent;
		std::chrono::steady_clock::time_point last_proven; // last fresh proof while running
		bool ended {};									   // its one run is over (decoy_may_deliver)
	};

	// One player's decoy record since the plugin loaded, kept by SteamID64
	// across maps and reconnects.
	struct decoy_player_record
	{
		std::string name;
		uint32_t aims {};
		uint32_t shots {};
		uint32_t control_aims {};
		uint32_t control_shots {};
		uint32_t jumps {};		   // jumps of real decoys his aim followed
		uint32_t control_jumps {}; // the same at control twins
		// Blind hits: gun hits, and those on an enemy not sent to him.
		uint32_t gun_hits {};
		uint32_t blind_hits {};
		double blind_evidence_marked {}; // whole blind-hit evidence last acted on
		double real_seconds {};	   // readiness of his real decoys, summed
		double control_seconds {}; // readiness of his control twins, summed
		double evidence_at_kick {}; // decoy_evidence when cs2glaz_decoy_kick last kicked him
		double evidence_reported {}; // whole evidence last reported to CSVILKA
		uint32_t kicks {};
		bool this_map {};

		decoy_exposure exposure() const
		{
			return {static_cast<uint64_t>(aims) + shots + jumps, static_cast<uint64_t>(control_aims) + control_shots + control_jumps, real_seconds,
					control_seconds};
		}
	};

	struct decoy_counters
	{
		// Transmit ticks, guarded by the transmit lock: a proven-hidden decoy the
		// engine had in its viewer's list (so it reached him), or not (outside
		// his PVS; CS2GLAZ never adds entities, so it was not sent).
		uint64_t ticks_sent {};
		uint64_t ticks_outside_pvs {};
		uint64_t candidates {};
		uint64_t spawned {};
		uint64_t reused {};
		uint64_t exposed {};
		uint64_t spawn_failures {};
		uint64_t aims {};
		uint64_t shots {};
		uint64_t kicks {};
		uint64_t controls {};
		uint64_t control_aims {};
		uint64_t control_shots {};
		uint64_t undelivered {}; // real decoys moved after the engine kept them from their viewer
		uint64_t latched_ticks {}; // sent through a moment without a fresh proof (k_decoy_latch_ms)
		uint64_t jumps {};		   // decoys (and twins) that made their jump
		uint64_t jump_follows {};
		uint64_t control_jump_follows {};
		uint64_t gun_hits {};	   // since load, by humans on enemies
		uint64_t blind_hits {};
		uint64_t runs_ended {};	   // retired because their one run reached its end
	};

	// A fake player that exists for one viewer only (ghosts.cpp): ESPs that
	// draw only players (controllers 1-64 and their pawns) show it, and its
	// pawn stands in for one of his decoys.
	inline constexpr size_t k_max_ghosts = 2;

	struct ghost_player
	{
		int slot {-1};						  // its player slot; -1 none
		uint32_t viewer {k_max_players};	  // the one player who receives it
		uint8_t team {};					  // the team it plays on (its viewer's enemy)
		std::string name;
		std::chrono::steady_clock::time_point created_at;
		std::chrono::steady_clock::time_point last_join_attempt;
		std::chrono::steady_clock::time_point released_at; // last decoy run over
		std::chrono::steady_clock::time_point unused_since;
		uint32_t join_attempts {};
		uint32_t driving_id {}; // the decoy slot id it stands in for; 0 none
		CEntityHandle pawn;		// its pawn while alive and kept harmless
		bool harmless {};		// fields applied this update
		bool has_bomb {};
		int32_t health {};		// when it was last made harmless
		std::chrono::steady_clock::time_point bomb_since;
	};

	// What CheckTransmit needs about a ghost; guarded by the transmit lock.
	struct ghost_transmit_entry
	{
		int controller {-1};	// entity index of its controller
		uint32_t viewer {k_max_players};
		CEntityHandle pawn;		// sent to the viewer only through its decoy slot
		CEntityHandle observer; // never sent
	};

	struct ghost_counters
	{
		uint64_t created {};
		uint64_t kicked {};
		uint64_t join_commands {};
		uint64_t runs {};
		uint64_t suicides {}; // its team had nobody else alive
		uint64_t bomb_drops {};
		uint64_t observers_moved {};
		uint64_t events_hidden {};
		uint64_t radar_entries {};
	};

	enum class decoy_report : uint8_t
	{
		aim,
		shot,
		jump,
	};

	struct view_sample
	{
		bool valid {};
		vec3 eye;
		float pitch {};
		float yaw {};
	};

	class plugin final : public ISmmPlugin, public IMetamodListener, public IGameEventListener2
	{
	public:
		bool Load(PluginId id, ISmmAPI* api, char* error, size_t max_length, bool late) override;
		bool Unload(char* error, size_t max_length) override;

		void AllPluginsLoaded() override {}
		// The bridge with CSVILKA (bridge.cpp, anticheat_bridge.h).
		void* OnMetamodQuery(const char* iface, int* ret) override;
		void OnPluginLoad(PluginId id) override;
		void OnPluginUnload(PluginId id) override;
		void mark_suspect(uint64_t xuid, float seconds, const char* reason, const char* source);
		bool filtering_active_now() const;
		bool decoy_evidence_for(uint64_t xuid, anticheat_bridge::decoy_evidence& evidence) const;
		void suspect_command(const CCommand& args);
		// The transmit journal (journal.cpp, transmit_journal.h).
		void entity_command(const CCommand& args);

		void OnLevelInit(char const* map_name, char const*, char const*, char const*, bool, bool) override;
		void OnLevelShutdown() override;
		void hook_game_frame(bool simulating, bool first_tick, bool last_tick);
		void hook_check_transmit(CCheckTransmitInfo** infos, int count, CBitVec<MAX_EDICTS>&, CBitVec<MAX_EDICTS>&, const Entity2Networkable_t**,
								 const uint16*, int);
		KHook::Return<void> khook_game_frame(IServerGameDLL* server, bool simulating, bool first_tick, bool last_tick);
		KHook::Return<void> khook_check_transmit(ISource2GameEntities* entities, CCheckTransmitInfo** infos, int count,
												 CBitVec<MAX_EDICTS>& union_transmit, CBitVec<MAX_EDICTS>& union_transmit_always,
												 const Entity2Networkable_t** networkables, const uint16* entity_indices, int entity_index_count);
		KHook::Return<int> khook_load_events_from_file(IGameEventManager2* manager, const char* filename, bool search_all);
		KHook::Return<void> khook_post_event(IGameEventSystem* system, CSplitScreenSlot slot, bool local_only, int client_count, const uint64* clients,
											 INetworkMessageInternal* event, const CNetMessage* data, unsigned long size, NetChannelBufType_t buffer);
		void FireGameEvent(IGameEvent* event) override;
		void print_status() const;
		void print_metrics() const;
		void print_radar_filter() const;
		void print_why(const std::string& filter);
		void print_why_probe(const player_state& viewer, const player_state& enemy, const visibility_result& result) const;
		std::string entity_model_name(CEntityInstance* entity) const;
		void scan_occluder_candidates(CGameEntitySystem* system);
		bool read_occluder(CEntityInstance* entity, occluder_kind kind, visibility_occluder& output) const;
		void capture_occluders(CGameEntitySystem* system, visibility_snapshot& value, std::chrono::steady_clock::time_point now);
		void print_props(float radius);
		void capture_dead_viewers(CGameEntitySystem* system, const std::array<lifecycle_key, k_max_players>& keys, float game_time,
								  visibility_snapshot& value) const;
		int forcecamera_mode() const;
		std::string slot_name(CGameEntitySystem* system, uint32_t slot) const;
		void filter_radar_message(const uint64* clients, const CNetMessage* data);
		void print_help() const;
		void reload_config();
		void check_config() const;
		void check_update();
		void config_loaded();
		void settings_changed(uint32_t changes);
		// Status commands call this so a hibernating server (no game frames) still
		// finishes a completed bake and the pending limited-mode check.
		void refresh_state();
		void reset_transmit_state();

		const char* GetAuthor() override
		{
			return "glazki2";
		}

		const char* GetName() override
		{
			return "CS2GLAZ";
		}

		const char* GetDescription() override
		{
			return "Server-side fog-of-war visibility culling for Counter-Strike 2";
		}

		const char* GetURL() override
		{
			return "https://github.com/glazki2/cs2antiwh";
		}

		const char* GetLicense() override
		{
			return "MIT";
		}

		const char* GetVersion() override
		{
			return CS2GLAZ_VERSION;
		}

		const char* GetDate() override
		{
			return __DATE__;
		}

		const char* GetLogTag() override
		{
			return "CS2GLAZ";
		}

	private:
		bool resolve_map_source(const std::string& map, map_source& source, std::string& error) const;
		bool load_map_bake(const std::filesystem::path& path, const std::string& map, const map_source& source, bvh8_data& data,
						   std::string& error) const;
		void start_automatic_bake(const std::string& map, const map_source& source, const std::filesystem::path& output, const std::string& reason);
		void poll_automatic_bake();
		void activate(bvh8_data data);
		void announce_active();
		void finish_limited_validation(bool simulating);
		void request_map_change(const std::string& map);
		void finish_config_load(bool success);
		void change_map(const std::string& map);
		void disable(std::string reason);
		bool validate_limited_runtime(std::string& error) const;
		bool checktransmit_layout_plausible(CCheckTransmitInfo** infos, int count) const;
		bool checktransmit_lists_readable(CCheckTransmitInfo** infos, int count) const;
		bool checktransmit_recipients_consistent(CCheckTransmitInfo** infos, int count) const;
		CGameEntitySystem* entity_system() const;
		CEntityInstance* controller(uint32_t slot) const;
		CEntityInstance* pawn(CEntityInstance* controller) const;
		lifecycle_key player_lifecycle(uint32_t slot, CGameEntitySystem* system, live_player* live) const;
		weapon_muzzle_class active_weapon_muzzle_class(CGameEntitySystem* system, CEntityInstance* pawn) const;
		void collect_smoke_entities(CGameEntitySystem* system, float game_time, bool include_candidates,
									std::array<CEntityInstance*, k_max_smoke_volumes>& smokes, size_t& smoke_count, bool& smoke_overflow);
		void verify_runtime_smoke_layout(const std::array<CEntityInstance*, k_max_smoke_volumes>& smokes, size_t count, float game_time);
		// What the smoke layout check read at one volume offset of a live smoke.
		struct smoke_layout_probe
		{
			bool readable {};
			bool header_ok {};
			bool voxels_read {};
			bool voxels_ok {};
			vec3 center;
			float center_distance {};
			float age {};
			int32_t frame {-1};
			const std::byte* storage {};
			smoke_voxel_stats voxels;
		};
		smoke_layout_probe probe_smoke_layout(const CEntityInstance* smoke, uint32_t volume_offset, vec3 detonation, float game_time) const;
		void write_smoke_layout_report(const CEntityInstance* smoke, vec3 detonation, float game_time, const smoke_layout_probe& probe,
									   uint32_t matches);
		bool smoke_header_readable(const CEntityInstance* smoke) const;
		void print_smoke_layout() const;
		const char* smoke_layout_summary() const;
		bool collect_player_visual_group(CGameEntitySystem* system, CEntityInstance* pawn, visual_entity_group& group) const;
		bool collect_attached_entities(CGameEntitySystem* system, CEntityInstance* pawn, const visual_entity_group& owned,
									   attached_entity_group& attached) const;
		bool group_fully_marked(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* bits, const visual_entity_group& group) const;
		void withhold_dead_hidden(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count);
		void forget_pair_decisions();
		template<size_t max_count>
		void withhold_group(CGameEntitySystem* system, int recipient, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* second_list,
							const hidden_entity_group<CEntityHandle, max_count>& group, withhold_reason reason);
		// Clears an entity in both lists of one recipient and notes it in his
		// journal when anything was set. Transmit lock held.
		bool withhold_entity(int recipient, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* second_list, int index, withhold_reason reason);
		void apply_check_transmit(CCheckTransmitInfo** infos, int count, const std::shared_ptr<const visibility_result>& result, bool filtering,
								  std::chrono::steady_clock::time_point now);
		void finish_transmit_journal(CCheckTransmitInfo** infos, int count);
		double journal_now() const;
		void journal_disconnect(IGameEvent* event);
		void print_journal_summary(uint32_t slot, double from_seconds, double to_seconds) const;
		bool capture(visibility_snapshot& value, float game_time);
		bool capture_animated_capsules(CEntityInstance* pawn, uint32_t slot, player_state& player, std::chrono::steady_clock::time_point now);
		bool capture_smokes(const std::array<CEntityInstance*, k_max_smoke_volumes>& entities, size_t count, bool overflow, float game_time,
							visibility_snapshot& value);
		bool teammates_are_enemies() const;
		int decoy_mode() const;
		int decoy_entity_mode() const; // how a decoy entity is drawn: 1 not at all, 2 drawn (testing)
		void resolve_decoy_functions();
		void update_decoys(CGameEntitySystem* system, visibility_snapshot& value, std::chrono::steady_clock::time_point now);
		bool spawn_decoy(CGameEntitySystem* system, decoy_slot& slot, const std::string& model, int mode);
		void discard_decoy_entity(CEntityInstance* entity);
		void walk_decoy(CEntityInstance* entity, decoy_slot& slot, const player_state& viewer, std::span<const vec3> enemies, std::span<const vec3> living,
						std::span<const vec3> taken, std::chrono::steady_clock::time_point now, float elapsed_ms, const visibility_snapshot& value);
		void remove_decoy(CGameEntitySystem* system, decoy_slot& slot);
		void remove_all_decoys(bool remove_entities);
		void prune_decoy_graveyard(CGameEntitySystem* system);
		void publish_decoy_transmit();
		void withhold_decoys(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count, const visibility_result* result,
							 std::chrono::steady_clock::time_point now);
		// Ghost players (ghosts.cpp).
		bool ghosts_available(std::string& reason) const;
		void update_ghosts(CGameEntitySystem* system, const visibility_snapshot& value, std::chrono::steady_clock::time_point now);
		bool ghost_take(CGameEntitySystem* system, uint32_t viewer, decoy_slot& slot, std::chrono::steady_clock::time_point now);
		void ghost_release(uint32_t decoy_id, std::chrono::steady_clock::time_point now);
		bool ghost_pawn_alive(CGameEntitySystem* system, CEntityHandle pawn) const;
		void ghost_command(int slot, const char* command);
		void kick_ghost(ghost_player& ghost, const char* reason);
		void kick_all_ghosts(const char* reason);
		void publish_ghost_transmit();
		void withhold_ghosts(CGameEntitySystem* system, CCheckTransmitInfo** infos, int count);
		bool ghost_event(IGameEvent* event) const;
		bool ghost_slot(int slot) const
		{
			return slot >= 0 && slot < static_cast<int>(k_max_players) && ((ghost_event_slots_.load(std::memory_order_relaxed) >> slot) & 1u) != 0;
		}
		bool ghost_capture_slot(uint32_t slot) const
		{
			return slot < k_max_players && ((ghost_slots_.load(std::memory_order_relaxed) >> slot) & 1u) != 0;
		}
		bool ghost_radar_entity(int index) const;
		void print_ghost_status() const;
		KHook::Return<bool> khook_fire_event(IGameEventManager2* manager, IGameEvent* event, bool dont_broadcast);
		void decoy_weapon_fire(IGameEvent* event);
		void decoy_bullet_impact(IGameEvent* event);
		void decoy_shot(uint32_t shooter, vec3 eye, vec3 direction);
		bool live_view(uint32_t slot, view_sample& view) const;
		decoy_player_record* decoy_record(uint32_t viewer);
		void report_decoy(CGameEntitySystem* system, uint32_t viewer, const decoy_slot& slot, decoy_report kind, float distance);
		bool jump_decoy(CEntityInstance* entity, decoy_slot& slot, const player_state& viewer, std::span<const vec3> enemies, std::span<const vec3> living,
						std::span<const vec3> taken, const visibility_snapshot& value, std::chrono::steady_clock::time_point now);
		void blind_hit_event(IGameEvent* event);
		void note_pawns_sent(CCheckTransmitInfo** infos, int count, const visibility_result* result);
		void write_decoy_log(uint64_t xuid, const char* event, int distance, const decoy_player_record& record) const;
		void write_decoy_map_summary();
		void print_decoy_status() const;
		bool human_player(uint32_t slot) const;

		ISmmAPI* api_ {};
		IServerGameDLL* server_ {};
		ISource2GameEntities* game_entities_ {};
		IVEngineServer2* engine_ {};
		ICvar* cvar_ {};
		ConVarRef teammates_are_enemies_;
		ConVarRef forcecamera_;
		IFileSystem* filesystem_ {};
		IGameEventManager2* game_events_ {};
		game_resource_service* game_resource_ {};
		runtime_compatibility compatibility_;
		updater_service updater_;
		// Metamod:Source 2.0 API 18 removed SourceHook; these KHook virtual hooks
		// are removed by Unload and again by Metamod when the plugin unloads.
		KHook::Virtual<IServerGameDLL, void, bool, bool, bool> game_frame_hook_ {&IServerGameDLL::GameFrame, this, nullptr,
																				  &plugin::khook_game_frame};
		KHook::Virtual<ISource2GameEntities, void, CCheckTransmitInfo**, int, CBitVec<MAX_EDICTS>&, CBitVec<MAX_EDICTS>&,
					   const Entity2Networkable_t**, const uint16*, int>
			check_transmit_hook_ {&ISource2GameEntities::CheckTransmit, this, nullptr, &plugin::khook_check_transmit};
		KHook::Virtual<IGameEventManager2, int, const char*, bool> game_event_load_hook_ {&IGameEventManager2::LoadEventsFromFile, this, nullptr,
																						 &plugin::khook_load_events_from_file};
		KHook::Virtual<IGameEventManager2, bool, IGameEvent*, bool> game_event_fire_hook_ {&IGameEventManager2::FireEvent, this,
																						  &plugin::khook_fire_event, nullptr};
		KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long,
					   NetChannelBufType_t>
			post_event_hook_ {&IGameEventSystem::PostEventAbstract, this, &plugin::khook_post_event, nullptr};
		IGameEventSystem* game_event_system_ {};
		bool post_event_hooked_ {};
		// Set once the radar message is recognised by its protobuf name; null until then.
		std::atomic<INetworkMessageInternal*> radar_message_ {};
		std::atomic<const void*> radar_verified_vtable_ {};
		std::atomic_bool radar_filter_broken_ {};
		radar_filter_stats radar_stats_;
		// AddGlobal reads the vtable through its argument, so this holds the gamedata vtable address.
		void* game_event_manager_vtable_ {};
		bool game_frame_hooked_ {};
		bool check_transmit_hooked_ {};
		bool game_event_load_hooked_ {};
		bool game_event_fire_hooked_ {};
		std::string map_;
		std::string pending_map_;
		std::string disabled_reason_ {"no map loaded"};
		map_source source_;
		bvh8_data data_;
		visibility_worker worker_;
		automatic_baker automatic_baker_;
		bool he_event_available_ {};
		he_clearance_history he_clearance_history_;
		// The last smoke copy, reused for up to k_smoke_copy_interval while the same
		// smokes and HE clearances exist (copying every volume every tick was
		// megabytes per tick on the game thread).
		std::shared_ptr<const smoke_snapshot> smoke_cache_;
		std::chrono::steady_clock::time_point smoke_cache_copied_ {};
		std::vector<std::pair<const void*, float>> smoke_cache_key_;
		std::vector<std::pair<const void*, float>> smoke_key_scratch_; // reused each tick, no allocation
		uint32_t smoke_cache_he_count_ {};
		// Limited mode: the gamedata smoke layout is proven on live smokes first.
		enum class smoke_layout_state
		{
			unchecked,
			verified,
			failed,
		};
		struct smoke_seen
		{
			uint32_t handle {};
			float first_seen {};
			float next_probe {};
			bool judged {};
		};
		smoke_layout_state smoke_layout_state_ {smoke_layout_state::unchecked};
		std::array<smoke_seen, k_max_smoke_volumes> smoke_seen_ {};
		uint32_t smoke_layout_failures_ {};
		uint32_t smoke_layout_judged_ {};
		int64_t smoke_layout_shift_ {};
		// addons/cs2glaz/logs/smoke_layout.txt is started over once per plugin
		// load and then appended to, one entry per failed check.
		bool smoke_report_started_ {};
		// Without the HE event listener, an HE grenade projectile that disappears
		// is taken as its detonation at the last position seen.
		struct tracked_grenade
		{
			uint32_t handle {};
			vec3 position;
			bool recorded {}; // its explosion is already in he_clearance_history_
		};
		std::array<tracked_grenade, 32> he_tracked_ {};
		// Smoke and HE projectiles the last full entity scan found. The scan walks
		// every entity on the map, so it runs every k_grenade_scan_interval; in
		// between only these are read (a grenade flies for a second or more
		// before it pops or explodes, far longer than the interval).
		struct grenade_candidate
		{
			CEntityHandle handle;
			bool smoke {};
		};
		std::vector<grenade_candidate> grenade_candidates_;
		std::chrono::steady_clock::time_point grenade_scan_next_ {};
		uint64_t grenade_scans_ {};
		uint32_t he_tracked_count_ {};
		uint64_t he_tracked_detonations_ {};
		// HE detonations recorded this map from the hegrenade_detonate event, and
		// the game time of the last one from either source (for cs2glaz_status).
		uint64_t he_event_detonations_ {};
		// The sequence of the last snapshot submitted to the worker (game thread).
		std::atomic<uint64_t> submitted_sequence_ {};
		// CheckTransmit ticks that waited for this tick's result, and those that
		// got it in time.
		std::atomic<uint64_t> result_waits_ {};
		std::atomic<uint64_t> result_waits_met_ {};
		float he_last_detonation_ {std::numeric_limits<float>::quiet_NaN()};
		std::array<lifecycle_guard, k_max_players> lifecycle_;
		std::array<std::array<pair_guard, k_max_players>, k_max_players> pair_guards_;
		std::array<std::array<visual_entity_group, k_max_players>, k_max_players> hidden_groups_;
		// Per recipient and target: the pawn of an enemy who died while hidden from
		// the recipient. Sending a client the body of a player it never had can
		// crash it ("CopyExistingEntity: missing client entity"; CS2Fixes keeps
		// every dead player hidden for this), so it stays hidden until he respawns.
		std::array<std::array<CEntityHandle, k_max_players>, k_max_players> dead_hidden_pawns_ {};
		std::atomic<bool> dead_hidden_live_ {}; // any pawn held in dead_hidden_pawns_
		// CheckTransmit sent everything without updating pair_decisions_.
		std::atomic<bool> pair_decisions_stale_ {};
		std::array<target_transmit_cache, k_max_players> transmit_target_cache_;
		std::array<player_bone_cache, k_max_players> player_bone_cache_;
		mutable std::mutex transmit_state_mutex_;
		runtime_timing_stats capture_timing_;
		runtime_timing_stats bone_timing_;
		runtime_timing_stats transmit_timing_;
		uint32_t capsule_players_ {};
		uint32_t capsule_failed_players_ {};
		std::chrono::steady_clock::time_point last_snapshot_ {};
		// Set from CheckTransmit when the recipient list looks structurally wrong;
		// the game thread turns it into a disabled state until the next map.
		std::atomic_bool transmit_layout_invalid_ {};
		// The recipient lists were proven readable with guarded reads this map.
		bool transmit_lists_verified_ {};
		transmit_decision_stats transmit_decisions_;
		std::array<std::array<pair_decision, k_max_players>, k_max_players> pair_decisions_ {};
		// Per recipient slot: what CS2GLAZ took from or gave back to his client,
		// kept until the slot gets another player or the map changes. Transmit
		// lock; journal_stale_ is set where CheckTransmit returns without it.
		std::array<recipient_journal, k_max_players> journals_ {};
		quick_resend_counts quick_resends_ {};
		std::chrono::steady_clock::time_point journal_epoch_ {std::chrono::steady_clock::now()};
		std::atomic_bool journal_stale_ {};
		bool disconnect_listening_ {};
		std::array<std::chrono::steady_clock::time_point, k_max_players> recipient_decided_at_ {};
		// Doors and box props found by the last entity-list walk (once a second).
		struct occluder_candidate
		{
			CEntityHandle handle;
			occluder_kind kind {};
		};
		std::vector<occluder_candidate> occluder_candidates_;
		std::chrono::steady_clock::time_point occluder_scan_next_ {};
		uint32_t occluders_active_ {};
		std::unordered_map<uint32_t, bool> occluder_class_cache_;
		std::string occluder_class_cache_map_;
		// Limited mode validates the entity system on the first simulated frame.
		bool limited_validation_pending_ {};
		uint32_t limited_validation_attempts_ {};
		uint64_t snapshot_sequence_ {};
		uint32_t active_worker_threads_ {};
		// Decoys: game-thread state, and the copy CheckTransmit reads.
		decoy_functions decoy_functions_;
		bool decoy_functions_resolved_ {};
		std::array<std::array<decoy_slot, k_max_decoys_per_viewer>, k_max_players> decoys_ {};
		std::array<std::array<decoy_transmit_entry, k_max_decoys_per_viewer>, k_max_players> decoy_transmit_ {};
		std::array<std::array<decoy_delivery, k_max_decoys_per_viewer>, k_max_players> decoy_delivery_ {};
		std::atomic_bool decoys_live_ {};
		// Removed decoys stay withheld from everyone until the game deletes them;
		// parked ones for as long as they are parked. Both go to CheckTransmit
		// through decoy_graveyard_transmit_.
		std::vector<CEntityHandle> decoy_graveyard_;
		std::vector<parked_decoy> decoy_pool_;
		std::array<CEntityHandle, k_max_players * k_max_decoys_per_viewer> decoy_graveyard_transmit_ {};
		uint32_t decoy_graveyard_count_ {};
		decoy_spot_history decoy_spots_;
		std::chrono::steady_clock::time_point decoy_spots_next_ {};
		std::chrono::steady_clock::time_point decoy_last_update_ {};
		uint32_t decoy_next_id_ {};
		uint32_t decoy_seed_ {0x2545f491u};
		// Since the plugin loaded: every player's record by SteamID64, and the
		// whole server's totals (the baseline of honest coincidences).
		std::unordered_map<uint64_t, decoy_player_record> decoy_players_;
		decoy_exposure decoy_server_ {};
		decoy_counters decoy_counters_;
		std::array<view_sample, k_max_players> last_view_ {};
		// When each target's pawn was last in each recipient's final lists
		// (journal clock, seconds), for blind hits; transmit lock.
		std::array<std::array<double, k_max_players>, k_max_players> pawn_sent_at_ {};
		// Last time CheckTransmit sent everything without the lock (journal clock).
		std::atomic<double> everything_sent_at_ {-1.0e9};
		bool player_hurt_listening_ {};
		std::array<ghost_player, k_max_ghosts> ghosts_ {};
		std::array<ghost_transmit_entry, k_max_ghosts> ghost_transmit_ {}; // transmit lock
		std::array<std::atomic<int>, k_max_ghosts> ghost_pawn_index_ {};   // for the radar filter
		std::atomic<uint64_t> ghost_slots_ {};		 // slots that are ghosts now
		std::atomic<uint64_t> ghost_event_slots_ {}; // the same, plus slots kicked in the last seconds
		std::array<std::chrono::steady_clock::time_point, k_max_players> ghost_kicked_at_ {};
		bool ghost_creating_ {};
		uint32_t ghost_lost_ {}; // ghosts that disappeared without CS2GLAZ kicking them
		std::string ghost_error_; // ghosts turned off until the plugin reloads
		std::chrono::steady_clock::time_point ghost_next_create_ {};
		uint32_t ghost_name_seed_ {0x9e3779b9u};
		mutable ghost_counters ghost_counters_ {};
		bool weapon_fire_listening_ {};
		bool bullet_impact_listening_ {};
		bool bullet_impact_tried_ {};
		// CSVILKA, found through Metamod and forgotten when it unloads; players it
		// (or an administrator) asked to watch, by SteamID64. Main thread only.
		struct suspect_entry
		{
			std::chrono::steady_clock::time_point expires;
			std::string reason;
			std::string source;
		};
		anticheat_bridge::csvilka* csvilka_bridge(bool force = false);
		bool is_suspect(uint32_t slot, std::chrono::steady_clock::time_point now) const;
		void print_bridge_status() const;
		anticheat_bridge::csvilka* csvilka_ {};
		PluginId csvilka_id_ {};
		std::chrono::steady_clock::time_point csvilka_next_search_ {};
		std::unordered_map<uint64_t, suspect_entry> suspects_;
		uint64_t csvilka_reports_ {};
		uint64_t suspects_marked_ {};
	};

	extern plugin g_plugin;

} // namespace cs2glaz
