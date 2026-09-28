#pragma once

// Proves from a vtable's RTTI, before any virtual call, that `base` is a base
// subobject located `base - owner` bytes into a live object whose class name
// contains `class_name`. Every memory access goes through `read`, which returns
// false instead of faulting, so an unexpected layout only yields false.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cs2glaz
{

	template<typename read_function>
	bool rtti_names_class_at_offset(const void* owner, const void* base, std::string_view class_name, read_function read)
	{
		const auto offset = static_cast<std::ptrdiff_t>(static_cast<const char*>(base) - static_cast<const char*>(owner));
		const void* const* vtable = nullptr;
		if (offset <= 0 || !read(base, &vtable, sizeof(vtable)) || vtable == nullptr)
		{
			return false;
		}
		const char* name = nullptr;
#if defined(_MSC_VER)
		// MSVC x64: vtable[-1] is the RTTI Complete Object Locator; its type
		// descriptor holds the decorated class name after two pointers.
		struct complete_object_locator
		{
			uint32_t signature;
			uint32_t offset;
			uint32_t constructor_offset;
			int32_t type_descriptor;
			int32_t class_descriptor;
			int32_t self;
		};
		const void* locator_address = nullptr;
		complete_object_locator locator {};
		if (!read(vtable - 1, &locator_address, sizeof(locator_address)) || !read(locator_address, &locator, sizeof(locator))
			|| locator.signature != 1 || static_cast<std::ptrdiff_t>(locator.offset) != offset)
		{
			return false;
		}
		const char* image = static_cast<const char*>(locator_address) - locator.self;
		name = image + locator.type_descriptor + 2 * sizeof(void*);
#else
		// Itanium: vtable[-2] is the offset back to the full object and vtable[-1]
		// the std::type_info, whose second word is the mangled class name.
		std::ptrdiff_t offset_to_top = 0;
		const void* type_info = nullptr;
		if (!read(vtable - 2, &offset_to_top, sizeof(offset_to_top)) || offset_to_top != -offset || !read(vtable - 1, &type_info, sizeof(type_info))
			|| type_info == nullptr || !read(static_cast<const char*>(type_info) + sizeof(void*), &name, sizeof(name)) || name == nullptr)
		{
			return false;
		}
#endif
		char text[192] {};
		for (size_t index = 0; index + 1 < sizeof(text); ++index)
		{
			if (!read(name + index, &text[index], 1))
			{
				return false;
			}
			if (text[index] == '\0')
			{
				break;
			}
		}
		return std::string_view(text).find(class_name) != std::string_view::npos;
	}

} // namespace cs2glaz
