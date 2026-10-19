// Doors only a quest's script opens, shared by the stage that opens them (src/game/ScriptDoors.h): Vault 114's gear
// door, which the host opened with its console while the friend's stayed shut (2026-10-19). What a game reports, when
// it sets a friend's stage, and the stage's way through the session server to a friend and to a late joiner.

#include "ServerFixture.h"

#include "game/ScriptDoors.h"

#include <algorithm>

using namespace TestServer;

namespace
{
	using ScriptDoors::Act;
	using ScriptDoors::Book;
	using ScriptDoors::Look;
	using Clock = ScriptDoors::Clock;
	using namespace std::chrono_literals;

	// Fallout4.esm: DN142 (Vault 114, a dungeon quest), its stage 50 that opens the gear door, the door.
	constexpr std::uint32_t DN142 = 0x0013B747;
	constexpr std::uint16_t OPENS = 50;
	constexpr std::uint32_t GEAR_DOOR = 0x000B25D1;
	constexpr std::size_t   VAULT_114 = 0;  // its place in ScriptDoors::DOORS

	// The quests as a game with Fallout4.esm loaded has them (its load-order byte is 00).
	const std::vector<std::uint32_t> QUESTS{ DN142 };

	// At the door, in the world, the quest running: what a friend's game sees once it walks up to the vault.
	Look AtTheDoor(bool a_done = false)
	{
		return { .running = true, .done = a_done, .doorLoaded = true, .canApply = true };
	}

	Look Elsewhere(bool a_done = false)
	{
		return { .running = true, .done = a_done, .doorLoaded = false, .canApply = true };
	}

	// Looks at the door every half second from a_from for a_for, as WorldSync does; what each look said.
	std::vector<Act> LookFor(Book& a_book, const Look& a_look, Clock::time_point& a_now, Clock::duration a_for)
	{
		std::vector<Act> acts;
		for (const auto end = a_now + a_for; a_now < end; a_now += 500ms) {
			acts.push_back(a_book.See(VAULT_114, a_look, a_now));
		}
		return acts;
	}

	std::size_t CountOf(const std::vector<Act>& a_acts, Act a_act)
	{
		return static_cast<std::size_t>(std::ranges::count(a_acts, a_act));
	}
}

TEST("script doors: the list holds Vault 114's gear door (DN142's 50) and only stages it was checked for")
{
	REQUIRE(ScriptDoors::DOORS.size() == 1);
	const auto& vault = ScriptDoors::DOORS[VAULT_114];
	CHECK(vault.file == "Fallout4.esm");
	CHECK(vault.quest == DN142);
	CHECK(vault.stage == OPENS);
	CHECK(vault.door == GEAR_DOOR);
	for (const auto& door : ScriptDoors::DOORS) {
		// IDs within their file, with no load-order byte: the game adds its own.
		CHECK(door.quest != 0 && (door.quest >> 24) == 0);
		CHECK(door.door != 0 && (door.door >> 24) == 0);
		CHECK(!door.what.empty());
	}

	CHECK(ScriptDoors::Find(QUESTS, DN142, OPENS) == VAULT_114);
	CHECK(!ScriptDoors::Find(QUESTS, DN142, 5));        // DN142's other stages are its own
	CHECK(!ScriptDoors::Find(QUESTS, 0x000B8464, 15));  // Vault 81's console: V81_00_Intro's 15 starts the intercom talk (QuestSync's)
	CHECK(!ScriptDoors::Find(QUESTS, 0x0001F25E, 50));  // MQ104, the story quest that leads there
	CHECK(!ScriptDoors::Find({ 0 }, 0, OPENS));         // a door whose plugin isn't loaded is never matched
	CHECK(!ScriptDoors::Find({}, DN142, OPENS));        // nor before the game's data is loaded
}

