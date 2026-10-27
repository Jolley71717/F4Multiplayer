// Shared quests played end to end: simulated players (tests/sim/Sim.h) report the stages their games
// reach through the real server, and each game sets its friends' stages as QuestSync does, with the
// plugin's own StageLog and StageRules (tests/sim/QuestGame.h). The quests are the game's own form IDs, so
// what a stage does to the player (moves them, takes their caps or controls, moves their story on) comes
// from the table generated from the game's quest scripts. Each test names what a player would see go
// wrong if it failed.

#include "sim/Sim.h"

#include <algorithm>

using namespace Sim;
using namespace std::chrono_literals;

namespace
{
	// Fallout4.esm's quests.
	constexpr std::uint32_t SIDE = 0x0001ED86;   // a side quest none of whose stages does anything to the player
	constexpr std::uint32_t SIDE2 = 0x0001ED87;  // another
	constexpr std::uint32_t MS17 = 0x00022A08;   // a side quest whose 150 and 175 move the player, and whose 315 and 331 scripts wait
	constexpr std::uint32_t MIN01 = 0x0005DEE4;  // When Freedom Calls: 1500, 2000, 2010 and 2100 move the Minutemen's story on
	constexpr std::uint32_t MQ104 = 0x0001F25E;  // Unlikely Valentine: its 210 starts Getting a Clue (MQ105) at 10
	constexpr std::uint32_t MQ105 = 0x000229E6;  // Getting a Clue: 177 pays a bribe out of the player's caps
	constexpr std::uint32_t MQ206 = 0x000B1752;  // Molecular Level: 500 takes the controls, 1000 is the teleporter, 1200 gives them back
	constexpr std::uint32_t LONG = 0x0001ED88;   // a side quest given hundreds of stages, none of which does anything to the player
	constexpr std::uint32_t V81 = 0x000B8464;    // Vault 81 (V81_00_Intro): the intercom talk's stages show no objective; 55 opens the vault door
	constexpr std::uint32_t DC_GATE = 0x000190BC;  // DialogueDiamondCityEntrance: type 0, no objectives; 32, 40 and 100 open Diamond City's gate

	constexpr const char* SIDE_ACHIEVEMENT = "the side quest's achievement";
	constexpr const char* SANCTUARY_ACHIEVEMENT = "Sanctuary's achievement";
	constexpr const char* INSTITUTE_ACHIEVEMENT = "the Institute's achievement";
	constexpr const char* VAULT_DOOR_OPENS = "Vault 81's door and walkway open";
	constexpr const char* DC_GATE_OPENS = "Diamond City's gate opens";  // the gate stage's script ran

	using Stages = std::set<std::uint16_t>;
	using History = std::vector<std::pair<std::uint32_t, std::uint16_t>>;

	// A quest the player has started (its first objective is in the Pip-Boy).
	Quest Started(Stages a_stages, std::map<std::uint16_t, std::string> a_awards = {})
	{
		Quest quest;
		quest.stages = std::move(a_stages);
		quest.running = true;
		quest.started = true;
		quest.awards = std::move(a_awards);
		return quest;
	}

	// A quest the game runs in the background, which the player hasn't come across yet.
	Quest NotStarted(Stages a_stages)
	{
		auto quest = Started(std::move(a_stages));
		quest.started = false;
		return quest;
	}

	Quest Sanctuary()
	{
		auto quest = Started({ 10, 100, 200, 1000, 1500, 2000, 2010, 2100 }, { { 2000, SANCTUARY_ACHIEVEMENT } });
		quest.finishes = { 2100 };  // its script stops the quest
		return quest;
	}

	Quest MolecularLevel()
	{
		auto quest = Started({ 40, 500, 1000, 1050, 1200 }, { { 1200, INSTITUTE_ACHIEVEMENT } });
		quest.story = true;
		return quest;
	}

	Quest GettingAClue()
	{
		auto quest = Started({ 10, 100, 177, 200 });
		quest.story = true;
		return quest;
	}

	// Unlikely Valentine, at the talk with Valentine; Getting a Clue is still to come (only MQ104's 210 starts it).
	void AtValentine(Client& a_player)
	{
		auto valentine = Started({ 200, 210, 1000 });
		valentine.story = true;
		valentine.starts = { { 210, { MQ105, 10 } } };
		a_player.GiveQuest(MQ104, std::move(valentine));
		auto clue = GettingAClue();
		clue.running = false;
		clue.started = false;
		a_player.GiveQuest(MQ105, std::move(clue));
	}

	// The player reaches these stages of a quest, one after another.
	void Reach(Client& a_player, std::uint32_t a_quest, std::initializer_list<std::uint16_t> a_stages)
	{
		for (const auto stage : a_stages) {
			a_player.Stage(a_quest, stage);
		}
	}

	// Every player has these quests.
	void GiveQuests(std::initializer_list<Client*> a_players)
	{
		for (const auto player : a_players) {
			player->GiveQuest(SIDE, Started({ 10, 20, 30, 40 }, { { 30, SIDE_ACHIEVEMENT } }));
			player->GiveQuest(SIDE2, Started({ 5, 10, 20, 50 }));
			player->GiveQuest(MS17, Started({ 0, 150, 175, 315, 331, 450 }));
			player->GiveQuest(MIN01, Sanctuary());
			player->GiveQuest(MQ105, GettingAClue());
			player->GiveQuest(MQ206, MolecularLevel());
		}
	}

	// The games have been in the world a moment: quest scripts have settled after the load (QuestSync
	// reports nothing for 5 s).
	void Settled(Session& a_session)
	{
		a_session.PlayFor(6s);
	}

	Stages Done(const Client& a_player, std::uint32_t a_quest) { return a_player.Quests().Of(a_quest).done; }

	// The stages of a quest set in this player's game, in the order they were set.
	std::vector<std::uint16_t> Order(const Client& a_player, std::uint32_t a_quest)
	{
		std::vector<std::uint16_t> out;
		for (const auto& [quest, stage] : a_player.Quests().History()) {
			if (quest == a_quest) {
				out.push_back(stage);
			}
		}
		return out;
	}

	// Vault 81 as the game has it at the vault door: the game started the quest when the player came near
	// (10 turns the intercom on, 15 plays its first lines), and none of its objectives is shown until the
	// vault door opens. The talk at the intercom sets 20 to 51; 55's script opens the gear door and
	// extends the walkway (the quest script's OpenGearDoor), then shows the first objective.
	Quest AtVault81Door()
	{
		Quest quest;
		quest.stages = { 0, 10, 15, 20, 30, 40, 50, 51, 55, 60, 70, 100, 200, 300, 310, 400, 500, 1000 };
		quest.running = true;
		quest.done = { 10, 15 };
		quest.current = 15;
		quest.quiet = { 10, 15, 20, 30, 40, 50, 51 };
		quest.showsLater = { 55 };
		quest.awards = { { 55, VAULT_DOOR_OPENS } };
		return quest;
	}

	// Diamond City's entrance scene, before anyone has come to the gate: Piper argues with Danny Sullivan
	// over the intercom, then the gate opens. One of the game's dialogue quests (DialogueDiamondCityEntrance,
	// shared with the story), with no objectives: none of its stages ever shows one. Its stages aren't
	// reached in order: a player's game plays the argument (0 to 30, 50 to 95 and 400), and 32 opens the
	// gate, 40 goes on and 100 stops the scene, the gate open.
	Quest DiamondCityGate()
	{
		Quest quest;
		quest.stages = { 0, 1, 2, 3, 5, 7, 10, 20, 25, 30, 32, 40, 50, 52, 55, 90, 95, 100, 400 };
		quest.quiet = quest.stages;
		quest.story = true;
		quest.dialogue = true;
		quest.stops = { 100 };
		quest.awards = { { 32, DC_GATE_OPENS } };
		return quest;
	}

	// The scene as the live friend's game had it: the argument played up to its 400, the gate still shut.
	Quest AtDiamondCityGate()
	{
		auto quest = DiamondCityGate();
		quest.running = true;
		quest.done = { 0, 1, 2, 3, 5, 7, 10, 20, 25, 30, 50, 52, 55, 90, 95, 400 };
		quest.current = 400;
		return quest;
	}

