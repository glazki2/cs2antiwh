// Finds the PhysAggregateData KV3 inside a compiled resource, turns spheres,
// capsules, hulls and meshes into triangles in map space, and groups them by
// collision attribute and surface property in the same order and with the same
// tessellation as ValveResourceFormat 19.2's physics GLB export.

#include "physics_import.h"

#include "kv3.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <unordered_map>
#include <utility>

namespace cs2glaz
{
	namespace
	{

		constexpr uint16_t k_resource_header_version = 12;
		constexpr uint32_t k_max_resource_blocks = 256;

		constexpr uint32_t fourcc(char a, char b, char c, char d)
		{
			return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8u
				   | static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16u | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24u;
		}

		struct resource_block
		{
			uint32_t type {};
			uint32_t offset {};
			uint32_t size {};
		};

		template<typename type>
		bool read_at(std::span<const uint8_t> data, size_t offset, type& output)
		{
			if (offset > data.size() || sizeof(type) > data.size() - offset)
			{
				return false;
			}
			std::memcpy(&output, data.data() + offset, sizeof(type));
			return true;
		}

		bool read_resource_blocks(std::span<const uint8_t> file, std::vector<resource_block>& blocks, std::string& error)
		{
			uint16_t header_version = 0;
			uint32_t block_offset = 0;
			uint32_t block_count = 0;
			if (!read_at(file, 4, header_version) || !read_at(file, 8, block_offset) || !read_at(file, 12, block_count))
			{
				error = "physics resource header is truncated";
				return false;
			}
			if (header_version != k_resource_header_version || block_count > k_max_resource_blocks)
			{
				error = "file is not a compiled Source 2 resource";
				return false;
			}
			const uint64_t table = 8ull + block_offset;
			for (uint32_t index = 0; index < block_count; ++index)
			{
				const uint64_t entry = table + static_cast<uint64_t>(index) * 12u;
				resource_block block;
				uint32_t relative = 0;
				if (entry > file.size() || !read_at(file, static_cast<size_t>(entry), block.type)
					|| !read_at(file, static_cast<size_t>(entry + 4u), relative) || !read_at(file, static_cast<size_t>(entry + 8u), block.size))
				{
					error = "physics resource block table is truncated";
					return false;
				}
				const uint64_t offset = entry + 4u + relative;
				if (block.size != 0 && (offset > file.size() || block.size > file.size() - offset))
				{
					error = "physics resource block exceeds the file";
					return false;
				}
				block.offset = static_cast<uint32_t>(offset);
				blocks.push_back(block);
			}
			return true;
		}

		bool decode_block(std::span<const uint8_t> file, const resource_block& block, kv3::value& value, std::string& error)
		{
			if (block.size == 0)
			{
				error = "physics resource block is empty";
				return false;
			}
			return kv3::decode(file.subspan(block.offset, block.size), value, error);
		}

		// Model resources point at their physics block from CTRL; physics resources
		// (and older models) carry it as PHYS or DATA.
		bool find_physics_data(std::span<const uint8_t> file, kv3::value& physics, std::string& error)
		{
			std::vector<resource_block> blocks;
			if (!read_resource_blocks(file, blocks, error))
			{
				return false;
			}
			for (const resource_block& block : blocks)
			{
				if (block.type == fourcc('N', 'T', 'R', 'O') && block.size != 0)
				{
					error = "NTRO-based physics resources are not supported";
					return false;
				}
			}
			for (const resource_block& block : blocks)
			{
				if (block.type != fourcc('C', 'T', 'R', 'L') || block.size == 0)
				{
					continue;
				}
				kv3::value control;
				if (!decode_block(file, block, control, error))
				{
					return false;
				}
				const kv3::value* embedded = control.find("embedded_physics");
				const kv3::value* index = embedded == nullptr ? nullptr : embedded->find("phys_data_block");
				if (index != nullptr && index->is_number())
				{
					const int64_t block_index = index->as_int64();
					if (block_index < 0 || static_cast<size_t>(block_index) >= blocks.size())
					{
						error = "embedded physics block index is invalid";
						return false;
					}
					return decode_block(file, blocks[static_cast<size_t>(block_index)], physics, error);
				}
				break;
			}
			for (const uint32_t type : {fourcc('P', 'H', 'Y', 'S'), fourcc('D', 'A', 'T', 'A')})
			{
				for (const resource_block& block : blocks)
				{
					if (block.type == type && block.size != 0)
					{
						if (!decode_block(file, block, physics, error))
						{
							return false;
						}
						if (physics.find("m_parts") != nullptr)
						{
							return true;
						}
					}
				}
			}
			error = "resource contains no physics data";
			return false;
		}

