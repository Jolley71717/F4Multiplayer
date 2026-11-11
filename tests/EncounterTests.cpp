// Random encounters shared calm (src/game/Encounters.h): which quests are encounters, the spawn's kind on the wire, a
// line said by a spawn, the server granting two games' actors of one encounter quest and relaying a spawn's line from
// its sharer alone, and the book that stops our run of an encounter where a friend's already runs, from the server's
// order, as the real server sends it; a fight's shorter window and the run that hurt its player, and an encounter with an
// objective shared by its run (EncounterRunBook.h): ours sent once it stands, a friend's followed once near (with one of the
// owner's actors here), each stage once, the owner's end ending it here too, and its aliases kept on the owner's actors' copies.

#include "ServerFixture.h"

#include "game/EncounterRunBook.h"
#include "game/Encounters.h"
#include "game/SpawnBook.h"

#include <array>
#include <unordered_map>
#include <vector>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Encounters::Book;
	using Encounters::Theirs;
	using Protocol::SpawnKind;
	using RemoteEdits::Spot;

	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t WORLD = 0x0000003C;      // the Commonwealth
	constexpr std::uint32_t CELL = 0x0001F3A0;       // an interior
	constexpr std::uint32_t ACTOR_BASE = 0x0017E78C; // RESceneLC01's first actor's base
	constexpr std::uint32_t RAIDER = 0x0002898B;     // LvlRaider
	constexpr std::uint32_t MIRELURK = 0x001243FF;   // DN083_StartMirelurk
	constexpr std::uint32_t LC01 = 0x0017E729;       // RESceneLC01
	constexpr std::uint32_t KMK01 = 0x0002DFED;      // RETravelKMK01
	constexpr std::uint32_t DN083 = 0x001243D6;      // DN083_Barney, Gun Run (a quest actors' quest)
	constexpr std::uint32_t SPAWN = 0xFF001234;
	constexpr std::uint32_t SPAWN2 = 0xFF005678;
	constexpr std::uint32_t SPAWN3 = 0xFF009ABC;
	constexpr std::uint32_t OURS = 0xFF010001;       // our runtime NPC's id
	constexpr std::uint32_t OURS2 = 0xFF010002;

	Spot At(float a_x, float a_y = 2000.0f, std::uint32_t a_worldspace = WORLD)
	{
		return { 0, a_worldspace, a_x, a_y, 100.0f };
	}

	Protocol::Spawn Actor(std::uint32_t a_id, std::uint32_t a_quest, std::uint16_t a_alias, SpawnKind a_kind, std::uint32_t a_base = ACTOR_BASE, float a_x = 1000.0f)
	{
		return { 0, a_id, a_base, 0, WORLD, a_x, 2000.0f, 100.0f, 0.5f, a_quest, a_alias, a_kind };
	}

	auto Report(const Protocol::Spawn& a_spawn)
	{
		return Protocol::Encode(a_spawn, MT::kReportSpawn);
	}

	std::vector<Protocol::ActorOwner> Owners(const Packets& a_packets)
	{
		std::vector<Protocol::ActorOwner> out;
		for (const auto& batch : Decoded(a_packets, MT::kActorOwners, [](auto a_data) { return Protocol::DecodeOwners(a_data); })) {
			out.insert(out.end(), batch.begin(), batch.end());
		}
		return out;
	}

	bool HasOwner(const std::vector<Protocol::ActorOwner>& a_owners, std::uint32_t a_ref, std::uint32_t a_player)
	{
		return std::ranges::any_of(a_owners, [&](const auto& a_owner) { return a_owner.refId == a_ref && a_owner.playerId == a_player; });
	}

	std::vector<Protocol::Spawn> Spawned(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kSpawned, [](auto a_data) { return Protocol::DecodeSpawn(a_data); });
	}

	std::vector<Protocol::Line> Lines(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kLineSpoken, [](auto a_data) { return Protocol::DecodeLine(a_data); });
	}

	// A friend's encounter spawn as the book sees it.
	Theirs Friends(std::uint32_t a_spawn, std::uint32_t a_quest, const Spot& a_at, std::uint64_t a_seq, bool a_over = false)
	{
		return { a_spawn, a_quest, 7, a_at, a_seq, a_over };
	}

	// What a game does with the spawn messages the server sent it, in the order they came (Spawns::Add and OnOwners): a
	// friend's kSpawned is a friend's encounter with the next number, and a kActorOwners that grants one of our own
	// reported encounter actors gives our run its number. a_ours: our reported spawn ids, by the quest each is of.
	struct Game
	{
		Book                 book;
		std::vector<Theirs>  theirs;
		std::uint32_t        id = 0;

		void Take(const Packets& a_packets, const std::map<std::uint32_t, std::uint32_t>& a_ours)
		{
			for (const auto& packet : a_packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kSpawned)) {
					const auto spawn = Protocol::DecodeSpawn(packet);
					REQUIRE(spawn);
					if (spawn->kind == SpawnKind::kEncounter) {
						theirs.push_back({ spawn->refId, spawn->quest, spawn->playerId, { spawn->cell, spawn->worldspace, spawn->x, spawn->y, spawn->z }, book.Next(), false });
					}
				} else if (packet[0] == Type(MT::kActorOwners)) {
					const auto owners = Protocol::DecodeOwners(packet);
					REQUIRE(owners);
					for (const auto& owner : *owners) {
						if (const auto ours = a_ours.find(owner.refId); ours != a_ours.end() && owner.playerId == id) {
							book.OursGranted(ours->second, book.Next());
						}
					}
				}
			}
		}
	};
}

TEST("encounters: the game's random-encounter quests are told by their name, type and story event")
{
	using Encounters::IsEncounter;
	using Encounters::IsEncounterName;
	constexpr std::int8_t   MISC = Encounters::QUEST_TYPE_MISC;
	constexpr std::uint32_t SCPT = 0x54504353;  // the script event the triggers send (ENAM 'SCPT'), as the engine holds it
	constexpr std::uint32_t OAAT = 0x54414141;  // REActorAttach01's and 02's
	// Fallout4.esm's, of every kind the RE system has.
	for (const auto name : { "RESceneLC01", "RETravelKMK01", "REObjectKMK02", "RECampLC01", "REChokepointSC01", "REAssaultSC06", "RECheckpoint02_SynthAttackers", "REScene_VertibirdKMK01", "RESceneKMK_MS09Jack" }) {
		CHECK(IsEncounterName(name));
		CHECK(IsEncounter(MISC, SCPT, name));
	}
	CHECK(IsEncounter(MISC, OAAT, "REActorAttach01"));
	// The add-ons', after their prefix.
	for (const auto name : { "DLC03RESceneKMK01", "DLC04RETravelCT01", "DLC01RESceneKMK02", "DLC03_REObjectKMK01" }) {
		CHECK(IsEncounterName(name));
		CHECK(IsEncounter(MISC, SCPT, name));
	}
	// Other quests that happen to begin with the letters, and quests of other kinds.
	for (const auto name : { "ReconScopeQuest", "RelayTowerQuest", "Reunions", "RE", "R", "", "FFDiamondCity12", "DialogueDiamondCity", "DLC03Reunite" }) {
		CHECK(!IsEncounterName(name));
		CHECK(!IsEncounter(MISC, SCPT, name));
	}
	// The encounter system's own bookkeeping is no encounter, however it is named: of type none or a side quest, or misc
	// with no story event to start it (REFactionShared runs from the game's start, the treasure hunts' reward quests are
	// set going by a stage; Fallout4.esm's 212 RE-named misc quests have 207 with an event, these 5 without).
	CHECK(!IsEncounter(0, 0, "REParent"));
	CHECK(!IsEncounter(0, OAAT, "RECorpseSwarmCreator"));
	CHECK(!IsEncounter(7, SCPT, "RECheckpointBoSWon01_Init"));
	CHECK(!IsEncounter(7, SCPT, "REAssault_VertibirdKMK01"));
	CHECK(!IsEncounter(1, SCPT, "RESceneLC01"));
	CHECK(IsEncounterName("REFactionShared"));
	CHECK(!IsEncounter(MISC, 0, "REFactionShared"));
	CHECK(!IsEncounter(MISC, 0, "RETreasureHuntSC01_Reward"));
	CHECK(!IsEncounter(MISC, 0, "RESceneLC01"));  // the rule, not the name: an encounter quest always has one
	// What the running game holds for a quest no event starts is its own (REFactionShared read as not 0 on the rig): an
	// event is a word of four capitals or digits, and not the editor's "NONE".
	using Encounters::IsStoryEvent;
	constexpr std::uint32_t NONE = 0x454E4F4E;  // "NONE"
	for (const auto event : { SCPT, OAAT, 0x434F4C43u /* CLOC */, 0x4C4C494Bu /* KILL */, 0x30304C41u /* AL00 */ }) {
		CHECK(IsStoryEvent(event));
	}
	for (const auto none : { 0u, 0xFFFFFFFFu, NONE, 0x00504353u /* three letters */, 0x74706373u /* lower case */, 0x20504353u /* a space */ }) {
		CHECK(!IsStoryEvent(none));
	}
	CHECK(!IsEncounter(MISC, NONE, "REFactionShared"));
	CHECK(!IsEncounter(MISC, 0xFFFFFFFFu, "RETreasureHuntSC01_Reward"));
	CHECK(IsEncounter(MISC, SCPT, "REFactionShared"));  // the event decides, not the name
	// The story manager's own word comes first: a quest node lists every encounter and none of the bookkeeping, whatever
	// the event field reads; the event's shape decides only when no node could be read.
	CHECK(IsEncounter(MISC, true, NONE, "RESceneLC01"));
	CHECK(IsEncounter(MISC, true, 0, "RESceneLC01"));
	CHECK(!IsEncounter(MISC, false, SCPT, "REFactionShared"));
	CHECK(!IsEncounter(MISC, false, 0, "RETreasureHuntSC01_Reward"));
	CHECK(!IsEncounter(MISC, true, SCPT, "ReconScopeQuest"));  // listed, but not named so: the name rule still holds
	CHECK(!IsEncounter(7, true, SCPT, "RECheckpointBoSWon01_Init"));  // listed, but a side quest
	CHECK(IsEncounter(MISC, std::nullopt, SCPT, "RESceneLC01"));
	CHECK(!IsEncounter(MISC, std::nullopt, NONE, "REFactionShared"));
	CHECK(!IsEncounter(MISC, std::nullopt, 0, "RESceneLC01"));
}

