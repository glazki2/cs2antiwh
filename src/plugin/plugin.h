#pragma once

// Declares the one CS2GLAZ plugin object and the fixed runtime state shared by
// its modules. Live engine objects stay with game-thread/CheckTransmit callers;
// the worker receives copied snapshots, and uncertain state must fail open.

#include "automatic_baker.h"
#include "lifecycle_guard.h"
#include "map_source.h"
#include "runtime_compatibility.h"
#include "settings.h"
#include "transmit_debug.h"
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
#include <mutex>
#include <string>

PLUGIN_GLOBALVARS();

namespace cs2glaz
{

	inline constexpr uint32_t k_max_weapons = 64;
	inline constexpr uint32_t k_max_wearables = 32;
	inline constexpr uint32_t k_entity_scan_hard_limit = MAX_TOTAL_ENTITIES;
	inline constexpr uint32_t k_max_entity_name = 64;
	inline constexpr uint32_t k_max_hidden_player_entities = 1 + 2 + k_max_weapons + k_max_wearables + 1;
	// Other networked entities attached below a player's scene node. More than
	// this, or a hierarchy deeper than the walk budget, reveals the player.
	inline constexpr uint32_t k_max_attached_entities = 32;
	inline constexpr uint32_t k_max_scene_nodes_walked = 256;
	inline constexpr uint8_t k_life_alive = 0;
	inline constexpr uint8_t k_team_t = 2;
	inline constexpr uint8_t k_team_ct = 3;
	inline constexpr auto k_lifecycle_fail_open = std::chrono::milliseconds(1000);
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
	void copy_entity_name(CEntityInstance* entity, char (&name)[k_max_entity_name]);
	bool valid_networked_edict_index(int index);
	int resolve_entity_index(CGameEntitySystem* system, CEntityHandle handle);
	void print_transmit_decisions(const char* scope, const transmit_decision_stats& stats);
	// cs2glaz_filter_dead: dead players only get what their living team sees.
	bool filter_dead_players_requested();
	bool cs2glaz_filter_full_updates_value();

	class plugin final : public ISmmPlugin, public IMetamodListener, public IGameEventListener2
	{
	public:
		bool Load(PluginId id, ISmmAPI* api, char* error, size_t max_length, bool late) override;
		bool Unload(char* error, size_t max_length) override;

		void AllPluginsLoaded() override {}

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
		bool smoke_layout_matches(const CEntityInstance* smoke, uint32_t volume_offset, vec3 detonation, float game_time) const;
		bool smoke_header_readable(const CEntityInstance* smoke) const;
		void print_smoke_layout() const;
		const char* smoke_layout_summary() const;
		bool collect_player_visual_group(CGameEntitySystem* system, CEntityInstance* pawn, visual_entity_group& group) const;
		bool collect_attached_entities(CGameEntitySystem* system, CEntityInstance* pawn, const visual_entity_group& owned,
									   attached_entity_group& attached) const;
		bool group_fully_marked(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* bits, const visual_entity_group& group) const;
		template<size_t max_count>
		void withhold_group(CGameEntitySystem* system, CBitVec<MAX_EDICTS>* primary, CBitVec<MAX_EDICTS>* second_list,
							const hidden_entity_group<CEntityHandle, max_count>& group);
		bool capture(visibility_snapshot& value, float game_time);
		bool capture_animated_capsules(CEntityInstance* pawn, uint32_t slot, player_state& player, std::chrono::steady_clock::time_point now);
		bool capture_smokes(const std::array<CEntityInstance*, k_max_smoke_volumes>& entities, size_t count, bool overflow, float game_time,
							visibility_snapshot& value);
		bool teammates_are_enemies() const;

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
		std::string map_;
		std::string pending_map_;
		std::string disabled_reason_ {"no map loaded"};
		map_source source_;
		bvh8_data data_;
		visibility_worker worker_;
		automatic_baker automatic_baker_;
		bool he_event_available_ {};
		he_clearance_history he_clearance_history_;
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
			bool judged {};
		};
		smoke_layout_state smoke_layout_state_ {smoke_layout_state::unchecked};
		std::array<smoke_seen, k_max_smoke_volumes> smoke_seen_ {};
		uint32_t smoke_layout_failures_ {};
		uint32_t smoke_layout_judged_ {};
		int64_t smoke_layout_shift_ {};
		// Without the HE event listener, an HE grenade projectile that disappears
		// is taken as its detonation at the last position seen.
		struct tracked_grenade
		{
			uint32_t handle {};
			vec3 position;
		};
		std::array<tracked_grenade, 32> he_tracked_ {};
		uint32_t he_tracked_count_ {};
		uint64_t he_tracked_detonations_ {};
		std::array<lifecycle_guard, k_max_players> lifecycle_;
		std::array<std::array<pair_guard, k_max_players>, k_max_players> pair_guards_;
		std::array<std::array<visual_entity_group, k_max_players>, k_max_players> hidden_groups_;
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
	};

	extern plugin g_plugin;

} // namespace cs2glaz
