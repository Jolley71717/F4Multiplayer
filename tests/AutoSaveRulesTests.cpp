// The mod's own autosaves, what a player is told a death does, and what the log says of each save and load
// (src/game/AutoSaveRules.h, which AutoSave.cpp and Downed.cpp follow): a death loads the newest save, and in a
// session that save was often ten minutes old (F4Multiplayer-qx9n).

#include "Test.h"

#include "Text.h"
#include "game/AutoSaveRules.h"

#include <algorithm>

namespace
{
	using AutoSaveRules::Asked;
	using AutoSaveRules::Book;
	using AutoSaveRules::Clock;
	using AutoSaveRules::Facts;
	using AutoSaveRules::Why;
	using namespace std::chrono_literals;

	constexpr Facts TRAVEL{ .saveOnTravel = true };
	constexpr Facts DOWN_RULE{ .saveOnDown = true };

	const Clock::time_point START = Clock::time_point{} + 1h;
}

TEST("autosaves: landing after a teleport autosaves when the player's travel autosave setting is on, never in Survival")
{
	CHECK(AutoSaveRules::Wanted(Why::kTeleport, TRAVEL));
	CHECK(!AutoSaveRules::Wanted(Why::kTeleport, Facts{}));                                    // their setting is off
	CHECK(!AutoSaveRules::Wanted(Why::kTeleport, { .survival = true, .saveOnTravel = true }));  // a bed only
	CHECK(!AutoSaveRules::Wanted(Why::kTeleport, DOWN_RULE));                                  // the host's rule is for going down
	CHECK(!AutoSaveRules::Wanted(Why::kNone, { .saveOnTravel = true, .saveOnDown = true }));
}

TEST("autosaves: a teleport is travel into another cell or worldspace, or across the worldspace both are in, not a hop")
{
	using AutoSaveRules::TeleportIsTravel;
	using AutoSaveRules::TRAVEL_DISTANCE;
	CHECK(TeleportIsTravel(false, true, 0.0f));    // coc into a friend's interior
	CHECK(TeleportIsTravel(false, false, 10.0f));  // cow, however near
	// Both outside in the Commonwealth: SetPosition, no loading screen, and no autosave from the engine either.
	CHECK(TeleportIsTravel(true, false, TRAVEL_DISTANCE));
	CHECK(TeleportIsTravel(true, false, 100000.0f));
	CHECK(!TeleportIsTravel(true, false, TRAVEL_DISTANCE - 1.0f));  // a friend a few steps away
	CHECK(!TeleportIsTravel(true, true, 100000.0f));                // within one interior
}

TEST("autosaves: going down autosaves only with the host's bSaveOnDown, never in Survival")
{
	CHECK(AutoSaveRules::Wanted(Why::kDown, DOWN_RULE));
	CHECK(!AutoSaveRules::Wanted(Why::kDown, Facts{}));
	CHECK(!AutoSaveRules::Wanted(Why::kDown, TRAVEL));  // the player's travel setting is not the host's rule
	CHECK(!AutoSaveRules::Wanted(Why::kDown, { .survival = true, .saveOnDown = true }));
}

TEST("autosaves: a save the mod asked for is told as the mod's; any other as the player's or the game's")
{
	Book book;
	CHECK(book.Began(START) == Why::kNone);  // the player's own save, with nothing asked
	auto now = START + AutoSaveRules::MIN_GAP;
	CHECK(book.Ask(Why::kTeleport, TRAVEL, now) == Asked::kYes);
	CHECK(book.Began(now + 2s) == Why::kTeleport);
	CHECK(book.Began(now + 3s) == Why::kNone);  // the next one is not ours: one request, one save
	CHECK(book.asks == 1);
	CHECK(book.made == 1);
	CHECK(AutoSaveRules::WhyName(Why::kTeleport).find("mod") != std::string_view::npos);
	CHECK(AutoSaveRules::WhyName(Why::kDown).find("going down") != std::string_view::npos);
	CHECK(AutoSaveRules::WhyName(Why::kNone).find("player") != std::string_view::npos);
}

TEST("autosaves: none within a minute of the last save, of any kind, so a string of teleports doesn't stop the game each time")
{
	Book book;
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START) == Asked::kYes);  // the first of the game: nothing to wait for
	CHECK(book.Began(START + 1s) == Why::kTeleport);
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START + 30s) == Asked::kTooSoon);
	CHECK(book.Ask(Why::kDown, DOWN_RULE, START + 59s) == Asked::kTooSoon);
	CHECK(book.tooSoon == 2);
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START + 1s + AutoSaveRules::MIN_GAP) == Asked::kYes);
	// The player's own save counts too.
	Book other;
	CHECK(other.Began(START) == Why::kNone);
	CHECK(other.Ask(Why::kDown, DOWN_RULE, START + 10s) == Asked::kTooSoon);
	CHECK(other.SinceSave(START + 10s) == std::optional<Clock::duration>{ 10s });
	CHECK(!Book{}.SinceSave(START));
}

