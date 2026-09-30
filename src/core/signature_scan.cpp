#include "signature_scan.h"

#include <cstring>

namespace cs2glaz
{

	bool parse_byte_pattern(std::string_view text, byte_pattern& pattern)
	{
		pattern = {};
		size_t position = 0;
		while (position < text.size())
		{
			while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
			{
				++position;
			}
			if (position >= text.size())
			{
				break;
			}
			size_t end = position;
			while (end < text.size() && text[end] != ' ' && text[end] != '\t')
			{
				++end;
			}
			const std::string_view token = text.substr(position, end - position);
			position = end;
			if (token == "?" || token == "??")
			{
				pattern.bytes.push_back(0);
				pattern.mask.push_back(0);
				continue;
			}
			if (token.size() != 2)
			{
				pattern = {};
				return false;
			}
			const auto nibble = [](char character) -> int
			{
				if (character >= '0' && character <= '9')
				{
					return character - '0';
				}
				if (character >= 'a' && character <= 'f')
				{
					return character - 'a' + 10;
				}
				if (character >= 'A' && character <= 'F')
				{
					return character - 'A' + 10;
				}
				return -1;
			};
			const int high = nibble(token[0]);
			const int low = nibble(token[1]);
			if (high < 0 || low < 0)
			{
				pattern = {};
				return false;
			}
			pattern.bytes.push_back(static_cast<uint8_t>(high * 16 + low));
			pattern.mask.push_back(1);
		}
		if (pattern.bytes.empty() || pattern.mask.front() == 0)
		{
			pattern = {};
			return false;
		}
		return true;
	}

	pattern_matches find_byte_pattern(std::span<const std::byte> memory, const byte_pattern& pattern)
	{
		pattern_matches matches;
		const size_t length = pattern.bytes.size();
		if (length == 0 || pattern.mask.size() != length || memory.size() < length)
		{
			return matches;
		}
		const auto* data = reinterpret_cast<const uint8_t*>(memory.data());
		const size_t last = memory.size() - length;
		const uint8_t lead = pattern.bytes.front();
		for (size_t offset = 0; offset <= last; ++offset)
		{
			const void* found = std::memchr(data + offset, lead, last - offset + 1);
			if (found == nullptr)
			{
				break;
			}
			offset = static_cast<size_t>(static_cast<const uint8_t*>(found) - data);
			bool match = true;
			for (size_t index = 1; index < length; ++index)
			{
				if (pattern.mask[index] != 0 && data[offset + index] != pattern.bytes[index])
				{
					match = false;
					break;
				}
			}
			if (!match)
			{
				continue;
			}
			if (matches.count == 0)
			{
				matches.first = memory.data() + offset;
			}
			if (++matches.count >= 2)
			{
				break;
			}
		}
		return matches;
	}

} // namespace cs2glaz
