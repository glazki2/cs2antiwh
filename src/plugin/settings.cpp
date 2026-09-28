#include "settings.h"

#include "plugin.h"

#include <eiface.h>
#include <tier1/convar.h>

#include <chrono>

namespace cs2glaz
{

	namespace
	{

		template<typename type>
		void on_convar_changed(CConVar<type>*, CSplitScreenSlot, const type*, const type*);

	} // namespace

	CConVar<bool> cs2glaz_enable("cs2glaz_enable", FCVAR_NONE, "Enable CS2GLAZ when map data is valid", true, on_convar_changed<bool>);
	CConVar<bool> cs2glaz_smoke_occlusion("cs2glaz_smoke_occlusion", FCVAR_NONE, "Use live CS2 smoke for visibility", true, on_convar_changed<bool>);
	CConVar<float> cs2glaz_he_clear_radius_units("cs2glaz_he_clear_radius_units", FCVAR_NONE, "HE-cleared smoke channel radius", 100.0f, true, 0.0f,
												true, 320.0f, on_convar_changed<float>);
	CConVar<float> cs2glaz_he_clear_seconds("cs2glaz_he_clear_seconds", FCVAR_NONE, "HE-cleared smoke channel duration", 2.5f, true, 0.0f, true, 10.0f,
										   on_convar_changed<float>);
	CConVar<bool> cs2glaz_filter_teammates("cs2glaz_filter_teammates", FCVAR_NONE, "Apply visibility filtering to teammates", false,
										  on_convar_changed<bool>);
	CConVar<int> cs2glaz_update_interval_ms("cs2glaz_update_interval_ms", FCVAR_NONE, "Visibility worker update interval", 1, true, 1, true, 100,
										   on_convar_changed<int>);
	CConVar<int> cs2glaz_worker_threads("cs2glaz_worker_threads", FCVAR_NONE, "Visibility worker thread count (applies on map activation)", 2, true, 1,
									   true, 4, on_convar_changed<int>);
	CConVar<float> cs2glaz_shoulder_base_units("cs2glaz_shoulder_base_units", FCVAR_NONE, "Minimum sideways shoulder origin distance", 64.0f, true,
											  0.0f, true, 256.0f, on_convar_changed<float>);
	CConVar<float> cs2glaz_shoulder_rtt_scale("cs2glaz_shoulder_rtt_scale", FCVAR_NONE,
											 "Sideways shoulder units per RTT millisecond, applied in 25 ms steps", 0.64f, true, 0.0f, true, 4.0f,
											 on_convar_changed<float>);
	CConVar<float> cs2glaz_max_shoulder_units("cs2glaz_max_shoulder_units", FCVAR_NONE, "Maximum sideways shoulder origin distance; 0 disables the cap",
											 0.0f, true, 0.0f, true, 256.0f, on_convar_changed<float>);
	CConVar<int> cs2glaz_visibility_hold_ms("cs2glaz_visibility_hold_ms", FCVAR_NONE, "Minimum revealed duration", 1000, true, 0, true, 1000,
										   on_convar_changed<int>);
	CConVar<bool> cs2glaz_debug("cs2glaz_debug", FCVAR_NONE, "Enable CS2GLAZ diagnostic logging", false, on_convar_changed<bool>);
	CConVar<int> cs2glaz_debug_los_player("cs2glaz_debug_los_player", FCVAR_NONE,
										 "Temporarily draw one 1-based player's live capsule axes, muzzle, and AABB corners; 0 removes them", 0, true,
										 0, true, static_cast<int>(k_max_players), on_convar_changed<int>);
	CConVar<bool> cs2glaz_auto_update("cs2glaz_auto_update", FCVAR_NONE, "Automatically download verified compatible stable updates", false,
									 on_convar_changed<bool>);
	CConVar<bool> cs2glaz_limited_mode("cs2glaz_limited_mode", FCVAR_NONE,
									  "Allow walls-only filtering when the CS2 server build is not the one gamedata was verified for (applies on map "
									  "activation)",
									  true, on_convar_changed<bool>);

