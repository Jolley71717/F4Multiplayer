#pragma once

#include "Protocol.h"
#include "game/LooksOverlays.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// LooksMenu's body sliders (F4EE body morphs) carried to friends' stand-ins. Kept free of the game so it can be tested.
// LooksMenu keeps a character's sliders in its own store, by reference (not on the base NPC that Face reads), and other
// plugins reach them only through its Papyrus script BodyGen. Calls to it are queued to the script VM and answered a frame
// or so later on the VM's own threads. So the player's sliders are read in two rounds (their names, then each one's value),
// and a stand-in's are set first and its body rebuilt only once every set has been answered. Its overlays go the same way
// (game/LooksOverlays.h): read in the same round as the slider names, added to a stand-in with its sliders.
namespace BodyMorphs
{
	using Clock = std::chrono::steady_clock;

	// The player's sliders are read again this often, with no menu open: a change at a plastic surgeon reaches friends
	// a little after the player leaves the chair.
	constexpr auto READ_INTERVAL = std::chrono::seconds(15);
	// A read or a stand-in's sets the VM has not answered by then (the scripts lagging, or BodyGen missing) are given up.
	constexpr auto ANSWER_TIMEOUT = std::chrono::seconds(5);
	// At most this many slider names are asked about in one read: BodyGen also lists morphs other mods set under their
	// own keywords, and each name is a call.
	constexpr std::size_t MAX_NAMES_ASKED = 2 * Protocol::MAX_BODY_MORPHS;