TEST("encounters: a spawn says its kind on the wire, and a plain one names no quest")
{
	for (const auto type : { MT::kReportSpawn, MT::kSpawned }) {
		const auto encounter = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter), type));
		REQUIRE(encounter);
		CHECK(encounter->kind == SpawnKind::kEncounter);
		CHECK(encounter->quest == LC01 && encounter->alias == 3);
		CHECK(*encounter == Actor(SPAWN, LC01, 3, SpawnKind::kEncounter));

		const auto questActor = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, DN083, 26, SpawnKind::kQuestActor, MIRELURK), type));
		REQUIRE(questActor);
		CHECK(questActor->kind == SpawnKind::kQuestActor);

		const auto plain = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kPlain, RAIDER), type));
		REQUIRE(plain);
		CHECK(plain->kind == SpawnKind::kPlain && plain->quest == 0 && plain->alias == 0);

		// A plain spawn naming a quest, a kind naming none, and a kind the protocol has not: refused.
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, SpawnKind::kPlain), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kEncounter), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kQuestActor), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, static_cast<SpawnKind>(3)), type)));
		// The default is plain: a spawn made with no kind said is as before.
		CHECK(Protocol::Spawn{}.kind == SpawnKind::kPlain);
	}
}

TEST("encounters: a line may be said by a spawn, under its spawn id")
{
	for (const auto type : { MT::kReportLine, MT::kLineSpoken }) {
		const auto line = Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, SPAWN, "Hello there", 0x0017E7A0, 1, 0x0002F7A1 }, type));
		REQUIRE(line);
		CHECK(line->speaker == SPAWN);
		CHECK(line->info == 0x0017E7A0 && line->response == 1 && line->voiceType == 0x0002F7A1);
		// A plugin's NPC and the player's own still go; the one id no NPC has is refused.
		CHECK(Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0x0001F001, "Hello" }, type)));
		CHECK(Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0, "Hello" }, type)));
		CHECK(!Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0xFF000000, "Hello" }, type)));
	}
}

TEST("encounters: the server grants two games' actors of one encounter quest and alias, and still refuses a quest actor's twin")
{
	Fixture    f;
	const auto first = JoinAs(f, 1, "Player One");
	const auto second = JoinAs(f, 2, "Player Two");
	Clear(f, { 1, 2 });

	// The first game rolled RESceneLC01 and shares an actor of it; the second rolled the same encounter (elsewhere, or
	// at the same spot: the games settle that, not the server) and shares its own from the same alias. Both are granted.
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter)));
	const auto toSecond = Spawned(Received(f, 2));
	REQUIRE(toSecond.size() == 1);
	CHECK(toSecond[0].kind == SpawnKind::kEncounter && toSecond[0].quest == LC01 && toSecond[0].alias == 3 && toSecond[0].playerId == first);
	Clear(f, { 1 });
	f.fake->Deliver(2, Report(Actor(SPAWN2, LC01, 3, SpawnKind::kEncounter)));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN2, second));
	const auto toFirst = Spawned(Received(f, 1));
	REQUIRE(toFirst.size() == 1);
	CHECK(toFirst[0].kind == SpawnKind::kEncounter && toFirst[0].playerId == second);

	// A quest actor's twin is refused as before (protocol 61).
	f.fake->Deliver(1, Report(Actor(SPAWN3, DN083, 26, SpawnKind::kQuestActor, MIRELURK)));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Report(Actor(0xFF00DEF0, DN083, 26, SpawnKind::kQuestActor, MIRELURK)));
	const auto refused = Owners(Received(f, 2));
	CHECK(HasOwner(refused, 0xFF00DEF0, 0));
	CHECK(!HasOwner(refused, 0xFF00DEF0, second));
	CHECK(Spawned(Received(f, 1)).empty());

	// A joiner hears the kind of each spawn already shared.
	Sync(f, 1);
	JoinAs(f, 3, "Player Three");
	const auto toThird = Spawned(Received(f, 3));
	REQUIRE(toThird.size() == 3);
	CHECK(std::ranges::count_if(toThird, [](const auto& a_spawn) { return a_spawn.kind == SpawnKind::kEncounter; }) == 2);
	CHECK(std::ranges::count_if(toThird, [](const auto& a_spawn) { return a_spawn.kind == SpawnKind::kQuestActor; }) == 1);
}

TEST("encounters: the server relays a spawn's line from the game that shares it alone")
{
	Fixture f;
	JoinAs(f, 1, "Player One");
	JoinAs(f, 2, "Player Two");
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter)));
	Clear(f, { 1, 2 });

	// The sharer's line about its spawn reaches the other.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Line{ 0, SPAWN, "Hello there" }, MT::kReportLine));
	const auto heard = Lines(Received(f, 2));
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].speaker == SPAWN && heard[0].playerId == 1);
	std::this_thread::sleep_for(300ms);  // a player's lines go at most four a second

	// Another player's line under that spawn, or under a spawn the server doesn't hold, is dropped; their own is not.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Line{ 0, SPAWN, "Hello there" }, MT::kReportLine));
	CHECK(Lines(Received(f, 1)).empty());
	std::this_thread::sleep_for(300ms);
	f.fake->Deliver(2, Protocol::Encode(Protocol::Line{ 0, SPAWN2, "Hello there" }, MT::kReportLine));
	CHECK(Lines(Received(f, 1)).empty());
	std::this_thread::sleep_for(300ms);
	f.fake->Deliver(2, Protocol::Encode(Protocol::Line{ 0, 0, "Hello there" }, MT::kReportLine));
	CHECK(Lines(Received(f, 1)).size() == 1);
}