TEST("script doors: the stages the survey left out are not listed (docs/script-doors-survey.md)")
{
	// Each opens a way, and was read off the game's files and found to do more, to be set by a quest QuestSync
	// shares already, or to be set by nothing placed in the world.
	struct Stage
	{
		std::string_view file;
		std::uint32_t    quest;
		std::uint16_t    stage;
	};
	constexpr std::array LEFT_OUT{
		Stage{ "Fallout4.esm", 0x00045770, 15 },   // DN050 (General Atomics): the Proctor's test course doors, nothing sets 15
		Stage{ "Fallout4.esm", 0x00045770, 30 },   //   30 and 40 also start the Proctor's next test scene
		Stage{ "Fallout4.esm", 0x00045770, 40 },
		Stage{ "Fallout4.esm", 0x00084CE1, 10 },   // DN066 (Med-Tek): its lab doors, but nothing placed sets 10 (terminals open them)
		Stage{ "Fallout4.esm", 0x00084CE1, 20 },   //   the cell doors, same
		Stage{ "Fallout4.esm", 0x0003479A, 23 },   // DN102 (HalluciGen): the amnesiac's door, and sends him out
		Stage{ "Fallout4.esm", 0x0003479A, 55 },   //   55, 56, 58: lab doors, and the test subjects let go
		Stage{ "Fallout4.esm", 0x0003479A, 56 },
		Stage{ "Fallout4.esm", 0x0003479A, 58 },
		Stage{ "Fallout4.esm", 0x000CC19F, 10 },   // DN048 (Fraternal Post 115): unlocks the doors and springs an ambush
		Stage{ "Fallout4.esm", 0x000FF3C2, 400 },  // DN011 (Boston Public Library): makes the player a friend of the robots
		Stage{ "Fallout4.esm", 0x000C5093, 730 },  // DN019JoinCult: the amphitheater door, a reward for joining
		Stage{ "Fallout4.esm", 0x001243D6, 15 },   // DN083_Barney (Gun Run): objectives, and Barney runs to his bunker
		Stage{ "Fallout4.esm", 0x00080834, 200 },  // DN017 (Arcjet): set by BoS101, which QuestSync shares
		Stage{ "Fallout4.esm", 0x0006F08A, 20 },   // DN130 (Cambridge PD): set by BoS100, which QuestSync shares
		Stage{ "DLCCoast.esm", 0x000052B7, 100 },  // DLC03AcadiaDialogue: set by DLC03DialogueDiMA, which QuestSync shares
		Stage{ "DLCCoast.esm", 0x000140FC, 20 },   // DLC03KyeDialogue: ends the talk with Kye
		Stage{ "DLCCoast.esm", 0x00043822, 100 },  // DLC03MQ02Fight: Far Harbor's gate, and the story quest's 300
	};
	for (const auto& door : ScriptDoors::DOORS) {
		for (const auto& left : LEFT_OUT) {
			CHECK(!(door.file == left.file && door.quest == left.quest && door.stage == left.stage));
		}
	}
	for (const auto& left : LEFT_OUT) {
		CHECK(left.quest != 0 && (left.quest >> 24) == 0);  // IDs within their file, as the list has them
		CHECK(!ScriptDoors::Find(QUESTS, left.quest, left.stage));
	}
}

TEST("script doors: the player opening the door is reported once; a door already open in the save is no news")
{
	Book book;
	auto now = Clock::now();
	CHECK(book.See(VAULT_114, AtTheDoor(), now) == Act::kNone);  // the first look after a load: shut
	now += 500ms;
	CHECK(book.See(VAULT_114, AtTheDoor(true), now) == Act::kReport);  // the player pulled the lever
	CHECK(CountOf(LookFor(book, AtTheDoor(true), now, 30s), Act::kReport) == 0);

	Book loaded;  // a save with the door open
	CHECK(loaded.See(VAULT_114, AtTheDoor(true), now) == Act::kNone);
	CHECK(CountOf(LookFor(loaded, AtTheDoor(true), now, 10s), Act::kReport) == 0);
}

TEST("script doors: the host's game reports what its world has open once its server welcomes it, a joiner's doesn't")
{
	auto now = Clock::now();
	Book host;
	host.ReportOpen();
	CHECK(host.See(VAULT_114, Elsewhere(true), now) == Act::kReport);  // opened before hosting, even far from it
	CHECK(CountOf(LookFor(host, Elsewhere(true), now, 10s), Act::kReport) == 0);

	Book shut;
	shut.ReportOpen();
	CHECK(shut.See(VAULT_114, Elsewhere(), now) == Act::kNone);
	now += 500ms;
	CHECK(shut.See(VAULT_114, AtTheDoor(true), now) == Act::kReport);  // opened later: reported as it happens

	Book joiner;  // never told to report what it has open
	CHECK(CountOf(LookFor(joiner, Elsewhere(true), now, 10s), Act::kReport) == 0);
}

TEST("script doors: a friend's stage is set once the door has been loaded a moment, and once")
{
	Book book;
	auto now = Clock::now();
	CHECK(book.See(VAULT_114, Elsewhere(), now) == Act::kNone);
	book.Take(VAULT_114);
	CHECK(book.Pending(VAULT_114));

	// Elsewhere: the door's animation wouldn't play, and the stage can't be done again.
	CHECK(CountOf(LookFor(book, Elsewhere(), now, 5min), Act::kSet) == 0);
	CHECK(book.Pending(VAULT_114));

	// At the door: set after it has been loaded for SETTLE, then waited on.
	const auto acts = LookFor(book, AtTheDoor(), now, Book::SETTLE + 2s);
	CHECK(CountOf(acts, Act::kSet) == 1);
	CHECK(acts.front() == Act::kNone);
	CHECK(acts[static_cast<std::size_t>(Book::SETTLE / 500ms)] == Act::kSet);

	// The stage runs: the door opens, and nothing is reported back (it is the session's already).
	CHECK(book.See(VAULT_114, AtTheDoor(true), now) == Act::kNone);
	CHECK(!book.Pending(VAULT_114));
	CHECK(CountOf(LookFor(book, AtTheDoor(true), now, 10s), Act::kReport) == 0);
}

