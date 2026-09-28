#include "kv3.h"
#include "physics_import.h"
#include "test_suites.h"

// Native map-physics reader: LZ4 blocks, string tokens, every binary KV3 layout
// in the fixtures, physics triangles checked against ValveResourceFormat 19.2's
// export of the same files, and malformed input that must fail without crashing.

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cs2glaz;

namespace
{

	std::filesystem::path fixture(const std::filesystem::path& test_executable, const char* name)
	{
		std::vector<std::filesystem::path> roots {std::filesystem::current_path()};
		for (std::filesystem::path path = std::filesystem::absolute(test_executable).parent_path(); !path.empty() && path != path.root_path();
			 path = path.parent_path())
		{
			roots.push_back(path);
		}
		for (const std::filesystem::path& root : roots)
		{
			const std::filesystem::path candidate = root / "tests" / "fixtures" / name;
			if (std::filesystem::is_regular_file(candidate))
			{
				return candidate;
			}
		}
		assert(!"test fixture not found");
		return {};
	}

	std::vector<uint8_t> read_bytes(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		assert(stream);
		std::vector<uint8_t> bytes(static_cast<size_t>(stream.tellg()));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return bytes;
	}

	// Decodes the resource's DATA block, the way physics_import locates blocks.
	kv3::value decode_data_block(const std::vector<uint8_t>& file)
	{
		uint32_t block_offset = 0;
		uint32_t block_count = 0;
		assert(file.size() >= 16);
		std::memcpy(&block_offset, file.data() + 8, 4);
		std::memcpy(&block_count, file.data() + 12, 4);
		for (uint32_t index = 0; index < block_count; ++index)
		{
			const size_t entry = 8u + block_offset + index * 12u;
			assert(entry + 12u <= file.size());
			uint32_t relative = 0;
			uint32_t size = 0;
			std::memcpy(&relative, file.data() + entry + 4, 4);
			std::memcpy(&size, file.data() + entry + 8, 4);
			if (std::memcmp(file.data() + entry, "DATA", 4) != 0)
			{
				continue;
			}
			const size_t offset = entry + 4u + relative;
			assert(offset + size <= file.size());
			kv3::value root;
			std::string error;
			assert(kv3::decode(std::span<const uint8_t>(file.data() + offset, size), root, error));
			return root;
		}
		assert(!"DATA block not found");
		return {};
	}

	bool near(vec3 a, vec3 b)
	{
		return std::abs(a.x - b.x) < 1.0e-3f && std::abs(a.y - b.y) < 1.0e-3f && std::abs(a.z - b.z) < 1.0e-3f;
	}

	void test_lz4_blocks()
	{
		// Three literals, then a 9-byte overlapping match at distance 3, then the
		// mandatory literal-only final sequence.
		const std::vector<uint8_t> block {0x35, 'a', 'b', 'c', 0x03, 0x00, 0x10, 'd'};
		std::vector<uint8_t> output(13);
		size_t produced = 0;
		assert(kv3::lz4_decode_block(block, output, 0, output.size(), produced));
		assert(produced == 13 && std::string(output.begin(), output.end()) == "abcabcabcabcd");

		// Chained frames: a match may reach into output decoded before begin.
		const std::vector<uint8_t> chained {0x04, 0x03, 0x00, 0x10, '!'};
		std::vector<uint8_t> window {'x', 'y', 'z', 0, 0, 0, 0, 0, 0, 0, 0, 0};
		assert(kv3::lz4_decode_block(chained, window, 3, window.size(), produced));
		assert(produced == 9 && std::string(window.begin(), window.end()) == "xyzxyzxyzxy!");

		std::vector<uint8_t> small(4);
		assert(!kv3::lz4_decode_block(block, small, 0, small.size(), produced));
		const std::vector<uint8_t> zero_distance {0x14, 'a', 0x00, 0x00, 0x10, 'b'};
		assert(!kv3::lz4_decode_block(zero_distance, output, 0, output.size(), produced));
		const std::vector<uint8_t> before_start {0x14, 'a', 0x05, 0x00, 0x10, 'b'};
		assert(!kv3::lz4_decode_block(before_start, output, 0, output.size(), produced));
		const std::vector<uint8_t> truncated_literals {0x50, 'a', 'b'};
		assert(!kv3::lz4_decode_block(truncated_literals, output, 0, output.size(), produced));
		const std::vector<uint8_t> ends_after_match {0x14, 'a', 0x01, 0x00};
		assert(!kv3::lz4_decode_block(ends_after_match, output, 0, output.size(), produced));
		assert(!kv3::lz4_decode_block({}, output, 0, output.size(), produced));
	}

	void test_string_tokens()
	{
		assert(source2_string_token("") == 0);
		assert(source2_string_token("Concrete") == source2_string_token("concrete"));
		assert(source2_string_token("concrete") != source2_string_token("concret"));
		assert(surface_property_name(source2_string_token("concrete")) == "concrete");
		assert(surface_property_name(source2_string_token("metalgrate")) == "metalgrate");
		assert(surface_property_name(1u).rfind("unknown_surface_", 0) == 0);
		// Surfaces VRF 19.2 never knew stay unknown (and therefore see-through).
		assert(surface_property_name(source2_string_token("metalrailing")).rfind("unknown_surface_", 0) == 0);
	}