TEST("encounters: a friend's encounter that was first at the spot stops our run begun there")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	// The encounter running as the world came up is settled (Spawns lists those on the first in-world frame); the runs
	// seen after are fresh, our run of RESceneLC01 among them: its two actors seen, no grant yet.
	book.Settle(std::array{ KMK01 }, now - 1min);
	CHECK(book.Settled(KMK01));
	book.OursSeen(KMK01, OURS, At(50000.0f), now);
	CHECK(book.Settled(KMK01));
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	book.OursSeen(LC01, OURS2, At(1100.0f), now);
	CHECK(book.Knows(LC01));
	CHECK(!book.Settled(LC01));
	CHECK(book.MayStop(LC01, now + 1s));
	CHECK(!book.MayStop(KMK01, now + 1s));

	// A friend's actor of another encounter 500 units away, which came before any grant of ours: ours gives way.
	const std::array near{ Friends(SPAWN, KMK01, At(1500.0f), 1) };
	const auto       rival = book.RivalOf(LC01, near, now + 1s);
	REQUIRE(rival);
	CHECK(rival->spawnId == SPAWN && rival->quest == KMK01 && rival->player == 7);
	CHECK(rival->distance > 399.0f && rival->distance < 401.0f);  // from the nearer of ours

	// The same encounter rolled by both at one trigger is settled the same way.
	const std::array same{ Friends(SPAWN, LC01, At(1200.0f), 1) };
	CHECK(book.RivalOf(LC01, same, now + 1s));

	// Far away, in another place, or over (dead or let go of): no rival.
	const std::array far{ Friends(SPAWN, KMK01, At(1100.0f + Encounters::COLLIDE_RANGE + 1.0f), 1) };  // past the range of both of ours
	CHECK(!book.RivalOf(LC01, far, now + 1s));
	const std::array elsewhere{ Friends(SPAWN, KMK01, Spot{ CELL, 0, 1000.0f, 2000.0f, 100.0f }, 1) };
	CHECK(!book.RivalOf(LC01, elsewhere, now + 1s));
	const std::array otherWorld{ Friends(SPAWN, KMK01, At(1000.0f, 2000.0f, 0x0000DDDD), 1) };
	CHECK(!book.RivalOf(LC01, otherWorld, now + 1s));
	const std::array over{ Friends(SPAWN, KMK01, At(1500.0f), 1, true) };
	CHECK(!book.RivalOf(LC01, over, now + 1s));
	const std::array noQuest{ Friends(SPAWN, 0, At(1500.0f), 1) };
	CHECK(!book.RivalOf(LC01, noQuest, now + 1s));

	// A run the book doesn't know, a settled one, and nothing of theirs at all.
	CHECK(!book.RivalOf(DN083, near, now + 1s));
	const std::array besideSettled{ Friends(SPAWN, LC01, At(50100.0f), 1) };
	CHECK(!book.RivalOf(KMK01, besideSettled, now + 1s));
	CHECK(!book.RivalOf(LC01, std::span<const Theirs>{}, now + 1s));
}

TEST("encounters: the server's order decides, so two games decide alike")
{
	// What the server did: granted game A's actor first (its kActorOwners to both, kSpawned to B), then game B's.
	// Game A: its own grant arrives first, then B's spawn. (Each book also knows another run of ours, far away.)
	const auto now = Encounters::Clock::now();
	const auto Begun = [&](Book& a_book) { a_book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min); };
	Book a;
	Begun(a);
	a.OursSeen(LC01, OURS, At(1000.0f), now);
	a.OursGranted(LC01, a.Next());                           // 1
	const std::array bSpawn{ Friends(SPAWN2, KMK01, At(1300.0f), a.Next()) };  // 2
	CHECK(!a.RivalOf(LC01, bSpawn, now + 2s));  // ours was first: it stays

	// Game B: A's spawn arrives first, then its own grant.
	Book b;
	Begun(b);
	b.OursSeen(KMK01, OURS, At(1300.0f), now);
	const std::array aSpawn{ Friends(SPAWN, LC01, At(1000.0f), b.Next()) };  // 1
	b.OursGranted(KMK01, b.Next());                                            // 2
	const auto rival = b.RivalOf(KMK01, aSpawn, now + 2s);
	REQUIRE(rival);
	CHECK(rival->quest == LC01);

	// A later grant of a second actor of the run doesn't move the run's place in the order; an earlier one would.
	b.OursGranted(KMK01, 9);
	CHECK(b.RivalOf(KMK01, aSpawn, now + 2s));
	a.OursGranted(LC01, 5);
	CHECK(!a.RivalOf(LC01, bSpawn, now + 2s));
	Book c;
	Begun(c);
	c.OursSeen(KMK01, OURS, At(1300.0f), now);
	c.OursGranted(KMK01, 3);
	const std::array laterSpawn{ Friends(SPAWN, LC01, At(1000.0f), 4) };
	CHECK(!c.RivalOf(KMK01, laterSpawn, now + 2s));
	c.OursGranted(KMK01, 5);  // the lowest is kept
	CHECK(!c.RivalOf(KMK01, laterSpawn, now + 2s));
}

TEST("encounters: two games fed the real server's messages in arrival order decide alike")
{
	// Two players roll different encounters at one trigger, within a second of each other: each game sees its own
	// actor, reports it, and takes what the server sends in the order it comes. The server took the first player's
	// report first, so the first player's run stays and the second's gives way, in both games' books.
	Fixture    f;
	const auto first = JoinAs(f, 1, "Player One");
	const auto second = JoinAs(f, 2, "Player Two");
	Clear(f, { 1, 2 });
	const auto now = Encounters::Clock::now();
	Game a{ .id = first };
	Game b{ .id = second };
	for (auto* game : { &a, &b }) {
		game->book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	}
	a.book.OursSeen(LC01, OURS, At(1000.0f), now);
	b.book.OursSeen(KMK01, OURS2, At(1300.0f), now);
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter, ACTOR_BASE, 1000.0f)));
	f.fake->Deliver(2, Report(Actor(SPAWN2, KMK01, 14, SpawnKind::kEncounter, RAIDER, 1300.0f)));
	a.Take(Received(f, 1), { { SPAWN, LC01 } });
	b.Take(Received(f, 2), { { SPAWN2, KMK01 } });

	REQUIRE(a.theirs.size() == 1);
	REQUIRE(b.theirs.size() == 1);
	CHECK(!a.book.RivalOf(LC01, a.theirs, now + 1s));  // the first player's run stays
	const auto rival = b.book.RivalOf(KMK01, b.theirs, now + 1s);
	REQUIRE(rival);                                     // the second's gives way
	CHECK(rival->spawnId == SPAWN && rival->quest == LC01 && rival->player == first);
}

TEST("encounters: past the window ours stays, a stopped run is left alone, and a run met under way is never stopped")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	const std::array theirs{ Friends(SPAWN, KMK01, At(1200.0f), 1) };
	CHECK(book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW - 1s));
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW));
	CHECK(!book.MayStop(LC01, now + Encounters::STOP_WINDOW));
	// Seen again later: the run's start is still its first sighting.
	book.OursSeen(LC01, OURS, At(1000.0f), now + Encounters::STOP_WINDOW + 5s);
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW + 6s));
	CHECK(book.StartedAt(LC01) == now);

	// Stopped: left alone for STOPPED_FOR, and its actors seen again in that time make no new run.
	Book stopped;
	stopped.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);
	stopped.OursSeen(LC01, OURS, At(1000.0f), now);
	stopped.Stopped(LC01, now + 1s);
	CHECK(stopped.IsStopped(LC01, now + 1s));
	CHECK(!stopped.Knows(LC01));
	stopped.OursSeen(LC01, OURS, At(1000.0f), now + 2s);
	CHECK(!stopped.Knows(LC01));
	CHECK(!stopped.RivalOf(LC01, theirs, now + 3s));
	CHECK(!stopped.IsStopped(LC01, now + 1s + Encounters::STOPPED_FOR));
	CHECK(!stopped.IsStopped(KMK01, now + 1s));
	stopped.Expire(now + 1s + Encounters::STOPPED_FOR);
	CHECK(!stopped.IsStopped(LC01, now + 2s));  // forgotten: the quest may run here again

	// Over (the quest stopped running here): forgotten; running again later, it is a new run, judged afresh.
	Book over;
	over.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);
	over.OursSeen(LC01, OURS, At(1000.0f), now);
	over.OursOver(LC01);
	CHECK(!over.Knows(LC01));
	over.OursSeen(LC01, OURS, At(1000.0f), now + 5min);
	CHECK(over.Knows(LC01));
	CHECK(over.RivalOf(LC01, theirs, now + 5min + 1s));

	// Met under way: the encounter quests running as the world came up (a load, a session's start) are settled, never
	// stopped, whatever stands beside them. One a friend's older encounter stands beside, say ours ran long and we came
	// back to it after a load, stays; its actors seen later, even long after another run's, keep it settled.
	Book loaded;
	loaded.Settle(std::array{ LC01, KMK01 }, now);
	CHECK(loaded.Settled(LC01));
	CHECK(loaded.Settled(KMK01));
	loaded.OursSeen(KMK01, OURS2, At(5000.0f), now + 1s);
	loaded.OursSeen(LC01, OURS, At(1000.0f), now + 1min);
	CHECK(loaded.Settled(LC01));
	CHECK(!loaded.MayStop(LC01, now + 1min + 1s));
	CHECK(!loaded.RivalOf(LC01, theirs, now + 1min + 1s));
	const std::array olderBeside{ Friends(SPAWN, KMK01, At(1200.0f), 1) };
	CHECK(!loaded.RivalOf(LC01, olderBeside, now + 1min + 1s));
	// The first fresh roll after the world came up is judged as any, however soon it comes.
	loaded.OursSeen(DN083, 0xFF0FFFFE, At(1000.0f), now + 2s);
	CHECK(!loaded.Settled(DN083));
	CHECK(loaded.RivalOf(DN083, olderBeside, now + 3s));
	// Cleared (a load): nothing is settled until Spawns says what runs; a run seen before that is settled by it then.
	loaded.Clear();
	CHECK(loaded.Size() == 0);
	loaded.OursSeen(DN083, 0xFF0FFFFE, At(1000.0f), now + 1h);
	CHECK(!loaded.Settled(DN083));
	loaded.Settle(std::array{ DN083 }, now + 1h);
	CHECK(loaded.Settled(DN083));
	CHECK(!loaded.RivalOf(DN083, olderBeside, now + 1h + 1s));
	CHECK(loaded.Size() == 1);
}