	Server::Options SharedStory()
	{
		Server::Options options;
		options.sharedStory = true;
		return options;
	}
}

// ---- progress in both directions ------------------------------------------------------------------

TEST("sim quests: a side quest the host moves on moves on in the friend's game stage by stage, the friend's progress reaches the host, and the achievement fires in both")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE, 10);
	host.Stage(SIDE, 20);
	s.PlayFor(5s);
	CHECK((Done(bob, SIDE) == Stages{ 10, 20 }));
	CHECK((Order(bob, SIDE) == std::vector<std::uint16_t>{ 10, 20 }));  // each stage's script ran, in order

	bob.Stage(SIDE, 30);  // the stage with the achievement, in the friend's game
	s.PlayFor(3s);
	CHECK((Done(host, SIDE) == Stages{ 10, 20, 30 }));
	CHECK(host.Quests().Achievements().contains(SIDE_ACHIEVEMENT));
	CHECK(bob.Quests().Achievements().contains(SIDE_ACHIEVEMENT));

	// Nothing is sent back to the one who did it: each reported only its own.
	CHECK(host.Quests().reported == 2);
	CHECK(bob.Quests().reported == 1);
	CHECK(bob.Quests().WaitingCount() == 0);
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: a stage your game's script reaches straight after a friend's isn't sent back to them as yours, and your own step a few seconds later is")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE2, 5);
	s.PlayFor(1s);
	REQUIRE(Done(bob, SIDE2).contains(5));
	bob.Stage(SIDE2, 10);  // 5's script moving the quest on in the friend's game
	s.PlayFor(3s);
	CHECK(!Done(host, SIDE2).contains(10));
	CHECK(bob.Quests().reported == 0);

	s.PlayFor(1s);
	bob.Stage(SIDE2, 20);  // the friend's own doing
	s.PlayFor(2s);
	CHECK(Done(host, SIDE2).contains(20));
	CHECK(bob.Quests().reported == 1);
}

TEST("sim quests: with a shared story, Sanctuary moves on and ends in both games whoever plays it, and its achievement fires in each")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	Reach(host, MIN01, { 100, 200, 1000, 1500, 2000 });
	s.PlayFor(5s);
	CHECK((Done(bob, MIN01) == Stages{ 100, 200, 1000, 1500, 2000 }));
	CHECK(bob.Quests().Achievements().contains(SANCTUARY_ACHIEVEMENT));
	s.PlayFor(3s);  // a stage the friend's game reaches right after one set from the session is its script reacting, not news

	// The friend finishes it: the host's ends too.
	bob.Stage(MIN01, 2010);
	bob.Stage(MIN01, 2100);
	s.PlayFor(3s);
	CHECK(host.Quests().Of(MIN01).completed);
	CHECK(!host.Quests().Of(MIN01).running);
	CHECK(host.Quests().Achievements().contains(SANCTUARY_ACHIEVEMENT));
	CHECK(host.Quests().Skipped().empty());
	CHECK(bob.Quests().Skipped().empty());
}

TEST("sim quests: with each player's own story, a friend finishing Sanctuary moves the side quest on in your game but doesn't make you a Minuteman or end yours")
{
	Session s;  // sStory = own
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	Reach(bob, MIN01, { 100, 1000, 1500, 2000, 2010, 2100 });
	s.PlayFor(6s);
	CHECK((Done(host, MIN01) == Stages{ 100, 1000 }));
	CHECK(host.Quests().Of(MIN01).running);  // 2100 would have stopped it unfinished
	CHECK(!host.Quests().Of(MIN01).completed);
	CHECK(!host.Quests().Achievements().contains(SANCTUARY_ACHIEVEMENT));
	CHECK((host.Quests().Skipped() == History{ { MIN01, 1500 }, { MIN01, 2000 }, { MIN01, 2010 }, { MIN01, 2100 } }));
	CHECK(host.Quests().WaitingCount() == 0);  // skipped, not left waiting
}

TEST("sim quests: a friend's teleport into the Institute takes you along when you stand with them, after a warning when you don't, and you get your controls back in the Institute")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	host.Move(1000.0f, 1000.0f);
	bob.Move(1200.0f, 1000.0f);
	carol.Move(50000.0f, 1000.0f);  // across the map
	Settled(s);

	Reach(bob, MQ206, { 500, 1000, 1050, 1200 });
	CHECK(bob.Quests().Controls());  // his own game gave them back at 1200
	s.PlayFor(1s);
	CHECK(host.Quests().Has(MQ206, 500));
	CHECK(!host.Quests().Controls());  // at the teleporter
	s.PlayFor(15s);
	CHECK((Order(host, MQ206) == std::vector<std::uint16_t>{ 500, 1000, 1050, 1200 }));
	CHECK(host.Quests().Controls());
	CHECK(host.Quests().moved == 2);  // 500 and the teleporter itself
	CHECK(host.Quests().released == 0);
	CHECK(host.Quests().Achievements().contains(INSTITUTE_ACHIEVEMENT));
	CHECK(!host.Quests().Locked());
	CHECK(host.Quests().Notices().empty());  // no warning: it happened beside them

	// Carol, far away, is warned first and taken along 10 s later: nothing else starts the Institute.
	s.PlayFor(10s);
	REQUIRE(carol.Quests().Notices().size() == 1);
	CHECK(carol.Quests().Notices()[0].starts_with("Catching up with your friends' story in 10 s"));
	CHECK((Order(carol, MQ206) == std::vector<std::uint16_t>{ 500, 1000, 1050, 1200 }));
	CHECK(carol.Quests().Controls());
	CHECK(carol.Quests().Achievements().contains(INSTITUTE_ACHIEVEMENT));
}

// ---- catching up ------------------------------------------------------------------------------------

TEST("sim quests: a friend who joins late catches up on the session's stages in order, isn't moved by a step from the session, and gets the achievement")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(300.0f, 0.0f);
	Settled(s);

	Reach(host, SIDE, { 10, 20, 30, 40 });
	host.Stage(MS17, 150);  // moves the host: Bob stands with them and goes along
	host.Stage(MS17, 315);
	s.PlayFor(8s);
	bob.Stage(MS17, 331);
	s.PlayFor(3s);
	CHECK(bob.Quests().moved == 1);
	CHECK((Done(bob, MS17) == Stages{ 150, 315, 331 }));

	REQUIRE(carol.Join());
	s.PlayFor(20s);
	CHECK((Order(carol, SIDE) == std::vector<std::uint16_t>{ 10, 20, 30, 40 }));
	CHECK(carol.Quests().Achievements().contains(SIDE_ACHIEVEMENT));
	// The step that moved the host is caught up from the session, long after: not set, so Carol isn't
	// pulled across the map. The steps after it are.
	CHECK((Done(carol, MS17) == Stages{ 315, 331 }));
	CHECK((carol.Quests().Skipped() == History{ { MS17, 150 } }));
	CHECK(carol.Quests().moved == 0);
	CHECK(carol.Quests().caughtUp == 0);  // the quests were started in her game; nothing waited for them
	CHECK(carol.Quests().WaitingCount() == 0);
}