		struct pose
		{
			// Rows of a 3x4 affine transform: p' = (row0.p + t0, row1.p + t1, row2.p + t2).
			std::array<float, 12> m {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

			vec3 apply(vec3 p) const
			{
				return {p.x * m[0] + p.y * m[1] + p.z * m[2] + m[3], p.x * m[4] + p.y * m[5] + p.z * m[6] + m[7],
						p.x * m[8] + p.y * m[9] + p.z * m[10] + m[11]};
			}
		};

		vec3 operator+(vec3 a, vec3 b)
		{
			return {a.x + b.x, a.y + b.y, a.z + b.z};
		}

		vec3 operator-(vec3 a, vec3 b)
		{
			return {a.x - b.x, a.y - b.y, a.z - b.z};
		}

		vec3 operator*(vec3 a, float s)
		{
			return {a.x * s, a.y * s, a.z * s};
		}

		vec3 cross(vec3 a, vec3 b)
		{
			return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}

		vec3 normalize(vec3 a)
		{
			const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
			return {a.x / length, a.y / length, a.z / length};
		}

		bool read_vec3(const kv3::value* value, vec3& output)
		{
			if (value == nullptr || value->type != kv3::value_type::array || value->items.size() < 3 || !value->items[0].is_number()
				|| !value->items[1].is_number() || !value->items[2].is_number())
			{
				return false;
			}
			output = {static_cast<float>(value->items[0].as_double()), static_cast<float>(value->items[1].as_double()),
					  static_cast<float>(value->items[2].as_double())};
			return true;
		}

		bool read_float(const kv3::value* value, float& output)
		{
			if (value == nullptr || !value->is_number())
			{
				return false;
			}
			output = static_cast<float>(value->as_double());
			return true;
		}

		int64_t read_integer(const kv3::value& object, std::string_view key)
		{
			const kv3::value* value = object.find(key);
			return value != nullptr && value->is_number() ? value->as_int64() : 0;
		}

		bool read_pose(const kv3::value& value, pose& output)
		{
			if (value.type != kv3::value_type::array || value.items.size() < 12)
			{
				return false;
			}
			for (size_t index = 0; index < 12; ++index)
			{
				if (!value.items[index].is_number())
				{
					return false;
				}
				output.m[index] = static_cast<float>(value.items[index].as_double());
			}
			return true;
		}

		template<typename element>
		bool blob_elements(const kv3::value& value, std::vector<element>& output)
		{
			if (value.type != kv3::value_type::blob || value.blob.size() % sizeof(element) != 0)
			{
				return false;
			}
			output.resize(value.blob.size() / sizeof(element));
			if (!output.empty())
			{
				std::memcpy(output.data(), value.blob.data(), value.blob.size());
			}
			return true;
		}

		// Hull and mesh vertices: an array of vectors on old assets, otherwise a
		// float3 blob (m_VertexPositions once hulls gained explicit vertex indices).
		bool read_vertices(const kv3::value& shape, std::vector<vec3>& output)
		{
			const kv3::value* vertices = shape.find("m_Vertices");
			if (vertices != nullptr && vertices->type != kv3::value_type::blob)
			{
				if (vertices->type != kv3::value_type::array)
				{
					return false;
				}
				output.resize(vertices->items.size());
				for (size_t index = 0; index < output.size(); ++index)
				{
					if (!read_vec3(&vertices->items[index], output[index]))
					{
						return false;
					}
				}
				return true;
			}
			const kv3::value* positions = shape.find("m_VertexPositions") != nullptr ? shape.find("m_VertexPositions") : vertices;
			static_assert(sizeof(vec3) == 12);
			return positions != nullptr && blob_elements(*positions, output);
		}