TEST("encounters: the nearest of several rivals, and a run's actors are kept to a limit")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	const std::array theirs{ Friends(SPAWN, KMK01, At(2500.0f), 1), Friends(SPAWN2, DN083, At(1800.0f), 2), Friends(SPAWN3, KMK01, At(1200.0f), 3) };
	const auto       rival = book.RivalOf(LC01, theirs, now + 1s);
	REQUIRE(rival);
	CHECK(rival->spawnId == SPAWN3);
	CHECK(rival->distance > 199.0f && rival->distance < 201.0f);

	// A friend's spawn that came after our grant is no rival even when nearest.
	book.OursGranted(LC01, 3);
	const auto earlier = book.RivalOf(LC01, theirs, now + 1s);
	REQUIRE(earlier);
	CHECK(earlier->spawnId == SPAWN2);

	// Actors past MAX_ACTORS are not kept, so a crowd doesn't grow the book; a known actor's spot stays where it first
	// stood (the run's markers), wherever it walks to: a travelling group two cells on still holds its trigger.
	Book crowd;
	crowd.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);
	const auto spread = [](std::uint32_t a_i) { return At(100000.0f + static_cast<float>(a_i) * 2.0f * Encounters::COLLIDE_RANGE); };  // each out of the others' range
	for (std::uint32_t i = 0; i < Encounters::MAX_ACTORS + 8; ++i) {
		CHECK(crowd.OursSeen(KMK01, 0xFF020000 + i, spread(i), now));
	}
	CHECK(crowd.OursSeen(KMK01, 0xFF020000, At(1000.0f), now + 5s));  // the first actor walked here
	const std::array nearWalked{ Friends(SPAWN, LC01, At(1200.0f), 1) };
	CHECK(!crowd.RivalOf(KMK01, nearWalked, now + 6s));  // no run of ours at the spot it walked to
	const std::array nearFirst{ Friends(SPAWN, LC01, At(spread(0).x + 200.0f), 1) };
	const auto       stayed = crowd.RivalOf(KMK01, nearFirst, now + 6s);
	REQUIRE(stayed);
	CHECK(stayed->distance > 199.0f && stayed->distance < 201.0f);
	const std::array nearKept{ Friends(SPAWN, LC01, spread(Encounters::MAX_ACTORS - 1), 1) };
	CHECK(crowd.RivalOf(KMK01, nearKept, now + 1s));  // the last actor within the limit was noted
	const std::array nearLast{ Friends(SPAWN, LC01, spread(Encounters::MAX_ACTORS + 7), 1) };
	CHECK(!crowd.RivalOf(KMK01, nearLast, now + 1s));  // an actor past the limit was never noted
	auto quests = crowd.Quests();
	std::ranges::sort(quests);
	const std::vector<std::uint32_t> both{ KMK01, DN083 };  // sorted: KMK01 is the lower form ID
	CHECK(quests == both);
}

TEST("encounters: no stop while our player talks, nor before the world's runs are settled; the window runs on meanwhile")
{
	using Encounters::Holds;
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	const std::array theirs{ Friends(SPAWN, KMK01, At(1200.0f), 1) };
	// Our player is in a conversation or a scene (with the accusor, say): a stop would take the speaker out from under
	// them, so ours stays while they talk, and is judged again as soon as they are done.
	CHECK(!book.MayStop(LC01, now + 1s, Holds{ .talking = true }));
	CHECK(!book.RivalOf(LC01, theirs, now + 1s, Holds{ .talking = true }));
	CHECK(book.MayStop(LC01, now + 1s, Holds{}));
	CHECK(book.RivalOf(LC01, theirs, now + 2s, Holds{ .talking = false }));
	CHECK(book.RivalOf(LC01, theirs, now + 2s));
	// A talk that lasts the window out leaves ours standing for good: its player was in it.
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW - 1s, Holds{ .talking = true }));
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW + 1s, Holds{}));
	CHECK(book.Knows(LC01));
	CHECK(!book.IsStopped(LC01, now + Encounters::STOP_WINDOW + 1s));
	// Before Spawns has settled the world's runs (its first in-world frame) nothing is stopped either.
	Book fresh;
	fresh.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);
	fresh.OursSeen(LC01, OURS, At(1000.0f), now);
	CHECK(!fresh.MayStop(LC01, now + 1s, Holds{ .unsettled = true }));
	CHECK(!fresh.RivalOf(LC01, theirs, now + 1s, Holds{ .unsettled = true, .talking = false }));
	fresh.Settle(std::array{ KMK01 }, now + 1s);  // what ran before the book looked; ours is fresh
	CHECK(fresh.RivalOf(LC01, theirs, now + 2s, Holds{}));
}

TEST("encounters: a stopped run's actors are the game's to remove, the ones seen before the stop and the ones seen after")
{
	using Encounters::Book;
	using Doomed = Book::Doomed;
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away: not touched
	CHECK(book.OursSeen(LC01, OURS, At(1000.0f), now));
	CHECK(book.OursSeen(LC01, OURS2, At(1100.0f), now));
	CHECK(book.TakeDoomed().empty());  // a live run's actors are nobody's to remove
	// Stopped: the two seen so far are to be removed (the quest's own stop leaves some standing), each once.
	book.Stopped(LC01, now + 1s);
	const std::vector<Doomed> seen{ { OURS, LC01 }, { OURS2, LC01 } };
	CHECK(book.TakeDoomed() == seen);
	CHECK(book.TakeDoomed().empty());
	// A third actor of the quest, first seen after the stop: no new run, not offered, and not the book's to doom (it
	// may be a fresh roll's of the same quest, which runs again; the game tells the two apart by the quest running).
	constexpr std::uint32_t OURS3 = 0xFF010003;
	CHECK(!book.OursSeen(LC01, OURS3, At(1200.0f), now + 2s));
	CHECK(!book.Knows(LC01));
	CHECK(!book.OursSeen(LC01, OURS3, At(1200.0f), now + 3s));
	CHECK(book.TakeDoomed().empty());
	// An actor of the other run, and of the same quest once the stop is forgotten (a new run): a run's, not removed.
	CHECK(book.OursSeen(DN083, 0xFF0FFFFE, At(-90000.0f), now + 3s));
	CHECK(book.OursSeen(LC01, 0xFF010004, At(1000.0f), now + 1s + Encounters::STOPPED_FOR));
	CHECK(book.Knows(LC01));
	CHECK(book.TakeDoomed().empty());
	// Cleared (a load): nothing left to remove of the world left behind.
	book.Stopped(LC01, now + 2min);
	book.Clear();
	CHECK(book.TakeDoomed().empty());
}