TEST("sim quests: a stage a friend reaches while your game is loading, paused or in a conversation waits, then is set, in order")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Quests().loading = true;  // the host walks through a door
	bob.Stage(SIDE, 10);
	bob.Stage(SIDE, 20);
	s.PlayFor(5s);
	CHECK(Done(host, SIDE).empty());
	CHECK((host.Quests().Waiting(SIDE) == StageLog::Stages{ 10, 20 }));
	host.Quests().loading = false;
	host.Quests().paused = true;  // ... and opens the Pip-Boy
	s.PlayFor(5s);
	CHECK(Done(host, SIDE).empty());
	host.Quests().paused = false;
	host.Quests().talking = true;  // ... and talks to a settler
	s.PlayFor(5s);
	CHECK(Done(host, SIDE).empty());
	host.Quests().talking = false;
	s.PlayFor(3s);
	CHECK((Order(host, SIDE) == std::vector<std::uint16_t>{ 10, 20 }));
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: a friend's progress in a quest you haven't come across yet pushes you along: it starts in your game, the host's too, in order")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	host.GiveQuest(SIDE2, NotStarted({ 5, 10, 20, 50 }));  // run in the background in the host's game
	auto notRunning = NotStarted({ 0, 10, 20, 30, 40 });
	notRunning.running = false;
	host.GiveQuest(LONG, notRunning);  // not even running there
	bob.GiveQuest(LONG, Started({ 0, 10, 20, 30, 40 }));
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	// Bob is the one talking to the quests' people: the host goes along.
	bob.Stage(SIDE2, 10);
	bob.Stage(SIDE2, 20);
	bob.Stage(SIDE, 10);
	Reach(bob, LONG, { 10, 20, 30 });
	s.PlayFor(10s);
	CHECK((Order(host, SIDE2) == std::vector<std::uint16_t>{ 10, 20 }));
	CHECK((Done(host, SIDE) == Stages{ 10 }));
	// Starting LONG ran its start-up stage, then Bob's stages in order.
	CHECK((Order(host, LONG) == std::vector<std::uint16_t>{ 0, 10, 20, 30 }));
	CHECK((host.Quests().started == std::vector<std::uint32_t>{ SIDE2, LONG }));
	CHECK(host.Quests().caughtUp == 2);
	CHECK(host.Quests().caughtUpStarted == 2);
	CHECK(host.Quests().WaitingCount() == 0);
	CHECK(host.Quests().reported == 0);  // what Bob pushed isn't sent back as the host's own

	// Now the quests go on whoever moves them: the host's next step reaches Bob.
	host.Stage(LONG, 40);
	s.PlayFor(3s);
	CHECK(bob.Quests().Has(LONG, 40));
}

TEST("sim quests: the host's progress pushes a friend who hasn't started the quest along, and three players all go along with whoever moves it")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	host.GiveQuest(SIDE2, Started({ 10, 20, 30, 40 }));
	auto fresh = NotStarted({ 10, 20, 30, 40 });
	fresh.running = false;
	bob.GiveQuest(SIDE2, fresh);
	carol.GiveQuest(SIDE2, fresh);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(300.0f, 0.0f);
	carol.Move(50000.0f, 0.0f);  // across the map: being pushed along doesn't need her near
	Settled(s);

	host.Stage(SIDE2, 10);
	host.Stage(SIDE2, 20);
	s.PlayFor(6s);
	for (const auto player : { &bob, &carol }) {
		CHECK((Order(*player, SIDE2) == std::vector<std::uint16_t>{ 10, 20 }));
		CHECK((player->Quests().started == std::vector<std::uint32_t>{ SIDE2 }));
	}

	// Carol talks to the quest's people next: the host and Bob go along.
	carol.Stage(SIDE2, 30);
	s.PlayFor(5s);
	CHECK(host.Quests().Has(SIDE2, 30));
	CHECK(bob.Quests().Has(SIDE2, 30));
	// Then Bob, a while after (a step straight after a friend's is taken for his game's script reacting to it).
	bob.Stage(SIDE2, 40);
	s.PlayFor(3s);
	for (const auto player : { &host, &bob, &carol }) {
		CHECK((Done(*player, SIDE2) == Stages{ 10, 20, 30, 40 }));
		CHECK(player->Quests().WaitingCount() == 0);
	}
	CHECK(carol.Quests().moved == 0);
}

TEST("sim quests: with a shared story, a friend who hasn't started the main story's quests is pushed through them, the next one started by the step that starts it")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	AtValentine(host);
	AtValentine(bob);
	auto valentine = bob.Quests().Of(MQ104);
	valentine.running = false;  // Bob hasn't come across Unlikely Valentine
	valentine.started = false;
	bob.GiveQuest(MQ104, valentine);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(300.0f, 0.0f);
	Settled(s);

	Reach(host, MQ104, { 200, 210 });  // 210 starts Getting a Clue at 10 and ...
	host.Stage(MQ104, 1000);
	host.Stage(MQ105, 100);
	s.PlayFor(10s);
	CHECK((Order(bob, MQ104) == std::vector<std::uint16_t>{ 200, 210, 1000 }));
	CHECK((Order(bob, MQ105) == std::vector<std::uint16_t>{ 10, 100 }));
	// MQ104 was started by the host's 200; MQ105 by Bob's own 210, never twice.
	CHECK((bob.Quests().started == std::vector<std::uint32_t>{ MQ104 }));
	CHECK(bob.Quests().WaitingCount() == 0);
}

TEST("sim quests: a quest that ran in your game and was stopped isn't started over by a friend's progress, and none is started outside a session")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	auto stopped = NotStarted({ 10, 20, 30 });
	stopped.running = false;
	stopped.done = { 10 };  // Bob failed it long ago
	stopped.current = 10;
	bob.GiveQuest(SIDE2, stopped);
	auto fresh = NotStarted({ 10, 20, 30 });
	fresh.running = false;
	bob.GiveQuest(LONG, fresh);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE2, 20);
	s.PlayFor(5s);
	CHECK((Done(bob, SIDE2) == Stages{ 10 }));
	CHECK((bob.Quests().Waiting(SIDE2) == StageLog::Stages{ 20 }));
	CHECK(bob.Quests().started.empty());

	bob.Quests().loading = true;  // a stage of a quest Bob hasn't started is waiting when he leaves
	host.Stage(LONG, 10);
	s.PlayFor(2s);
	bob.Leave();
	bob.Quests().loading = false;
	s.PlayFor(5s);
	CHECK(!bob.Quests().Of(LONG).running);
	CHECK(bob.Quests().started.empty());
}

TEST("sim quests: a friend joining mid-quest is caught up to where the host's story is, from before the host hosted, and nothing goes backwards")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	auto&   dave = s.Player("Dave");
	GiveQuests({ &host, &bob, &carol, &dave });
	// The host played SIDE2 to its 20 and SIDE to its 20 alone, before hosting.
	auto midway = Started({ 5, 10, 20, 50 });
	midway.done = { 5, 10, 20 };
	midway.current = 20;
	host.GiveQuest(SIDE2, midway);
	auto side = Started({ 10, 20, 30, 40 }, { { 30, SIDE_ACHIEVEMENT } });
	side.done = { 10, 20 };
	side.current = 20;
	host.GiveQuest(SIDE, side);
	auto background = NotStarted({ 1, 2 });
	background.done = { 1 };  // a quest the host's game moved on in the background: not the player's progress
	background.current = 1;
	host.GiveQuest(LONG, background);
	// Bob hasn't started SIDE2; Carol is further on in it than the host, and has a stage of her own the host hasn't.
	auto fresh = NotStarted({ 5, 10, 20, 50 });
	fresh.running = false;
	bob.GiveQuest(SIDE2, fresh);
	auto ahead = Started({ 5, 10, 20, 50 });
	ahead.done = { 5, 50 };
	ahead.current = 50;
	carol.GiveQuest(SIDE2, ahead);
	bob.GiveQuest(LONG, NotStarted({ 1, 2 }));

	REQUIRE(host.Join());
	s.PlayFor(8s);
	CHECK(host.Quests().baselined == 5);
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	s.PlayFor(10s);
	CHECK((Order(bob, SIDE2) == std::vector<std::uint16_t>{ 5, 10, 20 }));
	CHECK((Order(bob, SIDE) == std::vector<std::uint16_t>{ 10, 20 }));
	CHECK(Done(bob, LONG).empty());
	// Carol's quest is past the host's: nothing of it is set again, and her own 50 never reaches the host.
	CHECK(Order(carol, SIDE2).empty());
	CHECK((Done(carol, SIDE2) == Stages{ 5, 50 }));
	CHECK(!host.Quests().Has(SIDE2, 50));
	CHECK((Order(carol, SIDE) == std::vector<std::uint16_t>{ 10, 20 }));

	// The host goes on; Dave joins later and gets all of it, in order.
	host.Stage(SIDE, 30);
	s.PlayFor(3s);
	REQUIRE(dave.Join());
	s.PlayFor(10s);
	CHECK((Order(dave, SIDE) == std::vector<std::uint16_t>{ 10, 20, 30 }));
	CHECK(dave.Quests().Achievements().contains(SIDE_ACHIEVEMENT));
	CHECK((Order(dave, SIDE2) == std::vector<std::uint16_t>{ 5, 10, 20 }));
	CHECK(bob.Quests().Has(SIDE, 30));
}