	void test_kv3_fixtures(const std::filesystem::path& test_executable)
	{
		const kv3::value legacy = decode_data_block(read_bytes(fixture(test_executable, "default_ents_kv3_v0.vents_c")));
		assert(legacy.type == kv3::value_type::object && legacy.members.size() == 5);
		assert(legacy.find("m_name") != nullptr && legacy.find("m_name")->text == "default_ents");
		assert(legacy.find("m_flags")->text == "ENTITY_LUMP_NONE");
		assert(legacy.find("m_entityKeyValues")->items.size() == 22);

		const kv3::value version1 = decode_data_block(read_bytes(fixture(test_executable, "default_ents_kv3_v1.vents_c")));
		assert(version1.type == kv3::value_type::object && version1.members.size() == 6);
		assert(version1.find("m_name")->text == "default_ents");

		const kv3::value zstd = decode_data_block(read_bytes(fixture(test_executable, "default_ents_kv3_v4_zstd.vents_c")));
		assert(zstd.members.size() == 4 && zstd.find("m_name")->text == "default_ents");
		const kv3::value* entities = zstd.find("m_entityKeyValues");
		assert(entities != nullptr && entities->type == kv3::value_type::array && entities->items.size() == 2064);
		const kv3::value& first = entities->items.front();
		assert(first.find("m_keyValuesData") != nullptr && first.find("m_keyValuesData")->type == kv3::value_type::blob);
		const kv3::value* data = first.find("keyValues3Data");
		assert(data != nullptr && data->find("version") != nullptr && data->find("version")->as_int64() == 1);
		const kv3::value* values = data->find("values");
		assert(values != nullptr && values->find("_dotatilegrid_fogbounds_max") != nullptr);
		assert(values->find("_dotatilegrid_fogbounds_max")->text == "8192.000000 8192.000000 0.000000");
	}

	void test_physics_fixtures(const std::filesystem::path& test_executable)
	{
		struct expectation
		{
			const char* name;
			triangle first;
		};
		const expectation cases[] {
			{"arch_apartment_ixia_01_top_cap_l_01.vmdl_c", {{-32.00023f, 0.000244f, 0.0f}, {-32.00023f, 0.000244f, 160.0f}, {0.0f, 0.000244f, 160.0f}}},
			{"unnamed_15451_kv3_v5_uncompressed.vmdl_c", {{-48.0f, 48.0f, 0.0f}, {-48.0f, 48.0f, 32.0f}, {48.0f, 48.0f, 32.0f}}},
		};
		for (const expectation& value : cases)
		{
			std::vector<triangle> triangles;
			std::vector<std::string> surfaces;
			import_report report;
			std::string error;
			assert(import_physics_resource_file(fixture(test_executable, value.name), triangles, report, error, &surfaces));
			assert(report.raw_triangles == 12 && report.accepted_triangles == 12 && report.rejected_invalid == 0);
			assert(triangles.size() == 12 && surfaces.size() == 12 && surfaces.front() == "default");
			assert(report.groups.size() == 1 && report.groups.front().name == "physics_group" && report.groups.front().accepted);
			assert(near(triangles.front().v0, value.first.v0) && near(triangles.front().v1, value.first.v1)
				   && near(triangles.front().v2, value.first.v2));
		}
	}

	void test_malformed_physics(const std::filesystem::path& test_executable)
	{
		const std::vector<uint8_t> original = read_bytes(fixture(test_executable, "arch_apartment_ixia_01_top_cap_l_01.vmdl_c"));
		std::vector<triangle> triangles;
		import_report report;
		std::string error;
		for (size_t length : {size_t {0}, size_t {15}, original.size() / 2, original.size() - 1})
		{
			const std::vector<uint8_t> truncated(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length));
			assert(!import_physics_resource(truncated, triangles, report, error) && !error.empty());
		}
		// Every single-byte corruption must either decode or fail cleanly.
		uint32_t failures = 0;
		for (size_t position = 0; position < original.size(); position += 7)
		{
			std::vector<uint8_t> corrupted = original;
			corrupted[position] ^= 0xA5u;
			failures += import_physics_resource(corrupted, triangles, report, error) ? 0u : 1u;
		}
		assert(failures != 0);
		kv3::value value;
		const std::vector<uint8_t> not_kv3 {'n', 'o', 'p', 'e'};
		assert(!kv3::decode(not_kv3, value, error));
	}

} // namespace

void run_physics_import_tests(const std::filesystem::path& test_executable)
{
	test_lz4_blocks();
	test_string_tokens();
	test_kv3_fixtures(test_executable);
	test_physics_fixtures(test_executable);
	test_malformed_physics(test_executable);
}
