#pragma once

#include "Protocol.h"
#include "game/BuiltHolders.h"
#include "game/LootSync.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <optional>
#include <span>
#include <vector>

// WorldSync's co-save record of the session's loot ('CONT'): the session the save's world is in, the first container change
// it lacks, the changes waiting to go into it, and (version 2) what it has of the containers others built. Kept free of the
// game so the tests read and write the bytes a save holds; WorldSync hands them to F4SE as they are.
//
// Version 1 (every build before built containers were shared): the session (8 bytes), the first change lacked, the number
// of changes waiting, then each one's index, container, item and count (4 bytes each). Version 2: each waiting change's
// builder after its count (Protocol::ContainerChange::builder), then the book's length and the book
// (BuiltHolders::Book::Record). A record is written in version 1 while nothing in it needs 2: a build from before reads
// only a 1, and with none it would ask for the whole session again, from its first change, and put every change at a
// plugin's chest into the world a second time.
namespace ResyncRecord
{
	inline constexpr std::uint32_t VERSION_1 = 1;
	inline constexpr std::uint32_t VERSION_2 = 2;  // built holders: each waiting change's builder, and the book

	struct Record
	{
		std::uint64_t                  session = 0;
		std::uint32_t                  next = 0;
		std::vector<LootSync::Pending> waiting;
		BuiltHolders::Book             book;
		bool                           bookRead = true;  // false: version 2's book didn't read (the book is unsure)
	};

	// The oldest version that holds it: 2 only for a waiting change at a built holder or a book with anything in it.
	[[nodiscard]] inline std::uint32_t VersionFor(const std::vector<LootSync::Pending>& a_waiting, const BuiltHolders::Book& a_book)
	{
		const bool built = !a_book.Empty() || std::ranges::any_of(a_waiting, [](const LootSync::Pending& a_pending) { return Protocol::IsBuiltHolder(a_pending.change); });
		return built ? VERSION_2 : VERSION_1;
	}

	[[nodiscard]] inline std::vector<std::uint8_t> Encode(const Protocol::WorldRequest& a_point, const std::vector<LootSync::Pending>& a_waiting, const BuiltHolders::Book& a_book,
		std::uint32_t a_version)
	{
		std::vector<std::uint8_t> out;
		const auto                put = [&](const auto& a_value) {
            const auto at = out.size();
            out.resize(at + sizeof(a_value));
            std::memcpy(out.data() + at, &a_value, sizeof(a_value));
		};
		put(a_point.sessionId);
		put(a_point.containerFrom);
		put(static_cast<std::uint32_t>(a_waiting.size()));
		for (const auto& entry : a_waiting) {
			put(entry.index);
			put(entry.change.container);
			put(entry.change.item);
			put(entry.change.count);
			if (a_version >= VERSION_2) {
				put(entry.change.builder);
			}
		}
		if (a_version >= VERSION_2) {
			const auto book = a_book.Record();
			put(static_cast<std::uint32_t>(book.size()));
			for (const auto value : book) {
				put(value);
			}
		}
		return out;
	}

	// None for a record that doesn't read (an unknown version, cut short).
	[[nodiscard]] inline std::optional<Record> Decode(std::uint32_t a_version, std::span<const std::uint8_t> a_bytes)
	{
		if (a_version != VERSION_1 && a_version != VERSION_2) {
			return std::nullopt;
		}
		std::size_t at = 0;
		const auto  get = [&](auto& a_value) {
            if (a_bytes.size() - at < sizeof(a_value)) {
                return false;
            }
            std::memcpy(&a_value, a_bytes.data() + at, sizeof(a_value));
            at += sizeof(a_value);
            return true;
		};
		Record        record;
		std::uint32_t count = 0;
		const bool    built = a_version >= VERSION_2;
		const auto    entry = (built ? 5u : 4u) * sizeof(std::uint32_t);
		if (!get(record.session) || !get(record.next) || !get(count) || count > (a_bytes.size() - at) / entry) {
			return std::nullopt;
		}
		record.waiting.reserve(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			LootSync::Pending pending{};
			get(pending.index);
			get(pending.change.container);
			get(pending.change.item);
			get(pending.change.count);
			if (built) {
				get(pending.change.builder);
			}
			record.waiting.push_back(pending);
		}
		if (built) {
			std::uint32_t              length = 0;
			std::vector<std::uint32_t> book;
			if (get(length) && length <= (a_bytes.size() - at) / sizeof(std::uint32_t)) {
				book.resize(length);
				for (auto& value : book) {
					get(value);
				}
			}
			record.bookRead = record.book.Load(book);  // (one that doesn't read leaves the book unsure)
		}
		return record;
	}
}
