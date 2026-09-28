#pragma once

#include "settings_model.h"

#include <chrono>
#include <cstdint>

class IVEngineServer2;
class ICvar;

namespace cs2glaz::settings
{

	using change_callback = void (*)(uint32_t changes);

	void initialize(IVEngineServer2* engine, ICvar* cvar, change_callback callback);
	void shutdown();

	const runtime_configuration& current();
	configuration_load_state load_state();
	std::chrono::steady_clock::time_point last_loaded();
	bool begin_load();
	bool complete_load();
	bool poll_timeout();
	bool cancel_load();
	bool loading();

	bool donttransmit_mode(int& value);
	bool playerid_mode(int& value);

} // namespace cs2glaz::settings