TEST("script doors: the wait starts over when the door unloads before it settled; nothing is set in a menu or the opening")
{
	Book book;
	auto now = Clock::now();
	book.Take(VAULT_114);
	CHECK(CountOf(LookFor(book, AtTheDoor(), now, Book::SETTLE - 500ms), Act::kSet) == 0);
	CHECK(book.See(VAULT_114, Elsewhere(), now) == Act::kNone);  // stepped out
	now += 500ms;
	CHECK(CountOf(LookFor(book, AtTheDoor(), now, Book::SETTLE - 500ms), Act::kSet) == 0);  // back: the full wait again

	auto paused = AtTheDoor();
	paused.canApply = false;  // the Pip-Boy, a loading screen, the opening
	CHECK(CountOf(LookFor(book, paused, now, 1min), Act::kSet) == 0);
	CHECK(CountOf(LookFor(book, AtTheDoor(), now, Book::SETTLE + 500ms), Act::kSet) == 1);
}

TEST("script doors: not while the quest doesn't run here, and not over a door already open here")
{
	Book book;
	auto now = Clock::now();
	book.Take(VAULT_114);
	auto stopped = AtTheDoor();
	stopped.running = false;  // setting the stage would start the quest over
	CHECK(CountOf(LookFor(book, stopped, now, 1min), Act::kSet) == 0);
	CHECK(book.Pending(VAULT_114));

	Book open;  // the friend pulled their own lever first
	CHECK(open.See(VAULT_114, AtTheDoor(true), now) == Act::kNone);
	open.Take(VAULT_114);
	const auto acts = LookFor(open, AtTheDoor(true), now, 10s);
	CHECK(CountOf(acts, Act::kSet) == 0);
	CHECK(CountOf(acts, Act::kReport) == 0);
	CHECK(!open.Pending(VAULT_114));

	Book twice;  // the session's stage live, then again in a world state
	twice.Take(VAULT_114);
	twice.Take(VAULT_114);
	CHECK(CountOf(LookFor(twice, AtTheDoor(), now, 10s), Act::kSet) == 1);
}

TEST("script doors: a stage set that never gets done is given up on, not set again")
{
	Book book;
	auto now = Clock::now();
	book.Take(VAULT_114);
	const auto acts = LookFor(book, AtTheDoor(), now, Book::SETTLE + Book::TIMEOUT + 5s);
	CHECK(CountOf(acts, Act::kSet) == 1);
	CHECK(CountOf(acts, Act::kGiveUp) == 1);
	CHECK(!book.Pending(VAULT_114));
	CHECK(CountOf(LookFor(book, AtTheDoor(), now, 1min), Act::kSet) == 0);
}

TEST("script doors: a load or the session's end forgets the session's stages and what was seen")
{
	Book book;
	auto now = Clock::now();
	book.ReportOpen();
	book.Take(VAULT_114);
	book.Clear();
	CHECK(!book.Pending(VAULT_114));
	CHECK(book.PendingCount() == 0);
	CHECK(CountOf(LookFor(book, AtTheDoor(), now, 10s), Act::kSet) == 0);
	CHECK(book.See(VAULT_114, AtTheDoor(true), now) == Act::kReport);  // seen shut since the load, then opened

	Book loaded;
	loaded.ReportOpen();
	loaded.Clear();  // a load after the welcome: reported again only if the host's game is told to again
	CHECK(loaded.See(VAULT_114, AtTheDoor(true), now) == Act::kNone);
}

TEST("script doors: the host's lever opens the friend's door and a late joiner's, through the session's quest stages")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	auto now = Clock::now();

	// The host's game sees DN142 at 50 after the lever: reported as a quest stage.
	Book host;
	CHECK(host.See(VAULT_114, AtTheDoor(), now) == Act::kNone);
	now += 500ms;
	REQUIRE(host.See(VAULT_114, AtTheDoor(true), now) == Act::kReport);
	f.fake->Deliver(1, Protocol::Encode(Protocol::QuestStage{ DN142, OPENS }, MT::kReportQuestStage));

	// The friend's game takes it as the door's stage (not QuestSync's), and sets it at the door.
	const auto live = Decoded(Received(f, 2), MT::kQuestStage, Protocol::DecodeQuestStage);
	REQUIRE(live.size() == 1);
	Book friendGame;
	CHECK(friendGame.See(VAULT_114, Elsewhere(), now) == Act::kNone);
	const auto door = ScriptDoors::Find(QUESTS, live[0].quest, live[0].stage);
	REQUIRE(door);
	friendGame.Take(*door);
	CHECK(CountOf(LookFor(friendGame, AtTheDoor(), now, Book::SETTLE + 1s), Act::kSet) == 1);

	// Someone joining later gets it with the session's world, and opens the door once they reach it.
	JoinAs(f, 3, "Carol");
	std::size_t caughtUp = 0;
	Book late;
	for (const auto& world : WorldStates(Received(f, 3))) {
		for (const auto& stage : world.questStages) {
			if (const auto found = ScriptDoors::Find(QUESTS, stage.quest, stage.stage)) {
				late.Take(*found);
				++caughtUp;
			}
		}
	}
	CHECK(caughtUp == 1);
	CHECK(late.Pending(VAULT_114));
	CHECK(CountOf(LookFor(late, AtTheDoor(), now, Book::SETTLE + 1s), Act::kSet) == 1);
}
