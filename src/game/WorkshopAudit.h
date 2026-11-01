#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// Which of the pieces the session keeps as ours our world no longer has. The server keeps each built piece under its
// builder and their reference ID (WorkshopStore), and only a scrap from its builder takes it out; but a game reports no
// scrap of a piece it lacks. A builder who loads a save from before they built something (the host reloading an older
// save) would leave it standing in every friend's game, sent again to everyone at every join, with no game able to take
// it out. So after the welcome and each load the server sends us what it keeps as ours (kOwnWorkshopItems), and our
// game looks for each piece: what it lacks it reports gone (kReportWorkshopGone), and the server takes it out for
// everyone, as a scrap of ours would.
//
// "Not in our world" must mean gone, not just not loaded: a built piece that isn't persistent is in memory only while
// its cell is loaded (the engine frees it as the cell unloads and makes it again from the cell's record of changes when
// the cell loads again). The engine (WorkshopSync.cpp's, or a test's stand-in) says what it sees of each piece:
//
//   Seen Look(const Protocol::WorkshopItem&)
//
//   kStands     a reference of ours with that ID and that object: it is there (our own reports keep it as it is)
//   kGone       a reference with that ID that is something else (another object, deleted, a copy of a friend's piece):
//               ours isn't, for sure
//   kMissing    no reference with that ID in memory, while the cell the piece stands in is loaded here: gone, once
//               seen so again a look later (a cell just loaded may still be making its references)
//   kUnseen     no reference with that ID, and its cell isn't loaded (towns away, another worldspace, its workbench a
//               plugin's we lack, no world loaded at all): it may well be there; asked again later, never called gone
//
// The list is forgotten at a load (the server sends it again after it) and when the session ends. A piece of ours
// reported since (placed, moved or scrapped) is no longer looked for: what the server keeps of it is that report.
// Free of the game: tests/WorkshopAuditTests.cpp and the session tests drive it.
namespace WorkshopAudit
{
	using Clock = std::chrono::steady_clock;

	inline constexpr auto        LOOK_EVERY = std::chrono::seconds(2);   // a round of looks at most this often
	inline constexpr auto        CONFIRM = std::chrono::seconds(3);      // missing for this long (two looks or more) is gone
	inline constexpr std::size_t LOOKS_PER_ROUND = 256;
	inline constexpr std::size_t MAX_KEPT = 20000;                       // the server keeps no more built pieces than this

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
		return a_item.op == Protocol::WorkshopOp::kPlaced && a_item.refId != 0 && !Protocol::IsShareableRef(a_item.refId);
	}

	class Audit
	{
	public:
		// One packet of the server's list of what it keeps as ours: to be looked for.
		void Add(const std::vector<Protocol::WorkshopItem>& a_items)
		{
			for (const auto& item : a_items) {
				if (!Audited(item) || (pieces.size() >= MAX_KEPT && !pieces.contains(item.refId))) {
					continue;
				}
				pieces[item.refId] = { item, std::nullopt };
			}
		}

		// We just reported the piece placed, moved or scrapped: that report is what the server keeps of it.
		void Reported(std::uint32_t a_ref) { pieces.erase(a_ref); }

		// A round of looks (at most every LOOK_EVERY, LOOKS_PER_ROUND pieces, taking turns): what is gone, each as a scrap
		// of the piece the server keeps (its base: the server takes out only what still is that).
		template <class Engine>
		std::vector<Protocol::WorkshopItem> Look(Engine& a_engine, Clock::time_point a_now)
		{
			std::vector<Protocol::WorkshopItem> gone;
			if (pieces.empty() || a_now < nextRound) {
				return gone;
			}
			nextRound = a_now + LOOK_EVERY;
			auto it = pieces.lower_bound(cursor);
			for (std::size_t n = pieces.size() < LOOKS_PER_ROUND ? pieces.size() : LOOKS_PER_ROUND; n > 0 && !pieces.empty(); --n) {
				if (it == pieces.end()) {
					it = pieces.begin();
				}
				auto& [ref, piece] = *it;
				bool  isGone = false;
				switch (a_engine.Look(piece.item)) {
				case Seen::kStands:
					it = pieces.erase(it);
					++found;
					continue;
				case Seen::kGone:
					isGone = true;
					break;
				case Seen::kMissing:
					if (!piece.missingSince) {
						piece.missingSince = a_now;
					}
					isGone = a_now - *piece.missingSince >= CONFIRM;
					break;
				case Seen::kUnseen:
					piece.missingSince.reset();
					break;
				}
				if (isGone) {
					auto scrap = piece.item;
					scrap.op = Protocol::WorkshopOp::kScrapped;
					gone.push_back(scrap);
					it = pieces.erase(it);
					++lost;
					continue;
				}
				++it;
			}
			cursor = it == pieces.end() ? 0 : it->first;
			return gone;
		}

		// A load, or the session left: the server sends the list again after a load.
		void Clear()
		{
			pieces.clear();
			cursor = 0;
			nextRound = {};
		}

		[[nodiscard]] std::size_t   Waiting() const { return pieces.size(); }
		[[nodiscard]] std::uint32_t Found() const { return found; }
		[[nodiscard]] std::uint32_t Lost() const { return lost; }

	private:
		struct Piece
		{
			Protocol::WorkshopItem          item;
			std::optional<Clock::time_point> missingSince;  // seen missing since, with its cell loaded
		};

		std::map<std::uint32_t, Piece> pieces;  // by our reference ID
		std::uint32_t                  cursor = 0;  // where the next round starts
		Clock::time_point              nextRound{};
		std::uint32_t                  found = 0;  // ever: pieces looked for and found
		std::uint32_t                  lost = 0;   // ever: reported gone
	};
}