		struct half_edge
		{
			uint8_t next;
			uint8_t twin;
			uint8_t origin;
			uint8_t face;
		};

		bool read_hull_edges(const kv3::value& hull, std::vector<half_edge>& output)
		{
			const kv3::value* edges = hull.find("m_Edges");
			if (edges == nullptr)
			{
				return false;
			}
			if (edges->type == kv3::value_type::blob)
			{
				return blob_elements(*edges, output);
			}
			if (edges->type != kv3::value_type::array)
			{
				return false;
			}
			output.resize(edges->items.size());
			for (size_t index = 0; index < output.size(); ++index)
			{
				const kv3::value& edge = edges->items[index];
				output[index] = {static_cast<uint8_t>(read_integer(edge, "m_nNext")), static_cast<uint8_t>(read_integer(edge, "m_nTwin")),
								 static_cast<uint8_t>(read_integer(edge, "m_nOrigin")), static_cast<uint8_t>(read_integer(edge, "m_nFace"))};
			}
			return true;
		}

		bool read_hull_faces(const kv3::value& hull, std::vector<uint8_t>& output)
		{
			const kv3::value* faces = hull.find("m_Faces");
			if (faces == nullptr)
			{
				return false;
			}
			if (faces->type == kv3::value_type::blob)
			{
				output = faces->blob;
				return true;
			}
			if (faces->type != kv3::value_type::array)
			{
				return false;
			}
			output.resize(faces->items.size());
			for (size_t index = 0; index < output.size(); ++index)
			{
				output[index] = static_cast<uint8_t>(read_integer(faces->items[index], "m_nEdge"));
			}
			return true;
		}

		struct mesh_triangle
		{
			int32_t a;
			int32_t b;
			int32_t c;
		};

		bool read_mesh_triangles(const kv3::value& mesh, std::vector<mesh_triangle>& output)
		{
			const kv3::value* triangles = mesh.find("m_Triangles");
			if (triangles == nullptr)
			{
				return false;
			}
			if (triangles->type == kv3::value_type::blob)
			{
				return blob_elements(*triangles, output);
			}
			if (triangles->type != kv3::value_type::array)
			{
				return false;
			}
			output.resize(triangles->items.size());
			for (size_t index = 0; index < output.size(); ++index)
			{
				const kv3::value* indices = triangles->items[index].find("m_nIndex");
				if (indices == nullptr || indices->type != kv3::value_type::array || indices->items.size() != 3)
				{
					return false;
				}
				output[index] = {static_cast<int32_t>(indices->items[0].as_int64()), static_cast<int32_t>(indices->items[1].as_int64()),
								 static_cast<int32_t>(indices->items[2].as_int64())};
			}
			return true;
		}

		void add_triangle(std::vector<triangle>& output, vec3 a, vec3 b, vec3 c)
		{
			output.push_back({a, b, c});
		}

		// ValveResourceFormat's procedural sphere: 16x16 latitude/longitude quads
		// with Y as the pole axis (its pole rows are degenerate and rejected later).
		void add_sphere(std::vector<triangle>& output, vec3 center, float radius)
		{
			constexpr int k_latitude = 16;
			constexpr int k_longitude = 16;
			std::array<vec3, (k_latitude + 1) * (k_longitude + 1)> vertices {};
			for (int latitude = 0; latitude <= k_latitude; ++latitude)
			{
				const float theta = static_cast<float>(latitude) * std::numbers::pi_v<float> / static_cast<float>(k_latitude);
				const float sin_theta = std::sin(theta);
				const float cos_theta = std::cos(theta);
				for (int longitude = 0; longitude <= k_longitude; ++longitude)
				{
					const float phi = static_cast<float>(longitude) * 2.0f * std::numbers::pi_v<float> / static_cast<float>(k_longitude);
					const vec3 normal {std::cos(phi) * sin_theta, cos_theta, std::sin(phi) * sin_theta};
					vertices[static_cast<size_t>(latitude * (k_longitude + 1) + longitude)] = center + normal * radius;
				}
			}
			for (int latitude = 0; latitude < k_latitude; ++latitude)
			{
				for (int longitude = 0; longitude < k_longitude; ++longitude)
				{
					const size_t first = static_cast<size_t>(latitude * (k_longitude + 1) + longitude);
					const size_t second = first + k_longitude + 1;
					add_triangle(output, vertices[first], vertices[second], vertices[first + 1]);
					add_triangle(output, vertices[second], vertices[second + 1], vertices[first + 1]);
				}
			}
		}