TEST("autosaves: a request the game never answers is told once, and asking again waits for it first")
{
	Book book;
	CHECK(book.Ask(Why::kDown, DOWN_RULE, START) == Asked::kYes);
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START + 1s) == Asked::kWaiting);  // one at a time
	CHECK(book.Unanswered(START + AutoSaveRules::ANSWER_TIME - 1s) == Why::kNone);
	CHECK(book.Unanswered(START + AutoSaveRules::ANSWER_TIME) == Why::kDown);
	CHECK(book.Unanswered(START + AutoSaveRules::ANSWER_TIME + 1s) == Why::kNone);  // told once
	CHECK(book.unanswered == 1);
	// A save that begins long after the request is not taken for it.
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START + 20s) == Asked::kYes);
	CHECK(book.Began(START + 20s + AutoSaveRules::ANSWER_TIME) == Why::kNone);
	CHECK(book.made == 0);
	// Not wanted is never booked.
	Book quiet;
	CHECK(quiet.Ask(Why::kTeleport, Facts{}, START) == Asked::kNotWanted);
	CHECK(quiet.Unanswered(START + 1min) == Why::kNone);
	CHECK(quiet.asks == 0);
}

TEST("autosaves: a save's name is logged by its kind, place and date, never the character's name")
{
	// "Nora" in hex is the engine's fourth part of the name.
	CHECK(AutoSaveRules::Label("Autosave3_0002A1C9_0_4E6F7261_Concord_000016_20261026211405_1_2") == "Autosave3 (Concord, 2026-10-26 21:14:05)");
	CHECK(AutoSaveRules::Label("C:\\Users\\x\\Documents\\My Games\\Fallout4\\Saves\\Quicksave0_0002A1C9_0_4E6F7261_SanctuaryExt_000016_20261026211405_1_2.fos") ==
	      "Quicksave0 (SanctuaryExt, 2026-10-26 21:14:05)");
	CHECK(AutoSaveRules::Label("Saves/Save12_0002A1C9_1_4E6F7261_Diamond_City_001234_20261026211405_1_2.FOS") == "Save12 (Diamond_City, 2026-10-26 21:14:05)");
	CHECK(AutoSaveRules::Label(std::string_view{ "Exitsave_0002A1C9_0_4E6F7261_Concord_000016_20261026211405_1_2\0\0", 64 }) == "Exitsave (Concord, 2026-10-26 21:14:05)");
	for (const auto raw : { "Autosave3_0002A1C9_0_4E6F7261_Concord_000016_20261026211405_1_2", "Save1_0002A1C9_0_4E6F7261_Concord_000016_bad_1_2" }) {
		CHECK(AutoSaveRules::Label(raw).find("4E6F7261") == std::string::npos);
	}
	CHECK(AutoSaveRules::Label("Save1_0002A1C9_0_4E6F7261_Concord_000016_bad_1_2") == "Save1 (Concord)");
	// Another form (another mod's) could hold anything, the player's name too: only its length is told.
	CHECK(AutoSaveRules::Label("Nora_Saved_Here") == "a save named in another form (15 characters)");
	CHECK(AutoSaveRules::Label("Nora1_0002A1C9_0_4E6F7261_Concord_000016_20261026211405_1_2") == "a save named in another form (59 characters)");
	CHECK(AutoSaveRules::Label("Save_x") == "a save named in another form (6 characters)");
	CHECK(AutoSaveRules::Label("") == "(no name)");
	CHECK(AutoSaveRules::Label(std::string_view{ "\0", 1 }) == "(no name)");
	CHECK(AutoSaveRules::Label(".fos") == "(no name)");
}

TEST("autosaves: going down, a last revive, a cooldown and a session without revives each say that a death loads the last save")
{
	using AutoSaveRules::DeathWarning;
	CHECK(DeathWarning({ .wentDown = true }) == "down.bleed_out_loads_save");
	CHECK(DeathWarning({ .lastRevive = true }) == "down.death_loads_save");
	CHECK(DeathWarning({ .cooldownSeconds = 30 }) == "down.death_loads_save");
	CHECK(DeathWarning({ .reviveOff = true }) == "down.death_loads_save");
	CHECK(DeathWarning({}).empty());
	CHECK(DeathWarning({ .cooldownSeconds = 0 }).empty());  // no cooldown rule: AfterRevive says -1, or 0
	// Each key is in the English table, the bleed-out one with the seconds.
	Text::Use("en", {});
	for (const auto key : { "down.bleed_out_loads_save", "down.death_loads_save", "rules.save_on_down" }) {
		CHECK(std::ranges::any_of(Text::English(), [&](const auto& a_entry) { return a_entry.key == key; }));
	}
	CHECK(Text::Format("down.bleed_out_loads_save", { { "n", 45 } }) == "If nobody helps you up in 45 seconds, you die and the game loads your last save");
}

