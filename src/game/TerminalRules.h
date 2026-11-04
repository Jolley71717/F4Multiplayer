#pragma once

#include "Protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Choices made at a quest's terminals that no quest stage carries: the terminal's fragment calls a function of the quest's
// script, which sets one of its properties and acts on it in that game alone. At Easy City Downs (DN035, Fallout4.esm) the
// race's terminals set DN035QuestScript's RaceState, RaceTargeting, RaceCombatInhibitors, RaceReserves and
// RaceSelfDestruct, and its UpdateRobots makes the robots hostile to the player, to the raiders or to each other (their
// factions' relations), wakes the reserves and sets them off. The robots are the plugin's, mirrored across games, but each
// game's factions said what they fought: turned on the player by one player's terminal, they stood idle in the friend's
// game. So our player's choice there goes to the friends (kReportTerminalChoice), and their game calls the same function
// with the same value (kTerminalChoice), as its terminal would have.
//
// A change is ours to send only while our player has a terminal open (or just closed it: CLOSE_GRACE), so what the quest's
// own stages and timers set (stage 10's route, the self-destruct's countdown) is never sent; what a friend's choice set
// here is never sent back. A friend's choice is made here only while the race is on here too (RaceHere: the quest running
// a while, set up and not over) and our player is near the friend who made it (NEAR_RANGE: at the Downs with them), with
// our player at no terminal, and only when ours differs; a row set once (the self-destruct) only when it has not happened
// here. One that can't be made goes stale (CHOICE_LIFE). A joiner hears the latest of each row that isn't set once, from
// a maker still in the session; a load makes a friend's latest again under the same rules, never our own (the loaded
// save's are its own) nor a once row. Kept free of the game so it can be tested (tests/TerminalRulesTests.cpp);
// TerminalSync reads and calls the scripts.
namespace TerminalRules
{
	using Clock = std::chrono::steady_clock;

	inline constexpr auto CLOSE_GRACE = std::chrono::seconds(2);   // the fragment's call lands a moment after the menu closes
	inline constexpr auto SETTLE = std::chrono::seconds(5);        // the quest's run here before a friend's choice is made
	inline constexpr auto CHOICE_LIFE = std::chrono::minutes(10);  // a friend's choice not made by then is dropped (the server's too)
	inline constexpr auto EXPECT_LIFE = std::chrono::seconds(3);   // a setter called here shows in its property by then (its timer: 0.25 s)
	inline constexpr float NEAR_RANGE = 8192.0f;                   // our player within this of the friend who made it

	// DN035's stages (read from the game's files): 10 runs as it starts (the route, the reserves home), 35 is the
	// self-destruct (the robots blow up), 255 its end.
	inline constexpr std::uint16_t RACE_SET_UP = 10;
	inline constexpr std::uint16_t RACE_DESTROYED = 35;
	inline constexpr std::uint16_t RACE_OVER = 255;

	struct Row
	{
		std::string_view plugin;    // its file
		std::uint32_t    quest;     // its form ID in that file
		std::string_view script;    // the quest's script
		std::string_view property;  // the Int property the terminals set
		std::string_view setter;    // the function they call: (Int value, Bool isPlayerDirected)
	};

	// DN035QuestScript's setters as the terminals TERM_DN035_* call them (their fragments, read from the game's files); each
	// row's values are Protocol::TERMINAL_VALUES. The route (SetRaceRoute) has no row (Not yet, docs/DESIGN.md).
	inline constexpr std::array<Row, Protocol::TERMINAL_ROWS> ROWS{ {
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceState", "SetRaceState" },                        // the race run or stopped
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceTargeting", "SetRaceTargeting" },                // the player, the raiders, or each other
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceCombatInhibitors", "SetRaceCombatInhibitors" },  // their combat inhibitors on or off
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceReserves", "SetRaceReserves" },                  // the reserves released (once)
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceSelfDestruct", "RaceSelfDestruct" },             // the robots' self-destruct (once: stage 35)
	} };

	constexpr bool Valid(std::uint8_t a_row, std::uint8_t a_value)
	{
		return Protocol::TerminalValueOk(a_row, a_value);
	}

	constexpr bool Once(std::uint8_t a_row)
	{
		return a_row < ROWS.size() && Protocol::TERMINAL_VALUES[a_row].once;
	}

	// What one reading of a row's property means for a friend: the value to send, if any. a_before: our last reading (none:
	// this is the first since the quest ran here or the game loaded, a baseline). a_expected: what a friend's choice made it
	// here, which is theirs, not ours to send.
	constexpr std::optional<std::uint8_t> ToSend(std::uint8_t a_row, std::optional<std::int32_t> a_before, std::int32_t a_now, bool a_atTerminal, std::optional<std::uint8_t> a_expected)
	{
		if (a_row >= ROWS.size() || !a_before || *a_before == a_now || !a_atTerminal || (a_expected && *a_expected == a_now)) {
			return std::nullopt;
		}
		if (Once(a_row)) {
			return *a_before == 0 && a_now >= 1 ? std::optional<std::uint8_t>{ 1 } : std::nullopt;
		}
		return a_now >= 0 && a_now <= 0xFF && Valid(a_row, static_cast<std::uint8_t>(a_now)) ? std::optional{ static_cast<std::uint8_t>(a_now) } : std::nullopt;
	}

