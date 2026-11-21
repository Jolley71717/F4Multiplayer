#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

// The open state of a door built in a settlement, across games (WorkshopSync). A built door is a reference its builder's
// game made at runtime; every other game holds a copy of it as a workshop piece, under its own runtime ID. Neither is a
// plugin's reference, so DoorShare (WorldSync) never carried their open state, and a built door opened in one game stayed
// shut in the others (a Nexus report, 0.20.4: "door open/close states are not syncing"). Here each game keeps the session's
// last word on each built door, by the key every game knows it by (the builder, and the builder's reference ID):
// - a word heard from the session (kWorkshopStates) is applied to our door or our copy as soon as it is loaded;
// - a door of ours or a copy read otherwise than the word (our player or a script moved it) becomes the new word and is
//   reported (kReportWorkshopStates), so the builder's door and every copy follow whoever opens it;
// - a door of ours seen for the first time is reported as it stands (friends' copies are placed shut), a copy seen for the
//   first time is taken as it stands (the builder's word is on its way, or their build has none).
// Kept free of the game so the rules can be tested.
namespace WorkshopDoors
{
	using Key = std::pair<std::uint32_t, std::uint32_t>;  // (builder, their reference ID)

	enum class Do : std::uint8_t
	{
		kNothing,
		kApply,  // set the door as the word says (Open(key)), then Applied(key)
		kSend,   // report its state as the new word
	};

	class Book
	{
	public:
		// How many doors are kept: a word on one more is dropped (no session builds that many; a game that makes them up
		// can't fill ours).
		static constexpr std::size_t MAX_DOORS = 4096;

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
			if (word.open != a_open) {
				word.open = a_open;
				return Do::kSend;
			}
			return Do::kNothing;
		}

		// The word was set on the door.
		void Applied(Key a_key)
		{
			if (const auto it = words.find(a_key); it != words.end()) {
				it->second.pending = false;
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

		void Forget(Key a_key) { words.erase(a_key); }
		// A load, a join, the session's end: every door is seen afresh (ours reported again, theirs told again).
		void Clear() { words.clear(); }
		[[nodiscard]] std::size_t Size() const { return words.size(); }

	private:
		struct Word
		{
			bool open = false;
			bool pending = false;  // heard, not yet set on our door or copy
		};
		std::map<Key, Word> words;
	};
}