TEST("encounters: an encounter's actor shared on plain under its id once its encounter is over; the copies stay")
{
	// The owner's game: its run ended (the quest stopped, the radstag lives on), so the spawn is reported again, plain.
	// The server takes the new kind and sends the spawn again to the others, whose records take it (Theirs::Rekind) and
	// whose copies stay; a joiner hears it plain. For the books that judge "one encounter per spot", it is no encounter
	// spawn any more: a radstag of an ended encounter once made a friend's game stop five new runs near it.
	Fixture    f;
	const auto first = JoinAs(f, 1, "Player One");
	const auto second = JoinAs(f, 2, "Player Two");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter)));
	const auto shared = Spawned(Received(f, 2));
	REQUIRE(shared.size() == 1);
	CHECK(shared[0].kind == SpawnKind::kEncounter);
	Clear(f, { 1 });

	// The friend's book: a copy placed of it, an encounter spawn at its spot.
	SpawnBook::Theirs book;
	REQUIRE(book.Add(shared[0], second));
	book.Placed(SPAWN, 0xFF200001, SpawnBook::Clock::now());
	CHECK(book.Find(SPAWN)->spawn.kind == SpawnKind::kEncounter);

	// Shared on plain: the others hear it again under its id, plain and with no quest; the owner is answered as its owner.
	f.fake->Deliver(1, Report(Actor(SPAWN, 0, 0, SpawnKind::kPlain, ACTOR_BASE, 1300.0f)));
	const auto again = Spawned(Received(f, 2));
	REQUIRE(again.size() == 1);
	CHECK(again[0].refId == SPAWN && again[0].kind == SpawnKind::kPlain && again[0].quest == 0 && again[0].alias == 0 && again[0].playerId == first);
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, first));
	CHECK(book.Rekind(SPAWN, again[0]));
	CHECK(book.Find(SPAWN)->spawn.kind == SpawnKind::kPlain && book.Find(SPAWN)->spawn.quest == 0);
	CHECK(book.Find(SPAWN)->local == 0xFF200001);  // the copy stays
	CHECK(book.LocalOf(SPAWN) == 0xFF200001);
	CHECK(!book.Rekind(SPAWN2, again[0]));

	// Any other report of a held id is answered with its owner, and nobody else hears of it: the owner's plain spawn
	// reported again, another player's report of the id, or a plain spawn said to be an encounter's.
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Report(Actor(SPAWN, 0, 0, SpawnKind::kPlain, ACTOR_BASE, 1300.0f)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, first));
	CHECK(Spawned(Received(f, 2)).empty());
	f.fake->Deliver(2, Report(Actor(SPAWN, 0, 0, SpawnKind::kPlain)));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN, first));
	CHECK(Spawned(Received(f, 1)).empty());
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, first));
	CHECK(Spawned(Received(f, 2)).empty());

	// A joiner hears it as it is now: plain.
	Sync(f, 1);
	JoinAs(f, 3, "Player Three");
	const auto toThird = Spawned(Received(f, 3));
	REQUIRE(toThird.size() == 1);
	CHECK(toThird[0].refId == SPAWN && toThird[0].kind == SpawnKind::kPlain && toThird[0].quest == 0);
}

TEST("encounters: a friend's travelling encounter holds its spot and where its actors stand now")
{
	// The rig (PR 337): the friend's merchant walked about 4000 units from the trigger he was rolled at, and a second
	// merchant was rolled beside him. Theirs counts at the spot it was shared at and where its owner's game has it now.
	const auto now = Encounters::Clock::now();
	Book       book;
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	book.OursSeen(LC01, OURS, At(10000.0f), now);
	const auto Walked = [](const Spot& a_now, std::uint64_t a_seq = 1, bool a_over = false) {
		Theirs theirs = Friends(SPAWN, KMK01, At(1000.0f), a_seq, a_over);  // rolled 9000 units from ours
		theirs.now = a_now;
		return theirs;
	};
	const std::array atSpot{ Friends(SPAWN, KMK01, At(1000.0f), 1) };
	CHECK(!book.RivalOf(LC01, atSpot, now + 1s));  // standing where it began: far from ours
	const std::array walkedHere{ Walked(At(10500.0f)) };
	const auto       rival = book.RivalOf(LC01, walkedHere, now + 1s);
	REQUIRE(rival);
	CHECK(rival->spawnId == SPAWN && rival->quest == KMK01);
	CHECK(rival->distance > 499.0f && rival->distance < 501.0f);
	// Walked off: its spot still holds (the nearer of the two counts).
	Book atTrigger;
	atTrigger.OursSeen(LC01, OURS, At(1300.0f), now);
	const std::array walkedOff{ Walked(At(40000.0f)) };
	const auto       stillThere = atTrigger.RivalOf(LC01, walkedOff, now + 1s);
	REQUIRE(stillThere);
	CHECK(stillThere->distance > 299.0f && stillThere->distance < 301.0f);
	const std::array walkedNearer{ Walked(At(1400.0f)) };
	const auto       nearer = atTrigger.RivalOf(LC01, walkedNearer, now + 1s);
	REQUIRE(nearer);
	CHECK(nearer->distance > 99.0f && nearer->distance < 101.0f);
	// Where it stands now in another place counts for nothing there; over (dead, let go of), or later in the server's
	// order than ours, it is no rival wherever it stands.
	const std::array inside{ Walked(Spot{ CELL, 0, 10000.0f, 2000.0f, 100.0f }) };
	CHECK(!book.RivalOf(LC01, inside, now + 1s));
	const std::array overHere{ Walked(At(10500.0f), 1, true) };
	CHECK(!book.RivalOf(LC01, overHere, now + 1s));
	book.OursGranted(LC01, 2);
	const std::array laterHere{ Walked(At(10500.0f), 3) };
	CHECK(!book.RivalOf(LC01, laterHere, now + 1s));
	CHECK(book.RivalOf(LC01, walkedHere, now + 1s));  // number 1: first, still

	// Two games: A's run was first; B's travelling group walks onto A's spot. A never gives way to it (B's came later),
	// and B gives way to A's only within its own window: where each stands never makes both stop.
	Book a;
	a.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);
	a.OursSeen(LC01, OURS, At(1000.0f), now);
	a.OursGranted(LC01, a.Next());                                                    // 1
	Theirs bOnA = Friends(SPAWN2, KMK01, At(20000.0f), a.Next());                    // 2
	bOnA.now = At(1100.0f);
	CHECK(!a.RivalOf(LC01, std::array{ bOnA }, now + 1s));
}

TEST("encounters: an ended encounter's actor no longer holds its spot in a friend's book, whoever was first")
{
	// The rig (PR 337): a host radstag whose encounter had ended (its quest stopped) was still shared as an encounter's
	// actor, and made the friend's game stop five new runs near it. Its owner shares it on plain under its id as the
	// quest stops; the friend's record takes the kind, and the book no longer counts it. Dead, or let go of, the same.
	Fixture    f;
	const auto host = JoinAs(f, 1, "Player One");
	const auto friendId = JoinAs(f, 2, "Player Two");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter, ACTOR_BASE, 1000.0f)));
	f.fake->Deliver(1, Report(Actor(SPAWN3, LC01, 4, SpawnKind::kEncounter, ACTOR_BASE, 1050.0f)));
	const auto shared = Spawned(Received(f, 2));
	REQUIRE(shared.size() == 2);

	// The friend's records (Spawns::Add) with the number each arrived as, and a fresh roll of its own beside them.
	SpawnBook::Theirs                                records;
	std::unordered_map<std::uint32_t, std::uint64_t> seqs;
	Book                                             book;
	const auto                                       now = Encounters::Clock::now();
	book.OursSeen(DN083, 0xFF0FFFFF, At(-90000.0f), now - 1min);  // another run of ours, far away
	for (const auto& spawn : shared) {
		REQUIRE(records.Add(spawn, friendId));
		seqs[spawn.refId] = book.Next();
	}
	book.OursSeen(KMK01, OURS, At(1200.0f), now);
	auto theirs = Encounters::TheirsOf(records, seqs);
	REQUIRE(theirs.size() == 2);
	CHECK(theirs[0].player == host && !theirs[0].over && theirs[0].now.has_value());
	CHECK(book.RivalOf(KMK01, theirs, now + 1s));  // the host's encounter runs there, and was first

	// The host's quest stops with both actors standing: each is shared on plain, and the friend takes the kind.
	Clear(f, { 1, 2 });
	for (const auto id : { SPAWN, SPAWN3 }) {
		f.fake->Deliver(1, Report(Actor(id, 0, 0, SpawnKind::kPlain, ACTOR_BASE, 1000.0f)));
	}
	for (const auto& again : Spawned(Received(f, 2))) {
		CHECK(again.kind == SpawnKind::kPlain);
		CHECK(records.Rekind(again.refId, again));
	}
	CHECK(Encounters::TheirsOf(records, seqs).empty());
	CHECK(!book.RivalOf(KMK01, Encounters::TheirsOf(records, seqs), now + 2s));
	CHECK(records.Find(SPAWN)->spawn.kind == SpawnKind::kPlain);  // the records, and so the copies, stay

	// An encounter's actor dead, or let go of (unloaded in its owner's game: the server ends it and names nobody): over.
	SpawnBook::Theirs other;
	REQUIRE(other.Add(shared[0], friendId));
	REQUIRE(other.Add(shared[1], friendId));
	CHECK(book.RivalOf(KMK01, Encounters::TheirsOf(other, seqs), now + 2s));
	other.Died(shared[0].refId);
	other.Gone(shared[1].refId);
	const auto ended = Encounters::TheirsOf(other, seqs);
	REQUIRE(ended.size() == 2);
	CHECK(ended[0].over && ended[1].over);
	CHECK(!book.RivalOf(KMK01, ended, now + 2s));

	// Its states move where it stands now; its spot stays where it was shared.
	SpawnBook::Theirs moving;
	REQUIRE(moving.Add(shared[0], friendId));
	moving.Moved(shared[0].refId, At(30000.0f), 0.0f, SpawnBook::Clock::now());
	const auto moved = Encounters::TheirsOf(moving, seqs);
	REQUIRE(moved.size() == 1);
	CHECK(moved[0].at.x == shared[0].x);
	REQUIRE(moved[0].now);
	CHECK(moved[0].now->x == 30000.0f);
}

