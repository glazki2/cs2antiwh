#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace cs2glaz
{
	// A list with a fixed capacity on the stack, for the per-tick lists of
	// players and decoys (at most 64 entries): no heap allocation every tick.
	// An item past the capacity is dropped.
	template<typename T, size_t capacity>
	class fixed_list
	{
	public:
		void push_back(const T& value)
		{
			if (size_ < capacity)
			{
				items_[size_++] = value;
			}
		}

		size_t size() const
		{
			return size_;
		}

		bool empty() const
		{
			return size_ == 0;
		}

		const T& operator[](size_t index) const
		{
			return items_[index];
		}

		const T* begin() const
		{
			return items_.data();
		}

		const T* end() const
		{
			return items_.data() + size_;
		}

		operator std::span<const T>() const
		{
			return {items_.data(), size_};
		}

	private:
		std::array<T, capacity> items_ {};
		size_t size_ {};
	};
} // namespace cs2glaz
