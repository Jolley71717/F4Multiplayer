#pragma once

#include "game/BodyMorphs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// The co-save's record of the form IDs LooksMenu may still hold a friend's sliders and overlays under as a save is written
// (BodyMorphs::StandIns::Saved): read back with the save, and cleared on whatever reference has one of them then. Kept
// free of the game so it can be tested.
namespace LooksSave
{
	// Only runtime references (stand-ins are made at runtime) have their IDs handed on to the next one the game makes.
	constexpr bool Runtime(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF;
	}

	// The record's bytes: the count, then each runtime ID; at most BodyMorphs::MAX_SAVED. Empty when there are none to
	// write.
	inline std::vector<std::uint8_t> Encode(const std::vector<std::uint32_t>& a_ids)
	{
		std::vector<std::uint32_t> ids;
		for (const auto id : a_ids) {
			if (Runtime(id) && ids.size() < BodyMorphs::MAX_SAVED) {
				ids.push_back(id);
			}
		}
		std::vector<std::uint8_t> out;
		if (ids.empty()) {
			return out;
		}
		const auto put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		put(static_cast<std::uint16_t>(ids.size()));
		for (const auto id : ids) {
			put(id);
		}
		return out;
	}

	// A save's record, sorted, each ID once; nothing if it can't be read (a count past MAX_SAVED, a length that is not the
	// count's, an ID that is not a runtime one).
	inline std::optional<std::vector<std::uint32_t>> Decode(std::span<const std::uint8_t> a_bytes)
	{
		std::uint16_t count = 0;
		if (a_bytes.size() < sizeof(count)) {
			return std::nullopt;
		}
		std::memcpy(&count, a_bytes.data(), sizeof(count));
		if (count > BodyMorphs::MAX_SAVED || a_bytes.size() != sizeof(count) + std::size_t{ count } * sizeof(std::uint32_t)) {
			return std::nullopt;
		}
		std::vector<std::uint32_t> ids(count);
		for (std::size_t i = 0; i < count; ++i) {
			std::memcpy(&ids[i], a_bytes.data() + sizeof(count) + i * sizeof(std::uint32_t), sizeof(std::uint32_t));
			if (!Runtime(ids[i])) {
				return std::nullopt;
			}
		}
		std::ranges::sort(ids);
		const auto repeats = std::ranges::unique(ids);
		ids.erase(repeats.begin(), repeats.end());
		return ids;
	}

	// A load's record, between the load's thread and the main thread: the co-save is reverted and read on the load's
	// thread while frames may still run (a load that outlasts the hold), so it is kept here and listed only once the game
	// says it loaded (Take), after what answers for the world before the load is dropped.
	class Loaded
	{
	public:
		// The co-save is reverted (before a load, or for a new game): no record, until one is read.
		void Revert()
		{
			std::scoped_lock l{ lock };
			ids.clear();
		}

		// The save's record: how many IDs it lists, or nothing if it can't be read (when there is none).
		std::optional<std::size_t> Read(std::span<const std::uint8_t> a_bytes)
		{
			auto read = Decode(a_bytes);
			const auto count = read ? std::optional{ read->size() } : std::nullopt;
			std::scoped_lock l{ lock };
			ids = read ? std::move(*read) : std::vector<std::uint32_t>{};
			return count;
		}

		// What the loaded save's record listed, once.
		std::vector<std::uint32_t> Take()
		{
			std::scoped_lock l{ lock };
			return std::exchange(ids, {});
		}

	private:
		std::mutex                 lock;
		std::vector<std::uint32_t> ids;
	};
}
