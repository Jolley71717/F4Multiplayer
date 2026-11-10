#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

// Catch-up XP (the host's iCatchUpXp, SessionRules::catchUpXp): a player below the group's average level earns more
// XP until the levels meet, so a friend who joins at level 8 beside a level-30 host can fight the enemies the session
// rolls for both (sNpcLevel = average). The rule is one number, the percent of extra XP per level behind the average,
// and it never takes anything away:
// - behind by g levels, the player's own kill XP (kills only: quest and discovery XP are the same for everyone) is
//   topped up by g * percent of it, at most doubled (MAX_BONUS_PERCENT);
// - ahead by g levels, the player's own XP is as the game gives it, and their share of friends' kill XP (fXpShare,
//   Party::ApplyXp) shrinks by g * percent of it, to nothing at the most: the gap closes from both sides.
// The average is the mean of the levels of the players in the session now, the player's own among them, those not
// heard from (0) left out, rounded as SessionLevel rounds. Alone, or with nobody heard from, nothing changes. Kept free
// of the game so it can be tested (tests/CatchUpXpTests.cpp); Party.cpp applies it.
namespace CatchUpXp
{
	// The most extra XP a player can get: as much again as the game gave (a doubling).
	inline constexpr std::uint32_t MAX_BONUS_PERCENT = 100;

	struct Factors
	{
		float         ownGain = 1.0f;  // what a rise of the player's own XP is multiplied by (1 to 2)
		float         share = 1.0f;    // what their share of a friend's kill XP is multiplied by (0 to 1)
		std::uint16_t average = 0;     // the group's average level (0: none)
		std::int32_t  behind = 0;      // levels behind the average (negative: ahead)

		bool operator==(const Factors&) const = default;
	};

	// The mean of the levels that are not 0, rounded to the nearest (a half up); nothing with none.
	[[nodiscard]] inline std::optional<std::uint16_t> Average(std::span<const std::uint16_t> a_levels)
	{
		std::uint64_t sum = 0;
		std::uint64_t count = 0;
		for (const auto level : a_levels) {
			if (level != 0) {
				sum += level;
				++count;
			}
		}
		if (count == 0) {
			return std::nullopt;
		}
		return static_cast<std::uint16_t>((sum * 2 + count) / (count * 2));
	}

	// The factors for a player at a_ownLevel among a_levels (their own level among them; a_levels may hold it already,
	// the caller's choice) with the host's percent per level. Off (0), alone, or with no level heard: 1 and 1.
	[[nodiscard]] inline Factors Of(std::uint8_t a_percentPerLevel, std::uint16_t a_ownLevel, std::span<const std::uint16_t> a_levels)
	{
		Factors out;
		const auto average = Average(a_levels);
		if (!average || a_percentPerLevel == 0 || a_ownLevel == 0) {
			return out;
		}
		out.average = *average;
		out.behind = static_cast<std::int32_t>(*average) - static_cast<std::int32_t>(a_ownLevel);
		const std::uint32_t gap = static_cast<std::uint32_t>(out.behind < 0 ? -out.behind : out.behind);
		const std::uint32_t percent = (std::min)(gap * a_percentPerLevel, MAX_BONUS_PERCENT);
		if (out.behind > 0) {
			out.ownGain = 1.0f + static_cast<float>(percent) / 100.0f;
		} else if (out.behind < 0) {
			out.share = 1.0f - static_cast<float>(percent) / 100.0f;
		}
		return out;
	}

	// The extra XP to give for a rise of a_gain under a_factors: whole points, nothing for a factor of 1.
	[[nodiscard]] inline std::uint32_t Bonus(const Factors& a_factors, float a_gain)
	{
		if (a_gain <= 0.0f || a_factors.ownGain <= 1.0f) {
			return 0;
		}
		return static_cast<std::uint32_t>(a_gain * (a_factors.ownGain - 1.0f) + 0.5f);
	}

	// A menu whose use earns XP (a lock picked, a terminal hacked, something crafted or built), as its own open and close
	// events timed it: whether it is open now, and when it last opened and last closed (the epoch: never).
	using Clock = std::chrono::steady_clock;

	struct MenuSeen
	{
		bool              open = false;
		Clock::time_point openedAt{};
		Clock::time_point closedAt{};
	};

	// How long before the held XP's first gain the window starts: a menu's XP lands as it closes, so XP from one shut
	// just before the kill's XP waits unclaimed with it and is held in the same batch.
	inline constexpr Clock::duration MENU_LOOKBACK = std::chrono::seconds(1);

	// Whether any of a_menus was open at some point of the hold's window, from MENU_LOOKBACK before a_heldSince (the
	// held XP's first gain) to a_flushAt (the poll that judges it): open now, or opened or closed in the window. Only
	// the last opening and closing are kept, and that is enough: an earlier stretch that reached into the window was
	// followed by an opening inside it. Then the kill XP held over it gets no top-up, as the menu's XP sits in the
	// hold with the kill's and can't be told apart by amount. A menu that opens after the flush is no business of this
	// hold's: its XP comes after and is judged with whatever it is held with.
	[[nodiscard]] inline bool MenuEarnedXp(std::span<const MenuSeen> a_menus, Clock::time_point a_heldSince, Clock::time_point a_flushAt)
	{
		constexpr Clock::time_point never{};
		const auto                  from = a_heldSince - MENU_LOOKBACK;
		const auto                  within = [&](Clock::time_point a_at) { return a_at != never && a_at >= from && a_at <= a_flushAt; };
		for (const auto& menu : a_menus) {
			if (menu.open || within(menu.openedAt) || within(menu.closedAt)) {
				return true;
			}
		}
		return false;
	}

	// The percent the factors amount to, for the log: +80 for a gain factor of 1.8, -50 for a share factor of 0.5, 0.
	[[nodiscard]] inline std::int32_t Percent(const Factors& a_factors)
	{
		if (a_factors.ownGain > 1.0f) {
			return static_cast<std::int32_t>((a_factors.ownGain - 1.0f) * 100.0f + 0.5f);
		}
		if (a_factors.share < 1.0f) {
			return -static_cast<std::int32_t>((1.0f - a_factors.share) * 100.0f + 0.5f);
		}
		return 0;
	}
}