TEST("encounters: a fight gives way only in its shorter window, and a run that hurt its player never does")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	// A calm run (a trader) and a fight (raiders: one of them seen hostile), both begun now.
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	book.OursSeen(KMK01, OURS2, At(9000.0f), now, true);
	CHECK(!book.Fight(LC01));
	CHECK(book.Fight(KMK01));
	CHECK(book.MayStop(KMK01, now + 9s));
	CHECK(!book.MayStop(KMK01, now + 11s));  // FIGHT_STOP_WINDOW
	CHECK(book.MayStop(LC01, now + 11s));
	CHECK(!book.MayStop(LC01, now + 21s));  // STOP_WINDOW
	CHECK(!book.Stands(KMK01, now + 9s));
	CHECK(book.Stands(KMK01, now + 11s));
	CHECK(!book.Stands(LC01, now + 11s));
	CHECK(book.Stands(LC01, now + 21s));

	// A calm actor that turns on our player makes its run a fight from then on; a calm sighting after doesn't undo it.
	book.OursSeen(LC01, OURS, At(1000.0f), now + 1s, true);
	book.OursSeen(LC01, OURS, At(1000.0f), now + 2s);
	CHECK(book.Fight(LC01));
	CHECK(!book.MayStop(LC01, now + 11s));

	// An actor of a fresh run hurt our player: never stopped, whatever was first at the spot; it stands at once.
	Book hurt;
	hurt.OursSeen(DN083, OURS, At(1000.0f), now);
	CHECK(hurt.MayStop(DN083, now + 1s));
	CHECK(hurt.OursHurt(DN083));
	CHECK(!hurt.OursHurt(DN083));  // once
	CHECK(hurt.Hurt(DN083));
	CHECK(hurt.Fight(DN083));
	CHECK(!hurt.MayStop(DN083, now + 1s));
	const std::array first{ Friends(SPAWN, KMK01, At(1500.0f), 1) };
	CHECK(!hurt.RivalOf(DN083, first, now + 1s));
	CHECK(hurt.Stands(DN083, now + 1s));
	CHECK(!hurt.OursHurt(LC01));  // a run the book doesn't know
	CHECK(!hurt.Hurt(LC01));

	// A settled run stands at once; a stopped one never; one it doesn't know neither.
	Book settled;
	settled.Settle(std::array{ KMK01 }, now);
	CHECK(settled.Stands(KMK01, now));
	settled.OursSeen(LC01, OURS, At(1000.0f), now);
	settled.Stopped(LC01, now);
	CHECK(!settled.Stands(LC01, now + 1min));
	CHECK(!settled.Stands(DN083, now + 1min));

	// A run the book doesn't know (no live actor: a treasure hunt's corpse and note) stands by its time running.
	CHECK(Encounters::RunStands(true, 0s));
	CHECK(!Encounters::RunStands(false, 1h));
	CHECK(!Encounters::RunStands(std::nullopt, 19s));
	CHECK(Encounters::RunStands(std::nullopt, 20s));
}

namespace
{
	constexpr std::uint32_t CHECKPOINT = 0x000AC4D4;  // RECheckpoint01: "Help the defenders"
	constexpr std::uint32_t TRIGGER = 0x0016A2B1;     // a trigger reference, the same in both games
	constexpr std::uint32_t HOST = 1, FRIEND = 2;

	Protocol::RadiantStart HostRun(std::uint32_t a_serial, std::vector<std::uint16_t> a_stages = { 10, 200 })
	{
		return { HOST, CHECKPOINT, a_serial, { { 0, 0, TRIGGER }, { 9, 0, SPAWN }, { 10, 0, Protocol::RADIANT_UNSHARED_REF } }, std::move(a_stages) };
	}
}

TEST("encounters: which encounters are shared by their run, and the run on the wire")
{
	CHECK(Encounters::SharesRun(true, 1));
	CHECK(!Encounters::SharesRun(true, 0));   // a scene with no objective: its actors alone
	CHECK(!Encounters::SharesRun(false, 3));  // no encounter

	// A run's fills name the trigger (a plugin's reference, no actor), the attackers by their spawn ids, and an actor the
	// owner doesn't share; all of them pass the wire as a radiant's run does.
	const auto start = HostRun(77);
	const auto back = Protocol::DecodeRadiantStart(Protocol::Encode(start, MT::kReportRadiantStart));
	REQUIRE(back);
	CHECK(back->fills == start.fills);
	CHECK(back->stages == start.stages);

	// The run's end goes as a stage no quest has, RUN_OVER, under the run, as any of its stages.
	const auto over = Protocol::DecodeRadiantStage(Protocol::Encode(Protocol::RadiantStage{ 0, CHECKPOINT, HOST, 77, Encounters::RUN_OVER }, MT::kReportRadiantStage));
	REQUIRE(over);
	CHECK(over->stage == Encounters::RUN_OVER);
	CHECK(over->origin == HOST);
	CHECK(over->serial == 77);
}

TEST("encounters: a friend's run is followed here once near, and each stage goes once each way")
{
	using Encounters::RunBook;
	using Encounters::Verdict;
	const auto now = Encounters::Clock::now();

	// The friend's game: no id yet, nothing of its own to share.
	RunBook book{ 1000 };
	CHECK(!book.Announce(LC01, {}, now));
	book.SetSelf(FRIEND);

	// The host's run heard: kept, once; never our own, never one with no origin.
	CHECK(book.Hear(HostRun(77), now));
	CHECK(!book.Hear(HostRun(77), now));
	auto ownRun = HostRun(5);
	ownRun.playerId = FRIEND;
	CHECK(!book.Hear(ownRun, now));
	ownRun.playerId = 0;
	CHECK(!book.Hear(ownRun, now));
	CHECK(!book.Shares(CHECKPOINT));
	CHECK(!book.Follows(CHECKPOINT));
	auto* run = book.Find({ HOST, 77 });
	REQUIRE(run);
	CHECK(run->state == RunBook::State::kHeard);

	// A stage of it before it runs here waits for it; one done already isn't taken twice.
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 77 }, 205));
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 77 }, 205));
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 99 }, 210));  // a run unknown
	CHECK(!book.TakeStage(LC01, { HOST, 77 }, 210));        // another quest under its name

	// Judged: far, it waits; near with a copy not placed, it waits a moment; then it starts.
	Encounters::HeardFacts facts;
	CHECK(book.Judge(*run, facts, now + 1s).verdict == Verdict::kWait);
	facts.near = true;
	facts.waiting = 1;
	CHECK(book.Judge(*run, facts, now + 2s).verdict == Verdict::kWait);
	CHECK(book.Judge(*run, facts, now + 2s + Encounters::FILL_WAIT).verdict == Verdict::kStart);
	facts.waiting = 0;

	// Our game runs the quest itself: dropped, unless our run of it is giving way (it waits for the stop).
	auto ours = facts;
	ours.runningHere = true;
	ours.oursGivingWay = true;
	CHECK(book.Judge(*run, ours, now + 3s).verdict == Verdict::kWait);
	ours.oursGivingWay = false;
	CHECK(book.Judge(*run, ours, now + 3s).verdict == Verdict::kDrop);
	// Their actors gone, or our player never near in time: dropped.
	auto gone = facts;
	gone.ownerLive = false;
	CHECK(book.Judge(*run, gone, now + 3s).verdict == Verdict::kDrop);
	CHECK(book.Judge(*run, facts, now + Encounters::LISTEN_FOR).verdict == Verdict::kDrop);

	// Started, then running: followed, with the host's stages to set here (those it had and the one that came since).
	CHECK(book.Judge(*run, facts, now + 3s).verdict == Verdict::kStart);
	book.Started({ HOST, 77 }, now + 3s);
	CHECK(book.Follows(CHECKPOINT));
	CHECK(book.Shares(CHECKPOINT));
	CHECK(book.Over(CHECKPOINT).empty());  // not running yet is no end of a starting run
	const auto toSet = book.Following({ HOST, 77 });
	CHECK((toSet == std::vector<std::uint16_t>{ 10, 200, 205 }));
	CHECK(book.Following({ HOST, 77 }).empty());

	// From now on the host's stages are set here once, and ours go to them once; none goes back where it came from.
	CHECK(book.TakeStage(CHECKPOINT, { HOST, 77 }, 210));
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 77 }, 210));
	CHECK(book.Report(CHECKPOINT, 210).empty());  // set here from theirs: not passed back
	CHECK(book.Report(CHECKPOINT, 10).empty());   // their start-up stage, done here as it started
	const auto reported = book.Report(CHECKPOINT, 400);
	REQUIRE(reported.size() == 1);
	CHECK((reported[0] == Encounters::RunKey{ HOST, 77 }));
	CHECK(book.Report(CHECKPOINT, 400).empty());

	// The quest stopped here: the run is over, and never followed again if the host sends it again.
	const auto over = book.Over(CHECKPOINT);
	REQUIRE(over.size() == 1);
	CHECK(!book.Follows(CHECKPOINT));
	CHECK(!book.Hear(HostRun(77), now + 1min));
	CHECK(book.Hear(HostRun(78), now + 1min));  // a new run of it is another
}

