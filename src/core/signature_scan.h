#pragma once

// Byte-pattern search for server functions on builds without verified
// addresses. A pattern is hex bytes separated by spaces, '?' or '??' for any
// byte: "48 8D 05 ? ? ? ? 55". A function is used only when its pattern occurs
// exactly once in the searched code.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cs2glaz
{

	struct byte_pattern
	{
		std::vector<uint8_t> bytes;
		std::vector<uint8_t> mask; // 1 = must match, 0 = any byte
	};

	// False for an empty pattern, a bad token, or one that starts with a
	// wildcard.
	bool parse_byte_pattern(std::string_view text, byte_pattern& pattern);

	struct pattern_matches
	{
		const std::byte* first {};
		uint32_t count {}; // stops counting at 2
	};

	pattern_matches find_byte_pattern(std::span<const std::byte> memory, const byte_pattern& pattern);

} // namespace cs2glaz
