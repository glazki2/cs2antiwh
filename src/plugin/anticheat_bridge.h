#pragma once

// The bridge between CS2GLAZ (anti-wallhack: withholds enemies a player cannot
// see, sets decoys for wallhacks) and CSVILKA (anti-cheat: aim, movement and
// client checks, evidence and punishments). The same file is in both
// projects. Change it in both, and change both interface names with any
// change to the declarations below: a plugin built against another version
// then simply does not find its partner.
//
// Each plugin answers IMetamodListener::OnMetamodQuery for its own name and
// finds the other with ISmmAPI::MetaFactory, and forgets it in
// IMetamodListener::OnPluginUnload. Every method is called on the server's
// main thread; strings are valid only during the call.

#include <cstdint>

#define CSVILKA_BRIDGE_INTERFACE "CSVILKA_ANTICHEAT_BRIDGE_001"
#define CS2GLAZ_BRIDGE_INTERFACE "CS2GLAZ_ANTICHEAT_BRIDGE_001"

namespace anticheat_bridge
{

	// What CS2GLAZ's decoys found on one player since it loaded.
	struct decoy_evidence
	{
		std::uint32_t aims;			   // aims that followed a real decoy through a wall
		std::uint32_t shots;		   // shots at a real decoy through a wall
		std::uint32_t control_reports; // aims and shots at control decoys no client ever had
		double real_seconds;		   // seconds real decoys stood ready before him
		double control_seconds;		   // the same for control decoys
		double expected;			   // real reports an honest player would have
		double evidence;			   // real reports beyond expected and three standard deviations
	};

	// Implemented by CSVILKA.
	class csvilka
	{
	public:
		// CS2GLAZ's decoys show that this player sees through walls: his evidence
		// rose by at least one since the last call. player_slot is 0-based.
		virtual void report_decoy_evidence(int player_slot, std::uint64_t steam_id64, const decoy_evidence& evidence) = 0;
		// Whether CSVILKA must never punish this player (csvilka_whitelist).
		virtual bool is_whitelisted(std::uint64_t steam_id64) = 0;

	protected:
		~csvilka() = default;
	};

	// Implemented by CS2GLAZ.
	class cs2glaz
	{
	public:
		// CSVILKA detected something on this player (reason: the detection's
		// name): watch him closely, decoys first, for this many seconds.
		virtual void mark_suspect(std::uint64_t steam_id64, float seconds, const char* reason) = 0;
		// Whether CS2GLAZ withholds enemies on this map right now.
		virtual bool filtering_active() = 0;
		// The player's decoy record since CS2GLAZ loaded; false when there is none.
		virtual bool get_decoy_evidence(std::uint64_t steam_id64, decoy_evidence& evidence) = 0;

	protected:
		~cs2glaz() = default;
	};

} // namespace anticheat_bridge
