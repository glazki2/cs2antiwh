#pragma once

// Tiny verified CheckTransmit helpers: parse unsigned gamedata values, read
// CS2's private full-update flag, and pair primary-list removals with the
// second list according to the selected mode. They never discover private list addresses.

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace cs2glaz
{

	struct checktransmit_private_offsets
	{
		uint32_t full_update_offset {};
	};

	inline std::string_view trim_ascii(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
		{
			text.remove_prefix(1);
		}
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
		{
			text.remove_suffix(1);
		}
		return text;
	}

	inline bool parse_gamedata_uint32(std::string_view text, uint32_t& value)
	{
		text = trim_ascii(text);
		const auto [end, conversion_error] = std::from_chars(text.data(), text.data() + text.size(), value);
		return !text.empty() && conversion_error == std::errc {} && end == text.data() + text.size();
	}

	inline bool valid_gamedata_offset(uint32_t offset, uint32_t alignment, uint32_t max_offset)
	{
		return offset != 0 && offset <= max_offset && offset % alignment == 0;
	}

	inline bool read_checktransmit_full_update(const void* info, uint32_t offset)
	{
		bool value {};
		if (info != nullptr)
		{
			std::memcpy(&value, static_cast<const char*>(info) + offset, sizeof(value));
		}
		return value;
	}

	template<typename mask_type>
	inline bool withhold_transmit_bit(mask_type* primary, mask_type* dont_transmit, int index)
	{
		if (primary == nullptr || dont_transmit == nullptr || !primary->IsBitSet(index))
		{
			return false;
		}
		dont_transmit->Set(index);
		primary->Clear(index);
		return true;
	}

	// How a hidden entity is withheld from one recipient, switched with a convar.
	// Mode 0 is the CE paired update; on CS2 1.41.8 the second list sends its
	// entities, so marking it leaked hidden enemies live and the default is 2.
	enum class transmit_mode : int
	{
		clear_and_mark = 0, // clear the primary bit, set the second list's bit (legacy)
		clear_primary = 1,	// clear the primary bit only
		clear_both = 2,		// clear the primary bit and the second list's bit (default)
		observe = 3,		// change nothing (for the read-only probe)
		clear_extended = 4, // clear_both, plus lists +16/+24 once the probe showed they carry pawns
		clear_union = 5,	// clear_both, then drop entities no recipient keeps from both union lists
	};

	template<typename mask_type>
	inline bool apply_transmit_mode(mask_type* primary, mask_type* second, int index, transmit_mode mode)
	{
		if (primary == nullptr || second == nullptr)
		{
			return false;
		}
		switch (mode)
		{
			case transmit_mode::clear_and_mark:
				return withhold_transmit_bit(primary, second, index);
			case transmit_mode::clear_primary:
				if (!primary->IsBitSet(index))
				{
					return false;
				}
				primary->Clear(index);
				return true;
			case transmit_mode::clear_both:
			case transmit_mode::clear_extended:
			case transmit_mode::clear_union:
			{
				const bool primary_set = primary->IsBitSet(index);
				const bool second_set = second->IsBitSet(index);
				if (primary_set)
				{
					primary->Clear(index);
				}
				if (second_set)
				{
					second->Clear(index);
				}
				return primary_set || second_set;
			}
			case transmit_mode::observe:
				return false;
		}
		return false;
	}

} // namespace cs2glaz
