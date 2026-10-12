#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

// A stand-in climbing into or out of its power armor frame copy. The engine does the climb in the
// stand-in's own AI update: it lines the stand-in up with the frame, plays the animation, puts it in
// the armor (the ExtraPowerArmor the game keeps on an actor in a frame) and only then lets it stand
// free again. So the AI runs until the engine says the climb is over, not for a fixed time. A climb that
// outlasted the six seconds it was once given (a slow game loads the armor's body and animations
// slowly) could leave the stand-in half in, and the pieces it was given then hung on a body that was
// not in the frame: knock-kneed, with the shoulder plates standing out.
namespace ArmorClimb
{
	using Duration = std::chrono::steady_clock::duration;
	using TimePoint = std::chrono::steady_clock::time_point;

	// The longest a climb is given. A stand-in that is not in (or out) by then is not waited for.
	constexpr auto LIMIT = std::chrono::seconds(15);
	// A climb the engine shows no sign of (the stand-in neither in the furniture's states nor where it
	// was going) is given up this soon: the frame copy stands at the stand-in, so a real climb is under
	// way well within it. Waiting the whole LIMIT kept a stand-in that never started frozen for 15 s.
	constexpr auto START_LIMIT = std::chrono::seconds(6);
	// In (or out of) the armor but still not back to the normal furniture state this long after: the
	// engine's sitting state is not waited for any longer (it is not known to come back for every frame).
	constexpr auto SETTLE_GRACE = std::chrono::seconds(8);
	// The most one update counts toward a climb: a hitch or a load is not climbing time.
	constexpr auto MAX_STEP = std::chrono::milliseconds(250);

	enum class Step
	{
		kWait,      // still climbing: its AI keeps running
		kDone,      // the engine has finished it
		kCutShort,  // it took too long, or never started: its AI stops anyway
	};

	// One climb into (or out of) a frame, timed in game time: while the game is paused (a menu, the
	// Pip-Boy) the stand-in's AI does not run, so that time is not counted. Counting it gave a climb up
	// when its owner opened the Pip-Boy as it started.
	struct Climb
	{
		bool                    in = true;   // into the frame (else out of it)
		Duration                elapsed{};   // game time since it started
		std::optional<Duration> reached;     // elapsed when the engine had the stand-in where it was going
		bool                    begun = false;  // the engine has shown a sign of it
		TimePoint               last{};

		constexpr Climb(bool a_in, TimePoint a_now) :
			in(a_in), last(a_now)
		{}

		// Once per update, before Next.
		constexpr void Tick(TimePoint a_now, bool a_paused)
		{
			const auto step = a_now - last;
			last = a_now;
			if (!a_paused && step > Duration::zero()) {
				elapsed += (std::min)(step, std::chrono::duration_cast<Duration>(MAX_STEP));
			}
		}

		// a_engineIn: the engine has the stand-in in power armor. a_settled: it is done with the frame as
		// furniture (its sitting state is back to normal).
		constexpr Step Next(bool a_engineIn, bool a_settled)
		{
			const bool there = a_engineIn == in;
			begun = begun || there || !a_settled;
			if (!there) {
				reached.reset();
			} else if (!reached) {
				reached = elapsed;
			}
			if (there && (a_settled || elapsed - *reached >= SETTLE_GRACE)) {
				return Step::kDone;
			}
			return elapsed >= LIMIT || (!begun && elapsed >= START_LIMIT) ? Step::kCutShort : Step::kWait;
		}
	};

	// A friend in a frame every game has (one placed in the world by a plugin, such as the museum's suit in
	// Concord): our copy of that very frame still stands where it was placed while their stand-in wears a
	// frame copy, two suits where they see one. Ours is hidden (not drawn, no collision) while they wear
	// theirs, and shown again once they are out and their stand-in is clear of it. It is only hidden, never
	// used, moved or deleted: it is a reference of our own world, its pieces and scripts belong to our own
	// game and save, and a stand-in in it would take its pieces with it each time it is deleted (before
	// every save).
	struct OriginalFacts
	{
		bool          found = false;     // a reference of that ID exists here and is not deleted or disabled
		bool          created = false;   // ours is one made in this game (0xFF......): not the one they mean
		std::uint32_t base = 0;          // its base form
		bool          loaded = false;    // its 3D is in
		bool          ours = false;      // our player is in it or last left it: our own suit, not theirs
		bool          inUse = false;     // another actor of ours is in it
	};

	enum class Original
	{
		kHide,       // hide ours while they wear theirs
		kNone,       // their frame is not one every game has (or no frame)
		kMissing,    // not here, or not the same frame
		kNotLoaded,  // not loaded yet: looked at again later
		kOurs,       // our player's own suit
		kInUse,      // someone here is in it
	};

	constexpr Original ChooseOriginal(std::uint32_t a_ref, std::uint32_t a_frameBase, const OriginalFacts& a_facts)
	{
		if (a_ref == 0 || (a_ref >> 24) == 0xFF || a_frameBase == 0) {
			return Original::kNone;
		}
		if (!a_facts.found || a_facts.created || a_facts.base != a_frameBase) {
			return Original::kMissing;
		}
		if (a_facts.ours) {
			return Original::kOurs;
		}
		if (a_facts.inUse) {
			return Original::kInUse;
		}
		return a_facts.loaded ? Original::kHide : Original::kNotLoaded;
	}
}