TEST("sim quests: the host's talk at Vault 81's intercom and the vault door it opens reach the friends, though the host had no objective of it yet")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	host.GiveQuest(V81, AtVault81Door());
	auto started = AtVault81Door();
	started.started = true;  // Bob has the quest in his Pip-Boy
	bob.GiveQuest(V81, started);
	carol.GiveQuest(V81, AtVault81Door());  // Carol's game played the intercom's first lines too, no more
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	Settled(s);

	Reach(host, V81, { 20, 30, 50, 51 });  // the talk at the intercom
	s.PlayFor(3s);
	CHECK(!bob.Quests().Has(V81, 20));  // not yet: the host hasn't started the quest
	host.Stage(V81, 55);  // the overseer opens the door; its script shows the objective a moment later
	s.PlayFor(8s);

	CHECK(host.Quests().Achievements().contains(VAULT_DOOR_OPENS));
	CHECK(host.Quests().reported == 5);
	CHECK((Order(bob, V81) == std::vector<std::uint16_t>{ 20, 30, 50, 51, 55 }));
	CHECK(bob.Quests().Achievements().contains(VAULT_DOOR_OPENS));
	CHECK(bob.Quests().WaitingCount() == 0);
	// Carol hadn't started the quest: the host's talk pushes her along too, and her vault door opens.
	CHECK((Order(carol, V81) == std::vector<std::uint16_t>{ 20, 30, 50, 51, 55 }));
	CHECK(carol.Quests().Achievements().contains(VAULT_DOOR_OPENS));
	CHECK((carol.Quests().started == std::vector<std::uint32_t>{ V81 }));
	CHECK(carol.Quests().WaitingCount() == 0);
}

TEST("sim quests: with a shared story, the host's talk at Diamond City's gate opens the gate in the friends' games too, though the scene has no objective to start it")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	host.GiveQuest(DC_GATE, DiamondCityGate());
	auto atGate = DiamondCityGate();  // Bob stands at the gate too: his game played the argument up to its 30
	atGate.running = true;
	atGate.done = { 0, 1, 2, 3, 5, 7, 10, 20, 25, 30 };
	atGate.current = 30;
	bob.GiveQuest(DC_GATE, atGate);
	carol.GiveQuest(DC_GATE, DiamondCityGate());  // Carol is nowhere near
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	Settled(s);

	Reach(host, DC_GATE, { 0, 1, 2, 3, 5, 7, 10, 20, 25, 30, 50, 52, 55, 90, 95, 400 });  // the argument
	s.PlayFor(20s);
	Reach(host, DC_GATE, { 32, 40, 100 });  // the gate opens; the scene stops
	s.PlayFor(5s);

	CHECK(host.Quests().reported == 19);
	CHECK(!host.Quests().Of(DC_GATE).running);
	CHECK((Order(bob, DC_GATE) == std::vector<std::uint16_t>{ 50, 52, 55, 90, 95, 400, 32, 40, 100 }));
	CHECK(bob.Quests().Achievements().contains(DC_GATE_OPENS));
	CHECK(bob.Quests().started.empty());  // his game was running the scene already
	CHECK(bob.Quests().WaitingCount() == 0);
	CHECK((Order(carol, DC_GATE) == std::vector<std::uint16_t>{ 0, 1, 2, 3, 5, 7, 10, 20, 25, 30, 50, 52, 55, 90, 95, 400, 32, 40, 100 }));
	CHECK(carol.Quests().Achievements().contains(DC_GATE_OPENS));
	CHECK((carol.Quests().started == std::vector<std::uint32_t>{ DC_GATE }));
	CHECK(carol.Quests().WaitingCount() == 0);
	CHECK(bob.Quests().reported == 0);  // nothing set from the host's game goes back to it
	CHECK(carol.Quests().reported == 0);
}

TEST("sim quests: with a shared story, a friend whose own scene at Diamond City's gate got to its 400 without opening the gate gets the host's 32, 40 and 100, and the gate opens")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	host.GiveQuest(DC_GATE, AtDiamondCityGate());
	bob.GiveQuest(DC_GATE, AtDiamondCityGate());  // the live case: running, at 400, without 32, 40 and 100
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	Reach(host, DC_GATE, { 32, 40, 100 });  // Danny opens the gate in the host's game
	s.PlayFor(5s);

	// Below Bob's 400, but not done in his game: set, in the order they came, and the gate's script runs.
	CHECK((Order(bob, DC_GATE) == std::vector<std::uint16_t>{ 32, 40, 100 }));
	CHECK(bob.Quests().Achievements().contains(DC_GATE_OPENS));
	CHECK(!bob.Quests().Of(DC_GATE).running);  // the scene is over in his game too
	CHECK(bob.Quests().WaitingCount() == 0);
	CHECK(bob.Quests().started.empty());
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: a stage below where your quest already is, though you never did it, still isn't set from a friend's game unless the quest is a dialogue quest")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	auto ahead = Started({ 5, 10, 20, 50 });
	ahead.done = { 5, 50 };  // Bob's quest went from 5 straight to 50
	ahead.current = 50;
	bob.GiveQuest(SIDE2, ahead);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE2, 5);
	host.Stage(SIDE2, 20);
	s.PlayFor(5s);
	CHECK(host.Quests().reported == 2);
	CHECK(Order(bob, SIDE2).empty());
	CHECK(!bob.Quests().Has(SIDE2, 20));
	CHECK(bob.Quests().WaitingCount() == 0);
}

TEST("sim quests: with a shared story, a friend who joins after the host opened Diamond City's gate is caught up and their gate opens")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	auto opened = DiamondCityGate();  // the host went through the gate before hosting; the scene stopped
	opened.done = opened.stages;
	opened.current = 400;
	host.GiveQuest(DC_GATE, opened);
	bob.GiveQuest(DC_GATE, DiamondCityGate());

	REQUIRE(host.Join());
	s.PlayFor(8s);
	CHECK(host.Quests().baselined == 19);
	REQUIRE(bob.Join());
	s.PlayFor(20s);
	// The baseline comes lowest first, so 100 stops Bob's scene before 400: the scene is over in his game,
	// and 400 is dropped rather than left waiting for good.
	CHECK((Order(bob, DC_GATE) == std::vector<std::uint16_t>{ 0, 1, 2, 3, 5, 7, 10, 20, 25, 30, 32, 40, 50, 52, 55, 90, 95, 100 }));
	CHECK(bob.Quests().Achievements().contains(DC_GATE_OPENS));
	CHECK(!bob.Quests().Of(DC_GATE).running);
	CHECK(bob.Quests().WaitingCount() == 0);
}

TEST("sim quests: what your game did in a quest just before you started it reaches your friends, and what it did in the background long before doesn't")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	auto background = NotStarted({ 5, 10, 20, 50 });
	background.quiet = { 5 };
	host.GiveQuest(SIDE2, background);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE2, 5);  // the game moves it on in the background
	s.PlayFor(2min);
	host.Stage(SIDE2, 10);  // the host starts it, minutes later
	s.PlayFor(5s);
	CHECK((Order(bob, SIDE2) == std::vector<std::uint16_t>{ 10 }));

	auto quick = NotStarted({ 5, 10, 20, 30, 40 });
	quick.quiet = { 5, 10 };
	quick.showsLater = { 20 };
	host.GiveQuest(SIDE, quick);
	bob.GiveQuest(SIDE, Started({ 5, 10, 20, 30, 40 }));
	s.PlayFor(1s);
	host.Stage(SIDE, 5);  // a conversation that leads to the quest
	s.PlayFor(20s);
	host.Stage(SIDE, 10);
	s.PlayFor(1s);
	host.Stage(SIDE, 20);  // whose script shows the first objective a moment after the stage reads done
	s.PlayFor(5s);
	CHECK((Order(bob, SIDE) == std::vector<std::uint16_t>{ 5, 10, 20 }));
	host.Stage(SIDE, 30);
	s.PlayFor(3s);
	CHECK((Order(bob, SIDE) == std::vector<std::uint16_t>{ 5, 10, 20, 30 }));
}