	namespace
	{

		constexpr auto k_config_timeout = std::chrono::seconds(5);

		IVEngineServer2* config_engine {};
		ICvar* config_cvar {};
		settings::change_callback config_change_callback {};
		configuration_transaction config_transaction;
		bool restoring_configuration {};

		void on_configuration_changed()
		{
			if (restoring_configuration || config_transaction.state() == configuration_load_state::pending)
			{
				return;
			}
			const uint32_t changes = config_transaction.apply_direct(
				{cs2glaz_enable.Get(), cs2glaz_smoke_occlusion.Get(), cs2glaz_he_clear_radius_units.Get(), cs2glaz_he_clear_seconds.Get(),
				 cs2glaz_filter_teammates.Get(), cs2glaz_update_interval_ms.Get(), cs2glaz_worker_threads.Get(), cs2glaz_shoulder_base_units.Get(),
				 cs2glaz_shoulder_rtt_scale.Get(), cs2glaz_max_shoulder_units.Get(), cs2glaz_visibility_hold_ms.Get(), cs2glaz_debug.Get(),
				 cs2glaz_debug_los_player.Get(), cs2glaz_auto_update.Get(), cs2glaz_limited_mode.Get()});
			if (changes != setting_change_none && config_change_callback != nullptr)
			{
				config_change_callback(changes);
			}
		}

		template<typename type>
		void on_convar_changed(CConVar<type>*, CSplitScreenSlot, const type* new_value, const type* old_value)
		{
			if (new_value != nullptr && old_value != nullptr && *new_value != *old_value)
			{
				on_configuration_changed();
			}
		}

		runtime_configuration read_convars()
		{
			return {cs2glaz_enable.Get(),
					cs2glaz_smoke_occlusion.Get(),
					cs2glaz_he_clear_radius_units.Get(),
					cs2glaz_he_clear_seconds.Get(),
					cs2glaz_filter_teammates.Get(),
					cs2glaz_update_interval_ms.Get(),
					cs2glaz_worker_threads.Get(),
					cs2glaz_shoulder_base_units.Get(),
					cs2glaz_shoulder_rtt_scale.Get(),
					cs2glaz_max_shoulder_units.Get(),
					cs2glaz_visibility_hold_ms.Get(),
					cs2glaz_debug.Get(),
					cs2glaz_debug_los_player.Get(),
					cs2glaz_auto_update.Get(),
					cs2glaz_limited_mode.Get()};
		}

		void write_convars(const runtime_configuration& value)
		{
			restoring_configuration = true;
			cs2glaz_enable.Set(value.enable);
			cs2glaz_smoke_occlusion.Set(value.smoke_occlusion);
			cs2glaz_he_clear_radius_units.Set(value.he_clear_radius_units);
			cs2glaz_he_clear_seconds.Set(value.he_clear_seconds);
			cs2glaz_filter_teammates.Set(value.filter_teammates);
			cs2glaz_update_interval_ms.Set(value.update_interval_ms);
			cs2glaz_worker_threads.Set(value.worker_threads);
			cs2glaz_shoulder_base_units.Set(value.shoulder_base_units);
			cs2glaz_shoulder_rtt_scale.Set(value.shoulder_rtt_scale);
			cs2glaz_max_shoulder_units.Set(value.max_shoulder_units);
			cs2glaz_visibility_hold_ms.Set(value.visibility_hold_ms);
			cs2glaz_debug.Set(value.debug);
			cs2glaz_debug_los_player.Set(value.debug_los_player);
			cs2glaz_auto_update.Set(value.automatic_updates);
			cs2glaz_limited_mode.Set(value.limited_mode);
			restoring_configuration = false;
		}

	} // namespace

	namespace settings
	{

		void initialize(IVEngineServer2* engine, ICvar* cvar, change_callback callback)
		{
			config_engine = engine;
			config_cvar = cvar;
			config_change_callback = callback;
			config_transaction.apply_direct(read_convars());
		}

