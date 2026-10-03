#pragma once

// What CS2GLAZ itself changed in one recipient's snapshots: an entity his
// client had that CS2GLAZ withheld, and an entity CS2GLAZ had withheld that
// went to him again. A client that fails with "CopyExistingEntity: missing
// client entity N" names an index the server believed it had; this record
// says whether CS2GLAZ took N away from that client, when, why, and how soon
// it was sent again. No SDK types: the plugin passes the list words.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace cs2glaz
{

	enum class withhold_reason : uint8_t
	{
		unknown,
		enemy_pawn,		// a hidden enemy
		enemy_item,		// his weapons, wearables, carried hostage
		enemy_attached, // other entities attached to his body
		dead_hidden,	// the body of an enemy who died while hidden
		decoy,			// a decoy: not (or no longer) proven hidden, or another viewer's
		decoy_control,	// a control twin, sent to nobody
		decoy_parked,	// parked for reuse, or removed
		ghost,			// a ghost player's controller, pawn or items (sent to its viewer only)
	};

	inline const char* withhold_reason_name(withhold_reason reason)
	{
		switch (reason)
		{
			case withhold_reason::enemy_pawn:
				return "hidden enemy";
			case withhold_reason::enemy_item:
				return "hidden enemy's item";
			case withhold_reason::enemy_attached:
				return "attached to a hidden enemy";
			case withhold_reason::dead_hidden:
				return "enemy who died hidden";
			case withhold_reason::decoy:
				return "decoy";
			case withhold_reason::decoy_control:
				return "control decoy";
			case withhold_reason::decoy_parked:
				return "parked decoy";
			case withhold_reason::ghost:
				return "ghost player";
			case withhold_reason::unknown:
				break;
		}
		return "unknown";
	}

	enum class journal_event_kind : uint8_t
	{
		withheld,	 // his client had it; CS2GLAZ withheld it
		sent_again,	 // CS2GLAZ had withheld it; it went to him again
		full_update, // his client rebuilt every entity
	};

	struct journal_event
	{
		double seconds {};			// journal clock
		double withheld_ms {-1.0}; // sent_again: since the matching withheld event; -1 unknown
		uint16_t index {};
		uint16_t serial {}; // of the entity at the index then; 0 when none
		withhold_reason reason {};
		journal_event_kind kind {};
		char classname[26] {};
	};

	inline constexpr size_t k_transmit_words = 16384 / 32;
	inline constexpr size_t k_journal_events = 512;
	inline constexpr size_t k_journal_marks = 1024;
	// Sent again sooner than this after being withheld: counted apart, since a
	// client may still be acknowledging the snapshot that took it away.
	inline constexpr double k_quick_resend_ms = 300.0;
	using transmit_words = std::array<uint32_t, k_transmit_words>;

	struct quick_resend_counts
	{
		uint64_t players {};
		uint64_t items {}; // weapons, wearables, attached entities, dead bodies
		uint64_t decoys {};
		uint64_t other {};
	};

	struct recipient_journal
	{
		transmit_words sent {};		// last finished snapshot: in either list
		transmit_words withheld {}; // last finished snapshot: cleared by CS2GLAZ
		transmit_words marked {};	// this snapshot, being built
		std::array<std::pair<uint16_t, withhold_reason>, k_journal_marks> marks {};
		size_t mark_count {};
		std::array<journal_event, k_journal_events> events {};
		size_t next {};
		size_t count {};
		bool primed {}; // sent/withheld describe a real previous snapshot
		uint64_t xuid {};
		char name[48] {};
	};

	inline void journal_clear_events(recipient_journal& journal)
	{
		journal.next = 0;
		journal.count = 0;
	}

	// Forgets the previous snapshot (the next one starts a fresh comparison),
	// keeping the events.
	inline void journal_unprime(recipient_journal& journal)
	{
		journal.primed = false;
		journal.marked.fill(0);
		journal.mark_count = 0;
	}

	// A new player in the slot, or a new map: nothing before applies.
	inline void journal_reset(recipient_journal& journal)
	{
		journal_unprime(journal);
		journal_clear_events(journal);
		journal.xuid = 0;
		journal.name[0] = '\0';
	}

	inline void journal_mark(recipient_journal& journal, int index, withhold_reason reason)
	{
		if (index < 0 || index >= static_cast<int>(k_transmit_words * 32))
		{
			return;
		}
		const uint32_t bit = uint32_t {1} << (static_cast<uint32_t>(index) & 31u);
		uint32_t& word = journal.marked[static_cast<size_t>(index) >> 5];
		if ((word & bit) != 0)
		{
			return;
		}
		word |= bit;
		if (journal.mark_count < journal.marks.size())
		{
			journal.marks[journal.mark_count++] = {static_cast<uint16_t>(index), reason};
		}
	}

	inline withhold_reason journal_marked_reason(const recipient_journal& journal, uint16_t index)
	{
		for (size_t mark = 0; mark < journal.mark_count; ++mark)
		{
			if (journal.marks[mark].first == index)
			{
				return journal.marks[mark].second;
			}
		}
		return withhold_reason::unknown;
	}

	// The newest event of this kind for the index, or nullptr.
	inline const journal_event* journal_find_last(const recipient_journal& journal, uint16_t index, journal_event_kind kind)
	{
		for (size_t back = 1; back <= journal.count; ++back)
		{
			const journal_event& event = journal.events[(journal.next + journal.events.size() - back) % journal.events.size()];
			if (event.kind == journal_event_kind::full_update)
			{
				return nullptr; // nothing before a rebuild matches the client any more
			}
			if (event.index == index && event.kind == kind)
			{
				return &event;
			}
		}
		return nullptr;
	}

	// The i-th newest event (0 = newest).
	inline const journal_event& journal_event_at(const recipient_journal& journal, size_t newest_first)
	{
		return journal.events[(journal.next + journal.events.size() - 1 - newest_first) % journal.events.size()];
	}

	inline journal_event& journal_push(recipient_journal& journal)
	{
		journal_event& event = journal.events[journal.next];
		journal.next = (journal.next + 1) % journal.events.size();
		journal.count = journal.count < journal.events.size() ? journal.count + 1 : journal.count;
		event = {};
		return event;
	}

	inline void journal_count_quick(quick_resend_counts& counts, withhold_reason reason)
	{
		switch (reason)
		{
			case withhold_reason::enemy_pawn:
				++counts.players;
				break;
			case withhold_reason::enemy_item:
			case withhold_reason::enemy_attached:
			case withhold_reason::dead_hidden:
				++counts.items;
				break;
			case withhold_reason::decoy:
			case withhold_reason::decoy_control:
			case withhold_reason::decoy_parked:
			case withhold_reason::ghost:
				++counts.decoys;
				break;
			case withhold_reason::unknown:
				++counts.other;
				break;
		}
	}

	// Ends one snapshot of one recipient. primary and second are the final
	// lists (what the engine sends him); describe(index, serial, classname)
	// names the entity at an index for a new event. Returns the events added.
	template<typename describe_fn>
	size_t journal_finish(recipient_journal& journal, const uint32_t* primary, const uint32_t* second, bool full_update, double now,
						  describe_fn&& describe, quick_resend_counts& quick)
	{
		size_t added = 0;
		if (primary == nullptr || second == nullptr)
		{
			journal_unprime(journal);
			return 0;
		}
		if (full_update)
		{
			journal_event& event = journal_push(journal);
			event.seconds = now;
			event.kind = journal_event_kind::full_update;
			++added;
		}
		for (size_t word = 0; word < k_transmit_words; ++word)
		{
			const uint32_t sent_now = primary[word] | second[word];
			const uint32_t withheld_now = journal.marked[word];
			if (journal.primed && !full_update)
			{
				// Withheld now and in his snapshot before; withheld before and sent now.
				uint32_t left = withheld_now & journal.sent[word];
				uint32_t back = sent_now & journal.withheld[word];
				for (int pass = 0; pass < 2; ++pass)
				{
					uint32_t bits = pass == 0 ? left : back;
					while (bits != 0)
					{
						const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
						bits &= bits - 1;
						const auto index = static_cast<uint16_t>(word * 32 + bit);
						journal_event event;
						event.seconds = now;
						event.index = index;
						describe(index, event.serial, event.classname);
						if (pass == 0)
						{
							event.kind = journal_event_kind::withheld;
							event.reason = journal_marked_reason(journal, index);
						}
						else
						{
							event.kind = journal_event_kind::sent_again;
							const journal_event* taken = journal_find_last(journal, index, journal_event_kind::withheld);
							if (taken != nullptr)
							{
								event.reason = taken->reason;
								event.withheld_ms = (now - taken->seconds) * 1000.0;
								if (event.withheld_ms < k_quick_resend_ms)
								{
									journal_count_quick(quick, event.reason);
								}
							}
						}
						journal_push(journal) = event;
						++added;
					}
				}
			}
			journal.sent[word] = sent_now;
			journal.withheld[word] = withheld_now;
		}
		journal.primed = true;
		journal.marked.fill(0);
		journal.mark_count = 0;
		return added;
	}

} // namespace cs2glaz
