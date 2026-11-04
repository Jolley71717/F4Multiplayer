#pragma once

#include "Protocol.h"
#include "game/WorkshopCells.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// Which of the pieces the session keeps as ours our world no longer has. The server keeps each built piece under its
// builder and their reference ID (WorkshopStore), and only a scrap from its builder takes it out; but a game reports no
// scrap of a piece it lacks. A builder who loads a save from before they built something (the host reloading an older
// save) would leave it standing in every friend's game, sent again to everyone at every join, with no game able to take
// it out. So after the welcome and each load, once our record says whose world we play, the server sends us what it
// keeps as built in this world (kOwnWorkshopItems), and our game looks for each piece: what it lacks it reports gone
// (kReportWorkshopGone), and the server takes it out for everyone, as a scrap of ours would. Taking a piece out is
// undone as easily: the server keeps it as lost and sends it again (kLost), and a lost piece our world has (a later save
// loaded, a cell we hadn't seen loaded after all) we report placed again, and it is back for everyone.
//
// "Not in our world" must mean gone, not just not loaded: a built piece that isn't persistent is in memory only while
// its cell is loaded (the engine frees it as the cell unloads and makes it again from the cell's record of changes when
// the cell loads again), and that cell is the one it was built in: one moved since keeps it (docs/DESIGN.md, "Moved
// references keep their cell"). The engine (WorkshopSync.cpp's, or a test's stand-in) says what it sees of each piece:
//
//   Seen Look(const Protocol::WorkshopItem&)
//
//   kStands     a reference of ours with that ID and that object: it is there
//   kGone       a reference with that ID that is something else (another object, deleted, a copy of a friend's piece):
//               ours isn't, once a second look says so too
//   kMissing    no reference with that ID in memory, while every cell around its spot (CellAges::SettledAround: and for a
//               wire, around each end) has been attached for CELL_SETTLE and we stand in its workbench's build area: gone,
//               once seen so CONFIRM_LOOKS times over CONFIRM
//   kUnseen     no reference with that ID, and not all of that holds (towns away, another worldspace, its workbench a
//               plugin's we lack, cells just loaded): it may well be there; asked again later, never called gone
//
// The engine sees only a settled world: the caller looks only while the player is in a cell with no loading screen, no
// menu stopping the game, no opening, and not as the session ends, and the time it passes (a_now) is that time alone
// (frames while the game runs), so a long menu or loading screen never counts toward CONFIRM.
//
// The list is forgotten at a load (the server sends it again after it) and when the session ends. A piece of ours
// reported since (placed, moved or scrapped) is no longer looked for: what the server keeps of it is that report.
// Free of the game: tests/WorkshopAuditTests.cpp and the session tests drive it.
namespace WorkshopAudit
{
	// The time the audit runs on: frames while the world is settled (see above), not the wall clock.
	using Duration = std::chrono::steady_clock::duration;

	inline constexpr Duration    LOOK_EVERY = std::chrono::seconds(2);    // a round of looks at most this often
	inline constexpr Duration    CONFIRM = std::chrono::seconds(10);      // missing over this long ...
	inline constexpr int         CONFIRM_LOOKS = 3;                       // ... and this many looks is gone
	inline constexpr int         GONE_LOOKS = 2;                          // something else at its ID this many looks running is gone
	inline constexpr Duration    CELL_SETTLE = std::chrono::seconds(10);  // a cell attached this long has made its references
	inline constexpr std::size_t LOOKS_PER_ROUND = 256;
	inline constexpr std::size_t MAX_KEPT = 20000 + 2048;                 // the server keeps no more built and lost pieces than this

	enum class Seen
	{
		kStands,
		kGone,
		kMissing,
		kUnseen,
	};

	// Only what a player built: a reference their game made (a piece of the world they moved is kept as built too, but
	// every game has it).
	[[nodiscard]] constexpr bool Audited(const Protocol::WorkshopItem& a_item)
	{
		return (a_item.op == Protocol::WorkshopOp::kPlaced || a_item.op == Protocol::WorkshopOp::kLost) && a_item.refId != 0 && !Protocol::IsShareableRef(a_item.refId);
	}