		// ValveResourceFormat's procedural capsule: an 8-ring, 16-segment cylinder
		// plus a full sphere at each end.
		void add_capsule(std::vector<triangle>& output, vec3 start, vec3 end, float radius)
		{
			constexpr int k_segments = 16;
			constexpr int k_rings = 8;
			const vec3 axis = end - start;
			const vec3 direction = normalize(axis);
			const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
			const vec3 center = (start + end) * 0.5f;
			vec3 right;
			vec3 up;
			if (std::abs(direction.y) < 0.9f)
			{
				right = normalize(cross(direction, {0.0f, 1.0f, 0.0f}));
			}
			else
			{
				right = normalize(cross(direction, {1.0f, 0.0f, 0.0f}));
			}
			up = cross(right, direction);
			std::array<vec3, (k_rings + 1) * (k_segments + 1)> vertices {};
			for (int ring = 0; ring <= k_rings; ++ring)
			{
				const float t = static_cast<float>(ring) / static_cast<float>(k_rings);
				const vec3 position = center + direction * ((t - 0.5f) * length);
				for (int segment = 0; segment <= k_segments; ++segment)
				{
					const float angle = static_cast<float>(segment) * 2.0f * std::numbers::pi_v<float> / static_cast<float>(k_segments);
					const vec3 normal = normalize(right * (std::cos(angle) * radius) + up * (std::sin(angle) * radius));
					vertices[static_cast<size_t>(ring * (k_segments + 1) + segment)] = position + normal * radius;
				}
			}
			for (int ring = 0; ring < k_rings; ++ring)
			{
				for (int segment = 0; segment < k_segments; ++segment)
				{
					const size_t current = static_cast<size_t>(ring * (k_segments + 1) + segment);
					const size_t next = current + k_segments + 1;
					add_triangle(output, vertices[current], vertices[next], vertices[current + 1]);
					add_triangle(output, vertices[next], vertices[next + 1], vertices[current + 1]);
				}
			}
			add_sphere(output, start, radius);
			add_sphere(output, end, radius);
		}

		bool add_hull(std::vector<triangle>& output, const kv3::value& hull, const pose& transform, std::string& error)
		{
			std::vector<vec3> positions;
			std::vector<half_edge> edges;
			std::vector<uint8_t> faces;
			if (!read_vertices(hull, positions) || !read_hull_edges(hull, edges) || !read_hull_faces(hull, faces))
			{
				error = "physics hull is malformed";
				return false;
			}
			for (vec3& position : positions)
			{
				position = transform.apply(position);
			}
			for (const uint8_t start : faces)
			{
				if (start >= edges.size())
				{
					error = "physics hull face references a missing edge";
					return false;
				}
				// Fan from the face's first vertex, walking the edge loop; the step
				// limit stops a corrupt loop that never returns to its start.
				size_t edge = edges[start].next;
				for (size_t steps = 0; edge != start; ++steps)
				{
					if (edge >= edges.size() || steps > edges.size())
					{
						error = "physics hull edge loop is malformed";
						return false;
					}
					const size_t next = edges[edge].next;
					if (next >= edges.size())
					{
						error = "physics hull edge loop is malformed";
						return false;
					}
					if (next == start)
					{
						break;
					}
					const size_t a = edges[start].origin;
					const size_t b = edges[edge].origin;
					const size_t c = edges[next].origin;
					if (a >= positions.size() || b >= positions.size() || c >= positions.size())
					{
						error = "physics hull references a missing vertex";
						return false;
					}
					add_triangle(output, positions[a], positions[b], positions[c]);
					edge = next;
				}
			}
			return true;
		}

