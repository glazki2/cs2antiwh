#pragma once

// Fixed-capacity evidence collected only for real primary-list clears. The
// CheckTransmit caller supplies one event; records aggregate recipients and
// reasons in place so the hot hook never allocates or prints automatically.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cs2glaz
{

	inline constexpr uint8_t k_transmit_reason_current = 1u << 0u;
	inline constexpr uint8_t k_transmit_reason_quarantine = 1u << 1u;

	struct transmit_debug_event
	{
		int edict {-1};
		uint32_t handle {};
		uint32_t source {};
		int recipient_slot {-1};
		uint8_t reason {};
		std::chrono::steady_clock::time_point when;
	};

	template<size_t name_size>
	struct transmit_debug_record
	{
		int edict {-1};
		uint32_t handle {};
		uint32_t source {};
		uint8_t reasons {};
		uint64_t recipients {};
		uint64_t clears {};
		std::chrono::steady_clock::time_point first_seen;
		std::chrono::steady_clock::time_point last_seen;
		std::array<char, name_size> name {};
		bool valid {};
	};

	template<size_t capacity, size_t name_size>
	class transmit_debug_log
	{
	public:
		using record_type = transmit_debug_record<name_size>;

		void record(const transmit_debug_event& event, const char* name)
		{
			for (record_type& record : records_)
			{
				if (record.valid && same_key(record, event))
				{
					update(record, event);
					return;
				}
			}
			record_type& record = records_[next_++ % records_.size()];
			record = {};
			record.valid = true;
			record.edict = event.edict;
			record.handle = event.handle;
			record.source = event.source;
			record.first_seen = event.when;
			std::snprintf(record.name.data(), record.name.size(), "%s", name == nullptr ? "<unknown>" : name);
			update(record, event);
		}

		void clear()
		{
			records_ = {};
			next_ = 0;
		}

		const std::array<record_type, capacity>& records() const
		{
			return records_;
		}

	private:
		static bool same_key(const record_type& record, const transmit_debug_event& event)
		{
			return record.handle == event.handle && record.source == event.source;
		}

		static void update(record_type& record, const transmit_debug_event& event)
		{
			record.edict = event.edict;
			record.reasons |= event.reason;
			if (event.recipient_slot >= 0 && event.recipient_slot < 64)
			{
				record.recipients |= uint64_t {1} << event.recipient_slot;
			}
			++record.clears;
			record.last_seen = event.when;
		}

		std::array<record_type, capacity> records_ {};
		size_t next_ {};
	};

} // namespace cs2glaz