		void shutdown()
		{
			config_engine = nullptr;
			config_cvar = nullptr;
			config_change_callback = nullptr;
		}

		const runtime_configuration& current()
		{
			return config_transaction.active();
		}

		configuration_load_state load_state()
		{
			return config_transaction.state();
		}

		std::chrono::steady_clock::time_point last_loaded()
		{
			return config_transaction.last_loaded();
		}

		bool begin_load()
		{
			if (config_engine == nullptr || !config_transaction.begin(std::chrono::steady_clock::now()))
			{
				return false;
			}
			config_engine->ServerCommand("exec cs2glaz.cfg");
			return true;
		}

		bool complete_load()
		{
			const auto now = std::chrono::steady_clock::now();
			if (config_transaction.timed_out(now, std::chrono::duration_cast<std::chrono::milliseconds>(k_config_timeout)))
			{
				const runtime_configuration previous = config_transaction.active();
				config_transaction.rollback();
				write_convars(previous);
				return false;
			}
			const uint32_t changes = config_transaction.commit(read_convars(), now);
			if (changes != setting_change_none && config_change_callback != nullptr)
			{
				config_change_callback(changes);
			}
			return true;
		}

		bool poll_timeout()
		{
			if (!config_transaction.timed_out(std::chrono::steady_clock::now(),
											  std::chrono::duration_cast<std::chrono::milliseconds>(k_config_timeout)))
			{
				return false;
			}
			const runtime_configuration previous = config_transaction.active();
			if (!config_transaction.rollback())
			{
				return false;
			}
			write_convars(previous);
			return true;
		}

		bool cancel_load()
		{
			if (!config_transaction.rollback())
			{
				return false;
			}
			write_convars(config_transaction.active());
			return true;
		}

		bool loading()
		{
			return config_transaction.state() == configuration_load_state::pending;
		}

		bool donttransmit_mode(int& value)
		{
			value = 0;
			if (config_cvar == nullptr)
			{
				return false;
			}
			const ConVarRef reference = config_cvar->FindConVar("sv_enable_donttransmit");
			if (!reference.IsValidRef())
			{
				return false;
			}
			value = CConVarRef<bool>(reference).Get() ? 1 : 0;
			return true;
		}

		bool playerid_mode(int& value)
		{
			value = 0;
			if (config_cvar == nullptr)
			{
				return false;
			}
			const ConVarRef reference = config_cvar->FindConVar("mp_playerid");
			if (!reference.IsValidRef())
			{
				return false;
			}
			value = CConVarRef<int>(reference).Get();
			return true;
		}

	} // namespace settings

	CON_COMMAND_F(cs2glaz_status, "Show concise CS2GLAZ health and protection state", FCVAR_NONE)
	{
		g_plugin.refresh_state();
		g_plugin.print_status();
	}

	CON_COMMAND_F(cs2glaz_metrics, "Show detailed CS2GLAZ runtime metrics", FCVAR_NONE)
	{
		g_plugin.refresh_state();
		g_plugin.print_metrics();
	}

	CON_COMMAND_F(cs2glaz_help, "Show CS2GLAZ administrator commands", FCVAR_NONE)
	{
		g_plugin.print_help();
	}

	CON_COMMAND_F(cs2glaz_reload, "Reload and validate cs2glaz.cfg", FCVAR_NONE)
	{
		g_plugin.reload_config();
	}

	CON_COMMAND_F(cs2glaz_check_config, "Check current CS2GLAZ settings without changing them", FCVAR_NONE)
	{
		g_plugin.check_config();
	}

	CON_COMMAND_F(cs2glaz_check_update, "Check for a CS2GLAZ update now", FCVAR_NONE)
	{
		g_plugin.check_update();
	}

	CON_COMMAND_F(cs2glaz_config_loaded, "Confirm that cs2glaz.cfg finished loading", FCVAR_HIDDEN)
	{
		g_plugin.config_loaded();
	}

} // namespace cs2glaz
