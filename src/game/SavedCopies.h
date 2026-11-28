#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// The copies of friends' pieces a save holds, and what a load of it does with them.
//
// A copy of a friend's piece is a reference our game made at run time (WorkshopSync's Create): their bed, their generator, the
// host's whole settlement at a settlement the host owns. A save made in a session writes every copy our game keeps then, as it
// writes any reference made at run time, and so does a save made after a session whose copies were waiting for their cell to
// load to be deleted (the graveyard, which a load forgets). Loaded alone, such a save kept them for good: out of workshop mode's
// reach (the block keyword, saved with each), linked to no workbench, and no session to remove them. A copy of the host's
// Sim Settlements 2 city planner's desk stood in the friend's own game as a desk nobody could use or scrap. The maintainer's
// rule is that a friend's save keeps nothing of the host's settlement.
//
// So each save's co-save record names the copies that save may hold (Encode): those our game keeps, those waiting to be
// deleted, and those an earlier load named that weren't found yet. After a load (Book::Load) each is looked for as its cell
// loads (Book::Look), and, out of a session, removed the way a session's end removes a copy. In a session one is left for the
// session's own report to find again (WorkshopApply's FindExisting); once the session keeps it as its copy, its own end
// removes it. An entry goes when its copy is removed, when the session keeps it, or when the id names something that is no
// copy of ours (another base, no block keyword, a piece linked to a workbench: the id was handed out again). One whose cell
// never loads is carried into each later save's record, up to MAX. Pure, so it can be tested without the game.
namespace SavedCopies
{
	constexpr std::size_t   MAX = 16384;  // entries a record holds (a large settlement shared whole is a few thousand pieces)
	constexpr std::uint16_t RECORD_VERSION = 1;

	struct Entry
	{
		std::uint32_t id = 0;    // our copy's form id (made at run time: 0xFF......)
		std::uint32_t base = 0;  // the base it is known by (WorkshopApply::ReportedBase)

		friend bool operator==(const Entry&, const Entry&) = default;
	};

	constexpr bool RuntimeId(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF && (a_id & 0x00FFFFFF) != 0;
	}

	// What the game holds under an entry's id now.
	enum class Found : std::uint8_t
	{
		kAbsent,  // nothing (its cell isn't loaded): looked for again later
		kGone,    // deleted already
		kKept,    // a copy this run keeps, or waits to delete: the session's own bookkeeping removes it
		kOther,   // something that is no copy of ours: another base, no block keyword, a piece linked to a workbench, a drop
		kOurs,    // the copy the save held: a reference made at run time, of that base, blocked, linked to nothing
	};

	// How long a session's building must have been quiet (nothing came from the server, nothing waits in line) before the copies a
	// save held that no report claimed are known to be the session's no longer: pieces its host scrapped or moved since that save
	// (a friend who loads an older save in a session, or joins with one while the host's world went on). Before it, a report may
	// still be on its way to claim one at its spot (FindExisting); after it, a copy still unclaimed stands for a piece that is
	// gone, the host's scrapped lamp or the old spot of a moved container, floating in the friend's game until their session
	// ends, and goes as it does out of a session.
	constexpr auto CLAIM_QUIET = std::chrono::seconds(10);

	// Whether the session may still claim the copies a save held: a_inLine reports wait to be applied, or the building last
	// came (the session began, the world loaded, a report came in) less than CLAIM_QUIET ago.
	constexpr bool SessionMayClaim(std::size_t a_inLine, std::chrono::steady_clock::duration a_quiet)
	{
		return a_inLine > 0 || a_quiet < CLAIM_QUIET;
	}

	// The bytes of a record: the version, the count, then each entry's id and base; nothing when there is none.
	inline std::vector<std::uint8_t> Encode(std::span<const Entry> a_entries)
	{
		std::vector<Entry> entries;
		entries.reserve((std::min)(a_entries.size(), MAX));
		for (const auto& entry : a_entries) {
			if (RuntimeId(entry.id) && entry.base != 0) {
				entries.push_back(entry);
			}
		}
		std::ranges::sort(entries, {}, &Entry::id);
		const auto repeats = std::ranges::unique(entries, {}, &Entry::id);
		entries.erase(repeats.begin(), repeats.end());
		if (entries.size() > MAX) {
			entries.resize(MAX);
		}
		std::vector<std::uint8_t> out;
		if (entries.empty()) {
			return out;
		}
		const auto put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		put(RECORD_VERSION);
		put(static_cast<std::uint32_t>(entries.size()));
		for (const auto& entry : entries) {
			put(entry.id);
			put(entry.base);
		}
		return out;
	}