// ---- leaving, solo play, coming back --------------------------------------------------------------

TEST("sim quests: after leaving, a stage still waiting from the session is never set in your solo game, and your solo progress never reaches the session")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	bob.GiveQuest(SIDE2, NotStarted({ 5, 10, 20, 50 }));
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE2, 5);
	host.Stage(SIDE2, 10);
	bob.Quests().loading = true;
	host.Stage(SIDE, 10);
	s.PlayFor(3s);
	CHECK((bob.Quests().Waiting(SIDE2) == StageLog::Stages{ 5, 10 }));
	CHECK((bob.Quests().Waiting(SIDE) == StageLog::Stages{ 10 }));

	bob.Leave();
	bob.Quests().loading = false;
	bob.Stage(SIDE2, 5);  // he starts it alone
	s.PlayFor(5s);
	CHECK((Done(bob, SIDE2) == Stages{ 5 }));  // not caught up to the session's 10
	CHECK(Done(bob, SIDE).empty());
	CHECK(bob.Quests().WaitingCount() == 0);

	// Alone, he gets further in a quest of his own; then he joins again.
	bob.Stage(SIDE, 40);
	s.PlayFor(3s);
	REQUIRE(bob.Join());
	s.PlayFor(5s);
	CHECK(!host.Quests().Has(SIDE, 40));
	auto& carol = s.Player("Carol");
	GiveQuests({ &carol });
	REQUIRE(carol.Join());
	s.PlayFor(5s);
	CHECK(!carol.Quests().Has(SIDE, 40));
	CHECK(carol.Quests().Has(SIDE, 10));  // what the session did reaches her
	// Back in the session, Bob's game takes the session's stages again (his own quest is past SIDE's 10).
	CHECK((Done(bob, SIDE2) == Stages{ 5, 10 }));
}

TEST("sim quests: a stage you reach while the connection is being remade reaches your friends once it is back")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	s.StopServer();  // the host's server goes down; Bob's game keeps trying
	bob.Stage(SIDE, 10);
	s.PlayFor(2s);
	s.StartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	s.PlayFor(3s);
	CHECK(host.Quests().Has(SIDE, 10));
}

TEST("sim quests: with a shared story, the main story step you finish while your connection drops reaches your friends once it is back, and the next quest starts in their game too")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	AtValentine(host);
	AtValentine(bob);
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(100.0f, 0.0f);
	Settled(s);

	bob.Stage(MQ104, 200);
	s.PlayFor(4s);
	REQUIRE(host.Quests().Has(MQ104, 200));

	bob.LoseConnection();  // a relay hiccup; his game keeps trying
	bob.Stage(MQ104, 210);  // the talk with Valentine ends: its script starts Getting a Clue
	bob.Stage(MQ104, 1000);
	s.PlayFor(2s);
	REQUIRE(bob.Join());
	s.PlayFor(1s);
	bob.Stage(MQ105, 100);
	s.PlayFor(6s);
	CHECK((Order(host, MQ104) == std::vector<std::uint16_t>{ 200, 210, 1000 }));
	CHECK(host.Quests().Of(MQ105).running);  // 210's script ran in the host's game too
	CHECK((Done(host, MQ105) == Stages{ 10, 100 }));
	CHECK(host.Quests().WaitingCount() == 0);  // nothing left waiting for a quest that never starts
}

TEST("sim quests: with a shared story, the host's own story progress while their server restarts reaches the friends")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	s.StopServer();  // a restart: the host's own game loses its connection too
	host.Stage(MQ105, 100);
	s.PlayFor(2s);
	s.StartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	s.PlayFor(3s);
	CHECK(bob.Quests().Has(MQ105, 100));
}

TEST("sim quests: hundreds of stages reached while the connection was down all reach the host once it is back, none lost to the server's flood limit")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	Stages many;
	for (std::uint16_t stage = 1; stage <= 600; ++stage) {
		many.insert(stage);
	}
	host.GiveQuest(LONG, Started(many));
	bob.GiveQuest(LONG, Started(many));
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	bob.LoseConnection();
	for (const auto stage : many) {
		bob.Stage(LONG, stage);
	}
	s.PlayFor(2s);
	REQUIRE(bob.Join());
	s.PlayInRealTime(4s);
	CHECK(!s.Logged("sends too many events"));
	auto arrived = Done(host, LONG);  // set in the host's game, or waiting their turn there
	for (const auto stage : host.Quests().Waiting(LONG)) {
		arrived.insert(stage);
	}
	CHECK(arrived == many);
}

TEST("sim quests: what you do alone for minutes while your game keeps trying a host who quit isn't sent when the host comes back")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	s.StopServer();  // the host quits for the night; Bob plays on, his game still trying the host
	bob.Stage(SIDE, 10);
	s.PlayFor(60s);
	bob.Stage(SIDE2, 5);
	s.PlayFor(10min);
	s.StartServer();  // the host is back, and Bob's game, still trying, gets in
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	s.PlayFor(5s);
	CHECK(!host.Quests().Has(SIDE, 10));
	CHECK(!host.Quests().Has(SIDE2, 5));

	bob.Stage(SIDE, 20);  // in the session again, his progress is shared again
	s.PlayFor(3s);
	CHECK(host.Quests().Has(SIDE, 20));
}

TEST("sim quests: what you do alone after your host quits never reaches the next friend's session you join")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE, 10);
	s.PlayFor(2s);
	REQUIRE(bob.Quests().Has(SIDE, 10));
	s.StopServer();  // the host quits; Bob plays on, his game still trying the host
	bob.Stage(SIDE, 20);
	bob.Stage(SIDE2, 5);
	s.PlayFor(30s);

	s.ServeAnotherSession();  // Carol hosts a session of her own, and invites Bob
	auto& carol = s.Host("Carol");
	GiveQuests({ &carol });
	REQUIRE(carol.Join());
	REQUIRE(bob.SwitchServer());
	s.PlayFor(5s);
	CHECK(Done(carol, SIDE).empty());
	CHECK(Done(carol, SIDE2).empty());
	auto& dave = s.Player("Dave");  // the session's own record has none of it either
	GiveQuests({ &dave });
	REQUIRE(dave.Join());
	s.PlayFor(5s);
	CHECK(Done(dave, SIDE).empty());
	CHECK(Done(dave, SIDE2).empty());

	bob.Stage(SIDE, 30);  // what he does in Carol's session is shared there
	s.PlayFor(3s);
	CHECK(carol.Quests().Has(SIDE, 30));
}

TEST("sim quests: a host who stops hosting, plays alone and hosts again starts the session from where their story is now")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Leave();  // Session::StopHosting: its own connection first, then the server
	s.StopServer();
	host.Stage(SIDE, 10);
	s.PlayFor(3s);
	s.StartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	s.PlayFor(5s);
	// The session's story is the host's (QuestSync::QueueBaseline): Bob is caught up to it.
	CHECK(bob.Quests().Has(SIDE, 10));
	CHECK(host.Quests().baselined == 1);
}

TEST("sim quests: a friend who rejoins catches up on what the others did while they were away, and nothing twice")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE, 10);
	s.PlayFor(2s);
	bob.Leave();
	host.Stage(SIDE, 20);
	host.Stage(SIDE, 30);
	s.PlayFor(2s);
	REQUIRE(bob.Join());
	s.PlayFor(5s);
	CHECK((Order(bob, SIDE) == std::vector<std::uint16_t>{ 10, 20, 30 }));
	CHECK(bob.Quests().Achievements().contains(SIDE_ACHIEVEMENT));
}