TEST("autosaves: the travel autosave setting is found by its name in Fallout4Prefs.ini's [MAIN], not in a section the code guessed")
{
	using AutoSaveRules::FindSetting;
	using AutoSaveRules::TRAVEL_SECTION;
	using AutoSaveRules::TRAVEL_SETTING;
	// The keys the game gives its prefs: "name:Section".
	const std::vector<std::string_view> prefs{ "bAlwaysActive:General", "bSaveOnPause:MAIN", "bSaveOnTravel:MAIN", "bSaveOnWait:MAIN", "bSaveOnRest:MAIN" };
	CHECK(FindSetting(prefs, TRAVEL_SETTING, TRAVEL_SECTION) == std::optional<std::size_t>{ 2 });
	// What the first build looked for ("bSaveOnTravel:SaveGame") is not in them: nothing is found for a wrong section's sake.
	CHECK(std::ranges::find(prefs, std::string_view{ "bSaveOnTravel:SaveGame" }) == prefs.end());

	// [MAIN] wins over another section; case does not matter; a name in another section alone is still found.
	CHECK(FindSetting({ "bSaveOnTravel:Gameplay", "bSaveOnTravel:MAIN" }, TRAVEL_SETTING, TRAVEL_SECTION) == std::optional<std::size_t>{ 1 });
	CHECK(FindSetting({ "bsaveontravel:main" }, TRAVEL_SETTING, TRAVEL_SECTION) == std::optional<std::size_t>{ 0 });
	CHECK(FindSetting({ "bAlwaysActive:General", "bSaveOnTravel:Gameplay" }, TRAVEL_SETTING, TRAVEL_SECTION) == std::optional<std::size_t>{ 1 });

	// Not there: no match, so the game's default stands (on) instead of "off". A longer name, a prefix and a key without a
	// section are not it.
	CHECK(!FindSetting({}, TRAVEL_SETTING, TRAVEL_SECTION));
	CHECK(!FindSetting({ "bSaveOnTravelAlways:MAIN", "bSaveOn:MAIN", "bSaveOnTravel", "bSaveOnRest:MAIN" }, TRAVEL_SETTING, TRAVEL_SECTION));
	CHECK(AutoSaveRules::TRAVEL_DEFAULT);
	CHECK(AutoSaveRules::Wanted(Why::kTeleport, Facts{ .saveOnTravel = AutoSaveRules::TRAVEL_DEFAULT }));
}

TEST("autosaves: a dead player is never saved, and a downed one gets no travel autosave, only the host's on going down")
{
	CHECK(!AutoSaveRules::Wanted(Why::kTeleport, { .saveOnTravel = true, .down = true }));  // landed, and went down meanwhile
	CHECK(!AutoSaveRules::Wanted(Why::kTeleport, { .saveOnTravel = true, .dead = true }));
	CHECK(AutoSaveRules::Wanted(Why::kDown, { .saveOnDown = true, .down = true }));  // what the rule is for: loaded, Downed stands them up
	CHECK(!AutoSaveRules::Wanted(Why::kDown, { .saveOnDown = true, .down = true, .dead = true }));
	Book book;
	CHECK(book.Ask(Why::kTeleport, { .saveOnTravel = true, .down = true }, START) == Asked::kNotWanted);
	CHECK(book.asks == 0);
}

TEST("autosaves: one taken back before it began is awaited no more, and a save after it is not taken for it")
{
	Book book;
	CHECK(book.Withdraw() == Why::kNone);  // nothing asked for
	CHECK(book.withdrawn == 0);
	CHECK(book.Ask(Why::kTeleport, TRAVEL, START) == Asked::kYes);
	CHECK(book.Withdraw() == Why::kTeleport);  // the player went down
	CHECK(book.withdrawn == 1);
	CHECK(book.Withdraw() == Why::kNone);  // once
	CHECK(book.Unanswered(START + AutoSaveRules::ANSWER_TIME) == Why::kNone);  // not told as unanswered
	// The autosave on going down is asked for at once, not left waiting on the one taken back.
	CHECK(book.Ask(Why::kDown, DOWN_RULE, START + 1s) == Asked::kYes);
	CHECK(book.Withdraw() == Why::kDown);  // bled out before the game made it
	CHECK(book.Began(START + 2s) == Why::kNone);  // a save that still comes is not the mod's
	CHECK(book.made == 0);
}
