#pragma once

// Declares the offline physics-GLB importer. It outputs accepted triangles plus
// a report explaining every geometry group; invalid input stops the bake.

#include "bvh8.h"
#include "physics_recipe.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cs2glaz
{

	bool import_physics_glb(const std::filesystem::path& path, std::vector<triangle>& triangles, import_report& report, std::string& error,
							std::vector<std::string>* triangle_surfaces = nullptr);
	
} // namespace cs2glaz
