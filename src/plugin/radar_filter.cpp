#include "plugin.h"
#include "rtti_check.h"

// Filters the radar message CCSUsrMsg_ProcessSpottedEntityUpdate. CS2 sends it
// with the position and yaw of spotted enemies that are not in a recipient's
// snapshot, which is exactly where CS2GLAZ puts hidden enemies, so a cheat can
// draw them from it for as long as they stay spotted. Entries about enemies that
// nobody on the recipient's team currently sees are removed; teammates, the
// bomb and hostages stay. Anything unexpected leaves the message untouched.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <bit>
#include <string_view>

namespace cs2glaz
{
	namespace
	{

		CConVar<int> cs2glaz_radar_filter("cs2glaz_radar_filter", FCVAR_NONE,
										  "Radar positions of enemies: 0 keep all, 1 only while someone on the recipient's team sees them, "
										  "2 only while the recipient sees them",
										  1, true, 0, true, 2);

		constexpr std::string_view k_radar_message_name = "CCSUsrMsg_ProcessSpottedEntityUpdate";

		// The protobuf base of a live CNetMessagePB<CCSUsrMsg_ProcessSpottedEntityUpdate>
		// starting at owner, proven from RTTI through guarded reads.
		bool radar_object_verified(const void* owner, const void* proto)
		{
			return rtti_names_class_at_offset(owner, proto, k_radar_message_name, [](const void* address, void* output, size_t size)
											  { return runtime_compatibility::safe_read(address, output, size); });
		}

	} // namespace

	KHook::Return<void> plugin::khook_post_event(IGameEventSystem*, CSplitScreenSlot, bool, int, const uint64* clients, INetworkMessageInternal* event,
												 const CNetMessage* data, unsigned long, NetChannelBufType_t)
	{
		if (event == nullptr || data == nullptr || radar_filter_broken_.load(std::memory_order_relaxed))
		{
			return {KHook::Action::Ignore};
		}
		INetworkMessageInternal* const radar = radar_message_.load(std::memory_order_relaxed);
		if (radar == nullptr)
		{
			const char* name = event->GetUnscopedName();
			if (name == nullptr || std::string_view(name).find("ProcessSpottedEntityUpdate") == std::string_view::npos)
			{
				return {KHook::Action::Ignore};
			}
			radar_message_.store(event, std::memory_order_relaxed);
		}
		else if (event != radar)
		{
			return {KHook::Action::Ignore};
		}
		filter_radar_message(clients, data);
		return {KHook::Action::Ignore};
	}

	void plugin::filter_radar_message(const uint64* clients, const CNetMessage* data)
	{
		radar_stats_.messages.fetch_add(1, std::memory_order_relaxed);
		const std::shared_ptr<const visibility_result> result = worker_.result();
		// One message per player is expected; one addressed to several players
		// or to everyone is left alone rather than guessing whose view applies.
		const int filter = cs2glaz_radar_filter.Get();
		const bool filter_players = filter > 0 && settings::current().enable && disabled_reason_.empty() && clients != nullptr
									&& std::popcount(clients[0]) == 1 && result && visibility_snapshot_fresh(result->captured, std::chrono::steady_clock::now());
		// Entries about a ghost player are dropped from every message, whoever it
		// is for: nobody but its viewer has it.
		const bool filter_ghosts = ghost_slots_.load(std::memory_order_relaxed) != 0;
		if (!filter_players && !filter_ghosts)
		{
			radar_stats_.skipped_messages.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const uint32_t recipient = filter_players ? static_cast<uint32_t>(std::countr_zero(clients[0])) : 0u;
		const radar_sight sight = filter >= 2 ? radar_sight::own : radar_sight::team;

		google::protobuf::Message* const message = const_cast<CNetMessage*>(data)->ToPB<google::protobuf::Message>();
		// Every message of this class shares one vtable, so the RTTI proof runs once.
		const void* vtable = nullptr;
		bool verified = runtime_compatibility::safe_read(message, &vtable, sizeof(vtable)) && vtable != nullptr
						&& vtable == radar_verified_vtable_.load(std::memory_order_relaxed);
		if (!verified && vtable != nullptr && radar_object_verified(data, message))
		{
			radar_verified_vtable_.store(vtable, std::memory_order_relaxed);
			verified = true;
		}
		const google::protobuf::Descriptor* descriptor = verified ? message->GetDescriptor() : nullptr;
		const google::protobuf::Reflection* reflection = descriptor == nullptr ? nullptr : message->GetReflection();
		const google::protobuf::FieldDescriptor* updates =
			reflection == nullptr || descriptor->name() != k_radar_message_name ? nullptr : descriptor->FindFieldByName("entity_updates");
		const google::protobuf::FieldDescriptor* index_field =
			updates == nullptr || !updates->is_repeated() || updates->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE
				? nullptr
				: updates->message_type()->FindFieldByName("entity_idx");
		if (index_field == nullptr || index_field->is_repeated() || index_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_INT32)
		{
			radar_filter_broken_.store(true);
			META_CONPRINTF("[CS2GLAZ] radar filter off: the spotted-entity message does not have the expected layout\n");
			return;
		}

		const int count = reflection->FieldSize(*message, updates);
		int kept = 0;
		for (int entry = 0; entry < count; ++entry)
		{
			const google::protobuf::Message& update = reflection->GetRepeatedMessage(*message, updates, entry);
			const int entity = update.GetReflection()->GetInt32(update, index_field);
			if (filter_ghosts && ghost_radar_entity(entity))
			{
				ghost_counters_.radar_entries++;
				continue;
			}
			const int target = filter_players ? radar_entry_target(*result, entity) : -1;
			if (filter_players && target < 0)
			{
				radar_stats_.unmatched_entries.fetch_add(1, std::memory_order_relaxed);
			}
			else if (filter_players && !radar_entry_allowed(*result, recipient, target, sight))
			{
				continue;
			}
			if (kept != entry)
			{
				reflection->SwapElements(message, updates, kept, entry);
			}
			++kept;
		}
		for (int removed = count; removed > kept; --removed)
		{
			reflection->RemoveLast(message, updates);
		}
		radar_stats_.filtered_messages.fetch_add(1, std::memory_order_relaxed);
		radar_stats_.kept_entries.fetch_add(static_cast<uint64_t>(kept), std::memory_order_relaxed);
		radar_stats_.dropped_entries.fetch_add(static_cast<uint64_t>(count - kept), std::memory_order_relaxed);
	}

	void plugin::print_radar_filter() const
	{
		const auto value = [](const std::atomic<uint64_t>& count) { return static_cast<unsigned long long>(count.load(std::memory_order_relaxed)); };
		META_CONPRINTF("[CS2GLAZ] radar filter enabled=%d hooked=%d recognised=%d broken=%d messages=%llu filtered=%llu skipped=%llu "
					   "entries kept=%llu dropped=%llu not_players=%llu\n",
					   cs2glaz_radar_filter.Get(), post_event_hooked_ ? 1 : 0, radar_message_.load() != nullptr ? 1 : 0,
					   radar_filter_broken_.load() ? 1 : 0, value(radar_stats_.messages), value(radar_stats_.filtered_messages),
					   value(radar_stats_.skipped_messages), value(radar_stats_.kept_entries), value(radar_stats_.dropped_entries),
					   value(radar_stats_.unmatched_entries));
	}

} // namespace cs2glaz
