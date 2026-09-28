#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cs2glaz
{

	struct server_binary_fingerprint
	{
		uint32_t size {};
		uint32_t crc32 {};
	};

	inline bool matches_server_binary_fingerprint(std::span<const server_binary_fingerprint> accepted, uint64_t actual_size, uint32_t actual_crc32)
	{
		return std::any_of(
			accepted.begin(), accepted.end(), [&](const server_binary_fingerprint& fingerprint)
			{ return fingerprint.size != 0 && fingerprint.crc32 != 0 && actual_size == fingerprint.size && actual_crc32 == fingerprint.crc32; });
	}

	enum class compatibility_state : uint8_t
	{
		compatible,
		// The server binary is not the one the gamedata was verified against. Only
		// schema fields and SDK-level layouts are used: walls filter with a
		// conservative hull-shaped body; bones, smoke and debug beams stay off.
		limited,
		update_required,
		unsupported_system,
		error
	};

	struct compatibility_report
	{
		compatibility_state state {compatibility_state::error};
		std::string technical_detail;
		std::vector<std::string> missing_capabilities;
		std::string operator_action;
	};

	inline compatibility_report make_compatibility_report(compatibility_state state, std::string detail, std::vector<std::string> missing = {})
	{
		compatibility_report report {state, std::move(detail), std::move(missing), {}};
		switch (state)
		{
			case compatibility_state::compatible:
				break;
			case compatibility_state::limited:
				report.operator_action =
					"Walls-only protection is active. Install gamedata verified for this CS2 build to restore animated capsules and smoke.";
				break;
			case compatibility_state::update_required:
				report.operator_action = "Install the CS2GLAZ package built for this CS2 server version.";
				break;
			case compatibility_state::unsupported_system:
				report.operator_action = "Move the server to an operating system and CPU with AVX support.";
				break;
			case compatibility_state::error:
				report.operator_action = "Run cs2glaz_metrics, then repair or reinstall the reported component.";
				break;
		}
		return report;
	}

	inline const char* compatibility_state_name(compatibility_state state)
	{
		switch (state)
		{
			case compatibility_state::compatible:
				return "compatible";
			case compatibility_state::limited:
				return "limited";
			case compatibility_state::update_required:
				return "update required";
			case compatibility_state::unsupported_system:
				return "unsupported system";
			case compatibility_state::error:
				return "error";
		}
		return "error";
	}

} // namespace cs2glaz