TEST("encounters: our run that stands is shared once, and a friend's stages of it come back once")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(HOST);

	const std::array<std::uint16_t, 2> done{ 200, 10 };
	const auto                         key = book.Announce(CHECKPOINT, done, now);
	REQUIRE(key);
	CHECK(key->origin == HOST);
	CHECK(key->serial == 1001);
	CHECK(!book.Announce(CHECKPOINT, done, now));  // one run of ours per quest
	REQUIRE(book.Find(*key));
	CHECK((book.Find(*key)->done == std::vector<std::uint16_t>{ 10, 200 }));
	CHECK(book.Shares(CHECKPOINT));
	CHECK(!book.Follows(CHECKPOINT));  // ours: our own actors stand

	// The friend's stage of our run is set here once; ours go out once; one under another run of ours is none of it.
	CHECK(book.TakeStage(CHECKPOINT, *key, 205));
	CHECK(!book.TakeStage(CHECKPOINT, *key, 205));
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 5 }, 210));
	CHECK(book.Report(CHECKPOINT, 205).empty());
	CHECK(book.Report(CHECKPOINT, 250).size() == 1);
	CHECK(book.Report(CHECKPOINT, 250).empty());

	// Over: a new run of the quest later is shared under a new number.
	CHECK(book.Over(CHECKPOINT).size() == 1);
	CHECK(!book.Shares(CHECKPOINT));
	const auto again = book.Announce(CHECKPOINT, {}, now + 5min);
	REQUIRE(again);
	CHECK(again->serial == 1002);

	// A full book takes no more.
	for (std::uint32_t i = 0; i < Encounters::MAX_RUNS; ++i) {
		book.Announce(0x00100000 + i, {}, now);
	}
	CHECK(book.Runs().size() == Encounters::MAX_RUNS);
	CHECK(!book.Hear(HostRun(500), now));
}

TEST("encounters: a friend's run that names the owner's actors starts only with one of them here")
{
	using Encounters::RunBook;
	using Encounters::Verdict;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(HostRun(77), now));
	auto* run = book.Find({ HOST, 77 });
	REQUIRE(run);

	// The trigger is close but none of the owner's actors is placed here (theirs away, or our copies not made yet): it
	// waits, past FILL_WAIT too; our own actors of it would be removed with nothing to stand in their place.
	Encounters::HeardFacts facts;
	facts.namesActors = true;
	facts.near = true;
	facts.waiting = 2;
	const auto none = book.Judge(*run, facts, now + 1s);
	CHECK(none.verdict == Verdict::kWait);
	CHECK(none.why == "none of the owner's actors is placed here yet");
	CHECK(book.Judge(*run, facts, now + 1s + 2 * Encounters::FILL_WAIT).verdict == Verdict::kWait);
	CHECK(!run->near);  // not near yet: FILL_WAIT counts from the first copy placed

	// One copy placed: the rest have FILL_WAIT to come, then it starts without them.
	facts.placed = 1;
	facts.waiting = 1;
	CHECK(book.Judge(*run, facts, now + 30s).verdict == Verdict::kWait);
	CHECK(book.Judge(*run, facts, now + 30s + Encounters::FILL_WAIT).verdict == Verdict::kStart);
	facts.waiting = 0;
	CHECK(book.Judge(*run, facts, now + 31s).verdict == Verdict::kStart);

	// A run that names none of the owner's actors (a treasure hunt's corpse and note) starts by its trigger alone.
	RunBook hunt{ 1000 };
	hunt.SetSelf(FRIEND);
	REQUIRE(hunt.Hear(HostRun(5), now));
	Encounters::HeardFacts trigger;
	trigger.near = true;
	CHECK(hunt.Judge(*hunt.Find({ HOST, 5 }), trigger, now + 1s).verdict == Verdict::kStart);
	trigger.near = false;
	trigger.placed = 1;  // not counted for a run that names none
	CHECK(hunt.Judge(*hunt.Find({ HOST, 5 }), trigger, now + 1s).verdict == Verdict::kWait);
}

TEST("encounters: our game's hollow run of a quest (a save's of one we followed) makes way for the friend's")
{
	using Encounters::RunBook;
	using Encounters::Verdict;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(HostRun(77), now));
	auto* run = book.Find({ HOST, 77 });
	REQUIRE(run);

	Encounters::HeardFacts facts;
	facts.runningHere = true;
	facts.oursHollow = true;
	facts.namesActors = true;
	CHECK(book.Judge(*run, facts, now + 1s).verdict == Verdict::kWait);  // not near: left as it is meanwhile
	facts.near = true;
	facts.placed = 1;
	CHECK(book.Judge(*run, facts, now + 2s).verdict == Verdict::kStopOurs);
	facts.oursGivingWay = true;  // ours being stopped already: wait for it
	CHECK(book.Judge(*run, facts, now + 2s).verdict == Verdict::kWait);
	facts.oursGivingWay = false;
	facts.oursHollow = false;  // ours with its actors: theirs is dropped, as before
	CHECK(book.Judge(*run, facts, now + 2s).verdict == Verdict::kDrop);
	// Stopped: next time ours no longer runs, and theirs starts.
	facts.runningHere = false;
	CHECK(book.Judge(*run, facts, now + 3s).verdict == Verdict::kStart);
}

TEST("encounters: the stages our own start set are never sent back to the run's owner")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(HostRun(77, { 10, 200 }), now));
	book.Started({ HOST, 77 }, now);
	// Our quest's start set 10 (the host's too) and 15 (a variant picked by our start alone).
	const std::array<std::uint16_t, 2> doneHere{ 10, 15 };
	const auto                         toSet = book.Following({ HOST, 77 }, doneHere);
	CHECK((toSet == std::vector<std::uint16_t>{ 10, 200 }));
	CHECK(book.Report(CHECKPOINT, 15).empty());
	CHECK(book.Report(CHECKPOINT, 10).empty());
	CHECK(book.Report(CHECKPOINT, 300).size() == 1);  // what our player does from then on goes
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 77 }, 15));  // and theirs of 15 is not set twice
}

TEST("encounters: the owner's run ends in the friend's game too, and a run that ended is never started again")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);

	// RUN_OVER is no stage: never taken, never reported.
	REQUIRE(book.Hear(HostRun(77), now));
	CHECK(!book.TakeStage(CHECKPOINT, { HOST, 77 }, Encounters::RUN_OVER));

	// Heard only (our player never came near): dropped for good, never started.
	CHECK(!book.OwnerEnded(CHECKPOINT, { HOST, 77 }, now));
	CHECK(!book.Find({ HOST, 77 }));
	CHECK(!book.Hear(HostRun(77), now));

	// Followed: it ends here RUN_END_WAIT later (its last stages first), unless its quest stopped by itself meanwhile.
	REQUIRE(book.Hear(HostRun(78), now));
	book.Started({ HOST, 78 }, now);
	book.Following({ HOST, 78 });
	CHECK(!book.OwnerEnded(LC01, { HOST, 78 }, now));  // under another quest
	CHECK(!book.OwnerEnded(CHECKPOINT, { HOST, 99 }, now));
	CHECK(book.OwnerEnded(CHECKPOINT, { HOST, 78 }, now + 1s));
	CHECK(book.OwnerEnded(CHECKPOINT, { HOST, 78 }, now + 3s));  // again: the first time counts
	CHECK(book.ToEnd(now + 1s + Encounters::RUN_END_WAIT - 1ms).empty());
	const auto end = book.ToEnd(now + 1s + Encounters::RUN_END_WAIT);
	REQUIRE(end.size() == 1);
	CHECK((end[0] == Encounters::RunKey{ HOST, 78 }));
	CHECK(book.TakeStage(CHECKPOINT, { HOST, 78 }, 400));  // a last stage still set meanwhile (the quest runs)
	book.Drop(end[0]);
	CHECK(!book.Follows(CHECKPOINT));
	CHECK(!book.Hear(HostRun(78), now + 1min));

	// Ours: only our game ends it.
	RunBook host{ 1000 };
	host.SetSelf(HOST);
	const auto ours = host.Announce(CHECKPOINT, {}, now);
	REQUIRE(ours);
	CHECK(!host.OwnerEnded(CHECKPOINT, *ours, now));
	CHECK(host.ToEnd(now + 1h).empty());
}

