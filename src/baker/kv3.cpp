// Reads binary KeyValues3 the way Source 2 writes it: a header, one or two
// (optionally compressed) buffers split into 1/2/4/8-byte value streams, a type
// stream, and optional binary blobs. ValveResourceFormat's reader is the format
// reference. Every read is bounds-checked; recursion and value counts are capped.

#include "kv3.h"

#include "zstd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace cs2glaz::kv3
{
	namespace
	{

		constexpr uint32_t k_magic_legacy = 0x03564B56u; // "VKV\x03"
		constexpr uint32_t k_magic_base = 0x4B563300u;	 // "\x0?3VK" with the version in the low byte
		constexpr uint32_t k_trailer = 0xFFEEDD00u;
		constexpr uint32_t k_legacy_trailer = 0xFFFFFFFFu;
		constexpr uint32_t k_lz4_frame_size = 16384;
		constexpr int k_max_depth = 128;
		constexpr uint64_t k_max_values = 16u * 1024u * 1024u;
		constexpr uint64_t k_max_buffer = 1024ull * 1024ull * 1024ull;

		constexpr std::array<uint8_t, 16> k_encoding_binary {0x00, 0x05, 0x86, 0x1B, 0xD8, 0xF7, 0xC1, 0x40,
															 0xAD, 0x82, 0x75, 0xA4, 0x82, 0x67, 0xE7, 0x14};
		constexpr std::array<uint8_t, 16> k_encoding_binary_lz4 {0x8A, 0x34, 0x47, 0x68, 0xA1, 0x63, 0x5C, 0x4F,
																 0xA1, 0x97, 0x53, 0x80, 0x6F, 0xD9, 0xB1, 0x19};

		enum node_type : uint8_t
		{
			node_null = 1,
			node_boolean = 2,
			node_int64 = 3,
			node_uint64 = 4,
			node_double = 5,
			node_string = 6,
			node_binary_blob = 7,
			node_array = 8,
			node_object = 9,
			node_array_typed = 10,
			node_int32 = 11,
			node_uint32 = 12,
			node_boolean_true = 13,
			node_boolean_false = 14,
			node_int64_zero = 15,
			node_int64_one = 16,
			node_double_zero = 17,
			node_double_one = 18,
			node_float = 19,
			node_int16 = 20,
			node_uint16 = 21,
			node_int32_as_byte = 23,
			node_array_type_byte_length = 24,
			node_array_type_auxiliary_buffer = 25
		};

		enum compression_method : uint32_t
		{
			compression_none = 0,
			compression_lz4 = 1,
			compression_zstd = 2
		};

		class cursor
		{
		public:
			cursor() = default;

			explicit cursor(std::span<const uint8_t> data) : data_(data) {}

			size_t remaining() const
			{
				return data_.size() - offset_;
			}

			size_t offset() const
			{
				return offset_;
			}

			std::span<const uint8_t> rest() const
			{
				return data_.subspan(offset_);
			}

			bool take(size_t count, std::span<const uint8_t>& output)
			{
				if (count > remaining())
				{
					return false;
				}
				output = data_.subspan(offset_, count);
				offset_ += count;
				return true;
			}

			template<typename type>
			bool read(type& output)
			{
				static_assert(std::is_trivially_copyable_v<type>);
				if (sizeof(type) > remaining())
				{
					return false;
				}
				std::memcpy(&output, data_.data() + offset_, sizeof(type));
				offset_ += sizeof(type);
				return true;
			}

			bool read_string(std::string& output)
			{
				const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset_);
				const auto end = std::find(begin, data_.end(), uint8_t {0});
				if (end == data_.end())
				{
					return false;
				}
				output.assign(begin, end);
				offset_ += static_cast<size_t>(end - begin) + 1u;
				return true;
			}

		private:
			std::span<const uint8_t> data_;
			size_t offset_ {};
		};

		struct value_streams
		{
			cursor bytes1;
			cursor bytes2;
			cursor bytes4;
			cursor bytes8;
		};

		struct context
		{
			int version {};
			cursor types;
			cursor object_lengths;
			cursor blob_lengths;
			cursor blobs;
			std::vector<std::string> strings;
			value_streams* current {};
			value_streams* auxiliary {};
			uint64_t values {};
		};

		size_t align_up(size_t offset, size_t alignment)
		{
			return (offset + alignment - 1u) / alignment * alignment;
		}

		// Carves one value stream out of a decompressed buffer, the way the writer
		// laid them out: an optional alignment, then count * width bytes.
		bool carve(std::span<const uint8_t> buffer, size_t& offset, int32_t count, size_t width, size_t alignment, cursor& output)
		{
			if (count < 0)
			{
				return false;
			}
			if (count == 0)
			{
				output = cursor {};
				return true;
			}
			const size_t start = align_up(offset, alignment);
			const uint64_t size = static_cast<uint64_t>(count) * width;
			if (start > buffer.size() || size > buffer.size() - start)
			{
				return false;
			}
			output = cursor(buffer.subspan(start, static_cast<size_t>(size)));
			offset = start + static_cast<size_t>(size);
			return true;
		}

		bool zstd_decompress(std::span<const uint8_t> input, std::span<uint8_t> output)
		{
			const size_t written = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
			return !ZSTD_isError(written) && written == output.size();
		}

		// Reads (and when needed decompresses) one KV3 buffer from the block stream.
		bool read_buffer(cursor& stream, uint32_t method, int32_t uncompressed_size, int32_t compressed_size, size_t output_size,
						 std::vector<uint8_t>& output, std::string& error)
		{
			if (uncompressed_size < 0 || compressed_size < 0 || output_size > k_max_buffer)
			{
				error = "KV3 buffer sizes are invalid";
				return false;
			}
			output.assign(output_size, 0);
			std::span<const uint8_t> input;
			switch (method)
			{
				case compression_none:
					if (!stream.take(static_cast<size_t>(uncompressed_size), input))
					{
						error = "KV3 buffer is truncated";
						return false;
					}
					std::copy(input.begin(), input.end(), output.begin());
					return true;
				case compression_lz4:
				{
					size_t produced = 0;
					if (!stream.take(static_cast<size_t>(compressed_size), input)
						|| !lz4_decode_block(input, output, 0, output.size(), produced) || produced != output.size())
					{
						error = "KV3 LZ4 buffer is invalid";
						return false;
					}
					return true;
				}
				case compression_zstd:
					if (!stream.take(static_cast<size_t>(compressed_size), input) || !zstd_decompress(input, output))
					{
						error = "KV3 Zstandard buffer is invalid";
						return false;
					}
					return true;
				default:
					error = "KV3 compression method is unknown";
					return false;
			}
		}

		bool read_type(context& state, uint8_t& type)
		{
			uint8_t byte = 0;
			if (!state.types.read(byte))
			{
				return false;
			}
			if ((byte & 0x80u) != 0)
			{
				byte &= state.version >= 3 ? 0x3Fu : 0x7Fu;
				uint8_t flag = 0;
				if (!state.types.read(flag))
				{
					return false;
				}
			}
			type = byte;
			return true;
		}

		bool read_string_id(cursor& stream, const std::vector<std::string>& strings, std::string& output)
		{
			int32_t id = 0;
			if (!stream.read(id))
			{
				return false;
			}
			if (id == -1)
			{
				output.clear();
				return true;
			}
			if (id < 0 || static_cast<size_t>(id) >= strings.size())
			{
				return false;
			}
			output = strings[static_cast<size_t>(id)];
			return true;
		}

		bool read_count(cursor& stream, int32_t& count)
		{
			return stream.read(count) && count >= 0;
		}

		bool read_value(context& state, uint8_t type, value& output, int depth);

		// Upper bound on how many elements the remaining input can still describe,
		// so a corrupt count fails before a large allocation.
		size_t element_capacity(const context& state, bool typed, uint8_t element_type)
		{
			constexpr size_t k_zero_width_elements = 1u << 20u;
			if (!typed)
			{
				return state.types.remaining();
			}
			const value_streams& streams = *state.current;
			switch (element_type)
			{
				case node_boolean:
				case node_int32_as_byte:
				case node_array_type_byte_length:
					return streams.bytes1.remaining();
				case node_int16:
				case node_uint16:
					return streams.bytes2.remaining() / 2u;
				case node_int32:
				case node_uint32:
				case node_float:
				case node_string:
				case node_array:
				case node_array_typed:
					return streams.bytes4.remaining() / 4u;
				case node_int64:
				case node_uint64:
				case node_double:
					return streams.bytes8.remaining() / 8u;
				case node_binary_blob:
					return state.version < 2 ? streams.bytes4.remaining() / 4u : state.blob_lengths.remaining() / 4u;
				case node_object:
					return state.version >= 5 ? state.object_lengths.remaining() / 4u : streams.bytes4.remaining() / 4u;
				case node_array_type_auxiliary_buffer:
					return streams.bytes1.remaining();
				default:
					return k_zero_width_elements;
			}
		}

		bool read_elements(context& state, value& output, int32_t count, bool typed, uint8_t element_type, int depth)
		{
			if (static_cast<uint64_t>(count) > k_max_values - state.values
				|| static_cast<size_t>(count) > element_capacity(state, typed, element_type))
			{
				return false;
			}
			output.type = value_type::array;
			output.items.resize(static_cast<size_t>(count));
			for (value& item : output.items)
			{
				uint8_t type = element_type;
				if (!typed && !read_type(state, type))
				{
					return false;
				}
				if (!read_value(state, type, item, depth + 1))
				{
					return false;
				}
			}
			return true;
		}

		bool read_value(context& state, uint8_t type, value& output, int depth)
		{
			if (depth > k_max_depth || ++state.values > k_max_values)
			{
				return false;
			}
			value_streams& streams = *state.current;
			switch (type)
			{
				case node_null:
					output.type = value_type::null;
					return true;
				case node_boolean_true:
				case node_boolean_false:
					output.type = value_type::boolean;
					output.boolean = type == node_boolean_true;
					return true;
				case node_int64_zero:
				case node_int64_one:
					output.type = value_type::signed_integer;
					output.signed_integer = type == node_int64_one ? 1 : 0;
					return true;
				case node_double_zero:
				case node_double_one:
					output.type = value_type::floating;
					output.floating = type == node_double_one ? 1.0 : 0.0;
					return true;
				case node_boolean:
				{
					uint8_t raw = 0;
					output.type = value_type::boolean;
					if (!streams.bytes1.read(raw))
					{
						return false;
					}
					output.boolean = raw == 1;
					return true;
				}
				case node_int32_as_byte:
				{
					uint8_t raw = 0;
					output.type = value_type::signed_integer;
					if (!streams.bytes1.read(raw))
					{
						return false;
					}
					output.signed_integer = raw;
					return true;
				}
				case node_int16:
				{
					int16_t raw = 0;
					output.type = value_type::signed_integer;
					if (!streams.bytes2.read(raw))
					{
						return false;
					}
					output.signed_integer = raw;
					return true;
				}
				case node_uint16:
				{
					uint16_t raw = 0;
					output.type = value_type::unsigned_integer;
					if (!streams.bytes2.read(raw))
					{
						return false;
					}
					output.unsigned_integer = raw;
					return true;
				}
				case node_int32:
				{
					int32_t raw = 0;
					output.type = value_type::signed_integer;
					if (!streams.bytes4.read(raw))
					{
						return false;
					}
					output.signed_integer = raw;
					return true;
				}
				case node_uint32:
				{
					uint32_t raw = 0;
					output.type = value_type::unsigned_integer;
					if (!streams.bytes4.read(raw))
					{
						return false;
					}
					output.unsigned_integer = raw;
					return true;
				}
				case node_float:
				{
					float raw = 0.0f;
					output.type = value_type::floating;
					if (!streams.bytes4.read(raw))
					{
						return false;
					}
					output.floating = raw;
					return true;
				}
				case node_int64:
					output.type = value_type::signed_integer;
					return streams.bytes8.read(output.signed_integer);
				case node_uint64:
					output.type = value_type::unsigned_integer;
					return streams.bytes8.read(output.unsigned_integer);
				case node_double:
					output.type = value_type::floating;
					return streams.bytes8.read(output.floating);
				case node_string:
					output.type = value_type::string;
					return read_string_id(streams.bytes4, state.strings, output.text);
				case node_binary_blob:
				{
					output.type = value_type::blob;
					int32_t length = 0;
					std::span<const uint8_t> bytes;
					if (state.version < 2)
					{
						if (!read_count(streams.bytes4, length) || !streams.bytes1.take(static_cast<size_t>(length), bytes))
						{
							return false;
						}
					}
					else if (!read_count(state.blob_lengths, length) || !state.blobs.take(static_cast<size_t>(length), bytes))
					{
						return false;
					}
					output.blob.assign(bytes.begin(), bytes.end());
					return true;
				}
				case node_array:
				{
					int32_t count = 0;
					return read_count(streams.bytes4, count) && read_elements(state, output, count, false, 0, depth);
				}
				case node_array_typed:
				case node_array_type_byte_length:
				{
					int32_t count = 0;
					if (type == node_array_type_byte_length)
					{
						uint8_t small = 0;
						if (!streams.bytes1.read(small))
						{
							return false;
						}
						count = small;
					}
					else if (!read_count(streams.bytes4, count))
					{
						return false;
					}
					uint8_t element_type = 0;
					return read_type(state, element_type) && read_elements(state, output, count, true, element_type, depth);
				}
				case node_array_type_auxiliary_buffer:
				{
					uint8_t count = 0;
					uint8_t element_type = 0;
					if (state.auxiliary == nullptr || !streams.bytes1.read(count) || !read_type(state, element_type))
					{
						return false;
					}
					std::swap(state.current, state.auxiliary);
					const bool read = read_elements(state, output, count, true, element_type, depth);
					std::swap(state.current, state.auxiliary);
					return read;
				}
				case node_object:
				{
					int32_t count = 0;
					if (!read_count(state.version >= 5 ? state.object_lengths : streams.bytes4, count)
						|| static_cast<uint64_t>(count) > k_max_values - state.values || static_cast<size_t>(count) > state.types.remaining())
					{
						return false;
					}
					output.type = value_type::object;
					output.members.resize(static_cast<size_t>(count));
					for (auto& [name, member] : output.members)
					{
						uint8_t member_type = 0;
						if (!read_type(state, member_type) || !read_string_id(state.current->bytes4, state.strings, name)
							|| !read_value(state, member_type, member, depth + 1))
						{
							return false;
						}
					}
					return true;
				}
				default:
					return false;
			}
		}

		// Legacy VKV3 stores values inline in one stream and names before types.
		bool read_legacy_value(context& state, cursor& stream, uint8_t type, value& output, int depth);

		bool read_legacy_type(cursor& stream, uint8_t& type)
		{
			uint8_t byte = 0;
			if (!stream.read(byte))
			{
				return false;
			}
			if ((byte & 0x80u) != 0)
			{
				byte &= 0x7Fu;
				uint8_t flag = 0;
				if (!stream.read(flag))
				{
					return false;
				}
			}
			type = byte;
			return true;
		}

		bool read_legacy_value(context& state, cursor& stream, uint8_t type, value& output, int depth)
		{
			if (depth > k_max_depth || ++state.values > k_max_values)
			{
				return false;
			}
			switch (type)
			{
				case node_null:
					output.type = value_type::null;
					return true;
				case node_boolean:
				{
					uint8_t raw = 0;
					output.type = value_type::boolean;
					if (!stream.read(raw))
					{
						return false;
					}
					output.boolean = raw != 0;
					return true;
				}
				case node_boolean_true:
				case node_boolean_false:
					output.type = value_type::boolean;
					output.boolean = type == node_boolean_true;
					return true;
				case node_int64_zero:
				case node_int64_one:
					output.type = value_type::signed_integer;
					output.signed_integer = type == node_int64_one ? 1 : 0;
					return true;
				case node_double_zero:
				case node_double_one:
					output.type = value_type::floating;
					output.floating = type == node_double_one ? 1.0 : 0.0;
					return true;
				case node_int64:
					output.type = value_type::signed_integer;
					return stream.read(output.signed_integer);
				case node_uint64:
					output.type = value_type::unsigned_integer;
					return stream.read(output.unsigned_integer);
				case node_int32:
				{
					int32_t raw = 0;
					output.type = value_type::signed_integer;
					if (!stream.read(raw))
					{
						return false;
					}
					output.signed_integer = raw;
					return true;
				}
				case node_uint32:
				{
					uint32_t raw = 0;
					output.type = value_type::unsigned_integer;
					if (!stream.read(raw))
					{
						return false;
					}
					output.unsigned_integer = raw;
					return true;
				}
				case node_double:
					output.type = value_type::floating;
					return stream.read(output.floating);
				case node_string:
					output.type = value_type::string;
					return read_string_id(stream, state.strings, output.text);
				case node_binary_blob:
				{
					int32_t length = 0;
					std::span<const uint8_t> bytes;
					output.type = value_type::blob;
					if (!read_count(stream, length) || !stream.take(static_cast<size_t>(length), bytes))
					{
						return false;
					}
					output.blob.assign(bytes.begin(), bytes.end());
					return true;
				}
				case node_array:
				case node_array_typed:
				{
					int32_t count = 0;
					uint8_t element_type = 0;
					if (!read_count(stream, count) || static_cast<uint64_t>(count) > k_max_values - state.values
						|| (static_cast<size_t>(count) > stream.remaining() && count > (1 << 20))
						|| (type == node_array_typed && !read_legacy_type(stream, element_type)))
					{
						return false;
					}
					output.type = value_type::array;
					output.items.resize(static_cast<size_t>(count));
					for (value& item : output.items)
					{
						uint8_t item_type = element_type;
						if ((type == node_array && !read_legacy_type(stream, item_type))
							|| !read_legacy_value(state, stream, item_type, item, depth + 1))
						{
							return false;
						}
					}
					return true;
				}
				case node_object:
				{
					int32_t count = 0;
					if (!read_count(stream, count) || static_cast<uint64_t>(count) > k_max_values - state.values
						|| static_cast<size_t>(count) > stream.remaining())
					{
						return false;
					}
					output.type = value_type::object;
					output.members.resize(static_cast<size_t>(count));
					for (auto& [name, member] : output.members)
					{
						uint8_t member_type = 0;
						if (!read_string_id(stream, state.strings, name) || !read_legacy_type(stream, member_type)
							|| !read_legacy_value(state, stream, member_type, member, depth + 1))
						{
							return false;
						}
					}
					return true;
				}
				default:
					return false;
			}
		}

		bool decode_legacy(cursor& stream, value& root, std::string& error)
		{
			std::span<const uint8_t> encoding;
			std::span<const uint8_t> format;
			if (!stream.take(16, encoding) || !stream.take(16, format))
			{
				error = "legacy KV3 header is truncated";
				return false;
			}
			std::vector<uint8_t> storage;
			std::span<const uint8_t> payload;
			if (std::equal(encoding.begin(), encoding.end(), k_encoding_binary.begin()))
			{
				payload = stream.rest();
			}
			else if (std::equal(encoding.begin(), encoding.end(), k_encoding_binary_lz4.begin()))
			{
				int32_t size = 0;
				size_t produced = 0;
				if (!read_count(stream, size) || static_cast<uint64_t>(size) > k_max_buffer)
				{
					error = "legacy KV3 LZ4 size is invalid";
					return false;
				}
				storage.assign(static_cast<size_t>(size), 0);
				if (!lz4_decode_block(stream.rest(), storage, 0, storage.size(), produced) || produced != storage.size())
				{
					error = "legacy KV3 LZ4 payload is invalid";
					return false;
				}
				payload = storage;
			}
			else
			{
				error = "legacy KV3 encoding is not supported";
				return false;
			}
			cursor data(payload);
			context state;
			uint32_t count = 0;
			if (!data.read(count) || count > payload.size())
			{
				error = "legacy KV3 string table is invalid";
				return false;
			}
			state.strings.resize(count);
			for (std::string& text : state.strings)
			{
				if (!data.read_string(text))
				{
					error = "legacy KV3 string table is truncated";
					return false;
				}
			}
			uint8_t type = 0;
			uint32_t trailer = 0;
			if (!read_legacy_type(data, type) || !read_legacy_value(state, data, type, root, 0) || !data.read(trailer)
				|| trailer != k_legacy_trailer)
			{
				error = "legacy KV3 values are invalid";
				return false;
			}
			return true;
		}

		bool decode_versioned(cursor& stream, int version, value& root, std::string& error)
		{
			std::span<const uint8_t> format;
			uint32_t method = 0;
			if (!stream.take(16, format) || !stream.read(method))
			{
				error = "KV3 header is truncated";
				return false;
			}
			uint16_t dictionary_id = 0;
			uint16_t frame_size = 0;
			int32_t count_bytes1 = 0;
			int32_t count_bytes2 = 0;
			int32_t count_bytes4 = 0;
			int32_t count_bytes8 = 0;
			int32_t count_types = 0;
			uint16_t count_objects = 0;
			uint16_t count_arrays = 0;
			int32_t size_uncompressed_total = 0;
			int32_t size_compressed_total = 0;
			int32_t count_blocks = 0;
			int32_t size_blobs = 0;
			int32_t size_block_compressed_sizes = 0;
			bool header = true;
			if (version == 1)
			{
				header = stream.read(count_bytes1) && stream.read(count_bytes4) && stream.read(count_bytes8) && stream.read(size_uncompressed_total);
				size_compressed_total = static_cast<int32_t>(std::min<size_t>(stream.remaining(), std::numeric_limits<int32_t>::max()));
			}
			else
			{
				header = stream.read(dictionary_id) && stream.read(frame_size) && stream.read(count_bytes1) && stream.read(count_bytes4)
						 && stream.read(count_bytes8) && stream.read(count_types) && stream.read(count_objects) && stream.read(count_arrays)
						 && stream.read(size_uncompressed_total) && stream.read(size_compressed_total) && stream.read(count_blocks)
						 && stream.read(size_blobs);
			}
			if (header && version >= 4)
			{
				header = stream.read(count_bytes2) && stream.read(size_block_compressed_sizes);
			}
			int32_t size_uncompressed_buffer1 = size_uncompressed_total;
			int32_t size_compressed_buffer1 = size_compressed_total;
			int32_t size_uncompressed_buffer2 = 0;
			int32_t size_compressed_buffer2 = 0;
			int32_t count_bytes1_buffer2 = 0;
			int32_t count_bytes2_buffer2 = 0;
			int32_t count_bytes4_buffer2 = 0;
			int32_t count_bytes8_buffer2 = 0;
			int32_t count_objects_buffer2 = 0;
			int32_t count_arrays_buffer2 = 0;
			int32_t unknown = 0;
			if (header && version >= 5)
			{
				header = stream.read(size_uncompressed_buffer1) && stream.read(size_compressed_buffer1) && stream.read(size_uncompressed_buffer2)
						 && stream.read(size_compressed_buffer2) && stream.read(count_bytes1_buffer2) && stream.read(count_bytes2_buffer2)
						 && stream.read(count_bytes4_buffer2) && stream.read(count_bytes8_buffer2) && stream.read(unknown)
						 && stream.read(count_objects_buffer2) && stream.read(count_arrays_buffer2) && stream.read(unknown);
			}
			if (!header)
			{
				error = "KV3 header is truncated";
				return false;
			}
			if (dictionary_id != 0 || (method == compression_lz4 && version >= 2 && frame_size != k_lz4_frame_size)
				|| (method != compression_lz4 && frame_size != 0) || (method == compression_zstd && version < 2) || count_blocks < 0 || size_blobs < 0
				|| size_uncompressed_buffer1 < 0 || size_uncompressed_buffer2 < 0 || count_types < 0)
			{
				error = "KV3 header values are not supported";
				return false;
			}
			(void)count_objects;
			(void)count_arrays;
			(void)count_arrays_buffer2;

			// Buffer 1. Before version 5, Zstandard also compressed the blobs into it.
			std::vector<uint8_t> buffer1;
			const bool blobs_in_buffer1 = version < 5 && method == compression_zstd;
			const size_t buffer1_output = static_cast<size_t>(size_uncompressed_buffer1) + (blobs_in_buffer1 ? static_cast<size_t>(size_blobs) : 0u);
			if (!read_buffer(stream, method, size_uncompressed_buffer1, size_compressed_buffer1, buffer1_output, buffer1, error))
			{
				return false;
			}
			const std::span<const uint8_t> buffer1_values(buffer1.data(), static_cast<size_t>(size_uncompressed_buffer1));
			value_streams streams1;
			size_t offset = 0;
			if (!carve(buffer1_values, offset, count_bytes1, 1, 1, streams1.bytes1) || !carve(buffer1_values, offset, count_bytes2, 2, 2, streams1.bytes2)
				|| !carve(buffer1_values, offset, count_bytes4, 4, 4, streams1.bytes4) || !carve(buffer1_values, offset, count_bytes8, 8, 8, streams1.bytes8))
			{
				error = "KV3 value streams exceed buffer 1";
				return false;
			}
			if (count_bytes8 == 0 && version < 5)
			{
				offset = align_up(offset, 8);
			}
			context state;
			state.version = version;
			int32_t count_strings = 0;
			if (!read_count(streams1.bytes4, count_strings) || static_cast<size_t>(count_strings) > buffer1_values.size())
			{
				error = "KV3 string count is invalid";
				return false;
			}
			state.strings.resize(static_cast<size_t>(count_strings));
			value_streams streams2;
			std::vector<uint8_t> buffer2;
			std::span<const uint8_t> blob_sizes;
			if (version >= 5)
			{
				for (std::string& text : state.strings)
				{
					if (!streams1.bytes1.read_string(text))
					{
						error = "KV3 string table is truncated";
						return false;
					}
				}
				if (!read_buffer(stream, method, size_uncompressed_buffer2, size_compressed_buffer2, static_cast<size_t>(size_uncompressed_buffer2),
								 buffer2, error))
				{
					return false;
				}
				size_t offset2 = 0;
				if (!carve(buffer2, offset2, count_objects_buffer2, 4, 1, state.object_lengths)
					|| !carve(buffer2, offset2, count_bytes1_buffer2, 1, 1, streams2.bytes1)
					|| !carve(buffer2, offset2, count_bytes2_buffer2, 2, 2, streams2.bytes2)
					|| !carve(buffer2, offset2, count_bytes4_buffer2, 4, 4, streams2.bytes4)
					|| !carve(buffer2, offset2, count_bytes8_buffer2, 8, 8, streams2.bytes8)
					|| !carve(buffer2, offset2, count_types, 1, 1, state.types))
				{
					error = "KV3 value streams exceed buffer 2";
					return false;
				}
				blob_sizes = std::span<const uint8_t>(buffer2).subspan(offset2);
				state.current = &streams2;
				state.auxiliary = &streams1;
			}
			else
			{
				cursor strings(buffer1_values.subspan(std::min(offset, buffer1_values.size())));
				const size_t strings_start = offset;
				for (std::string& text : state.strings)
				{
					if (!strings.read_string(text))
					{
						error = "KV3 string table is truncated";
						return false;
					}
				}
				offset = strings_start + strings.offset();
				const int64_t types_length = version == 1
												 ? static_cast<int64_t>(size_uncompressed_total) - static_cast<int64_t>(offset) - 4
												 : static_cast<int64_t>(count_types) - static_cast<int64_t>(strings.offset());
				if (types_length < 0 || static_cast<uint64_t>(types_length) > buffer1_values.size() - offset)
				{
					error = "KV3 type stream is invalid";
					return false;
				}
				state.types = cursor(buffer1_values.subspan(offset, static_cast<size_t>(types_length)));
				offset += static_cast<size_t>(types_length);
				blob_sizes = buffer1_values.subspan(offset);
				state.current = &streams1;
			}

			std::vector<uint8_t> blobs;
			if (count_blocks == 0)
			{
				cursor tail(blob_sizes);
				uint32_t trailer = 0;
				if (!tail.read(trailer) || trailer != k_trailer)
				{
					error = "KV3 buffer trailer is invalid";
					return false;
				}
			}
			else
			{
				if (version < 2)
				{
					error = "KV3 version 1 cannot contain binary blob blocks";
					return false;
				}
				cursor sizes(blob_sizes);
				std::span<const uint8_t> lengths;
				uint32_t trailer = 0;
				if (!sizes.take(static_cast<size_t>(count_blocks) * 4u, lengths) || !sizes.read(trailer) || trailer != k_trailer)
				{
					error = "KV3 blob table is invalid";
					return false;
				}
				state.blob_lengths = cursor(lengths);
				if (static_cast<uint64_t>(size_blobs) > k_max_buffer)
				{
					error = "KV3 blobs are too large";
					return false;
				}
				if (method == compression_none)
				{
					std::span<const uint8_t> raw;
					if (!stream.take(static_cast<size_t>(size_blobs), raw))
					{
						error = "KV3 blobs are truncated";
						return false;
					}
					blobs.assign(raw.begin(), raw.end());
				}
				else if (method == compression_lz4)
				{
					blobs.assign(static_cast<size_t>(size_blobs), 0);
					size_t decoded = 0;
					while (sizes.remaining() != 0)
					{
						uint16_t compressed = 0;
						std::span<const uint8_t> input;
						size_t produced = 0;
						const size_t frame = std::min<size_t>(k_lz4_frame_size, blobs.size() - decoded);
						if (!sizes.read(compressed) || !stream.take(compressed, input) || frame == 0
							|| !lz4_decode_block(input, blobs, decoded, decoded + frame, produced) || produced == 0)
						{
							error = "KV3 LZ4 blobs are invalid";
							return false;
						}
						decoded += produced;
					}
					if (decoded != blobs.size())
					{
						error = "KV3 LZ4 blobs are incomplete";
						return false;
					}
				}
				else if (version >= 5)
				{
					const int64_t compressed = static_cast<int64_t>(size_compressed_total) - size_compressed_buffer1 - size_compressed_buffer2;
					std::span<const uint8_t> input;
					blobs.assign(static_cast<size_t>(size_blobs), 0);
					if (size_block_compressed_sizes != 0 || compressed < 0 || !stream.take(static_cast<size_t>(compressed), input)
						|| !zstd_decompress(input, blobs))
					{
						error = "KV3 Zstandard blobs are invalid";
						return false;
					}
				}
				else
				{
					blobs.assign(buffer1.begin() + size_uncompressed_buffer1, buffer1.end());
				}
				uint32_t blob_trailer = 0;
				if (!stream.read(blob_trailer) || blob_trailer != k_trailer)
				{
					error = "KV3 blob trailer is invalid";
					return false;
				}
				state.blobs = cursor(blobs);
			}

			uint8_t root_type = 0;
			if (!read_type(state, root_type) || !read_value(state, root_type, root, 0))
			{
				error = "KV3 values are invalid";
				return false;
			}
			return true;
		}

	} // namespace

	const value* value::find(std::string_view key) const
	{
		for (const auto& [name, member] : members)
		{
			if (name == key)
			{
				return &member;
			}
		}
		return nullptr;
	}

	bool value::is_number() const
	{
		return type == value_type::signed_integer || type == value_type::unsigned_integer || type == value_type::floating
			   || type == value_type::boolean;
	}

	double value::as_double() const
	{
		switch (type)
		{
			case value_type::signed_integer:
				return static_cast<double>(signed_integer);
			case value_type::unsigned_integer:
				return static_cast<double>(unsigned_integer);
			case value_type::floating:
				return floating;
			case value_type::boolean:
				return boolean ? 1.0 : 0.0;
			default:
				return 0.0;
		}
	}

	int64_t value::as_int64() const
	{
		switch (type)
		{
			case value_type::signed_integer:
				return signed_integer;
			case value_type::unsigned_integer:
				return static_cast<int64_t>(unsigned_integer);
			case value_type::floating:
				return std::isfinite(floating) && std::abs(floating) < 9.0e18 ? static_cast<int64_t>(floating) : 0;
			case value_type::boolean:
				return boolean ? 1 : 0;
			default:
				return 0;
		}
	}

	bool lz4_decode_block(std::span<const uint8_t> input, std::span<uint8_t> output, size_t begin, size_t limit, size_t& produced)
	{
		produced = 0;
		if (input.empty() || begin > limit || limit > output.size())
		{
			return false;
		}
		const uint8_t* in = input.data();
		const uint8_t* const end = in + input.size();
		size_t out = begin;
		for (;;)
		{
			const uint8_t token = *in++;
			size_t literals = token >> 4u;
			if (literals == 15u)
			{
				uint8_t extra = 0;
				do
				{
					if (in == end)
					{
						return false;
					}
					extra = *in++;
					literals += extra;
				} while (extra == 255u);
			}
			if (literals > static_cast<size_t>(end - in) || literals > limit - out)
			{
				return false;
			}
			if (literals != 0)
			{
				std::memcpy(output.data() + out, in, literals);
			}
			in += literals;
			out += literals;
			if (in == end)
			{
				break;
			}
			if (end - in < 2)
			{
				return false;
			}
			const size_t distance = static_cast<size_t>(in[0]) | (static_cast<size_t>(in[1]) << 8u);
			in += 2;
			if (distance == 0 || distance > out)
			{
				return false;
			}
			size_t length = (token & 15u) + 4u;
			if ((token & 15u) == 15u)
			{
				uint8_t extra = 0;
				do
				{
					if (in == end)
					{
						return false;
					}
					extra = *in++;
					length += extra;
				} while (extra == 255u);
			}
			if (length > limit - out)
			{
				return false;
			}
			uint8_t* const bytes = output.data();
			for (size_t index = 0; index < length; ++index)
			{
				bytes[out + index] = bytes[out + index - distance];
			}
			out += length;
			if (in == end)
			{
				return false;
			}
		}
		produced = out - begin;
		return true;
	}

	bool decode(std::span<const uint8_t> data, value& root, std::string& error)
	{
		root = {};
		try
		{
			cursor stream(data);
			uint32_t magic = 0;
			if (!stream.read(magic))
			{
				error = "KV3 block is truncated";
				return false;
			}
			if (magic == k_magic_legacy)
			{
				return decode_legacy(stream, root, error);
			}
			const int version = static_cast<int>(magic & 0xFFu);
			if ((magic & 0xFFFFFF00u) != k_magic_base || version < 1 || version > 5)
			{
				error = "block is not binary KV3";
				return false;
			}
			return decode_versioned(stream, version, root, error);
		}
		catch (const std::bad_alloc&)
		{
			root = {};
			error = "KV3 data is too large";
			return false;
		}
	}

} // namespace cs2glaz::kv3
