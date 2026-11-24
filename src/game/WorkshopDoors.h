#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

// The open state of a door built in a settlement, across games (WorkshopSync). A built door is a reference its builder's
// game made at runtime; every other game holds a copy of it as a workshop piece, under its own runtime ID. Neither is a
// plugin's reference, so DoorShare (WorldSync) never carried their open state, and a built door opened in one game stayed
// shut in the others (a Nexus report, 0.20.4: "door open/close states are not syncing"). Here each game keeps the session's
// last word on each built door, by the key every game knows it by (the builder, and the builder's reference ID):
// - a word heard from the session (kWorkshopStates) is applied to our door or our copy as soon as it is loaded;
// - a door of ours or a copy read otherwise than the word (our player or a script moved it) becomes the new word and is
//   reported (kReportWorkshopStates), so the builder's door and every copy follow whoever opens it;
// - a door of ours seen for the first time is reported as it stands (friends' copies are placed shut), a copy seen for the
//   first time is taken as it stands (the builder's word is on its way, or their build has none);
// - a word just set on a door is set again while the door has not taken it yet (a few looks), rather than its old state
//   being sent back as a new word, which would undo the change in every game.
// Kept free of the game so the rules can be tested.
namespace WorkshopDoors
{
	using Key = std::pair<std::uint32_t, std::uint32_t>;  // (builder, their reference ID)

	// How many doors are kept: a word on one more is dropped (no session builds that many; a game that makes them up can't
	// fill ours), and no more of our own are watched.
	inline constexpr std::size_t MAX_DOORS = 4096;

	// How many looks a word just set on a door is set again while the door still reads otherwise (DOOR_POLL apart: about
	// two seconds), before a difference counts as a player's change again.
	inline constexpr std::uint8_t SETTLE_LOOKS = 4;

	enum class Do : std::uint8_t
	{
		kNothing,
		kApply,  // set the door as the word says (Open(key)), then Applied(key)
		kSend,   // report its state as the new word
	};

	class Book
	{
	public:
		static constexpr std::size_t MAX_DOORS = WorkshopDoors::MAX_DOORS;

		// The session's word on a door (a report from the builder's or a friend's game): applied at the next look.
		void Heard(Key a_key, bool a_open)
		{
			if (const auto it = words.find(a_key); it != words.end()) {
				it->second = { a_open, true };
			} else if (words.size() < MAX_DOORS) {
				words.emplace(a_key, Word{ a_open, true });
			}
		}

		// A loaded door of ours (a_own) or a copy, read a_open now: what to do.
		Do Look(Key a_key, bool a_open, bool a_own)
		{
			const auto it = words.find(a_key);
			if (it == words.end()) {
				words[a_key] = { a_open, false };
				return a_own ? Do::kSend : Do::kNothing;
			}
			auto& word = it->second;
			if (word.pending) {
				if (word.open == a_open) {
					word.pending = false;
					return Do::kNothing;
				}
				return Do::kApply;
			}
			if (word.open == a_open) {
				word.settling = 0;  // the door has it
				return Do::kNothing;
			}
			if (word.settling > 0) {
				--word.settling;  // set a moment ago and not taken yet: set again, not sent back
				return Do::kApply;
			}
			word.open = a_open;
			return Do::kSend;
		}

		// The word was set on the door: looked at again for a few looks (SETTLE_LOOKS) in case the door hasn't taken it.
		void Applied(Key a_key)
		{
			if (const auto it = words.find(a_key); it != words.end() && it->second.pending) {
				it->second.pending = false;
				it->second.settling = SETTLE_LOOKS;
			}
		}

		[[nodiscard]] bool Open(Key a_key) const
		{
			const auto it = words.find(a_key);
			return it != words.end() && it->second.open;
		}

		[[nodiscard]] bool Pending(Key a_key) const
		{
			const auto it = words.find(a_key);
			return it != words.end() && it->second.pending;
		}

		// The word on each of our own doors (a_local: our player's id), loaded or not, for a player who just joined: their
		// copies are placed shut, and the server keeps no door's state. By their reference IDs here, open or shut.
		[[nodiscard]] std::vector<std::pair<std::uint32_t, bool>> OwnWords(std::uint32_t a_local) const
		{
			std::vector<std::pair<std::uint32_t, bool>> own;
			for (const auto& [key, word] : words) {
				if (a_local != 0 && key.first == a_local) {
					own.emplace_back(key.second, word.open);
				}
			}
			return own;
		}

		void Forget(Key a_key) { words.erase(a_key); }
		// A load, a join, the session's end: every door is seen afresh (ours reported again, theirs told again).
		void Clear() { words.clear(); }
		[[nodiscard]] std::size_t Size() const { return words.size(); }

	private:
		struct Word
		{
			bool         open = false;
			bool         pending = false;  // heard, not yet set on our door or copy
			std::uint8_t settling = 0;     // looks left in which a door that reads otherwise is set again (Applied)
		};
		std::map<Key, Word> words;
	};

	// Our own built doors to look at, by their IDs here. Found as each cell attaches (its references walked), as each piece
	// is placed (the workshop's placed event, workshop mode's and the host's build for a friend alike), and from each
	// workbench's list of its pieces. Not from the workbench alone: one has its workshop data only once workshop mode has been
	// used there in this save, and its list may not hold every piece, so a door built before the session at a workbench not
	// opened since was never looked at, neither applying a friend's word nor sending its own (rig, 2026-11-22). An ID that
	// names no reference here any more (its cell unloaded, the door scrapped) is dropped at its next look, and found again
	// when its cell attaches again.
	class Found
	{
	public:
		static constexpr std::size_t MAX = MAX_DOORS;

		// Made at runtime: a door a player built. A plugin's door is DoorShare's, and never one of these.
		static constexpr bool Runtime(std::uint32_t a_id) { return (a_id >> 24) == 0xFF; }

		// A door found: true if it is new to the list.
		bool Note(std::uint32_t a_id)
		{
			if (!Runtime(a_id) || ids.size() >= MAX) {
				return false;
			}
			return ids.insert(a_id).second;
		}

		// Each door, looked at by a_look(id); one it says false of (gone here) is dropped.
		template <class F>
		void Each(F&& a_look)
		{
			for (auto it = ids.begin(); it != ids.end();) {
				if (a_look(*it)) {
					++it;
				} else {
					it = ids.erase(it);
				}
			}
		}

		[[nodiscard]] bool        Has(std::uint32_t a_id) const { return ids.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return ids.size(); }
		// A load, the session's end: the cells are walked again.
		void Clear() { ids.clear(); }

	private:
		std::set<std::uint32_t> ids;
	};
}
