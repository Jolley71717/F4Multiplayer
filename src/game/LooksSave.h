#pragma once

#include "game/BodyMorphs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
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
}
