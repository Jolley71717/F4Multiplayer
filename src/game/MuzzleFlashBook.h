#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

// A friend's shot (or a mirrored NPC's) is replayed on its stand-in as the firing animation and the gun's sound
// (WeaponFire::PlayShot); nothing is fired, so the engine draws no muzzle flash (ROADMAP #17). MuzzleFlash.cpp shows
// the flash itself: the gun's projectile names a muzzle flash model, which is shown at the stand-in's weapon muzzle
// for the projectile's muzzle flash time. This is the bookkeeping, pure so it can be tested without the game: how long
// a flash shows, how often one shooter may flash and how many flashes show at once (a minigun's stream of shots costs
// one flash at a time per shooter), and when each one comes off again.
namespace MuzzleFlashBook
{
	using Clock = std::chrono::steady_clock;

	// The node in a gun's model that the engine fires from and hangs its own flash under.
	constexpr std::string_view MUZZLE_NODE = "ProjectileNode";

	// How long a flash shows: the projectile's own muzzle flash time, kept within these bounds (a record with none, or
	// a nonsense one, shows the default).
	constexpr float DEFAULT_SECONDS = 0.1f;
	constexpr float MIN_SECONDS = 0.03f;
	constexpr float MAX_SECONDS = 0.25f;

	constexpr float FlashSeconds(float a_projectileSeconds)
	{
		if (!(a_projectileSeconds > 0.0f) || a_projectileSeconds > 10.0f) {  // also NaN
			return DEFAULT_SECONDS;
		}
		return a_projectileSeconds < MIN_SECONDS ? MIN_SECONDS : (a_projectileSeconds > MAX_SECONDS ? MAX_SECONDS : a_projectileSeconds);
	}

	inline Clock::duration ToDuration(float a_seconds)
	{
		return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>{ a_seconds });
	}

	// The fewest milliseconds between two flashes of one shooter: an automatic gun's shots come faster than a flash
	// lasts, and one showing is simply kept up (Book::Kind::kExtend) instead of shown again.
	constexpr Clock::duration MIN_GAP = std::chrono::milliseconds{ 50 };
	constexpr std::size_t     MAX_SHOWING = 12;  // flashes showing at once, over every shooter (a guard)

	// What to do with a shot's flash.
	enum class Kind : std::uint8_t
	{
		kShow,     // show a new flash
		kExtend,   // this shooter's flash still shows: it stays up for the new shot's time
		kTooSoon,  // this shooter flashed less than MIN_GAP ago and its flash is gone: nothing (a guard)
		kTooMany,  // MAX_SHOWING flashes show already: nothing
	};

	constexpr std::string_view Name(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kShow:
			return "show";
		case Kind::kExtend:
			return "extend";
		case Kind::kTooSoon:
			return "too soon";
		case Kind::kTooMany:
			return "too many";
		}
		return "?";
	}

	class Book
	{
	public:
		// A shot of this shooter's at a_now: whether its flash is shown, kept up or skipped. Changes nothing.
		[[nodiscard]] Kind Ask(std::uint32_t a_shooter, Clock::time_point a_now) const
		{
			if (const auto it = showing.find(a_shooter); it != showing.end()) {
				return Kind::kExtend;
			}
			if (const auto last = lastShown.find(a_shooter); last != lastShown.end() && a_now - last->second < MIN_GAP) {
				return Kind::kTooSoon;
			}
			return showing.size() >= MAX_SHOWING ? Kind::kTooMany : Kind::kShow;
		}

		// A test aid (dev `muzzle hold`): every flash shows this long, whatever its own time (zero: its own time).
		void SetHold(Clock::duration a_hold) { hold = a_hold; }

		// The flash Ask said to show is up, until a_now + a_seconds.
		void Shown(std::uint32_t a_shooter, Clock::time_point a_now, float a_seconds)
		{
			showing[a_shooter] = a_now + Length(a_seconds);
			lastShown[a_shooter] = a_now;
			if (lastShown.size() > 4 * MAX_SHOWING) {  // shooters long gone are forgotten
				std::erase_if(lastShown, [&](const auto& a_entry) { return a_now - a_entry.second >= MIN_GAP && !showing.contains(a_entry.first); });
			}
		}

		// The flash Ask said to keep up stays until a_now + a_seconds (never shorter than it was).
		void Extended(std::uint32_t a_shooter, Clock::time_point a_now, float a_seconds)
		{
			const auto it = showing.find(a_shooter);
			if (it == showing.end()) {
				return;
			}
			const auto until = a_now + Length(a_seconds);
			if (until > it->second) {
				it->second = until;
			}
			lastShown[a_shooter] = a_now;
		}

		// The shooters whose flash is due to come off at a_now, forgotten as showing.
		[[nodiscard]] std::vector<std::uint32_t> TakeDue(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> due;
			for (auto it = showing.begin(); it != showing.end();) {
				if (a_now >= it->second) {
					due.push_back(it->first);
					it = showing.erase(it);
				} else {
					++it;
				}
			}
			return due;
		}

		// The shooter's flash comes off now (its stand-in goes, or the flash could not be kept up): true if it showed.
		bool Forget(std::uint32_t a_shooter)
		{
			lastShown.erase(a_shooter);
			return showing.erase(a_shooter) > 0;
		}

		// Every flash comes off now (a save or a load, the session's end): the shooters whose flash showed.
		[[nodiscard]] std::vector<std::uint32_t> TakeAll()
		{
			std::vector<std::uint32_t> all;
			all.reserve(showing.size());
			for (const auto& [shooter, until] : showing) {
				all.push_back(shooter);
			}
			showing.clear();
			lastShown.clear();
			return all;
		}

		[[nodiscard]] std::size_t Showing() const { return showing.size(); }
		[[nodiscard]] bool        Shows(std::uint32_t a_shooter) const { return showing.contains(a_shooter); }

	private:
		Clock::duration Length(float a_seconds) const { return hold > Clock::duration::zero() ? hold : ToDuration(FlashSeconds(a_seconds)); }

		Clock::duration                                      hold{};
		std::unordered_map<std::uint32_t, Clock::time_point> showing;    // shooter -> when its flash comes off
		std::unordered_map<std::uint32_t, Clock::time_point> lastShown;  // shooter -> its last flash shown or kept up
	};
}
