#pragma once

// The bake recipe shared by every physics importer: which collision groups count
// as sight-blocking walls, and the per-group report written next to each bake.

#include <cstdint>
#include <string>
#include <vector>

namespace cs2glaz
{

	struct physics_group_report
	{
		std::string name;
		std::string surface_property;
		std::vector<std::string> tags;
		uint64_t triangles {};
		bool accepted {};
	};

	struct import_report
	{
		uint64_t raw_triangles {};
		uint64_t accepted_triangles {};
		uint64_t rejected_invalid {};
		std::vector<physics_group_report> groups;
	};

	bool physics_group_accepted(const std::vector<std::string>& tags, const std::string& surface_property);

} // namespace cs2glaz
