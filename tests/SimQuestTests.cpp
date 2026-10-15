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

	constexpr const char* SIDE_ACHIEVEMENT = "the side quest's achievement";
	constexpr const char* SANCTUARY_ACHIEVEMENT = "Sanctuary's achievement";
	constexpr const char* INSTITUTE_ACHIEVEMENT = "the Institute's achievement";
	constexpr const char* VAULT_DOOR_OPENS = "Vault 81's door and walkway open";

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

TEST("sim quests: a friend's progress in a quest you haven't come across yet waits until you start it, then catches you up")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	GiveQuests({ &host, &bob });
	host.GiveQuest(SIDE2, NotStarted({ 5, 10, 20, 50 }));
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	Settled(s);

	bob.Stage(SIDE2, 10);
	bob.Stage(SIDE2, 20);
	bob.Stage(SIDE, 10);  // a quest both have started isn't held up by it
	s.PlayFor(5s);
	CHECK(Done(host, SIDE2).empty());
	CHECK((Done(host, SIDE) == Stages{ 10 }));
	CHECK((host.Quests().Waiting(SIDE2) == StageLog::Stages{ 10, 20 }));

	host.Stage(SIDE2, 5);  // the host picks it up
	s.PlayFor(3s);
	CHECK((Done(host, SIDE2) == Stages{ 5, 10, 20 }));
	CHECK(host.Quests().caughtUp == 1);
	CHECK((Done(bob, SIDE2) == Stages{ 10, 20 }));  // the host's 5 is below Bob's: his quest is past it
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
	// Carol hasn't started the quest: they reached her game and wait there until she does.
	CHECK((carol.Quests().Waiting(V81) == StageLog::Stages{ 20, 30, 50, 51, 55 }));
	CHECK(!carol.Quests().Achievements().contains(VAULT_DOOR_OPENS));
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

TEST("sim quests: a host who stops hosting, plays alone and hosts again shares none of what they did alone")
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
	CHECK(!bob.Quests().Has(SIDE, 10));
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

	bob.GiveQuest(SIDE2, NotStarted({ 5, 10, 20, 50 }));  // Bob hasn't come across this one yet
	host.Stage(SIDE2, 10);
	Reach(host, MQ206, { 500, 1000, 1050, 1200 });
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