TEST("sim quests: after the host's server restarts from its session file, the shared stages are still the session's: a friend away meanwhile and a late joiner catch up")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	host.Stage(SIDE, 10);
	bob.Stage(MIN01, 1000);
	s.PlayFor(3s);
	bob.Leave();
	host.Stage(SIDE, 20);
	host.Stage(MIN01, 1500);
	host.Stage(MIN01, 2000);
	s.PlayFor(3s);
	const auto hostApplied = host.Quests().applied;

	s.RestartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	auto& carol = s.Player("Carol");
	GiveQuests({ &carol });
	REQUIRE(carol.Join());
	s.PlayFor(10s);
	CHECK(s.Logged("loaded the saved session"));
	for (const auto player : { &bob, &carol }) {
		CHECK((Done(*player, SIDE) == Stages{ 10, 20 }));
		CHECK((Done(*player, MIN01) == Stages{ 1000, 1500, 2000 }));
		CHECK(player->Quests().Achievements().contains(SANCTUARY_ACHIEVEMENT));
	}
	CHECK((Order(carol, MIN01) == std::vector<std::uint16_t>{ 1000, 1500, 2000 }));
	CHECK(host.Quests().applied == hostApplied);  // its own stages, back from the file, are nothing new to it
}

TEST("sim quests: a friend who loads a save from before the session's stages gets them back, in order")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);
	const auto save = bob.SaveGame();

	host.Stage(SIDE, 10);
	host.Stage(SIDE, 20);
	s.PlayFor(3s);
	REQUIRE((Done(bob, SIDE) == Stages{ 10, 20 }));

	bob.LoadGame(save);
	CHECK(Done(bob, SIDE).empty());
	s.PlayFor(8s);
	CHECK((Done(bob, SIDE) == Stages{ 10, 20 }));
	CHECK(bob.Quests().reported == 0);  // the stages set again aren't his own progress
}

// ---- what a friend's game can't make yours do -----------------------------------------------------

TEST("sim quests: a friend's game that says another player did a step can't move you with it: the server says who did it")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	GiveQuests({ &host, &bob, &carol });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	host.Move(0.0f, 0.0f);
	carol.Move(200.0f, 0.0f);  // with the host
	bob.Move(60000.0f, 0.0f);  // across the map
	Settled(s);

	// Bob's game claims the host moved on to the step that teleports; it was Bob.
	bob.Raw(Protocol::Encode(Protocol::QuestStage{ MS17, 150, host.Id() }, Protocol::MessageType::kReportQuestStage));
	s.PlayFor(3s);
	CHECK(!carol.Quests().Has(MS17, 150));
	CHECK(!host.Quests().Has(MS17, 150));
	CHECK(carol.Quests().moved == 0);
	CHECK(host.Quests().moved == 0);
	CHECK((carol.Quests().Skipped() == History{ { MS17, 150 } }));

	// A step Bob really does takes along the friend who stands with him, and only her.
	carol.Move(60100.0f, 0.0f);
	s.PlayFor(1s);
	bob.Stage(MS17, 175);
	s.PlayFor(3s);
	CHECK(carol.Quests().Has(MS17, 175));
	CHECK(carol.Quests().moved == 1);
	CHECK(!host.Quests().Has(MS17, 175));
	CHECK((host.Quests().Skipped() == History{ { MS17, 150 }, { MS17, 175 } }));
	bob.Stage(MS17, 315);  // a step that moves nobody reaches everyone
	s.PlayFor(3s);
	CHECK(host.Quests().Has(MS17, 315));
	CHECK(carol.Quests().Has(MS17, 315));
}

TEST("sim quests: a friend's bribe isn't paid from your caps, a stage your quest doesn't have is ignored, and neither holds up what comes after")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	bob.Stage(MQ105, 100);
	bob.Stage(MQ105, 177);  // Bob pays the bribe
	bob.Stage(MQ105, 200);
	bob.Raw(Protocol::Encode(Protocol::QuestStage{ SIDE, 999 }, Protocol::MessageType::kReportQuestStage));  // no such stage
	bob.Stage(SIDE, 10);
	s.PlayFor(4s);
	CHECK(bob.Quests().paid == 1);
	CHECK(host.Quests().paid == 0);
	CHECK((Done(host, MQ105) == Stages{ 100, 200 }));
	CHECK((host.Quests().Skipped() == History{ { MQ105, 177 } }));
	CHECK(host.Quests().unknown == 1);
	CHECK((Done(host, SIDE) == Stages{ 10 }));
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: a friend's game that sends only the step that takes your controls can't freeze you, and your other quests go on")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(100.0f, 0.0f);
	Settled(s);

	bob.Raw(Protocol::Encode(Protocol::QuestStage{ MQ206, 500 }, Protocol::MessageType::kReportQuestStage));
	bob.Stage(SIDE, 10);
	s.PlayFor(120s);
	CHECK(host.Quests().Controls());
	CHECK(!host.Quests().Has(MQ206, 500));
	CHECK((host.Quests().Waiting(MQ206) == StageLog::Stages{ 500 }));  // until the step that gives them back comes
	CHECK(host.Quests().Has(SIDE, 10));
}

// ---- the Institute's teleporter -------------------------------------------------------------------

TEST("sim quests: pausing for two minutes at the Institute teleporter doesn't give you your controls back and leave you behind")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(100.0f, 0.0f);
	Settled(s);

	Reach(bob, MQ206, { 500, 1000, 1050, 1200 });
	s.PlayFor(1s);
	REQUIRE(host.Quests().Has(MQ206, 500));
	REQUIRE(!host.Quests().Controls());
	host.Quests().paused = true;  // the host alt-tabs at the teleporter
	s.PlayFor(120s);
	CHECK(!host.Quests().Has(MQ206, 1000));
	CHECK(!host.Quests().Controls());
	host.Quests().paused = false;
	s.PlayFor(15s);
	CHECK((Order(host, MQ206) == std::vector<std::uint16_t>{ 500, 1000, 1050, 1200 }));  // taken through the teleporter
	CHECK(host.Quests().Controls());
	CHECK(host.Quests().released == 0);  // not by the timeout
}

TEST("sim quests: leaving the session in the middle of the Institute teleport still takes you through it and gives your controls back, and nothing else of the session follows you")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(100.0f, 0.0f);
	Settled(s);

	// Bob hasn't come across this one yet: it would start in his game once the teleport's steps before it are set.
	bob.GiveQuest(SIDE2, NotStarted({ 5, 10, 20, 50 }));
	Reach(host, MQ206, { 500, 1000, 1050, 1200 });
	s.PlayFor(500ms);
	host.Stage(SIDE2, 10);
	s.PlayFor(1s);
	REQUIRE(bob.Quests().Has(MQ206, 500));
	REQUIRE(!bob.Quests().Controls());
	REQUIRE((bob.Quests().Waiting(SIDE2) == StageLog::Stages{ 10 }));
	bob.Quests().paused = true;  // he alt-tabs at the teleporter, and the connection drops meanwhile
	s.PlayFor(1s);

	bob.Leave();  // his story is his own again (Story::SetShared(false))
	bob.Quests().paused = false;
	bob.Stage(SIDE2, 5);  // alone, he picks up the quest the host was further in
	s.PlayFor(15s);
	CHECK((Order(bob, MQ206) == std::vector<std::uint16_t>{ 500, 1000, 1050, 1200 }));
	CHECK(bob.Quests().Controls());
	CHECK(bob.Quests().released == 0);
	CHECK(bob.Quests().Achievements().contains(INSTITUTE_ACHIEVEMENT));
	CHECK((Done(bob, SIDE2) == Stages{ 5 }));  // the session's 10 stays out of his solo game
	CHECK(!bob.Quests().Locked());
	CHECK(bob.Quests().WaitingCount() == 0);
}

