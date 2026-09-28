#pragma once

// Reads map collision geometry straight from a compiled Source 2 resource
// (world_physics.vmdl_c, or a .vphys_c) and groups it exactly like the physics
// GLB that ValveResourceFormat 19.2 exported, so the baker needs no external
// tools. Malformed or unsupported input stops the bake with an error.

#include "bvh8.h"
#include "physics_recipe.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cs2glaz
{

	bool import_physics_resource(std::span<const uint8_t> resource, std::vector<triangle>& triangles, import_report& report, std::string& error,
								 std::vector<std::string>* triangle_surfaces = nullptr);
	bool import_physics_resource_file(const std::filesystem::path& path, std::vector<triangle>& triangles, import_report& report,
									  std::string& error, std::vector<std::string>* triangle_surfaces = nullptr);

	// Source 2 string token: MurmurHash2 of the lowercase ASCII text, seed 0x31415926.
	uint32_t source2_string_token(std::string_view text);
	// Surface property name for a token, or "unknown_surface_<token>".
	std::string surface_property_name(uint32_t token);

} // namespace cs2glaz