	// A save's record (not yet resolved for the load order); nothing if it can't be read (another version, a count past MAX,
	// a length that is not the count's, an id not made at run time, no base).
	inline std::optional<std::vector<Entry>> Decode(std::span<const std::uint8_t> a_bytes)
	{
		std::uint16_t version = 0;
		std::uint32_t count = 0;
		constexpr auto HEAD = sizeof(version) + sizeof(count);
		if (a_bytes.size() < HEAD) {
			return std::nullopt;
		}
		std::memcpy(&version, a_bytes.data(), sizeof(version));
		std::memcpy(&count, a_bytes.data() + sizeof(version), sizeof(count));
		if (version != RECORD_VERSION || count == 0 || count > MAX || a_bytes.size() != HEAD + std::size_t{ count } * 2 * sizeof(std::uint32_t)) {
			return std::nullopt;
		}
		std::vector<Entry> entries(count);
		for (std::size_t i = 0; i < count; ++i) {
			const auto at = a_bytes.data() + HEAD + i * 2 * sizeof(std::uint32_t);
			std::memcpy(&entries[i].id, at, sizeof(std::uint32_t));
			std::memcpy(&entries[i].base, at + sizeof(std::uint32_t), sizeof(std::uint32_t));
			if (!RuntimeId(entries[i].id) || entries[i].base == 0) {
				return std::nullopt;
			}
		}
		return entries;
	}

	// The copies a loaded save named that haven't been settled yet.
	//
	// The engine Look asks:
	//   Found Find(const Entry&)                   what the game holds under the id now
	//   bool  Ready(const Entry&, Duration seen)   whether that reference may be deleted now (its 3D in, or seen long enough:
	//                                              DeferredDelete::Safe), `seen` how long ago it was first found
	//   void  Remove(const Entry&)                 removes it as a session's end removes a copy
	class Book
	{
	public:
		using Clock = std::chrono::steady_clock;

		// After a load: the save's record (resolved), in place of anything before. A new game passes nothing.
		void Load(std::vector<Entry> a_entries)
		{
			entries.clear();
			for (const auto& entry : a_entries) {
				if (entries.size() >= MAX) {
					break;
				}
				if (RuntimeId(entry.id) && entry.base != 0 && std::ranges::find(entries, entry.id, &Waiting::id) == entries.end()) {
					entries.push_back({ entry.id, entry.base, {} });
				}
			}
			loaded += static_cast<std::uint32_t>(entries.size());
		}

		void Clear() { entries.clear(); }

		// Each entry looked at once. a_session: our game is in a session, whose reports find the copies the save held at their
		// spots (FindExisting): none is removed then. Returns how many were removed.
		template <class Engine>
		std::size_t Look(Engine& a_engine, bool a_session, Clock::time_point a_now)
		{
			std::size_t gone = 0;
			std::erase_if(entries, [&](Waiting& a_entry) {
				const Entry entry{ a_entry.id, a_entry.base };
				switch (a_engine.Find(entry)) {
				case Found::kAbsent:
					a_entry.seen.reset();  // its cell unloaded again: waited for afresh when it loads
					return false;
				case Found::kGone:
					++alreadyGone;
					return true;
				case Found::kKept:
					++keptBySession;
					return true;
				case Found::kOther:
					++notOurs;
					return true;
				case Found::kOurs:
					break;
				}
				if (!a_entry.seen) {
					a_entry.seen = a_now;
				}
				if (a_session || !a_engine.Ready(entry, a_now - *a_entry.seen)) {
					return false;
				}
				a_engine.Remove(entry);
				++removed;
				++gone;
				return true;
			});
			return gone;
		}

		// What a save made now should name of these (those not settled yet), beside the copies our game keeps.
		std::vector<Entry> Unsettled() const
		{
			std::vector<Entry> out;
			out.reserve(entries.size());
			for (const auto& entry : entries) {
				out.push_back({ entry.id, entry.base });
			}
			return out;
		}

		std::size_t   Size() const { return entries.size(); }
		bool          Empty() const { return entries.empty(); }
		std::uint32_t Loaded() const { return loaded; }
		std::uint32_t Removed() const { return removed; }
		std::uint32_t KeptBySession() const { return keptBySession; }
		std::uint32_t NotOurs() const { return notOurs; }
		std::uint32_t AlreadyGone() const { return alreadyGone; }

	private:
		struct Waiting
		{
			std::uint32_t                    id = 0;
			std::uint32_t                    base = 0;
			std::optional<Clock::time_point> seen;  // when it was first found loaded (this time its cell is loaded)
		};

		std::vector<Waiting> entries;
		std::uint32_t        loaded = 0, removed = 0, keptBySession = 0, notOurs = 0, alreadyGone = 0;
	};
}
