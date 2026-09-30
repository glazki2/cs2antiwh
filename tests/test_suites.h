#pragma once

// Names the two assert-based test groups and their shared worker benchmark.
// Tests return normally on success and abort at the exact failed assertion.

#include "bvh8.h"

#include <filesystem>
#include <string>

void run_map_and_bvh_tests(const std::filesystem::path& directory, const std::filesystem::path& test_executable);
void run_structure_tests();
void run_physics_import_tests(const std::filesystem::path& test_executable);
void run_visibility_and_transmit_tests();
void run_decoy_tests();
double run_worker_benchmark(const cs2glaz::bvh8_data& data, const std::string& label);
