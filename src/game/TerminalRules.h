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
// here is never sent back. Applied only once the quest has run here a while (SETTLE: not into a start's own setup), with
// our player at no terminal, and only when ours differs; a row that happens once (the self-destruct) only when it has not
// happened here. The server keeps each row's latest for a joiner; a load applies the session's again. Kept free of the
// game so it can be tested (tests/TerminalRulesTests.cpp); TerminalSync reads and calls the scripts.
namespace TerminalRules
{
	using Clock = std::chrono::steady_clock;

	inline constexpr auto CLOSE_GRACE = std::chrono::seconds(2);  // the fragment's call lands a moment after the menu closes
	inline constexpr auto SETTLE = std::chrono::seconds(5);       // the quest's run here before a friend's choice is made

	struct Row
	{
		std::string_view plugin;    // its file
		std::uint32_t    quest;     // its form ID in that file
		std::string_view script;    // the quest's script
		std::string_view property;  // the Int property the terminals set
		std::string_view setter;    // the function they call: (Int value, Bool isPlayerDirected)
		std::uint8_t     low;       // the values a terminal sets
		std::uint8_t     high;
		bool             once;      // set to 1 once (its own timers count on from there): shared as "it happened"
	};

	// DN035QuestScript's setters as the terminals TERM_DN035_* call them (their fragments, read from the game's files). The
	// route (SetRaceRoute) is set by stages 10 and 31 alone: no terminal row.
	inline constexpr std::array<Row, Protocol::TERMINAL_ROWS> ROWS{ {
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceState", "SetRaceState", 0, 1, false },                        // the race run or stopped
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceTargeting", "SetRaceTargeting", 0, 2, false },                // the player, the raiders, or each other
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceCombatInhibitors", "SetRaceCombatInhibitors", 0, 1, false },  // their combat inhibitors on or off
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceReserves", "SetRaceReserves", 1, 1, true },                    // the reserves released
		{ "Fallout4.esm", 0x0EF40C, "DN035QuestScript", "RaceSelfDestruct", "RaceSelfDestruct", 1, 1, true },               // the robots' self-destruct (stage 35)
	} };

	constexpr bool Valid(std::uint8_t a_row, std::uint8_t a_value)
	{
		return a_row < ROWS.size() && a_value >= ROWS[a_row].low && a_value <= ROWS[a_row].high;
	}

	// What one reading of a row's property means for a friend: the value to send, if any. a_before: our last reading (none:
	// this is the first since the quest ran here or the game loaded, a baseline). a_expected: what a friend's choice made it
	// here, which is theirs, not ours to send.
	constexpr std::optional<std::uint8_t> ToSend(std::uint8_t a_row, std::optional<std::int32_t> a_before, std::int32_t a_now, bool a_atTerminal, std::optional<std::uint8_t> a_expected)
	{
		if (a_row >= ROWS.size() || !a_before || *a_before == a_now || !a_atTerminal || (a_expected && *a_expected == a_now)) {
			return std::nullopt;
		}
		const auto& row = ROWS[a_row];
		if (row.once) {
			return *a_before == 0 && a_now >= 1 ? std::optional<std::uint8_t>{ 1 } : std::nullopt;
		}
		return a_now >= row.low && a_now <= row.high ? std::optional{ static_cast<std::uint8_t>(a_now) } : std::nullopt;
	}

	enum class Step : std::uint8_t
	{
		kWait,  // not now: the quest isn't running here (or only just), our player is at a terminal, or there's no reading
		kDone,  // ours is so already (or a once row happened here): nothing to do
		kCall,  // call the row's setter with the value
	};

	// A friend's choice (a_value) against our reading of the row (none: no quest script to read).
	constexpr Step ToApply(std::uint8_t a_row, std::uint8_t a_value, std::optional<std::int32_t> a_ours, bool a_settled, bool a_atTerminal)
	{
		if (!Valid(a_row, a_value)) {
			return Step::kDone;
		}
		if (!a_ours || !a_settled || a_atTerminal) {
			return Step::kWait;
		}
		if (ROWS[a_row].once) {
			return *a_ours == 0 ? Step::kCall : Step::kDone;
		}
		return *a_ours != a_value ? Step::kCall : Step::kDone;
	}

	// The rows' state in one game: our last readings, friends' choices waiting to be made here, and the session's latest
	// of each row (made again after a load).
	class Book
	{
	public:
		// Our reading of a row now (none: the quest isn't running here, or its script can't be read): what to send.
		std::optional<std::uint8_t> Read(std::uint8_t a_row, std::optional<std::int32_t> a_now, bool a_atTerminal)
		{
			if (a_row >= ROWS.size()) {
				return std::nullopt;
			}
			auto& row = rows[a_row];
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
				row.session = *send;
				row.pending.reset();  // ours is the latest now
			}
			return send;
		}

		// A friend's choice (or the session's latest of a row, to a joiner): made here when it can be (Due).
		void Heard(std::uint8_t a_row, std::uint8_t a_value)
		{
			if (!Valid(a_row, a_value)) {
				return;
			}
			rows[a_row].session = a_value;
			rows[a_row].pending = a_value;
		}

		// A friend's choice to make here now, and the step for it (ToApply).
		[[nodiscard]] Step Due(std::uint8_t a_row, bool a_settled, bool a_atTerminal) const
		{
			if (a_row >= ROWS.size() || !rows[a_row].pending) {
				return Step::kDone;
			}
			return ToApply(a_row, *rows[a_row].pending, rows[a_row].last, a_settled, a_atTerminal);
		}

		[[nodiscard]] std::optional<std::uint8_t> Pending(std::uint8_t a_row) const { return a_row < ROWS.size() ? rows[a_row].pending : std::nullopt; }

		// The pending choice was made here (kCall), or needs nothing (kDone): our next reading of that value is its, not ours.
		void Made(std::uint8_t a_row, bool a_called)
		{
			if (a_row >= ROWS.size()) {
				return;
			}
			auto& row = rows[a_row];
			if (a_called && row.pending) {
				row.expected = ROWS[a_row].once ? std::optional<std::uint8_t>{} : row.pending;
				if (ROWS[a_row].once) {
					row.last = 1;  // its timers count on here from 1: none of that is ours to send
				}
			}
			row.pending.reset();
		}

		// A save was loaded: readings start afresh, and the session's choices are made again where the loaded game differs.
		void OnGameLoaded()
		{
			for (auto& row : rows) {
				row.last.reset();
				row.expected.reset();
				row.pending = row.session;
			}
		}

		void Clear() { rows = {}; }

	private:
		struct State
		{
			std::optional<std::int32_t> last;
			std::optional<std::uint8_t> expected;
			std::optional<std::uint8_t> pending;
			std::optional<std::uint8_t> session;
		};
		std::array<State, ROWS.size()> rows{};
	};
}