	// The exterior cells a piece reaches, as grid coordinates: its spot's, and a wire's ends' (its half extents from its
	// middle, either way), so a wire strung between pylons in two cells waits for both.
	[[nodiscard]] inline std::vector<std::pair<std::int32_t, std::int32_t>> Spots(const Protocol::WorkshopItem& a_item)
	{
		std::vector<std::pair<std::int32_t, std::int32_t>> out;
		const auto add = [&](float a_x, float a_y) {
			const std::pair spot{ WorkshopCells::Grid(a_x), WorkshopCells::Grid(a_y) };
			if (std::ranges::find(out, spot) == out.end()) {
				out.push_back(spot);
			}
		};
		add(a_item.position[0], a_item.position[1]);
		if (a_item.wire) {
			const float dx = std::abs(a_item.wire->halfExtents[0]);
			const float dy = std::abs(a_item.wire->halfExtents[1]);
			for (const float sx : { -1.0f, 1.0f }) {
				for (const float sy : { -1.0f, 1.0f }) {
					add(a_item.position[0] + sx * dx, a_item.position[1] + sy * dy);
				}
			}
		}
		return out;
	}

	// How long each attached cell has been attached, in the audit's time: a cell just attached may still be making its
	// references (and a piece's own cell, where it was built, may be the next one over).
	class CellAges
	{
	public:
		// The cells attached now (all of them, at each look at the cells): one attached since the last is new, one not
		// in the list is no longer attached (it starts afresh if it attaches again).
		void Update(const std::vector<WorkshopCells::Where>& a_attached, Duration a_now)
		{
			std::map<WorkshopCells::Where, Duration> now;
			for (const auto& where : a_attached) {
				const auto it = since.find(where);
				now[where] = it != since.end() ? it->second : a_now;
			}
			since = std::move(now);
		}

		// Attached for CELL_SETTLE or longer.
		[[nodiscard]] bool Settled(const WorkshopCells::Where& a_where, Duration a_now) const
		{
			const auto it = since.find(a_where);
			return it != since.end() && a_now - it->second >= CELL_SETTLE;
		}

		// The exterior cell at (a_x, a_y) of a_space and the eight around it are all Settled.
		[[nodiscard]] bool SettledAround(std::uint32_t a_space, std::int32_t a_x, std::int32_t a_y, Duration a_now) const
		{
			for (std::int32_t dx = -1; dx <= 1; ++dx) {
				for (std::int32_t dy = -1; dy <= 1; ++dy) {
					if (!Settled({ a_space, a_x + dx, a_y + dy }, a_now)) {
						return false;
					}
				}
			}
			return true;
		}

		void Clear() { since.clear(); }

		[[nodiscard]] std::size_t Size() const { return since.size(); }

	private:
		std::map<WorkshopCells::Where, Duration> since;
	};

	// What a round of looks found.
	struct Round
	{
		std::vector<Protocol::WorkshopItem> gone;   // to report gone: each as a scrap of the piece the server keeps (its base)
		std::vector<Protocol::WorkshopItem> found;  // lost pieces our world has: to report placed again, as they stand now
	};

	class Audit
	{
	public:
		// One packet of the server's list of what it keeps as ours in this world: to be looked for (kLost: to be found).
		void Add(const std::vector<Protocol::WorkshopItem>& a_items)
		{
			for (const auto& item : a_items) {
				if (!Audited(item) || (pieces.size() >= MAX_KEPT && !pieces.contains(item.refId))) {
					continue;
				}
				Piece piece;
				piece.item = item;
				piece.lost = item.op == Protocol::WorkshopOp::kLost;
				piece.item.op = Protocol::WorkshopOp::kPlaced;
				pieces[item.refId] = piece;
			}
		}

