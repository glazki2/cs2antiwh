#pragma once

// Decodes Source 2 binary KeyValues3 blocks (legacy VKV3 and KV3 versions 1-5,
// uncompressed, LZ4 or Zstandard) into a small value tree. The baker uses it to
// read map physics without external tools; malformed input returns an error.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cs2glaz::kv3
{

	enum class value_type : uint8_t
	{
		null,
		boolean,
		signed_integer,
		unsigned_integer,
		floating,
		string,
		blob,
		array,
		object
	};

	struct value
	{
		value_type type {value_type::null};
		bool boolean {};
		int64_t signed_integer {};
		uint64_t unsigned_integer {};
		double floating {};
		std::string text;
		std::vector<uint8_t> blob;
		std::vector<value> items;
		std::vector<std::pair<std::string, value>> members;

		const value* find(std::string_view key) const;
		bool is_number() const;
		// Numeric conversions accept any numeric or boolean type.
		double as_double() const;
		int64_t as_int64() const;
	};

	bool decode(std::span<const uint8_t> data, value& root, std::string& error);

	// Decodes one raw LZ4 block into output[begin, limit). Matches may reach back
	// before begin (chained frames share one output buffer). produced receives the
	// number of bytes written after begin.
	bool lz4_decode_block(std::span<const uint8_t> input, std::span<uint8_t> output, size_t begin, size_t limit, size_t& produced);

} // namespace cs2glaz::kv3