	// Whether a slider name can travel: whole (it is a morph in the body's meshes, so a cut name would be another
	// morph or none), and plain text.
	inline bool Sendable(const std::string& a_name)
	{
		return !a_name.empty() && a_name.size() <= Protocol::MAX_BODY_MORPH_NAME &&
		       std::ranges::none_of(a_name, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
	}

	// What a read found, made fit to send: each name once, sorted by name so that two reads of the same body compare
	// equal (a face that differs goes out again and remakes every friend's stand-in), with no slider at zero (LooksMenu
	// drops those itself) and every value in the range the wire carries; at most MAX_BODY_MORPHS.
	inline std::vector<Protocol::BodyMorph> Collect(std::vector<Protocol::BodyMorph> a_read)
	{
		std::erase_if(a_read, [](const Protocol::BodyMorph& a_morph) {
			return !Sendable(a_morph.name) || !std::isfinite(a_morph.value) || a_morph.value == 0.0f;
		});
		for (auto& morph : a_read) {
			morph.value = std::clamp(morph.value, -Protocol::MAX_BODY_MORPH_VALUE, Protocol::MAX_BODY_MORPH_VALUE);
		}
		std::ranges::stable_sort(a_read, {}, &Protocol::BodyMorph::name);
		const auto repeats = std::ranges::unique(a_read, {}, &Protocol::BodyMorph::name);
		a_read.erase(repeats.begin(), repeats.end());
		if (a_read.size() > Protocol::MAX_BODY_MORPHS) {
			a_read.resize(Protocol::MAX_BODY_MORPHS);
		}
		return a_read;
	}

	// When the player's sliders are read, and what the last read found.
	class Reading
	{
	public:
		// Whether to start a read now: none is under way, and none has finished yet (the first goes at once, menu or
		// not, since the face waits for it), or the last one finished READ_INTERVAL ago with no menu open.
		[[nodiscard]] bool ShouldStart(bool a_menuOpen, Clock::time_point a_now) const
		{
			if (pending) {
				return false;
			}
			if (!lastDone) {
				return true;
			}
			return !a_menuOpen && a_now - *lastDone >= READ_INTERVAL;
		}

		// A read starts: its answers carry this number, so one that comes back after it was given up is told apart.
		std::uint32_t Start(Clock::time_point a_now)
		{
			pending = true;
			startedAt = a_now;
			return ++generation;
		}

		[[nodiscard]] bool Pending() const { return pending; }
		[[nodiscard]] std::uint32_t Generation() const { return generation; }

		// The read is answered: what it found is what the face carries from now on. An answer to a read given up, or
		// to one before the last load, changes nothing. a_overlays: the overlays it found, or none if that call was not
		// answered (an older LooksMenu, say), when the ones read before stay.
		bool Finish(std::uint32_t a_generation, std::vector<Protocol::BodyMorph> a_found, Clock::time_point a_now,
			const std::optional<std::vector<LooksOverlays::Entry>>& a_overlays = std::nullopt)
		{
			if (!pending || a_generation != generation) {
				return false;
			}
			morphs = Collect(std::move(a_found));
			if (a_overlays) {
				overlays = LooksOverlays::Collect(*a_overlays);
			}
			pending = false;
			lastDone = a_now;
			return true;
		}

		// The read failed (BodyGen could not be called): the face goes with what an earlier read found, if any, and
		// another is tried after READ_INTERVAL.
		void Fail(std::uint32_t a_generation, Clock::time_point a_now)
		{
			if (pending && a_generation == generation) {
				pending = false;
				lastDone = a_now;
			}
		}

		// Gives up a read the VM has not answered within ANSWER_TIMEOUT, as a failed one. True if it did.
		bool Expire(Clock::time_point a_now)
		{
			if (!pending || a_now - startedAt < ANSWER_TIMEOUT) {
				return false;
			}
			Fail(generation, a_now);
			return true;
		}

		// Whether the face may go out: a read has finished or been given up once. The first face then carries the
		// sliders, rather than going without and being followed a moment later by one with them, which would make
		// every friend's stand-in twice.
		[[nodiscard]] bool Settled() const { return lastDone.has_value(); }

		[[nodiscard]] const std::vector<Protocol::BodyMorph>& Morphs() const { return morphs; }
		[[nodiscard]] const std::vector<Protocol::Overlay>& Overlays() const { return overlays; }

		// A save was loaded: it may be another character, read afresh (a read under way answers for the old world).
		void Reset()
		{
			pending = false;
			lastDone.reset();
			morphs.clear();
			overlays.clear();
			++generation;
		}

	private:
		bool                              pending = false;
		Clock::time_point                 startedAt{};
		std::optional<Clock::time_point>  lastDone;
		std::uint32_t                     generation = 0;
		std::vector<Protocol::BodyMorph>  morphs;
		std::vector<Protocol::Overlay>    overlays;
	};

	// Whether our face may go out: at once without LooksMenu, else once the first read is done (see Reading::Settled).
	inline bool FaceReady(bool a_installed, const Reading& a_reading)
	{
		return !a_installed || a_reading.Settled();
	}

	// What becomes of a stand-in's sliders (and overlays) once every set on it has been answered.
	enum class Then
	{
		kNothing,  // not one we set any on (or answered already)
		kUpdate,   // rebuild its body with them, and lay the overlays on it
		kClear,    // it went meanwhile: take them out of LooksMenu's store again
	};

	// The stand-ins we set sliders or overlays on, by form ID. LooksMenu keeps a reference's sliders and overlays by form
	// ID until told to forget them (into the save, too), and the game hands a deleted stand-in's form ID to the next
	// reference it makes: a raider spawned later would have a friend's body and tattoos. So a stand-in's are taken out when
	// it goes, but never while its sets are still queued, since a set answered after the removal would put them back.
	class StandIns
	{
	public:
		// Sets were queued for this stand-in.
		void Setting(std::uint32_t a_actor)
		{
			entries[a_actor] = Entry{ .setting = true, .leaving = false };
		}

		// Every set queued for it is answered (or given up).
		Then Answered(std::uint32_t a_actor)
		{
			const auto it = entries.find(a_actor);
			if (it == entries.end() || !it->second.setting) {
				return Then::kNothing;
			}
			if (it->second.leaving) {
				entries.erase(it);
				return Then::kClear;
			}
			it->second.setting = false;
			return Then::kUpdate;
		}

		// The stand-in goes. True if its sliders are to be taken out now; false if it has none of ours, or its sets are
		// still queued (they are taken out once answered: see Answered).
		bool Leaving(std::uint32_t a_actor)
		{
			const auto it = entries.find(a_actor);
			if (it == entries.end()) {
				return false;
			}
			if (it->second.setting) {
				it->second.leaving = true;
				return false;
			}
			entries.erase(it);
			return true;
		}

		[[nodiscard]] bool Has(std::uint32_t a_actor) const { return entries.contains(a_actor); }
		[[nodiscard]] std::size_t Count() const { return entries.size(); }

		// A save was loaded: LooksMenu dropped what it kept for references not in that save, and every stand-in went
		// before the load.
		void Reset() { entries.clear(); }

	private:
		struct Entry
		{
			bool setting = false;
			bool leaving = false;
		};
		std::map<std::uint32_t, Entry> entries;
	};
}