TEST("sim quests: leaving mid-teleport with more of the session's Institute steps waiting gives your controls back, and nothing past that step is set in your solo game")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	for (const auto player : { &host, &bob }) {
		auto quest = MolecularLevel();
		quest.stages.insert(1300);  // a step of the session's after the one that gives the controls back
		player->GiveQuest(MQ206, std::move(quest));
	}
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(0.0f, 0.0f);
	bob.Move(100.0f, 0.0f);
	Settled(s);

	Reach(host, MQ206, { 500, 1000, 1050, 1200, 1300 });
	s.PlayFor(1s);
	REQUIRE(bob.Quests().Has(MQ206, 500));
	REQUIRE(!bob.Quests().Controls());
	bob.Quests().paused = true;  // he alt-tabs at the teleporter
	s.PlayFor(1s);

	bob.Leave();
	bob.Quests().paused = false;
	s.PlayFor(15s);
	CHECK((Order(bob, MQ206) == std::vector<std::uint16_t>{ 500, 1000, 1050, 1200 }));
	CHECK(bob.Quests().Controls());
	CHECK(!bob.Quests().Locked());
	CHECK(bob.Quests().WaitingCount() == 0);  // nothing of the session is left to be set in the next one he joins
}


// ---- Out of Time: Vault 111, then Codsworth in Sanctuary ------------------------------------------------

namespace
{
	// Fallout4.esm's, read off its quest records and fragments (QF_MQ102_0001CC2A, QF_DialogueCodsworthPostWar_0001E64E).
	constexpr std::uint32_t MQ102 = 0x0001CC2A;      // Out of Time: main quest (type 1); below 10 the vault's, each player's own
	constexpr std::uint32_t CODSWORTH = 0x0001E64E;  // DialogueCodsworthPostWar: type 0, no objectives, runs from the start; his talk and search
	constexpr std::uint32_t VAULT111 = 0x0001ED62;   // DialogueVault111: the vault's own scenes, each player's own
	constexpr std::uint32_t MQ103 = 0x000229E5;      // When Freedom Calls
	// Codsworth's search of Sanctuary's houses (MQ102_CodsworthSearch01): a scene of his quest with Codsworth and the
	// player as its actors, started by his 50's script and stopped by his 100's. A scene of the player's own, for
	// contrast: Preston's balcony scene (MQ103DetectiveSignScene, of When Freedom Calls, which nothing here moves).
	constexpr std::uint32_t SEARCH_SCENE = 0x0019E137;
	constexpr std::uint32_t OWN_SCENE = 0x000865AB;

	// Out of Time from the cryopod to Concord. 1 to 9 are the vault (each player's own), 10 the trigger at the top of
	// the lift, 15 on the way home, 20 "Talk to Codsworth", 22 his holotape, 25 the bloatflies, 30 to 38 the search (he
	// walks off and stops at the houses; 34 or 38, whichever comes second, sets 40), 40 sets Codsworth's 100 (his
	// search scene stops), 50 "Look for help in Concord" (it completes 10 to 34), 60 starts Preston's balcony scene.
	Quest OutOfTime()
	{
		auto quest = Started({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 22, 25, 30, 32, 34, 36, 38, 40, 50, 60, 65, 70, 150, 200 });
		quest.story = true;
		quest.starts = { { 38, { MQ102, 40 } }, { 40, { CODSWORTH, 100 } } };
		return quest;
	}

	// Codsworth's own quest: his 50 is the talk, which sets Out of Time's 30 and starts his search scene (the player in
	// it); his 100, set by Out of Time's 40, stops the scene; his 200, back with him after it, sets its 50. These are
	// in StageRules::SETS_ANOTHER (with his 275, and Out of Time's 34, 38 and 40).
	Quest CodsworthPostWar()
	{
		Quest quest;
		quest.stages = { 5, 15, 20, 30, 35, 50, 100, 200, 250, 275, 300, 500 };
		quest.quiet = quest.stages;
		quest.story = true;
		quest.dialogue = true;
		quest.running = true;
		quest.starts = { { 50, { MQ102, 30 } }, { 200, { MQ102, 50 } } };
		quest.startsScene = { { 50, { SEARCH_SCENE, CODSWORTH } } };
		quest.stopsScene = { { 100, SEARCH_SCENE } };
		quest.stops = { 500 };
		return quest;
	}

	Quest Vault111Scenes()
	{
		Quest quest;
		quest.stages = { 40, 55, 95, 97, 98, 100, 110 };
		quest.quiet = quest.stages;
		quest.story = true;
		quest.dialogue = true;
		quest.running = true;
		return quest;
	}

	// A new character in the cryopod: the vault's stages so far done, still in the opening.
	void InTheVault(Client& a_player, std::initializer_list<std::uint16_t> a_done)
	{
		auto outOfTime = OutOfTime();
		for (const auto stage : a_done) {
			outOfTime.done.insert(stage);
			outOfTime.current = (std::max)(outOfTime.current, stage);
		}
		a_player.GiveQuest(MQ102, std::move(outOfTime));
		a_player.GiveQuest(CODSWORTH, CodsworthPostWar());
		a_player.GiveQuest(VAULT111, Vault111Scenes());
		a_player.Quests().opening = true;
	}

	// Out of the vault: the trigger at the top of the lift.
	void OutOfTheVault(Client& a_player)
	{
		a_player.Quests().opening = false;
		a_player.Stage(MQ102, 10);
	}

	// Home, Codsworth's talk, his search and Concord's lead, as one player plays them: a few seconds between steps,
	// so each goes in a report of its own (what is done in one poll goes in one, in the game's order of quests).
	void Sanctuary(Session& a_session, Client& a_player)
	{
		const auto step = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
			a_player.Stage(a_quest, a_stage);
			a_session.PlayFor(3s);
		};
		step(MQ102, 15);
		step(MQ102, 20);
		step(MQ102, 22);
		step(MQ102, 25);
		step(CODSWORTH, 50);  // the talk: Out of Time's 30, and he walks off searching
		step(MQ102, 32);
		step(MQ102, 34);
		step(MQ102, 36);
		step(MQ102, 38);       // its script sets 40, whose sets his 100
		step(CODSWORTH, 200);  // back with him: Out of Time's 50
		step(MQ102, 60);       // Concord
	}

	const std::vector<std::uint16_t> OUT_OF_SANCTUARY{ 15, 20, 22, 25, 30, 32, 34, 36, 38, 40, 50, 60 };
	const std::vector<std::uint16_t> CODSWORTH_SEARCH{ 50, 100, 200 };

	// The stages of Out of Time set in this game past the vault's lift (10), in order.
	std::vector<std::uint16_t> PastTheVault(const Client& a_player)
	{
		auto order = Order(a_player, MQ102);
		std::erase_if(order, [](std::uint16_t a_stage) { return a_stage <= 10; });
		return order;
	}

	// Host and friend both out of the vault, in a session.
	void BothOut(Session& a_session, Client& a_host, Client& a_friend)
	{
		GiveQuests({ &a_host, &a_friend });
		InTheVault(a_host, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
		a_host.Quests().opening = false;
		InTheVault(a_friend, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
		a_friend.Quests().opening = false;
		REQUIRE(a_host.Join());
		REQUIRE(a_friend.Join());
		Settled(a_session);
	}
}

TEST("sim quests: with a shared story, a friend's talk with Codsworth and his search of Sanctuary move the host's Out of Time along as they play it")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	Sanctuary(s, bob);
	s.PlayFor(10s);
	CHECK(PastTheVault(host) == OUT_OF_SANCTUARY);
	CHECK(Order(host, CODSWORTH) == CODSWORTH_SEARCH);  // his search started and stopped in the host's game too
	CHECK(host.Quests().WaitingCount() == 0);
	CHECK(host.Quests().Skipped().empty());
	CHECK(PastTheVault(bob) == OUT_OF_SANCTUARY);  // nothing came back to Bob as the host's
}