		bool add_mesh(std::vector<triangle>& output, const kv3::value& mesh, const pose& transform, std::string& error)
		{
			std::vector<vec3> positions;
			std::vector<mesh_triangle> indices;
			if (!read_vertices(mesh, positions) || !read_mesh_triangles(mesh, indices))
			{
				error = "physics mesh is malformed";
				return false;
			}
			for (vec3& position : positions)
			{
				position = transform.apply(position);
			}
			output.reserve(output.size() + indices.size());
			for (const mesh_triangle& value : indices)
			{
				if (value.a < 0 || value.b < 0 || value.c < 0 || static_cast<size_t>(value.a) >= positions.size()
					|| static_cast<size_t>(value.b) >= positions.size() || static_cast<size_t>(value.c) >= positions.size())
				{
					error = "physics mesh references a missing vertex";
					return false;
				}
				add_triangle(output, positions[static_cast<size_t>(value.a)], positions[static_cast<size_t>(value.b)],
							 positions[static_cast<size_t>(value.c)]);
			}
			return true;
		}

		enum class shape_kind : uint8_t
		{
			sphere,
			capsule,
			hull,
			mesh
		};

		struct shape_reference
		{
			const kv3::value* shape {};
			size_t part {};
			shape_kind kind {};
		};

		bool finite(vec3 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool valid_triangle(const triangle& value)
		{
			if (!finite(value.v0) || !finite(value.v1) || !finite(value.v2))
			{
				return false;
			}
			const vec3 edge_a = value.v1 - value.v0;
			const vec3 edge_b = value.v2 - value.v0;
			const vec3 normal = cross(edge_a, edge_b);
			return normal.x * normal.x + normal.y * normal.y + normal.z * normal.z > 1.0e-12f;
		}

		std::vector<std::string> string_array(const kv3::value* value)
		{
			std::vector<std::string> output;
			if (value != nullptr && value->type == kv3::value_type::array)
			{
				for (const kv3::value& item : value->items)
				{
					if (item.type == kv3::value_type::string)
					{
						output.push_back(item.text);
					}
				}
			}
			return output;
		}

		std::string group_name(const kv3::value& attributes, const std::vector<std::string>& tags, const std::string& surface)
		{
			const kv3::value* group = attributes.find("m_CollisionGroupString");
			std::string name = "physics_group";
			const auto lower = [](std::string text)
			{
				for (char& character : text)
				{
					character = static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
				}
				return text;
			};
			if (group != nullptr && group->type == kv3::value_type::string && lower(group->text) != "default")
			{
				name = "physics_" + group->text;
			}
			if (!tags.empty())
			{
				name = "physics";
				for (const std::string& tag : tags)
				{
					name += "_" + tag;
				}
			}
			if (!surface.empty() && lower(surface) != "default")
			{
				name += "_" + surface;
			}
			return name;
		}

		const std::unordered_map<uint32_t, const char*>& known_strings()
		{
			static const std::unordered_map<uint32_t, const char*> table = []
			{
				static constexpr const char* k_names[] = {
#include "surface_names.inc"
				};
				std::unordered_map<uint32_t, const char*> result;
				result.reserve(std::size(k_names));
				for (const char* name : k_names)
				{
					result.emplace(source2_string_token(name), name);
				}
				return result;
			}();
			return table;
		}

	} // namespace

