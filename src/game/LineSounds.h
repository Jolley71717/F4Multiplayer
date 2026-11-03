#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The sounds of friends' lines (src/game/Conversations.cpp): each one a recording played as a plain 3D sound from
// its voice file. The game ends an NPC's own line by its voice timer, set from the recording's length; a sound we
// play has no such timer, and one that never ends by itself (a voice file the audio manager decodes as an endless
// buzz, or loops) sounded on forever, every new speaker's line adding another one on top. So each sound has a hard
// end, a little after its recording's length or a ceiling when that is unknown, and only a few sound at once.
// Pure bookkeeping, so it can be tested without the game; Handle is RE::BSSoundHandle in the game, the key is the
// friend's player ID and the speaker (a speaker says one line at a time).
template <class Handle>
class LineSounds
{
public:
	using Clock = std::chrono::steady_clock;
	using Key = std::uint64_t;

	static constexpr auto TAIL = std::chrono::milliseconds(1000);      // past the recording's own length
	static constexpr auto SHORTEST = std::chrono::milliseconds(2500);  // a recording shorter than this still gets it
	static constexpr auto LONGEST = std::chrono::seconds(30);          // a recording that claims more is not believed
	static constexpr auto UNKNOWN = std::chrono::seconds(15);          // a recording whose length we could not read
	static constexpr std::size_t MAX_PLAYING = 4;                      // more at once, and the oldest is ended

	// Why a sound is ended.
	enum class Why : std::uint8_t
	{
		kNext,     // its speaker says a new line (not a cap)
		kTooLong,  // it ran to its hard end
		kTooMany,  // MAX_PLAYING were sounding and a new line came
	};

	struct Entry
	{
		Key                   key = 0;
		Handle                handle{};
		Clock::time_point     started{};
		Clock::duration       cap{};
		std::optional<double> seconds;  // the recording's length as read from its file
	};

	// How long a line's sound may sound: its recording's length and TAIL, between SHORTEST and LONGEST; UNKNOWN
	// when the length could not be read (or is no length at all).
	static Clock::duration CapFor(std::optional<double> a_seconds)
	{
		if (a_seconds && std::isfinite(*a_seconds) && *a_seconds > 0.0) {
			const auto ms = std::chrono::milliseconds(static_cast<std::int64_t>(std::min(*a_seconds, 600.0) * 1000.0 + 0.5));
			return std::clamp<Clock::duration>(ms + TAIL, SHORTEST, LONGEST);
		}
		return UNKNOWN;
	}

	// A new line's sound. The same speaker's sound before it is ended (kNext), and while MAX_PLAYING are kept the
	// oldest is ended (kTooMany): a_end(entry, why) for each, before the new one is kept.
	template <class End>
	void Start(Key a_key, Handle a_handle, Clock::time_point a_now, std::optional<double> a_seconds, End&& a_end)
	{
		std::erase_if(entries, [&](Entry& a_entry) {
			if (a_entry.key != a_key) {
				return false;
			}
			a_end(a_entry, Why::kNext);
			return true;
		});
		while (entries.size() >= MAX_PLAYING) {
			const auto oldest = std::min_element(entries.begin(), entries.end(), [](const Entry& a_l, const Entry& a_r) { return a_l.started < a_r.started; });
			a_end(*oldest, Why::kTooMany);
			entries.erase(oldest);
		}
		entries.push_back({ a_key, std::move(a_handle), a_now, CapFor(a_seconds), a_seconds });
	}

	// Calls a_end(entry, kTooLong) for every sound at its hard end and forgets it. Returns how many.
	template <class End>
	std::size_t EndDue(Clock::time_point a_now, End&& a_end)
	{
		std::size_t ended = 0;
		std::erase_if(entries, [&](Entry& a_entry) {
			if (a_now - a_entry.started < a_entry.cap) {
				return false;
			}
			a_end(a_entry, Why::kTooLong);
			++ended;
			return true;
		});
		return ended;
	}

	// Calls a_end(entry) for every sound kept and forgets them (the session's end).
	template <class End>
	void EndAll(End&& a_end)
	{
		for (auto& entry : entries) {
			a_end(entry);
		}
		entries.clear();
	}

	// The sounds kept: each until its hard end, its speaker's next line or MAX_PLAYING newer ones, whether it still
	// sounds or not (one that ended by itself is ended again, which does nothing: a sound's ID is never reused). A
	// sound is never let go early on the audio manager's word that it is over, so none is lost before its end.
	[[nodiscard]] std::size_t Kept() const { return entries.size(); }

private:
	std::vector<Entry> entries;
};