	enum class Step : std::uint8_t
	{
		kWait,  // not now: the race isn't on here (or only just), our player is away from its maker or at a terminal
		kDone,  // ours is so already, a once row happened here, the race is over here, or the choice went stale: dropped
		kCall,  // call the row's setter with the value
	};

	// This game's race as a friend's choice is weighed against it.
	struct RaceHere
	{
		std::optional<std::int32_t> ours;  // our reading of the row (none: the quest isn't running here, or no script)
		bool settled = false;              // the quest has run here SETTLE
		bool setUp = false;                // RACE_SET_UP done
		bool over = false;                 // RACE_DESTROYED or RACE_OVER done: the race is no more here
		bool nearMaker = false;            // our player near the friend who made the choice (NEAR_RANGE, same space)
		bool atTerminal = false;           // our player at a terminal (or just away from one)
	};

	// A friend's choice (a_value) against this game's race.
	constexpr Step ToApply(std::uint8_t a_row, std::uint8_t a_value, const RaceHere& a_here)
	{
		if (!Valid(a_row, a_value)) {
			return Step::kDone;
		}
		if (a_here.ours && a_here.over) {
			return Step::kDone;
		}
		if (!a_here.ours || !a_here.settled || !a_here.setUp || !a_here.nearMaker || a_here.atTerminal) {
			return Step::kWait;
		}
		if (Once(a_row)) {
			return *a_here.ours == 0 ? Step::kCall : Step::kDone;
		}
		return *a_here.ours != a_value ? Step::kCall : Step::kDone;
	}

	// A friend's choice waiting to be made here.
	struct Choice
	{
		std::uint8_t      value = 0;
		std::uint32_t     playerId = 0;  // who made it
		Clock::time_point at{};          // when we heard it
	};

	// The rows' state in one game: our last readings, friends' choices waiting to be made here, and a friend's latest of
	// each row (made again after a load).
	class Book
	{
	public:
		// Our reading of a row now (none: the quest isn't running here, or its script can't be read): what to send.
		std::optional<std::uint8_t> Read(std::uint8_t a_row, std::optional<std::int32_t> a_now, bool a_atTerminal, Clock::time_point a_time)
		{
			if (a_row >= ROWS.size()) {
				return std::nullopt;
			}
			auto& row = rows[a_row];
			if (row.expected && a_time >= row.expectedUntil) {
				row.expected.reset();  // the call never took here (the script refused it): our own choice of that value is ours again
			}
			if (!a_now) {
				row.last.reset();
				return std::nullopt;
			}
			const auto send = ToSend(a_row, row.last, *a_now, a_atTerminal, row.expected);
			if (row.expected && *row.expected == *a_now) {
				row.expected.reset();  // a friend's choice took here
			}
			row.last = *a_now;
			if (send) {
				row.pending.reset();      // ours is the latest now
				row.friendLatest.reset();  // and is never made again here: a loaded save keeps its own
			}
			return send;
		}

		// A friend's choice (live, or the latest of a row, to a joiner): made here when it can be (Due).
		void Heard(std::uint8_t a_row, std::uint8_t a_value, std::uint32_t a_playerId, Clock::time_point a_time)
		{
			if (!Valid(a_row, a_value)) {
				return;
			}
			const Choice choice{ a_value, a_playerId, a_time };
			rows[a_row].pending = choice;
			if (!Once(a_row)) {
				rows[a_row].friendLatest = choice;
			}
		}

		// The pending choice and the step for it now (ToApply); a stale one is dropped.
		[[nodiscard]] Step Due(std::uint8_t a_row, RaceHere a_here, Clock::time_point a_time) const
		{
			if (a_row >= ROWS.size() || !rows[a_row].pending) {
				return Step::kDone;
			}
			const auto& pending = *rows[a_row].pending;
			if (a_time - pending.at > CHOICE_LIFE) {
				return Step::kDone;
			}
			a_here.ours = rows[a_row].last;
			return ToApply(a_row, pending.value, a_here);
		}

		[[nodiscard]] std::optional<Choice> Pending(std::uint8_t a_row) const { return a_row < ROWS.size() ? rows[a_row].pending : std::nullopt; }

		// The pending choice was made here (kCall), or needs nothing (kDone): our next reading of that value, for a while,
		// is its, not ours.
		void Made(std::uint8_t a_row, bool a_called, Clock::time_point a_time)
		{
			if (a_row >= ROWS.size()) {
				return;
			}
			auto& row = rows[a_row];
			if (a_called && row.pending) {
				if (Once(a_row)) {
					row.last = 1;  // its timers count on here from 1: none of that is ours to send
				} else {
					row.expected = row.pending->value;
					row.expectedUntil = a_time + EXPECT_LIFE;
				}
			}
			row.pending.reset();
		}

		// A save was loaded: readings start afresh, and a friend's latest of a row (not a once row, not stale) is made
		// again under the same rules; none of ours.
		void OnGameLoaded(Clock::time_point a_time)
		{
			for (auto& row : rows) {
				row.last.reset();
				row.expected.reset();
				row.pending.reset();
				if (row.friendLatest && a_time - row.friendLatest->at <= CHOICE_LIFE) {
					row.pending = row.friendLatest;
				}
			}
		}

		void Clear() { rows = {}; }

	private:
		struct State
		{
			std::optional<std::int32_t> last;
			std::optional<std::uint8_t> expected;
			Clock::time_point           expectedUntil{};
			std::optional<Choice>       pending;
			std::optional<Choice>       friendLatest;
		};
		std::array<State, ROWS.size()> rows{};
	};
}