	uint32_t source2_string_token(std::string_view text)
	{
		constexpr uint32_t k_seed = 0x31415926u;
		constexpr uint32_t k_m = 0x5bd1e995u;
		constexpr int k_r = 24;
		if (text.empty())
		{
			return 0;
		}
		const auto byte_at = [&](size_t index)
		{
			const char character = text[index];
			return static_cast<uint32_t>(static_cast<uint8_t>(character >= 'A' && character <= 'Z' ? character | 0x20 : character));
		};
		uint32_t hash = k_seed ^ static_cast<uint32_t>(text.size());
		size_t index = 0;
		size_t length = text.size();
		while (length >= 4)
		{
			uint32_t k = byte_at(index) | byte_at(index + 1) << 8u | byte_at(index + 2) << 16u | byte_at(index + 3) << 24u;
			k *= k_m;
			k ^= k >> k_r;
			k *= k_m;
			hash *= k_m;
			hash ^= k;
			index += 4;
			length -= 4;
		}
		switch (length)
		{
			case 3:
				hash ^= byte_at(index + 2) << 16u;
				[[fallthrough]];
			case 2:
				hash ^= byte_at(index + 1) << 8u;
				[[fallthrough]];
			case 1:
				hash ^= byte_at(index);
				hash *= k_m;
				break;
			default:
				break;
		}
		hash ^= hash >> 13u;
		hash *= k_m;
		hash ^= hash >> 15u;
		return hash;
	}

	std::string surface_property_name(uint32_t token)
	{
		const auto& table = known_strings();
		const auto found = table.find(token);
		return found != table.end() ? std::string(found->second) : "unknown_surface_" + std::to_string(token);
	}

