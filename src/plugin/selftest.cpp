#include "plugin.h"

// cs2glaz_selftest: one command that checks every part CS2GLAZ depends on and
// says what to do about each problem. It only reads: no hook, convar or
// entity is changed, and the decoy signatures are scanned into a local copy.

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace cs2glaz
{
	namespace
	{
		// CS2 servers always run 64 ticks a second.
		constexpr double k_tick_ms = 1000.0 / 64.0;

		enum class check_level : uint8_t
		{
			ok,
			off,
			warn,
			fail,
		};

		class selftest_report
		{
		public:
			void line(check_level level, const char* name, const std::string& detail, const char* action = nullptr)
			{
				++counts_[static_cast<size_t>(level)];
				static constexpr const char* labels[] = {"OK  ", "OFF ", "WARN", "FAIL"};
				META_CONPRINTF("[CS2GLAZ] %s %-14s %s\n", labels[static_cast<size_t>(level)], name, detail.c_str());
				if (action != nullptr && level != check_level::ok)
				{
					META_CONPRINTF("[CS2GLAZ]      %-14s -> %s\n", "", action);
				}
			}

			uint32_t count(check_level level) const
			{
				return counts_[static_cast<size_t>(level)];
			}

		private:
			std::array<uint32_t, 4> counts_ {};
		};

		std::string format(const char* pattern, ...)
		{
			char buffer[512];
			va_list arguments;
			va_start(arguments, pattern);
			std::vsnprintf(buffer, sizeof(buffer), pattern, arguments);
			va_end(arguments);
			return buffer;
		}
	} // namespace

	void plugin::run_selftest()
	{
		selftest_report report;
		const runtime_configuration& configuration = settings::current();
		META_CONPRINTF("[CS2GLAZ] selftest CS2GLAZ %s\n", CS2GLAZ_VERSION);

		// Game build and gamedata.
		const compatibility_report& compatibility = compatibility_.report();
		if (!compatibility_.valid())
		{
			report.line(check_level::fail, "game build", format("%s: %s", compatibility_state_name(compatibility.state), compatibility.technical_detail.c_str()),
						compatibility.operator_action.c_str());
		}
		else if (compatibility_.limited())
		{
			report.line(configuration.limited_mode ? check_level::warn : check_level::fail, "game build",
						configuration.limited_mode ? "not the build the gamedata was verified on: limited mode (walls with a hull-shaped body)"
												   : "not the verified build and cs2glaz_limited_mode is 0",
						configuration.limited_mode ? "Works as is; animated bodies come back with gamedata for this build (an update of CS2GLAZ)."
												   : "Set cs2glaz_limited_mode 1 in cs2glaz.cfg.");
		}
		else
		{
			report.line(check_level::ok, "game build", "verified gamedata (animated bodies, smoke)");
		}

		// Configuration.
		switch (settings::loading() ? configuration_load_state::pending : settings::load_state())
		{
			case configuration_load_state::loaded:
				report.line(check_level::ok, "config", "cs2glaz.cfg loaded");
				break;
			case configuration_load_state::pending:
				report.line(check_level::warn, "config", "cs2glaz.cfg is still loading", "Wait a few seconds and run cs2glaz_selftest again.");
				break;
			case configuration_load_state::failed:
				report.line(check_level::fail, "config", "cs2glaz.cfg failed to load; the previous settings stay",
							"Run cs2glaz_check_config, fix cs2glaz.cfg, then cs2glaz_reload.");
				break;
			default:
				report.line(check_level::warn, "config", "cs2glaz.cfg was not loaded; compiled defaults are used",
							"Put cs2glaz.cfg in game/csgo/cfg/cs2glaz/ (it comes in the release archive).");
				break;
		}

		// Hooks and the transmit filter.
		if (!game_frame_hooked_ || !check_transmit_hooked_)
		{
			report.line(check_level::fail, "hooks", format("GameFrame=%s CheckTransmit=%s", game_frame_hooked_ ? "on" : "missing",
														   check_transmit_hooked_ ? "on" : "missing"),
						"Reinstall Metamod:Source 2.0 and CS2GLAZ from the latest releases.");
		}
		else if (transmit_layout_invalid_.load())
		{
			report.line(check_level::fail, "hooks", "the CheckTransmit recipient lists did not look right; filtering stopped until the next map",
						"Update CS2GLAZ; this CS2 build changed the transmit layout.");
		}
		else
		{
			report.line(check_level::ok, "hooks", "GameFrame and CheckTransmit hooked");
		}

		// Protection on this map.
		const bool protecting = configuration.enable && disabled_reason_.empty();
		if (!configuration.enable)
		{
			report.line(check_level::off, "protection", "cs2glaz_enable 0", "Set cs2glaz_enable 1 in cs2glaz.cfg.");
		}
		else if (protecting)
		{
			report.line(check_level::ok, "protection", format("active on %s (%u triangles)", map_.c_str(), data_.header.triangle_count));
		}
		else if (disabled_reason_ == "automatic bake in progress" || disabled_reason_ == "validating map" || disabled_reason_ == "no map loaded"
				 || disabled_reason_ == "loading configuration")
		{
			report.line(check_level::warn, "protection", format("not active yet: %s", disabled_reason_.c_str()),
						"Wait for it (a new map bakes once, up to a minute) and run cs2glaz_selftest again.");
		}
		else
		{
			report.line(check_level::fail, "protection", format("off: %s", disabled_reason_.c_str()), "Run cs2glaz_metrics and check the first error.");
		}

		// Work per tick, against one 15.6 ms tick.
		const worker_stats stats = worker_.stats();
		runtime_timing_stats capture_timing;
		runtime_timing_stats transmit_timing;
		{
			std::lock_guard<std::mutex> lock(transmit_state_mutex_);
			capture_timing = capture_timing_;
			transmit_timing = transmit_timing_;
		}
		const std::shared_ptr<const visibility_result> result = worker_.result();
		const double age_ms = result == nullptr ? -1.0 : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->captured).count();
		if (!protecting || stats.cycles == 0)
		{
			report.line(check_level::off, "performance", "no visibility work measured yet (needs an active map with players)");
		}
		else
		{
			const double game_thread_ms = capture_timing.average_ms() + transmit_timing.average_ms();
			const std::string detail = format("worker p99 %.2f ms, game thread %.2f ms a tick (capture %.2f, transmit %.2f), result age %.1f ms",
											  stats.recent_p99_ms, game_thread_ms, capture_timing.average_ms(), transmit_timing.average_ms(), age_ms);
			if (stats.recent_p99_ms > k_tick_ms || game_thread_ms > k_tick_ms * 0.25)
			{
				report.line(check_level::warn, "performance", detail,
							"A slow tick delays reveals by a tick; check the server's CPU load and cs2glaz_worker_threads (2-4).");
			}
			else
			{
				report.line(check_level::ok, "performance", detail);
			}
		}

		// Smoke and HE grenades.
		const bool smoke_available = result != nullptr ? result->smoke_available : compatibility_.smoke_available();
		if (!configuration.smoke_occlusion)
		{
			report.line(check_level::off, "smoke", "cs2glaz_smoke_occlusion 0", "Set cs2glaz_smoke_occlusion 1 to hide enemies behind smoke.");
		}
		else if (smoke_available)
		{
			report.line(check_level::ok, "smoke", compatibility_.limited() ? "layout verified on a live smoke" : "verified gamedata");
		}
		else if (smoke_layout_state_ == smoke_layout_state::unchecked && compatibility_.smoke_layout_candidate())
		{
			// Normal after every start on a build the gamedata does not verify.
			report.line(check_level::warn, "smoke", "waiting for the first live smoke of this run to prove the layout",
						"Nothing to fix: the first smoke thrown turns it on. To see it now, throw one (a bot round is enough) and run "
						"cs2glaz_selftest again.");
		}
		else
		{
			report.line(check_level::warn, "smoke", format("unavailable: %s", smoke_layout_summary()),
						"Enemies behind smoke are sent as without CS2GLAZ; an update of CS2GLAZ brings it back.");
		}

		// Game events.
		if (game_events_ == nullptr)
		{
			report.line(check_level::warn, "game events",
						format("unavailable%s%s%s", game_event_manager_error_.empty() ? "" : " (", game_event_manager_error_.c_str(),
							   game_event_manager_error_.empty() ? "" : ")"),
						"Shots at decoys, blind hits and ghost players are off; update CS2GLAZ (cs2glaz.signatures.txt).");
		}
		else
		{
			std::string missing;
			const auto need = [&](bool listening, const char* name)
			{
				if (!listening)
				{
					missing += missing.empty() ? name : std::string(", ") + name;
				}
			};
			if (decoy_mode() != 0)
			{
				// Listened to once decoys are on (update_decoys).
				need(weapon_fire_listening_, "weapon_fire");
				need(bullet_impact_listening_ || !bullet_impact_tried_, "bullet_impact");
				need(player_hurt_listening_, "player_hurt");
			}
			need(he_event_available_, "hegrenade_detonate");
			need(disconnect_listening_, "player_disconnect");
			if (missing.empty())
			{
				report.line(check_level::ok, "game events", "manager found, all events heard");
			}
			else
			{
				report.line(check_level::warn, "game events", format("manager found; not heard: %s", missing.c_str()),
							"Usually they come with the first map; change the map and run cs2glaz_selftest again.");
			}
		}

		// The radar filter.
		if (!post_event_hooked_)
		{
			report.line(check_level::warn, "radar", "the game event system was not found: the radar shows every spotted enemy",
						"Update CS2GLAZ.");
		}
		else if (radar_filter_broken_.load())
		{
			report.line(check_level::warn, "radar", "the radar message did not look right; the filter stopped", "Update CS2GLAZ.");
		}
		else
		{
			report.line(check_level::ok, "radar", "spotted enemies filtered");
		}

		// mp_playerid: the name under the crosshair would show hidden enemies.
		int playerid = 0;
		if (!settings::playerid_mode(playerid))
		{
			report.line(check_level::warn, "mp_playerid", "cannot read it", "Set mp_playerid 1 in server.cfg.");
		}
		else if (playerid == 0)
		{
			report.line(check_level::warn, "mp_playerid", "0: names of enemies behind walls can show", "Set mp_playerid 1 in server.cfg.");
		}
		else
		{
			report.line(check_level::ok, "mp_playerid", format("%d", playerid));
		}

		// Decoys: the signatures scanned again here, without touching the
		// decoy state the game thread uses.
		const int mode = decoy_mode();
		if (mode == 0)
		{
			report.line(check_level::off, "decoys", "cs2glaz_decoys 0",
						"To use them set cs2glaz_decoys in cs2glaz.cfg: the cfg is applied again on every map, a console value does not last.");
		}
		else
		{
			// Scanning the server's code takes tens of ms on the game thread:
			// only when the decoys have not scanned it yet.
			const decoy_functions found = decoy_functions_resolved_ ? decoy_functions_ : find_decoy_functions();
			std::string teleport_error;
			bool teleport_wrong = false;
			if (found.ready)
			{
				// The Teleport slot of the world entity's vtable must point into
				// the server, as it must for every prop that becomes a decoy.
				CGameEntitySystem* system = entity_system();
				CEntityInstance* world = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(0));
				void** vtable = nullptr;
				void* teleport = nullptr;
				if (world == nullptr)
				{
					teleport_error = "no map loaded to check the Teleport index on";
				}
				else if (!runtime_compatibility::safe_read(world, &vtable, sizeof(vtable)) || vtable == nullptr
						 || !runtime_compatibility::safe_read(vtable + found.teleport_vtable_index, &teleport, sizeof(teleport))
						 || !compatibility_.address_in_server_module(teleport))
				{
					teleport_error = "the Teleport vtable index does not point into the server";
					teleport_wrong = true;
				}
			}
			if (!found.ready)
			{
				// A failed scan, or decoys turned off at run time (a spawn that
				// did not behave): the reason says which.
				report.line(check_level::fail, "decoys", format("unavailable: %s", found.error.c_str()),
							"Restart the server; if it stays, update CS2GLAZ (cs2glaz.signatures.txt) and until then set cs2glaz_decoys 0.");
			}
			else if (!teleport_error.empty())
			{
				report.line(teleport_wrong ? check_level::fail : check_level::warn, "decoys", teleport_error,
							"Update CS2GLAZ (cs2glaz.signatures.txt); until then set cs2glaz_decoys 0.");
			}
			else
			{
				report.line(mode == 2 ? check_level::warn : check_level::ok, "decoys",
							format("mode %d (%s), signatures found", mode, mode == 1 ? "invisible" : mode == 2 ? "drawn, testing only" : "watched players"),
							"Mode 2 draws the decoys and records nothing; use 1 or 3 on a live server.");
			}

			std::string reason;
			if (!phantoms_available(reason))
			{
				report.line(check_level::warn, "phantoms", reason, "Phantom players and front decoys stay off; props are used instead.");
			}
			else
			{
				report.line(check_level::ok, "phantoms", format("up to %d", phantom_limit()));
			}
			if (front_mode() == 0)
			{
				report.line(check_level::off, "front decoys", "cs2glaz_decoy_front 0");
			}
			else if (!front_enabled())
			{
				report.line(check_level::warn, "front decoys", "configured but off: phantoms are unavailable or limited to 0");
			}
			else if (front_mode() == 2 && !front_hittable())
			{
				report.line(check_level::warn, "front decoys",
							format("against aimbots only; against triggerbots off: %s",
								   !front_hittable_error_.empty() ? front_hittable_error_.c_str()
								   : !bullet_impact_listening_	  ? "bullet_impact is not heard"
																  : "the schema lacks the collision attributes"));
			}
			else
			{
				report.line(check_level::ok, "front decoys", front_mode() == 2 ? "against aimbots and triggerbots" : "against aimbots");
			}
			if (ghost_limit() == 0)
			{
				report.line(check_level::off, "ghosts", "cs2glaz_decoy_ghosts 0",
							"Optional: ghosts are real player pawns, which cheats that skip phantoms still draw.");
			}
			else if (!ghosts_available(reason))
			{
				report.line(check_level::warn, "ghosts", reason, "Ghost players stay off; the other decoys work.");
			}
			else
			{
				report.line(check_level::ok, "ghosts", format("up to %d", ghost_limit()));
			}
		}

		// CSVILKA.
		if (csvilka_bridge(true) != nullptr)
		{
			report.line(check_level::ok, "CSVILKA", "connected: decoy evidence reaches its rules and Discord webhook");
		}
		else
		{
			report.line(check_level::off, "CSVILKA", "not loaded", "Optional: with CSVILKA decoy evidence goes to its Discord webhook.");
		}

		const uint32_t failures = report.count(check_level::fail);
		const uint32_t warnings = report.count(check_level::warn);
		META_CONPRINTF("[CS2GLAZ] selftest: %s (%u ok, %u off, %u warnings, %u failures)\n",
					   failures != 0 ? "FAILED" : warnings != 0 ? "works with warnings" : "all good", report.count(check_level::ok),
					   report.count(check_level::off), warnings, failures);
	}

	CON_COMMAND_F(cs2glaz_selftest, "Check every part CS2GLAZ depends on and say what to fix", FCVAR_NONE)
	{
		g_plugin.refresh_state();
		g_plugin.run_selftest();
	}

} // namespace cs2glaz