TEST("encounters: a friend who leaves ends their runs we follow, and drops those heard")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(HostRun(77), now));
	REQUIRE(book.Hear(HostRun(78), now));
	auto third = HostRun(5);
	third.playerId = 3;
	REQUIRE(book.Hear(third, now));
	book.Started({ HOST, 78 }, now);
	book.Following({ HOST, 78 });
	const auto ours = book.Announce(LC01, {}, now);
	REQUIRE(ours);

	book.OwnerLeft(HOST, now + 1s);
	CHECK(!book.Find({ HOST, 77 }));  // heard: dropped
	REQUIRE(book.Find({ HOST, 78 }));
	CHECK(book.Find({ HOST, 78 })->ended == now + 1s);
	CHECK(book.Find({ 3, 5 }));  // another player's: theirs still
	CHECK(!book.Find({ 3, 5 })->ended);
	CHECK(book.Find(*ours));
	const auto end = book.ToEnd(now + 1s + Encounters::RUN_END_WAIT);
	REQUIRE(end.size() == 1);
	CHECK((end[0] == Encounters::RunKey{ HOST, 78 }));
	book.OwnerLeft(FRIEND, now + 2s);  // our own id: our runs are never ended so
	CHECK(book.Find(*ours));
}

TEST("encounters: our run is sent again to a game that joined or reloaded only once its actors are shared")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(HOST);
	const auto key = book.Announce(CHECKPOINT, {}, now);
	REQUIRE(key);
	REQUIRE(book.Hear(Protocol::RadiantStart{ FRIEND, LC01, 3, {}, {} }, now));
	auto& ours = *book.Find(*key);
	auto& theirs = *book.Find({ FRIEND, 3 });
	CHECK(!RunBook::TakeResend(ours, true));  // nobody asked

	book.ResendOurs();
	CHECK(!RunBook::TakeResend(theirs, true));  // a friend's is theirs to send
	CHECK(!RunBook::TakeResend(ours, false));   // an actor of it unshared (unloaded since our load): not yet
	CHECK(RunBook::TakeResend(ours, true));
	CHECK(!RunBook::TakeResend(ours, true));  // once
}

TEST("encounters: a followed run's aliases are forced again onto the owner's actors' copies placed anew")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(Protocol::RadiantStart{ HOST, CHECKPOINT, 77,
		{ { 0, 0, TRIGGER }, { 9, 0, SPAWN }, { 10, 0, Protocol::RADIANT_UNSHARED_REF }, { 11, 0, SPAWN2 }, { 4, 1, 0x0001F228 } }, {} }, now));
	auto& run = *book.Find({ HOST, 77 });

	std::unordered_map<std::uint32_t, std::uint32_t> copies{ { SPAWN, 0xFF100001 } };  // SPAWN2's copy not placed
	std::unordered_map<std::uint32_t, std::uint32_t> here{ { 9, 0xFF100001 }, { 0, TRIGGER } };
	const auto copyOf = [&](std::uint32_t a_spawn) { return copies.contains(a_spawn) ? copies[a_spawn] : 0u; };
	const auto hereOf = [&](std::uint32_t a_alias) { return here.contains(a_alias) ? here[a_alias] : 0u; };
	using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // heard, not followed: nothing
	book.Started({ HOST, 77 }, now);
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // starting: the start forces them
	book.Following({ HOST, 77 });
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // each alias holds its copy, or has none placed

	// SPAWN2's copy placed later (our game had made its own for the alias, removed): forced in, once the ask is taken.
	copies[SPAWN2] = 0xFF100002;
	CHECK((RunBook::Refills(run, copyOf, hereOf) == Pairs{ { 11, 0xFF100002 } }));
	RunBook::Forced(run, 11, 0xFF100002, true);
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // asked once; the script VM runs it a frame or so later
	here[11] = 0xFF100002;

	// A save: every copy deleted (the aliases hold nothing the game knows) and placed again under new references.
	here.erase(9);
	here.erase(11);
	copies.clear();
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // none placed yet
	copies = { { SPAWN, 0xFF100003 }, { SPAWN2, 0xFF100004 } };
	CHECK((RunBook::Refills(run, copyOf, hereOf) == Pairs{ { 9, 0xFF100003 }, { 11, 0xFF100004 } }));
	RunBook::Forced(run, 9, 0xFF100003, true);
	RunBook::Forced(run, 11, 0xFF100004, true);
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());

	// An ask the VM did not take is made again on later polls, MAX_FORCE_TRIES times in all; then the copy is given up on
	// until another is placed.
	here.erase(9);
	copies[SPAWN] = 0xFF100005;
	for (std::uint32_t i = 0; i < Encounters::MAX_FORCE_TRIES; ++i) {
		CHECK((RunBook::Refills(run, copyOf, hereOf) == Pairs{ { 9, 0xFF100005 } }));
		RunBook::Forced(run, 9, 0xFF100005, false);
	}
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());  // given up on
	copies[SPAWN] = 0xFF100006;
	CHECK((RunBook::Refills(run, copyOf, hereOf) == Pairs{ { 9, 0xFF100006 } }));
	RunBook::Forced(run, 9, 0xFF100006, false);
	RunBook::Forced(run, 9, 0xFF100006, true);  // taken on the second ask
	CHECK(RunBook::Refills(run, copyOf, hereOf).empty());
}

TEST("encounters: a last stage still waiting in the quest queue holds the owner's end back, up to a limit")
{
	using Encounters::RunBook;
	const auto now = Encounters::Clock::now();
	RunBook    book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(HostRun(78), now));
	book.Started({ HOST, 78 }, now);
	book.Following({ HOST, 78 });
	REQUIRE(book.OwnerEnded(CHECKPOINT, { HOST, 78 }, now));

	bool       queued = true;
	const auto waiting = [&](std::uint32_t a_quest) { return a_quest == CHECKPOINT && queued; };
	CHECK(book.ToEnd(now + Encounters::RUN_END_WAIT, waiting).empty());  // the owner's clean-up stage waits in the queue (a combat hold)
	CHECK(book.ToEnd(now + Encounters::RUN_END_LIMIT - 1ms, waiting).empty());
	CHECK(book.ToEnd(now + Encounters::RUN_END_LIMIT, waiting).size() == 1);  // but not for ever
	queued = false;
	CHECK(book.ToEnd(now + Encounters::RUN_END_WAIT, waiting).size() == 1);  // the queue drained: ended as before
	CHECK(book.ToEnd(now + Encounters::RUN_END_WAIT - 1ms, waiting).empty());

	// The end marker's value is never reported as a stage of a run (a quest's own 65535 would read as its end).
	RunBook host{ 1000 };
	host.SetSelf(HOST);
	REQUIRE(host.Announce(CHECKPOINT, {}, now));
	CHECK(host.Report(CHECKPOINT, Encounters::RUN_OVER).empty());
	CHECK(host.Report(CHECKPOINT, 400).size() == 1);
}

TEST("encounters: a hollow run of ours that nobody shares is stopped once it has run its wait")
{
	using namespace std::chrono_literals;
	// Hollow (every alias its start created empty: a save's of a run we followed), nobody's: stopped after HOLLOW_WAIT.
	CHECK(!Encounters::EndsHollow(true, false, Encounters::HOLLOW_WAIT - 1s));
	CHECK(Encounters::EndsHollow(true, false, Encounters::HOLLOW_WAIT));
	// Its actors stand: our own run, never stopped so.
	CHECK(!Encounters::EndsHollow(false, false, 1h));
	// A friend's run of it is heard: theirs replaces it once our player is near (Verdict::kStopOurs), not this.
	CHECK(!Encounters::EndsHollow(true, true, 1h));
}