	bool import_physics_resource(std::span<const uint8_t> resource, std::vector<triangle>& triangles, import_report& report, std::string& error,
								 std::vector<std::string>* triangle_surfaces)
	{
		triangles.clear();
		report = {};
		if (triangle_surfaces != nullptr)
		{
			triangle_surfaces->clear();
		}
		kv3::value physics;
		if (!find_physics_data(resource, physics, error))
		{
			return false;
		}
		const kv3::value* parts = physics.find("m_parts");
		const kv3::value* attributes = physics.find("m_collisionAttributes");
		const kv3::value* hashes = physics.find("m_surfacePropertyHashes");
		if (parts == nullptr || parts->type != kv3::value_type::array || attributes == nullptr || attributes->type != kv3::value_type::array
			|| hashes == nullptr || hashes->type != kv3::value_type::array)
		{
			error = "physics data lacks parts, collision attributes or surface properties";
			return false;
		}
		std::vector<pose> poses;
		if (const kv3::value* bind_pose = physics.find("m_bindPose"); bind_pose != nullptr && bind_pose->type == kv3::value_type::array)
		{
			poses.resize(bind_pose->items.size());
			for (size_t index = 0; index < poses.size(); ++index)
			{
				if (!read_pose(bind_pose->items[index], poses[index]))
				{
					error = "physics bind pose is malformed";
					return false;
				}
			}
		}
		if (!poses.empty() && poses.size() < parts->items.size())
		{
			error = "physics bind pose does not cover every part";
			return false;
		}
		std::vector<std::string> surfaces;
		for (const kv3::value& hash : hashes->items)
		{
			surfaces.push_back(surface_property_name(static_cast<uint32_t>(hash.as_int64())));
		}

		// Bucket shapes by (collision attribute, surface property) while keeping the
		// exporter's order: parts, then spheres, capsules, hulls and meshes.
		const size_t attribute_count = attributes->items.size();
		const size_t surface_count = surfaces.size();
		std::vector<std::vector<shape_reference>> buckets(attribute_count * surface_count);
		constexpr std::array<std::pair<const char*, shape_kind>, 4> k_shape_lists {{{"m_spheres", shape_kind::sphere},
																				   {"m_capsules", shape_kind::capsule},
																				   {"m_hulls", shape_kind::hull},
																				   {"m_meshes", shape_kind::mesh}}};
		constexpr std::array<const char*, 4> k_shape_members {"m_Sphere", "m_Capsule", "m_Hull", "m_Mesh"};
		for (size_t part_index = 0; part_index < parts->items.size(); ++part_index)
		{
			const kv3::value* shape = parts->items[part_index].find("m_rnShape");
			if (shape == nullptr)
			{
				error = "physics part lacks a shape";
				return false;
			}
			for (const auto& [list_name, kind] : k_shape_lists)
			{
				const kv3::value* list = shape->find(list_name);
				if (list == nullptr || list->type != kv3::value_type::array)
				{
					continue;
				}
				for (const kv3::value& descriptor : list->items)
				{
					const int64_t attribute = read_integer(descriptor, "m_nCollisionAttributeIndex");
					const int64_t surface = read_integer(descriptor, "m_nSurfacePropertyIndex");
					const kv3::value* body = descriptor.find(k_shape_members[static_cast<size_t>(kind)]);
					if (body == nullptr)
					{
						error = "physics shape descriptor lacks its shape";
						return false;
					}
					if (attribute < 0 || surface < 0 || static_cast<uint64_t>(attribute) >= attribute_count
						|| static_cast<uint64_t>(surface) >= surface_count)
					{
						continue;
					}
					buckets[static_cast<size_t>(attribute) * surface_count + static_cast<size_t>(surface)].push_back({body, part_index, kind});
				}
			}
		}

		std::vector<triangle> group;
		for (size_t attribute = 0; attribute < attribute_count; ++attribute)
		{
			const kv3::value& attribute_value = attributes->items[attribute];
			std::vector<std::string> tags = string_array(attribute_value.find("m_InteractAsStrings"));
			if (attribute_value.find("m_InteractAsStrings") == nullptr)
			{
				tags = string_array(attribute_value.find("m_PhysicsTagStrings"));
			}
			for (size_t surface = 0; surface < surface_count; ++surface)
			{
				const std::vector<shape_reference>& shapes = buckets[attribute * surface_count + surface];
				if (shapes.empty())
				{
					continue;
				}
				group.clear();
				for (const shape_reference& reference : shapes)
				{
					const pose transform = poses.empty() ? pose {} : poses[reference.part];
					const kv3::value& body = *reference.shape;
					switch (reference.kind)
					{
						case shape_kind::sphere:
						{
							vec3 center;
							float radius = 0.0f;
							if (!read_vec3(body.find("m_vCenter"), center) || !read_float(body.find("m_flRadius"), radius))
							{
								error = "physics sphere is malformed";
								return false;
							}
							add_sphere(group, transform.apply(center), radius);
							break;
						}
						case shape_kind::capsule:
						{
							const kv3::value* centers = body.find("m_vCenter");
							vec3 start;
							vec3 end;
							float radius = 0.0f;
							if (centers == nullptr || centers->type != kv3::value_type::array || centers->items.size() < 2
								|| !read_vec3(&centers->items[0], start) || !read_vec3(&centers->items[1], end)
								|| !read_float(body.find("m_flRadius"), radius))
							{
								error = "physics capsule is malformed";
								return false;
							}
							add_capsule(group, transform.apply(start), transform.apply(end), radius);
							break;
						}
						case shape_kind::hull:
							if (!add_hull(group, body, transform, error))
							{
								return false;
							}
							break;
						case shape_kind::mesh:
							if (!add_mesh(group, body, transform, error))
							{
								return false;
							}
							break;
					}
				}
				physics_group_report group_report;
				group_report.surface_property = surfaces[surface];
				group_report.tags = tags;
				group_report.name = group_name(attribute_value, tags, surfaces[surface]);
				group_report.accepted = physics_group_accepted(tags, surfaces[surface]);
				group_report.triangles = group.size();
				report.raw_triangles += group.size();
				if (group_report.accepted)
				{
					report.accepted_triangles += group.size();
					for (const triangle& value : group)
					{
						if (!valid_triangle(value))
						{
							++report.rejected_invalid;
							continue;
						}
						triangles.push_back(value);
						if (triangle_surfaces != nullptr)
						{
							triangle_surfaces->push_back(surfaces[surface]);
						}
					}
				}
				report.groups.push_back(std::move(group_report));
			}
		}
		if (triangles.empty())
		{
			error = "physics data contains no accepted triangles";
			return false;
		}
		return true;
	}

	bool import_physics_resource_file(const std::filesystem::path& path, std::vector<triangle>& triangles, import_report& report,
									  std::string& error, std::vector<std::string>* triangle_surfaces)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream || stream.tellg() < 0)
		{
			error = "could not read physics resource";
			return false;
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(stream.tellg()));
		stream.seekg(0);
		if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		{
			error = "could not read physics resource";
			return false;
		}
		return import_physics_resource(bytes, triangles, report, error, triangle_surfaces);
	}

} // namespace cs2glaz
