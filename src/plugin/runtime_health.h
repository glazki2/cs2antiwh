#pragma once

#include <cstdint>

namespace cs2glaz
{

	enum class runtime_health_state : uint8_t
	{
		starting,
		loading_configuration,
		loading_map,
		baking,
		protected_state,
		disabled,
		update_required,
		unsupported_system,
		error
	};

	inline const char* runtime_health_state_name(runtime_health_state state)
	{
		switch (state)
		{
			case runtime_health_state::starting:
				return "STARTING";
			case runtime_health_state::loading_configuration:
				return "LOADING CONFIGURATION";
			case runtime_health_state::loading_map:
				return "LOADING MAP";
			case runtime_health_state::baking:
				return "BAKING";
			case runtime_health_state::protected_state:
				return "PROTECTED";
			case runtime_health_state::disabled:
				return "DISABLED";
			case runtime_health_state::update_required:
				return "UPDATE REQUIRED";
			case runtime_health_state::unsupported_system:
				return "UNSUPPORTED SYSTEM";
			case runtime_health_state::error:
				return "ERROR";
		}
		return "ERROR";
	}

} // namespace cs2glaz
