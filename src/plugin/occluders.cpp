#include "plugin.h"

// Doors and box-shaped props as live occluders. The baked map holds only the
// static world, so a closed door or a crate that is an entity used to be
// transparent and anyone behind it was sent. Candidates are found by walking
// the entity list once a second; every snapshot then copies each candidate's
// current collision box, rotation and solidity. Anything unreadable, not solid
// or of an unexpected shape is left out, which keeps players visible.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace cs2glaz
{
	namespace
	{

		CConVar<bool> cs2glaz_dynamic_occluders("cs2glaz_dynamic_occluders", FCVAR_NONE,
												"Treat doors and box-shaped props (crates, boxes, containers) as sight blockers (resets on restart)",
												true);

		constexpr auto k_occluder_scan_interval = std::chrono::seconds(1);
		constexpr uint32_t k_max_occluder_candidates = 1024;
		constexpr size_t k_max_occluder_class_cache = 32768;
		constexpr uint8_t k_solid_bsp = 1;
		constexpr uint8_t k_solid_bbox = 2;
		constexpr uint8_t k_solid_obb = 3;
		constexpr uint8_t k_solid_vphysics = 6;
		constexpr uint8_t k_solid_flag_not_solid = 0x04;
		constexpr uint8_t k_solid_flag_trigger = 0x08;

		template<typename type>
		type& field(void* object, uint32_t offset)
		{
			return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset);
		}

		bool starts_with(const char* text, std::string_view prefix)
		{
			return text != nullptr && std::strncmp(text, prefix.data(), prefix.size()) == 0;
		}

		bool contains_any(std::string_view text, std::initializer_list<std::string_view> words)
		{
			return std::any_of(words.begin(), words.end(), [&](std::string_view word) { return text.find(word) != std::string_view::npos; });
		}

		// Model paths of things that are see-through, have gaps, or are not box
		// shaped (vehicles, carts); such a model never becomes an occluder, even as
		// a door.
		bool see_through_model(std::string_view model)
		{
			return contains_any(model, {"glass", "window", "fence", "gate", "grate", "chain", "bars", "mesh", "net", "wire", "vent", "basket", "shelf",
										"rack", "cage", "frame", "plant", "tree", "bush", "cloth", "tarp", "curtain", "sign", "lamp", "pole", "rope",
										"truck", "vehicle", "forklift", "cart", "trolley"});
		}

		bool box_model(std::string_view model)
		{
			return contains_any(model, {"crate", "box", "container", "dumpster"});
		}

		bool door_class(const char* name)
		{
			return starts_with(name, "prop_door") || starts_with(name, "func_door");
		}

		bool prop_class(const char* name)
		{
			return name != nullptr
				   && (std::strcmp(name, "prop_dynamic") == 0 || std::strcmp(name, "prop_dynamic_override") == 0 || std::strcmp(name, "prop_physics") == 0
					   || std::strcmp(name, "prop_physics_override") == 0 || std::strcmp(name, "prop_physics_multiplayer") == 0);
		}

	} // namespace

	std::string plugin::entity_model_name(CEntityInstance* entity) const
	{
		if (entity == nullptr || !compatibility_.dynamic_occluders_available())
		{
			return {};
		}
		void* body = field<void*>(entity, compatibility_.fields().body_component);
		void* node = body == nullptr ? nullptr : field<void*>(body, compatibility_.fields().scene_node);
		if (node == nullptr)
		{
			return {};
		}
		// A brush entity's scene node may not be a skeleton instance, so the model
		// state is read through guarded reads only.
		const char* pointer = nullptr;
		const auto* symbol = reinterpret_cast<const std::byte*>(node) + compatibility_.fields().model_state + compatibility_.fields().model_name;
		if (!runtime_compatibility::safe_read(symbol, &pointer, sizeof(pointer)) || pointer == nullptr)
		{
			return {};
		}
		char text[128] {};
		if (!runtime_compatibility::safe_read(pointer, text, sizeof(text) - 1))
		{
			for (size_t index = 0; index + 1 < sizeof(text); ++index)
			{
				if (!runtime_compatibility::safe_read(pointer + index, &text[index], 1) || text[index] == '\0')
				{
					text[index] = '\0';
					break;
				}
			}
		}
		text[sizeof(text) - 1] = '\0';
		std::string model(text);
		for (char& character : model)
		{
			if (static_cast<unsigned char>(character) < 0x20 || static_cast<unsigned char>(character) > 0x7e)
			{
				return {};
			}
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		return model;
	}

	void plugin::scan_occluder_candidates(CGameEntitySystem* system)
	{
		occluder_candidates_.clear();
		// Model names are read once per entity handle; a handle is never reused
		// with the same serial, and the cache restarts with each map.
		if (occluder_class_cache_map_ != map_ || occluder_class_cache_.size() > k_max_occluder_class_cache)
		{
			occluder_class_cache_.clear();
			occluder_class_cache_map_ = map_;
		}
		// Doors and props are networked, so their index is below MAX_EDICTS; the
		// index lookup goes through the identity chunks limited mode verified.
		for (int index = k_max_players + 1; index < MAX_EDICTS && occluder_candidates_.size() < k_max_occluder_candidates; ++index)
		{
			CEntityInstance* entity = system->GetEntityInstance(CEntityIndex(index));
			CEntityIdentity* identity = entity == nullptr ? nullptr : entity->m_pEntity;
			const char* name = identity == nullptr ? nullptr : identity->m_designerName.String();
			if (name == nullptr)
			{
				continue;
			}
			const bool door = door_class(name);
			if (!door && !prop_class(name))
			{
				continue;
			}
			const uint32_t key = static_cast<uint32_t>(identity->m_EHandle.ToInt());
			auto cached = occluder_class_cache_.find(key);
			if (cached == occluder_class_cache_.end())
			{
				const std::string model = entity_model_name(entity);
				const bool used = !see_through_model(model) && (door || box_model(model));
				cached = occluder_class_cache_.emplace(key, used).first;
			}
			if (cached->second)
			{
				occluder_candidates_.push_back({identity->m_EHandle, door ? occluder_kind::door : occluder_kind::prop});
			}
		}
	}

	bool plugin::read_occluder(CEntityInstance* entity, occluder_kind kind, visibility_occluder& output) const
	{
		const schema_offsets& fields = compatibility_.fields();
		void* collision = field<void*>(entity, fields.collision);
		void* body = field<void*>(entity, fields.body_component);
		void* node = body == nullptr ? nullptr : field<void*>(body, fields.scene_node);
		if (collision == nullptr || node == nullptr)
		{
			return false;
		}
		const uint8_t solid_type = field<uint8_t>(collision, fields.solid_type);
		const uint8_t solid_flags = field<uint8_t>(collision, fields.solid_flags);
		if ((solid_flags & (k_solid_flag_not_solid | k_solid_flag_trigger)) != 0
			|| (solid_type != k_solid_bsp && solid_type != k_solid_bbox && solid_type != k_solid_obb && solid_type != k_solid_vphysics))
		{
			return false;
		}
		const Vector& origin = field<Vector>(node, fields.abs_origin);
		const qangle rotation = solid_type == k_solid_bbox ? qangle {} : field<qangle>(node, fields.abs_rotation);
		const Vector& mins = field<Vector>(collision, fields.mins);
		const Vector& maxs = field<Vector>(collision, fields.maxs);
		return make_occluder({origin.x, origin.y, origin.z}, {rotation.x, rotation.y, rotation.z}, {mins.x, mins.y, mins.z}, {maxs.x, maxs.y, maxs.z},
							 kind, output);
	}

	void plugin::capture_occluders(CGameEntitySystem* system, visibility_snapshot& value, std::chrono::steady_clock::time_point now)
	{
		if (!cs2glaz_dynamic_occluders.Get() || !compatibility_.dynamic_occluders_available())
		{
			occluder_candidates_.clear();
			occluders_active_ = 0;
			return;
		}
		if (now >= occluder_scan_next_)
		{
			occluder_scan_next_ = now + k_occluder_scan_interval;
			scan_occluder_candidates(system);
		}
		value.occluders.reserve(std::min<size_t>(occluder_candidates_.size(), k_max_dynamic_occluders));
		for (const occluder_candidate& candidate : occluder_candidates_)
		{
			if (value.occluders.size() >= k_max_dynamic_occluders)
			{
				break;
			}
			CEntityInstance* entity = candidate.handle.IsValid() ? system->GetEntityInstance(candidate.handle) : nullptr;
			visibility_occluder occluder;
			if (entity != nullptr && read_occluder(entity, candidate.kind, occluder))
			{
				value.occluders.push_back(occluder);
			}
		}
		occluders_active_ = static_cast<uint32_t>(value.occluders.size());
	}

	void plugin::print_props(float radius)
	{
		CGameEntitySystem* system = entity_system();
		const std::shared_ptr<const visibility_result> result = worker_.result();
		if (system == nullptr || engine_ == nullptr || !result)
		{
			META_CONPRINTF("[CS2GLAZ] props: no visibility result yet\n");
			return;
		}
		META_CONPRINTF("[CS2GLAZ] props: dynamic occluders %s, %zu candidates, %u in the last snapshot\n",
					   !compatibility_.dynamic_occluders_available() ? "unavailable (schema)"
					   : cs2glaz_dynamic_occluders.Get()			 ? "on"
																	 : "off",
					   occluder_candidates_.size(), occluders_active_);
		const schema_offsets& fields = compatibility_.fields();
		for (uint32_t me = 0; me < k_max_players; ++me)
		{
			const player_state& player = result->players[me];
			if (!player.valid || engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(me))) == nullptr)
			{
				continue;
			}
			META_CONPRINTF("[CS2GLAZ] props within %.0f units of %s:\n", radius, slot_name(system, me).c_str());
			uint32_t printed = 0;
			for (int index = k_max_players + 1; index < MAX_EDICTS && printed < 40; ++index)
			{
				CEntityInstance* entity = system->GetEntityInstance(CEntityIndex(index));
				CEntityIdentity* identity = entity == nullptr ? nullptr : entity->m_pEntity;
				const char* name = identity == nullptr ? nullptr : identity->m_designerName.String();
				if (name == nullptr || starts_with(name, "player") || starts_with(name, "cs_player") || starts_with(name, "weapon_"))
				{
					continue;
				}
				void* collision = field<void*>(entity, fields.collision);
				void* body = field<void*>(entity, fields.body_component);
				void* node = body == nullptr ? nullptr : field<void*>(body, fields.scene_node);
				if (collision == nullptr || node == nullptr)
				{
					continue;
				}
				const Vector& origin = field<Vector>(node, fields.abs_origin);
				const float dx = origin.x - player.origin.x;
				const float dy = origin.y - player.origin.y;
				const float dz = origin.z - player.origin.z;
				if (dx * dx + dy * dy + dz * dz > radius * radius)
				{
					continue;
				}
				const Vector& mins = field<Vector>(collision, fields.mins);
				const Vector& maxs = field<Vector>(collision, fields.maxs);
				const uint8_t solid_type = compatibility_.dynamic_occluders_available() ? field<uint8_t>(collision, fields.solid_type) : 0;
				const uint8_t solid_flags = compatibility_.dynamic_occluders_available() ? field<uint8_t>(collision, fields.solid_flags) : 0;
				const CEntityHandle handle = identity->m_EHandle;
				const bool used = std::any_of(occluder_candidates_.begin(), occluder_candidates_.end(),
											  [&](const occluder_candidate& candidate) { return candidate.handle == handle; });
				META_CONPRINTF("[CS2GLAZ]   %s %s solid=%u flags=0x%02x size=%.0fx%.0fx%.0f at %.0f units%s\n", name, entity_model_name(entity).c_str(),
							   solid_type, solid_flags, maxs.x - mins.x, maxs.y - mins.y, maxs.z - mins.z, std::sqrt(dx * dx + dy * dy + dz * dz),
							   used ? " [occluder]" : "");
				++printed;
			}
		}
	}

	CON_COMMAND_F(cs2glaz_props, "List solid entities near each living human and whether they block sight: cs2glaz_props [radius, default 400]",
				  FCVAR_NONE)
	{
		float radius = 400.0f;
		if (args.ArgC() > 1)
		{
			radius = std::clamp(static_cast<float>(std::atof(args.Arg(1))), 16.0f, 4096.0f);
		}
		g_plugin.print_props(radius);
	}

} // namespace cs2glaz