		// We just reported the piece placed, moved or scrapped: that report is what the server keeps of it.
		void Reported(std::uint32_t a_ref) { pieces.erase(a_ref); }

		// A round of looks (at most every LOOK_EVERY, LOOKS_PER_ROUND pieces, taking turns), a_now in the audit's time.
		// What is gone is kept, as lost, to be found again.
		template <class Engine>
		Round Look(Engine& a_engine, Duration a_now)
		{
			Round out;
			if (pieces.empty() || (started && a_now < nextRound)) {
				return out;
			}
			started = true;
			nextRound = a_now + LOOK_EVERY;
			auto it = pieces.lower_bound(cursor);
			for (std::size_t n = pieces.size() < LOOKS_PER_ROUND ? pieces.size() : LOOKS_PER_ROUND; n > 0 && !pieces.empty(); --n) {
				if (it == pieces.end()) {
					it = pieces.begin();
				}
				auto&      piece = it->second;
				const auto seen = a_engine.Look(piece.item);
				if (piece.lost) {
					// Only standing matters: anything else, it stays lost.
					if (seen == Seen::kStands) {
						out.found.push_back(piece.item);
						++restored;
						it = pieces.erase(it);
						continue;
					}
					++it;
					continue;
				}
				switch (seen) {
				case Seen::kStands:
					it = pieces.erase(it);
					++found;
					continue;
				case Seen::kGone:
					++piece.goneLooks;
					piece.Absent(a_now);
					break;
				case Seen::kMissing:
					piece.goneLooks = 0;
					piece.Absent(a_now);
					break;
				case Seen::kUnseen:
					piece.goneLooks = 0;
					piece.absentLooks = 0;
					piece.absentSince.reset();
					break;
				}
				if (piece.goneLooks >= GONE_LOOKS || (piece.absentLooks >= CONFIRM_LOOKS && a_now - *piece.absentSince >= CONFIRM)) {
					auto scrap = piece.item;
					scrap.op = Protocol::WorkshopOp::kScrapped;
					out.gone.push_back(scrap);
					++lost;
					piece.lost = true;  // the server keeps it as lost: if it turns up, it comes back
					piece.goneLooks = 0;
					piece.absentLooks = 0;
					piece.absentSince.reset();
				}
				++it;
			}
			cursor = it == pieces.end() ? 0 : it->first;
			return out;
		}

		// A load, or the session left: the server sends the list again after a load.
		void Clear()
		{
			pieces.clear();
			cursor = 0;
			started = false;
			nextRound = {};
		}

		[[nodiscard]] std::size_t Waiting() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(pieces, [](const auto& a_entry) { return !a_entry.second.lost; }));
		}
		[[nodiscard]] std::size_t   LostWaiting() const { return pieces.size() - Waiting(); }
		[[nodiscard]] std::uint32_t Found() const { return found; }
		[[nodiscard]] std::uint32_t Lost() const { return lost; }
		[[nodiscard]] std::uint32_t Restored() const { return restored; }

	private:
		struct Piece
		{
			Protocol::WorkshopItem  item;
			bool                    lost = false;  // the server keeps it as lost: looked for only to be found
			int                     goneLooks = 0;    // looks running that saw something else at its ID
			int                     absentLooks = 0;  // looks running that saw it gone or missing
			std::optional<Duration> absentSince;      // since the first of those

			void Absent(Duration a_now)
			{
				++absentLooks;
				if (!absentSince) {
					absentSince = a_now;
				}
			}
		};

		std::map<std::uint32_t, Piece> pieces;  // by our reference ID
		std::uint32_t                  cursor = 0;  // where the next round starts
		bool                           started = false;
		Duration                       nextRound{};
		std::uint32_t                  found = 0;     // ever: pieces looked for and found
		std::uint32_t                  lost = 0;      // ever: reported gone
		std::uint32_t                  restored = 0;  // ever: lost pieces found standing, reported placed again
	};
}