TEST("sim quests: with a shared story, a host who was in a conversation while a friend did Codsworth's search catches up on all of it in order, none of it lost to Codsworth's stages")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	host.Quests().talking = true;  // the host talks with someone else the whole time
	Sanctuary(s, bob);
	CHECK(PastTheVault(host).empty());
	CHECK(host.Quests().WaitingCount() == 15);  // 12 of Out of Time and 3 of Codsworth's

	// All at once: Codsworth's 50 and 200 set Out of Time's 30 and 50. Set ahead of the stages of it that came before
	// them, those would read as behind it and be dropped: 0.9.1 left the host without 20 to 25, and the end of the
	// search (36 to 40) never ran.
	host.Quests().talking = false;
	s.PlayFor(30s);
	CHECK(PastTheVault(host) == OUT_OF_SANCTUARY);
	CHECK(Order(host, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(host.Quests().WaitingCount() == 0);
	CHECK(host.Quests().Skipped().empty());
}

TEST("sim quests: with a shared story, the host's talk with Codsworth and his search reach a friend once they are out of the vault, and a late joiner too; the vault stays each player's")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	auto&   old = s.Player("Old");  // an older version's game: it reports the vault's stages, which 0.9.0 stopped sharing
	GiveQuests({ &host, &bob, &carol });
	InTheVault(host, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
	host.Quests().opening = false;
	InTheVault(bob, { 0, 1, 2 });
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);
	CHECK(host.Quests().baselined == 1);  // its 10; not the vault's 0 to 9

	Sanctuary(s, host);
	REQUIRE(old.Join());
	old.Stage(MQ102, 4);      // the Pip-Boy's effect
	old.Stage(VAULT111, 40);  // into the cryopod
	s.PlayFor(10s);
	CHECK((Done(bob, MQ102) == Stages{ 0, 1, 2 }));  // still in the vault: it all waits
	CHECK(bob.Quests().openingDropped == 2);         // the old game's vault stages are dropped, not kept for later

	Reach(bob, MQ102, { 3, 4, 5, 6, 7, 8, 9 });
	OutOfTheVault(bob);
	s.PlayFor(30s);
	CHECK(PastTheVault(bob) == OUT_OF_SANCTUARY);
	CHECK(Order(bob, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(Done(bob, VAULT111).empty());
	CHECK(Done(host, VAULT111).empty());
	CHECK(bob.Quests().WaitingCount() == 0);
	CHECK(bob.Quests().Skipped().empty());

	// Carol joins later, a new character in the vault: she plays it herself, then catches up from the session.
	InTheVault(carol, { 0, 1 });
	REQUIRE(carol.Join());
	s.PlayFor(8s);
	CHECK((Done(carol, MQ102) == Stages{ 0, 1 }));
	Reach(carol, MQ102, { 2, 3, 4, 5, 6, 7, 8, 9 });
	OutOfTheVault(carol);
	s.PlayFor(30s);
	CHECK(PastTheVault(carol) == OUT_OF_SANCTUARY);
	CHECK(Order(carol, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(Done(carol, VAULT111).empty());
	CHECK(carol.Quests().Skipped().empty());
}

TEST("sim quests: with a shared story, a friend's talk with Codsworth starts his search scene in the host's game too, and the friend's stages carry the host through it instead of the scene holding them back")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	const auto step = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		bob.Stage(a_quest, a_stage);
		s.PlayFor(3s);
	};
	step(MQ102, 15);
	step(MQ102, 20);
	step(MQ102, 22);
	step(MQ102, 25);
	step(CODSWORTH, 50);  // the talk: set here, its script starts the search with the host's player in it
	CHECK(Done(host, MQ102).contains(30));
	CHECK(host.Quests().PlayerScene() == SEARCH_SCENE);
	CHECK(host.Quests().SessionScene() == SEARCH_SCENE);  // the session's, not the host's own: it holds nothing back
	// Codsworth searches the houses in Bob's game; 0.9.2 held these back as long as the host was in the scene, which
	// only 40 ends (it sets Codsworth's 100): the host had to follow Codsworth through the search themselves.
	step(MQ102, 32);
	CHECK(Done(host, MQ102).contains(32));
	step(MQ102, 34);
	step(MQ102, 36);
	step(MQ102, 38);  // its script sets 40, whose sets his 100, which stops the scene
	CHECK(Done(host, MQ102).contains(40));
	CHECK(Done(host, CODSWORTH).contains(100));
	CHECK(host.Quests().PlayerScene() == 0);
	CHECK(host.Quests().SessionScene() == 0);
	step(CODSWORTH, 200);
	step(MQ102, 60);
	s.PlayFor(10s);
	CHECK(PastTheVault(host) == OUT_OF_SANCTUARY);
	CHECK(Order(host, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(host.Quests().WaitingCount() == 0);
	CHECK(host.Quests().Skipped().empty());
	CHECK(bob.Quests().PlayerScene() == 0);  // nothing came back to Bob as the host's
}

TEST("sim quests: with a shared story, a friend whose connection drops in the middle of the host's Codsworth search is still carried through it once back")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	const auto step = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		host.Stage(a_quest, a_stage);
		s.PlayFor(3s);
	};
	step(MQ102, 15);
	step(MQ102, 20);
	step(MQ102, 22);
	step(MQ102, 25);
	step(CODSWORTH, 50);  // the search starts in Bob's game too, Bob in it
	CHECK(bob.Quests().SessionScene() == SEARCH_SCENE);

	// A Steam hiccup: Bob's session ends and is made again, his player still in the search scene. Taken for his own
	// then, it would hold the rest of the search back again, and only its end (40) ends it.
	bob.LoseConnection();
	s.PlayFor(5s);
	REQUIRE(bob.Join());
	s.PlayFor(3s);
	CHECK(bob.Quests().SessionScene() == SEARCH_SCENE);
	step(MQ102, 32);
	CHECK(Done(bob, MQ102).contains(32));  // as it comes, not held back
	step(MQ102, 34);
	step(MQ102, 36);
	step(MQ102, 38);
	CHECK(Done(bob, MQ102).contains(40));
	CHECK(bob.Quests().PlayerScene() == 0);
	step(CODSWORTH, 200);
	step(MQ102, 60);
	s.PlayFor(30s);
	CHECK(PastTheVault(bob) == OUT_OF_SANCTUARY);
	CHECK(Order(bob, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(bob.Quests().PlayerScene() == 0);
	CHECK(bob.Quests().WaitingCount() == 0);
}

TEST("sim quests: with a shared story, a scene the host walks into right after a friend's stage came is the host's own: the friend's next stages wait for it")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	bob.Stage(MQ102, 15);
	s.PlayFor(3s);
	REQUIRE(Done(host, MQ102).contains(15));
	host.Quests().EnterScene(OWN_SCENE, MQ103);  // a moment after it: Preston's, not anything 15 started
	s.PlayFor(1s);
	CHECK(host.Quests().SessionScene() == 0);
	bob.Stage(MQ102, 20);
	s.PlayFor(5s);
	CHECK(!Done(host, MQ102).contains(20));  // not in the middle of the host's own scene

	host.Quests().LeaveScene();
	s.PlayFor(5s);
	CHECK(Done(host, MQ102).contains(20));
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: with a shared story, the host's own scene still holds a friend's Codsworth search back until they are out of it")
{
	Session s{ SharedStory() };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	host.Quests().EnterScene(OWN_SCENE, MQ103);  // the host walked into a scene of their own, of a quest nothing moves
	Sanctuary(s, bob);
	CHECK(PastTheVault(host).empty());
	CHECK(host.Quests().SessionScene() == 0);
	CHECK(host.Quests().WaitingCount() == 15);  // 12 of Out of Time and 3 of Codsworth's

	host.Quests().LeaveScene();
	s.PlayFor(30s);
	CHECK(PastTheVault(host) == OUT_OF_SANCTUARY);
	CHECK(Order(host, CODSWORTH) == CODSWORTH_SEARCH);
	CHECK(host.Quests().PlayerScene() == 0);  // the search scene started here by Codsworth's 50 and was stopped by his 100
	CHECK(host.Quests().WaitingCount() == 0);
}

TEST("sim quests: with each player's own story, a friend's Codsworth search never reaches the host's Out of Time")
{
	Session s;  // sStory = own
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	BothOut(s, host, bob);

	Sanctuary(s, bob);
	s.PlayFor(10s);
	CHECK(PastTheVault(host).empty());  // the host talks to Codsworth and follows him around Sanctuary themselves
	CHECK(Done(host, CODSWORTH).empty());
	CHECK(host.Quests().WaitingCount() == 0);
}
