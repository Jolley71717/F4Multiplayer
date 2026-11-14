// Random encounters shared calm (src/game/Encounters.h): which quests are encounters, the spawn's kind on the wire, a
// line said by a spawn, the server granting two games' actors of one encounter quest and relaying a spawn's line from
// its sharer alone, and the book that stops our run of an encounter where a friend's already runs, from the server's
// order, as the real server sends it; a fight's shorter window and the run that hurt its player, and an encounter with an
// objective shared by its run (EncounterRunBook.h): ours sent once it stands, a friend's followed once near (with one of the
// owner's actors here), each stage once, the owner's end ending it here too, and its aliases kept on the owner's actors' copies.

#include "ServerFixture.h"

#include "game/EncounterRunBook.h"
#include "game/Encounters.h"
#include "game/QuestStartRules.h"
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
	// The running game holds a quest's event as the event's index among the story manager's registered events (its
	// InitItemImpl turns the record's ENAM word into it), all ones for none.
	constexpr std::uint32_t SCPT = 12;  // the script event the triggers send (ENAM 'SCPT'), as an index
	constexpr std::uint32_t OAAT = 3;   // REActorAttach01's and 02's
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
	constexpr std::uint32_t NONE = Encounters::NO_STORY_EVENT;
	CHECK(!IsEncounter(0, NONE, "REParent"));
	CHECK(!IsEncounter(0, OAAT, "RECorpseSwarmCreator"));
	CHECK(!IsEncounter(7, SCPT, "RECheckpointBoSWon01_Init"));
	CHECK(!IsEncounter(7, SCPT, "REAssault_VertibirdKMK01"));
	CHECK(!IsEncounter(1, SCPT, "RESceneLC01"));
	CHECK(IsEncounterName("REFactionShared"));
	CHECK(!IsEncounter(MISC, NONE, "REFactionShared"));
	CHECK(!IsEncounter(MISC, NONE, "RETreasureHuntSC01_Reward"));
	CHECK(!IsEncounter(MISC, NONE, "RESceneLC01"));  // the rule, not the name: an encounter quest always has one
	// Any index is an event, the first too; only all ones is none (REFactionShared reads FFFFFFFF on the rig, where no
	// quest's event ever read as a word: DLC01Lair_ChangeLocation's CLOC was not named as its refusal's cause, 2026-11-12).
	using Encounters::IsStoryEvent;
	for (const auto event : { 0u, 1u, SCPT, OAAT, 40u, 0x434F4C43u /* a word, as the record has it */ }) {
		CHECK(IsStoryEvent(event));
	}
	CHECK(!IsStoryEvent(0xFFFFFFFFu));
	CHECK(IsEncounter(MISC, 0, "RESceneLC01"));  // the registered event at index 0
	CHECK(IsEncounter(MISC, SCPT, "REFactionShared"));  // the event decides, not the name
	// The story manager's own word comes first: a quest node lists every encounter and none of the bookkeeping, whatever
	// the event field reads; the event decides only when no node could be read.
	CHECK(IsEncounter(MISC, true, NONE, "RESceneLC01"));
	CHECK(IsEncounter(MISC, true, 0, "RESceneLC01"));
	CHECK(!IsEncounter(MISC, false, SCPT, "REFactionShared"));
	CHECK(!IsEncounter(MISC, false, NONE, "RETreasureHuntSC01_Reward"));
	CHECK(!IsEncounter(MISC, true, SCPT, "ReconScopeQuest"));  // listed, but not named so: the name rule still holds
	CHECK(!IsEncounter(7, true, SCPT, "RECheckpointBoSWon01_Init"));  // listed, but a side quest
	CHECK(IsEncounter(MISC, std::nullopt, SCPT, "RESceneLC01"));
	CHECK(!IsEncounter(MISC, std::nullopt, NONE, "REFactionShared"));
	CHECK(!IsEncounter(MISC, std::nullopt, NONE, "RESceneLC01"));
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
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, static_cast<SpawnKind>(4)), type)));
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

TEST("encounters: an encounter's kind is the word after RE, with scenes and objects one kind")
{
	using Encounters::Kind;
	using Encounters::KindOf;
	// Fallout4.esm's, of every kind a trigger rolls (the names in Encounters.h's comments among them).
	CHECK(KindOf("REAssaultSC06") == Kind::kAssault);
	CHECK(KindOf("REAssaultKMK_COMPreston") == Kind::kAssault);
	CHECK(KindOf("RECampLC01") == Kind::kCamp);
	CHECK(KindOf("RECamprj03") == Kind::kCamp);
	CHECK(KindOf("RECampKMK_DN079B") == Kind::kCamp);
	CHECK(KindOf("RECheckpoint02_SynthAttackers") == Kind::kCheckpoint);
	CHECK(KindOf("RECheckpoint09_Raiders") == Kind::kCheckpoint);
	CHECK(KindOf("RECheckpoint05_Vertibird") == Kind::kCheckpoint);
	CHECK(KindOf("REChokepointSC01") == Kind::kChokepoint);
	CHECK(KindOf("REChokepointSC04_FactionInstRelayAmbush01") == Kind::kChokepoint);
	CHECK(KindOf("RESceneLC01") == Kind::kScene);
	CHECK(KindOf("RESceneKMK_MS09Jack") == Kind::kScene);
	CHECK(KindOf("REScene_VertibirdKMK01") == Kind::kScene);
	CHECK(KindOf("REObjectKMK02") == Kind::kScene);  // an Object trigger rolls scenes too (RESceneQuests takes either keyword)
	CHECK(KindOf("REObject_VertibirdKMK01") == Kind::kScene);
	CHECK(KindOf("RETravelKMK01") == Kind::kTravel);
	CHECK(KindOf("RETravel_VertibirdKMK02") == Kind::kTravel);
	CHECK(KindOf("RETreasureHuntSC01") == Kind::kTreasureHunt);
	// The add-ons', after their prefix.
	CHECK(KindOf("DLC03RESceneKMK01") == Kind::kScene);
	CHECK(KindOf("DLC04RETravelCT01") == Kind::kTravel);
	CHECK(KindOf("DLC01REAssaultSC05_FactionMMvsMechBots") == Kind::kAssault);
	CHECK(KindOf("DLC03_REObjectKMK01") == Kind::kScene);
	CHECK(KindOf("DLC01RETravel_VertibirdKMK01") == Kind::kTravel);
	// None of the words, or no encounter's name: no known kind.
	for (const auto name : { "DLC03REVertibirdKMK01", "REActorAttach01", "REParent", "RE_Something", "ReconScopeQuest", "RelayTowerQuest", "", "RE", "Camp" }) {
		CHECK(KindOf(name) == Kind::kUnknown);
	}
	// Only one kind meets another of its own; one of no known kind meets any.
	CHECK(Encounters::MayCollide(Kind::kCamp, Kind::kCamp));
	CHECK(!Encounters::MayCollide(Kind::kCheckpoint, Kind::kCamp));
	CHECK(!Encounters::MayCollide(Kind::kScene, Kind::kTravel));
	CHECK(Encounters::MayCollide(Kind::kUnknown, Kind::kCamp));
	CHECK(Encounters::MayCollide(Kind::kCheckpoint, Kind::kUnknown));
	CHECK(Encounters::MayCollide(Kind::kUnknown, Kind::kUnknown));
}

TEST("encounters: a run gives way only to a friend's encounter of its own kind, which a trigger of another kind never rolled")
{
	using Encounters::Kind;
	constexpr std::uint32_t CP09 = 0x000AF884;    // RECheckpoint09_Raiders
	constexpr std::uint32_t CAMP03 = 0x001A9670;  // RECamprj03
	constexpr std::uint32_t CP03 = 0x000AF87E;    // RECheckpoint03_Gunners
	constexpr std::uint32_t KMK02 = 0x0002DFEE;   // RETravelKMK02
	const auto              kinded = [](Theirs a_theirs, Kind a_kind) {
		a_theirs.kind = a_kind;
		return a_theirs;
	};
	Book       book;
	const auto now = Encounters::Clock::now();
	// The rig's run 4 (2026-11-12): our checkpoint's attacker at its trigger, a friend's camp (first in the order) 2309 units off.
	book.OursSeen(CP09, OURS, At(1000.0f), now, true, Kind::kCheckpoint);
	const std::array camp{ kinded(Friends(SPAWN, CAMP03, At(3309.0f), 1), Kind::kCamp) };
	CHECK(!book.RivalOf(CP09, camp, now + 1s));  // another trigger's: in one game both would run
	// A friend's checkpoint at the same spot (two players crossing the one trigger) still stops ours.
	const std::array checkpoint{ kinded(Friends(SPAWN, CP03, At(1200.0f), 1), Kind::kCheckpoint) };
	const auto       rival = book.RivalOf(CP09, checkpoint, now + 1s);
	REQUIRE(rival);
	CHECK(rival->quest == CP03);
	// With both beside it, the checkpoint is the rival though the camp is nearer.
	const std::array both{ kinded(Friends(SPAWN, CAMP03, At(1100.0f), 1), Kind::kCamp), kinded(Friends(SPAWN2, CP03, At(2500.0f), 2), Kind::kCheckpoint) };
	const auto       nearest = book.RivalOf(CP09, both, now + 1s);
	REQUIRE(nearest);
	CHECK(nearest->spawnId == SPAWN2);
	// A friend's encounter of no known kind meets any, as before kinds.
	const std::array unknown{ Friends(SPAWN, CAMP03, At(3309.0f), 1) };
	CHECK(book.RivalOf(CP09, unknown, now + 1s));

	// A travelling merchant two cells on still holds the road for a second merchant (Travel against Travel, by where his
	// group stands now), but not against a camp rolled beside where he stands.
	book.OursSeen(KMK01, OURS2, At(30000.0f), now, false, Kind::kTravel);
	auto merchant = kinded(Friends(SPAWN3, KMK02, At(1000.0f), 1), Kind::kTravel);
	merchant.now = At(30500.0f);
	CHECK(book.RivalOf(KMK01, std::array{ merchant }, now + 1s));
	Book campBook;
	campBook.OursSeen(CAMP03, OURS2, At(30000.0f), now, false, Kind::kCamp);
	CHECK(!campBook.RivalOf(CAMP03, std::array{ merchant }, now + 1s));

	// Our run of no known kind (its quest's name gave none) gives way to any, as before kinds.
	Book unknownBook;
	unknownBook.OursSeen(CP09, OURS, At(1000.0f), now);
	CHECK(unknownBook.RivalOf(CP09, camp, now + 1s));
	// The kind is the first one told: a later sighting with none keeps it.
	book.OursSeen(CP09, OURS2, At(1050.0f), now + 1s, true);
	CHECK(!book.RivalOf(CP09, camp, now + 2s));
}

TEST("encounters: friends' encounter spawns take their quest's kind from our game's files")
{
	SpawnBook::Theirs                                records;
	std::unordered_map<std::uint32_t, std::uint64_t> seqs;
	for (const auto& [id, quest] : { std::pair{ SPAWN, LC01 }, std::pair{ SPAWN2, KMK01 } }) {
		auto spawn = Actor(id, quest, 1, SpawnKind::kEncounter);
		spawn.playerId = 7;
		REQUIRE(records.Add(spawn, 1));
	}
	const auto kinds = Encounters::TheirsOf(records, seqs, [](std::uint32_t a_quest) { return a_quest == LC01 ? Encounters::KindOf("RESceneLC01") : Encounters::KindOf("RETravelKMK01"); });
	REQUIRE(kinds.size() == 2);
	for (const auto& theirs : kinds) {
		CHECK(theirs.kind == (theirs.quest == LC01 ? Encounters::Kind::kScene : Encounters::Kind::kTravel));
	}
	// With no kinds told, none is known (each meets any).
	for (const auto& theirs : Encounters::TheirsOf(records, seqs)) {
		CHECK(theirs.kind == Encounters::Kind::kUnknown);
	}
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
	using Encounters::RefillWhy;
	auto    now = Encounters::Clock::now();
	RunBook book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(Protocol::RadiantStart{ HOST, CHECKPOINT, 77,
		{ { 0, 0, TRIGGER }, { 9, 0, SPAWN }, { 10, 0, Protocol::RADIANT_UNSHARED_REF }, { 11, 0, SPAWN2 }, { 4, 1, 0x0001F228 } }, {} }, now));
	auto& run = *book.Find({ HOST, 77 });

	std::unordered_map<std::uint32_t, std::uint32_t> copies{ { SPAWN, 0xFF100001 } };  // SPAWN2's copy not placed
	std::unordered_map<std::uint32_t, std::uint32_t> here{ { 9, 0xFF100001 }, { 0, TRIGGER } };
	const auto copyOf = [&](std::uint32_t a_spawn) { return copies.contains(a_spawn) ? copies[a_spawn] : 0u; };
	const auto hereOf = [&](std::uint32_t a_alias) { return here.contains(a_alias) ? here[a_alias] : 0u; };
	using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
	// The (alias, copy) pairs to force now, read back first as the game does.
	const auto refills = [&] {
		RunBook::ReadBack(run, hereOf);
		Pairs out;
		for (const auto& refill : RunBook::Refills(run, copyOf, hereOf, now)) {
			out.emplace_back(refill.alias, refill.copy);
		}
		return out;
	};

	CHECK(refills().empty());  // heard, not followed: nothing
	book.Started({ HOST, 77 }, now);
	CHECK(refills().empty());  // starting: the start forces them
	book.Following({ HOST, 77 });
	CHECK(refills().empty());  // each alias holds its copy, or has none placed

	// SPAWN2's copy placed later (our game had made its own for the alias, removed): forced in, once the ask is taken.
	copies[SPAWN2] = 0xFF100002;
	CHECK((refills() == Pairs{ { 11, 0xFF100002 } }));
	CHECK(RunBook::Refills(run, copyOf, hereOf, now).front().why == RefillWhy::kNew);
	RunBook::NoteForced(run, 11, 0xFF100002, true, now);
	CHECK(refills().empty());  // asked once; the script VM runs it a frame or so later
	here[11] = 0xFF100002;
	now += Encounters::FORCE_READ_BACK;
	CHECK(refills().empty());  // read back holding it

	// A save: every copy deleted (the aliases hold nothing the game knows) and placed again under new references.
	here.erase(9);
	here.erase(11);
	copies.clear();
	CHECK(refills().empty());  // none placed yet
	copies = { { SPAWN, 0xFF100003 }, { SPAWN2, 0xFF100004 } };
	CHECK((refills() == Pairs{ { 9, 0xFF100003 }, { 11, 0xFF100004 } }));
	RunBook::NoteForced(run, 9, 0xFF100003, true, now);
	RunBook::NoteForced(run, 11, 0xFF100004, true, now);
	here[9] = 0xFF100003;
	here[11] = 0xFF100004;
	CHECK(refills().empty());

	// An ask the VM did not take is made again on later polls, MAX_FORCE_TRIES times in all; then the copy is given up on
	// until another is placed.
	here.erase(9);
	copies[SPAWN] = 0xFF100005;
	for (std::uint32_t i = 0; i < Encounters::MAX_FORCE_TRIES; ++i) {
		CHECK((refills() == Pairs{ { 9, 0xFF100005 } }));
		RunBook::NoteForced(run, 9, 0xFF100005, false, now);
	}
	CHECK(refills().empty());  // given up on
	copies[SPAWN] = 0xFF100006;
	CHECK((refills() == Pairs{ { 9, 0xFF100006 } }));
	RunBook::NoteForced(run, 9, 0xFF100006, false, now);
	CHECK(RunBook::Refills(run, copyOf, hereOf, now).front().why == RefillWhy::kNotTaken);
	RunBook::NoteForced(run, 9, 0xFF100006, true, now);  // taken on the second ask
	CHECK(refills().empty());
}

// The rig, 2026-11-11 and 12 (radiant-rr): the friend's game logged "alias 5 forced onto our copy" (the ask taken by the
// script VM), yet the alias still held the friend's own courser, removed, when read seconds later; nothing read it back,
// so it was never asked again.
TEST("encounters: a forced alias is read back, and asked again when it doesn't hold the copy")
{
	using Encounters::RunBook;
	using Encounters::RefillWhy;
	using Backs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
	auto    now = Encounters::Clock::now();
	RunBook book{ 1000 };
	book.SetSelf(FRIEND);
	REQUIRE(book.Hear(Protocol::RadiantStart{ HOST, CHECKPOINT, 78, { { 5, 0, SPAWN } }, {} }, now));
	auto& run = *book.Find({ HOST, 78 });
	book.Started({ HOST, 78 }, now);
	book.Following({ HOST, 78 });

	constexpr std::uint32_t OWN_COURSER = 0xFF001096, COPY = 0xFF001104;
	std::unordered_map<std::uint32_t, std::uint32_t> copies{ { SPAWN, COPY } };
	std::uint32_t                                    held = OWN_COURSER;  // our own courser, made by our start
	const auto copyOf = [&](std::uint32_t a_spawn) { return copies.contains(a_spawn) ? copies[a_spawn] : 0u; };
	const auto hereOf = [&](std::uint32_t) { return held; };
	Backs                            backs;
	std::vector<Encounters::Refill> refills;
	const auto                       poll = [&] {
        backs = RunBook::ReadBack(run, hereOf);
        refills = RunBook::Refills(run, copyOf, hereOf, now);
	};

	poll();
	REQUIRE(refills.size() == 1);
	CHECK((refills[0] == Encounters::Refill{ 5, COPY, RefillWhy::kNew, OWN_COURSER }));
	RunBook::NoteForced(run, 5, COPY, true, now);

	// Not read back before FORCE_READ_BACK: the call may not have run yet.
	now += Encounters::FORCE_READ_BACK / 2;
	poll();
	CHECK(backs.empty());
	CHECK(refills.empty());

	// Read back still holding ours (removed: the game reads it as nothing): asked again.
	held = 0;
	now += Encounters::FORCE_READ_BACK;
	poll();
	REQUIRE(refills.size() == 1);
	CHECK((refills[0] == Encounters::Refill{ 5, COPY, RefillWhy::kNotHeld, 0 }));
	RunBook::NoteForced(run, 5, COPY, true, now);

	// The second ask holds: read back, with the asks it took.
	held = COPY;
	now += Encounters::FORCE_READ_BACK;
	poll();
	CHECK((backs == Backs{ { 5, 2 } }));
	CHECK(refills.empty());
	poll();
	CHECK(backs.empty());  // once
	CHECK(refills.empty());

	// Emptied since (the quest's own Clear, its target dead): left so.
	held = 0;
	poll();
	CHECK(refills.empty());

	// Another reference put in since (the quest's own script): forced back, with the asks left for this copy (one).
	held = 0xFF002000;
	poll();
	REQUIRE(refills.size() == 1);
	CHECK(refills[0].why == RefillWhy::kLost);
	RunBook::NoteForced(run, 5, COPY, true, now);
	now += Encounters::FORCE_READ_BACK;
	poll();
	REQUIRE(refills.size() == 1);
	CHECK(refills[0].why == RefillWhy::kGiveUp);  // three asks made for this copy
	RunBook::GiveUp(run, 5);
	poll();
	CHECK(refills.empty());

	// A new copy placed (after a save): its own asks, from the first.
	copies[SPAWN] = 0xFF001200;
	poll();
	REQUIRE(refills.size() == 1);
	CHECK((refills[0] == Encounters::Refill{ 5, 0xFF001200, RefillWhy::kNew, 0xFF002000 }));
}

// Every ask a forced alias takes (first and again) is read back: a copy the VM never holds is given up on after
// MAX_FORCE_TRIES asks, not asked forever.
TEST("encounters: a forced alias that never holds its copy is given up on after MAX_FORCE_TRIES asks")
{
	auto                                     now = Encounters::Clock::now();
	std::vector<Encounters::AliasForce>      forced;
	const std::vector<Protocol::RadiantFill> fills{ { 5, 0, SPAWN } };
	const auto                               copyOf = [](std::uint32_t) { return 0xFF001104u; };
	const auto                               hereOf = [](std::uint32_t) { return 0xFF001096u; };
	std::uint32_t                            asks = 0;
	for (int poll = 0; poll < 20; ++poll, now += Encounters::FORCE_READ_BACK) {
		Encounters::ReadBack(forced, hereOf);
		for (const auto& refill : Encounters::RefillsOf(fills, forced, copyOf, hereOf, now)) {
			if (refill.why == Encounters::RefillWhy::kGiveUp) {
				Encounters::GiveUp(forced, refill.alias);
				continue;
			}
			++asks;
			Encounters::NoteForced(forced, refill.alias, refill.copy, true, now);
		}
	}
	CHECK(asks == Encounters::MAX_FORCE_TRIES);
	REQUIRE(forced.size() == 1);
	CHECK(forced[0].state == Encounters::ForceState::kGivenUp);
}

TEST("encounters: an ask made again goes to the engine's own force, and none is made while the engine takes none")
{
	using Encounters::ForceWay;
	using Encounters::RefillWhy;
	// A first ask, and one the script VM did not take, go to the script VM; one made because the alias did not hold the
	// copy the VM took (RRR07's CourserTarget on the rig), or lost it since, to the engine's TESQuest::ForceRefIntoAlias.
	CHECK(Encounters::ForceWayOf(RefillWhy::kNew) == ForceWay::kScript);
	CHECK(Encounters::ForceWayOf(RefillWhy::kNotTaken) == ForceWay::kScript);
	CHECK(Encounters::ForceWayOf(RefillWhy::kNotHeld) == ForceWay::kEngine);
	CHECK(Encounters::ForceWayOf(RefillWhy::kLost) == ForceWay::kEngine);

	// The console's own test before its ForceRefIntoAlias: the quest enabled, no stop or start under way, no promotion
	// of its references pending.
	constexpr std::uint16_t RUNNING = Encounters::QUEST_ENABLED | 0x0040;  // and any flag the test doesn't read
	CHECK(Encounters::ForceOpen(RUNNING, false));
	CHECK(!Encounters::ForceOpen(RUNNING, true));
	CHECK(!Encounters::ForceOpen(RUNNING | Encounters::QUEST_STOP_START, false));
	CHECK(!Encounters::ForceOpen(0x0040, false));
	CHECK(Encounters::QUEST_ENABLED == QuestStartRules::ENABLED);
	CHECK(Encounters::QUEST_STOP_START == QuestStartRules::STOP_START);

	// An ask waited through is no ask: the copy is asked for as new once the engine takes forces again, and has all its
	// asks left.
	auto                                     now = Encounters::Clock::now();
	std::vector<Encounters::AliasForce>      forced;
	const std::vector<Protocol::RadiantFill> fills{ { 5, 0, SPAWN } };
	const auto                               copyOf = [](std::uint32_t) { return 0xFF001091u; };
	const auto                               hereOf = [](std::uint32_t) { return 0u; };
	for (int poll = 0; poll < 5; ++poll, now += Encounters::FORCE_READ_BACK) {
		const auto refills = Encounters::RefillsOf(fills, forced, copyOf, hereOf, now);
		REQUIRE(refills.size() == 1);
		CHECK(refills[0].why == RefillWhy::kNew);  // waited (ForceOpen false): NoteForced not called
	}
	CHECK(forced.empty());
}

TEST("encounters: our own actor of a followed run goes only once our start is done with it and it has stood a moment")
{
	using Encounters::FollowRemoval;
	using Encounters::JudgeFollowRemoval;
	// Our start of the run done with its references: no stop or start under way, no promotion pending; running or not.
	constexpr std::uint16_t RUNNING = Encounters::QUEST_ENABLED | 0x0040;
	CHECK(Encounters::StartDone(RUNNING, false));
	CHECK(Encounters::StartDone(0x0040, false));  // stopped since: nothing of a start left to wait for
	CHECK(!Encounters::StartDone(RUNNING, true));
	CHECK(!Encounters::StartDone(RUNNING | Encounters::QUEST_STOP_START, false));

	// The checkpoint's raider our start made beside our player, asked to go 284 ms after the start (the rig, 2026-11-12):
	// it waits for the start, then stands SHARE_SETTLE loaded, then goes.
	Encounters::ShareSettle settle;
	const auto              asked = Encounters::Clock::now();
	const auto              judge = [&](bool a_startDone, bool a_loaded, Encounters::Clock::time_point a_now) {
		const bool stood = a_loaded && settle.Ready(SPAWN, a_now, false);
		return JudgeFollowRemoval(a_startDone, stood, a_now - asked);
	};
	CHECK(judge(false, true, asked) == FollowRemoval::kWait);         // the promotion pending
	CHECK(judge(false, true, asked + 500ms) == FollowRemoval::kWait);
	CHECK(judge(true, true, asked + 1s) == FollowRemoval::kWait);     // done, but loaded only a second
	CHECK(judge(true, true, asked + Encounters::SHARE_SETTLE - 1ms) == FollowRemoval::kWait);
	CHECK(judge(true, true, asked + Encounters::SHARE_SETTLE) == FollowRemoval::kNow);

	// Not loaded yet (the refill's: an alias holding ours, not seen loaded): it waits for its 3D and SHARE_SETTLE after.
	settle.Clear();
	CHECK(judge(true, false, asked + 5s) == FollowRemoval::kWait);
	CHECK(judge(true, true, asked + 6s) == FollowRemoval::kWait);
	CHECK(judge(true, true, asked + 7s) == FollowRemoval::kWait);  // RemoveFollowed looks each frame
	CHECK(judge(true, true, asked + 8s) == FollowRemoval::kWait);
	CHECK(judge(true, true, asked + 6s + Encounters::SHARE_SETTLE) == FollowRemoval::kNow);

	// Never kept for ever: a start that never settles, or 3D that never comes, goes at FOLLOW_REMOVE_LIMIT as before.
	CHECK(JudgeFollowRemoval(false, false, Encounters::FOLLOW_REMOVE_LIMIT - 1ms) == FollowRemoval::kWait);
	CHECK(JudgeFollowRemoval(false, false, Encounters::FOLLOW_REMOVE_LIMIT) == FollowRemoval::kNow);
	CHECK(JudgeFollowRemoval(true, false, Encounters::FOLLOW_REMOVE_LIMIT) == FollowRemoval::kNow);
	// One that stood loaded long before it was asked about (a radiant's courser our player walked up to) still waits
	// SHARE_SETTLE from the first look: the wait is the removal's own.
	Encounters::ShareSettle fresh;
	CHECK(!fresh.Ready(SPAWN2, asked, false));
	CHECK(!fresh.Ready(SPAWN2, asked + 1s, false));
	CHECK(!fresh.Ready(SPAWN2, asked + 2s, false));
	CHECK(fresh.Ready(SPAWN2, asked + Encounters::SHARE_SETTLE, false));
}

TEST("encounters: an encounter stop waits while its quest's start or promotion is under way, and goes at the 30 s limit")
{
	using Encounters::StopDue;
	constexpr std::uint16_t RUNNING = Encounters::QUEST_ENABLED | 0x0040;
	// A natural roll beside a friend's run, booked for a stop as its start still promotes its references: it waits.
	CHECK(!StopDue(Encounters::StartDone(RUNNING, true), 0ms));
	CHECK(!StopDue(Encounters::StartDone(RUNNING | Encounters::QUEST_STOP_START, false), 284ms));
	CHECK(!StopDue(false, Encounters::FOLLOW_REMOVE_LIMIT - 1ms));
	// The start done (or the quest gone): stopped at once, as before the wait.
	CHECK(StopDue(Encounters::StartDone(RUNNING, false), 0ms));
	CHECK(StopDue(true, 0ms));
	CHECK(StopDue(true, 5s));
	// Never kept for ever: a start that never settles is stopped at FOLLOW_REMOVE_LIMIT all the same.
	CHECK(StopDue(false, Encounters::FOLLOW_REMOVE_LIMIT));
	CHECK(StopDue(false, Encounters::FOLLOW_REMOVE_LIMIT + 1s));
	// Well inside the book's STOPPED_FOR: the run's actors are never offered while the stop waits.
	CHECK(Encounters::FOLLOW_REMOVE_LIMIT < Encounters::STOPPED_FOR);

	// The book's doomed actors of a run whose stop waits stay for a later take; the others' are taken now.
	using Encounters::Book;
	using Doomed = Book::Doomed;
	Book       book;
	const auto now = Encounters::Clock::now();
	CHECK(book.OursSeen(LC01, OURS, At(1000.0f), now));
	CHECK(book.OursSeen(DN083, OURS2, At(-90000.0f), now));
	book.Stopped(LC01, now + 1s);
	book.Stopped(DN083, now + 1s);
	const std::array<std::uint32_t, 1> held{ LC01 };
	const std::vector<Doomed> other{ { OURS2, DN083 } }, waited{ { OURS, LC01 } };
	CHECK(book.TakeDoomed(held) == other);
	CHECK(book.TakeDoomed(held).empty());
	CHECK(book.TakeDoomed() == waited);  // its stop due: taken once
	CHECK(book.TakeDoomed().empty());
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

TEST("encounters: a save's run we followed is hollow with our own disabled reinforcements left in it")
{
	using Encounters::CreatedFills;
	using Encounters::IsHollow;
	// Every alias its start created empty: our copies of the owner's actors were never saved.
	CHECK(IsHollow(CreatedFills{ .created = 5, .empty = 5, .enabled = 0 }));
	// Rig run 11 (2026-11-12): RECheckpoint09 creates seven: its five attackers' aliases empty after the load, its two
	// reinforcements' holding our own, still disabled as the owner's were (unshared there, so our start made its own).
	// Not ours to share.
	CHECK(IsHollow(CreatedFills{ .created = 7, .empty = 5, .enabled = 0 }));
	// One of its actors stands (alive or a corpse, enabled): our own run.
	CHECK(!IsHollow(CreatedFills{ .created = 7, .empty = 5, .enabled = 1 }));
	CHECK(!IsHollow(CreatedFills{ .created = 1, .empty = 0, .enabled = 1 }));  // a treasure hunt's corpse
	// A checkpoint of our own just started: its attackers stand; its optional reinforcements (disabled) may not have been
	// made at all. Never hollow.
	CHECK(!IsHollow(CreatedFills{ .created = 7, .empty = 2, .enabled = 5 }));
	CHECK(!IsHollow(CreatedFills{ .created = 7, .empty = 0, .enabled = 5 }));
	// Every alias it creates filled, even with all of them waiting disabled: never hollow.
	CHECK(!IsHollow(CreatedFills{ .created = 6, .empty = 0, .enabled = 0 }));
	// A quest that creates nothing is no hollow run.
	CHECK(!IsHollow(CreatedFills{}));
}

TEST("encounters: a followed run's own actors not loaded in aliases the owner fills with its actors are disabled, so a save of it is hollow")
{
	using Encounters::HeldHere;
	using Fill = Protocol::RadiantFill;
	constexpr std::uint32_t VERTIBIRD = 0xFF020001;  // ours, made by our start for Attacker01 (alias 59), flying in far off
	constexpr std::uint32_t RAIDER_HERE = 0xFF020002;
	constexpr std::uint32_t REINFORCE = 0xFF020003;
	constexpr std::uint32_t COPY = 0xFF030001;
	// RECheckpoint05_Vertibird as the owner's game sent it: its vertibird and an attacker are its shared actors, a
	// reinforcement it left unshared (still disabled there), one alias empty there.
	const std::array fills{ Fill{ 59, 0, SPAWN }, Fill{ 60, 0, SPAWN2 }, Fill{ 83, 0, Protocol::RADIANT_UNSHARED_REF }, Fill{ 84, 0, 0 } };
	// Ours: the vertibird not loaded (far off), alias 60 forced onto our copy already, our own reinforcement disabled.
	const std::array here{ HeldHere{ 59, VERTIBIRD, true, false, true }, HeldHere{ 60, COPY, false, true, true }, HeldHere{ 83, REINFORCE, true, false, false } };
	CHECK(Encounters::DisableOnFollow(fills, here) == std::vector<std::uint32_t>{ VERTIBIRD });
	// Loaded, ours is the scan's to remove once it has stood a moment (never disabled under our player's eyes); disabled
	// already, nothing to do.
	const std::array loaded{ HeldHere{ 59, VERTIBIRD, true, true, true } };
	CHECK(Encounters::DisableOnFollow(fills, loaded).empty());
	const std::array done{ HeldHere{ 59, VERTIBIRD, true, false, false } };
	CHECK(Encounters::DisableOnFollow(fills, done).empty());
	// An alias the owner left unshared or empty keeps ours as it is, enabled or not: no copy of theirs will take its place.
	const std::array unshared{ HeldHere{ 83, REINFORCE, true, false, true }, HeldHere{ 84, RAIDER_HERE, true, false, true } };
	CHECK(Encounters::DisableOnFollow(fills, unshared).empty());
	// An alias the owner's run doesn't name, and one holding nothing.
	const std::array other{ HeldHere{ 90, RAIDER_HERE, true, false, true }, HeldHere{ 59, 0, true, false, true } };
	CHECK(Encounters::DisableOnFollow(fills, other).empty());

	// A save of the run then loaded after the owner's run ended: the aliases forced onto copies come back empty (no copy
	// is ever saved), the vertibird's alias holds ours disabled, the reinforcement's ours disabled: hollow, never shared
	// as our own run. Before, ours stood enabled in alias 59 and the run was taken for ours.
	CHECK(Encounters::IsHollow(Encounters::CreatedFills{ .created = 4, .empty = 2, .enabled = 0 }));
	CHECK(!Encounters::IsHollow(Encounters::CreatedFills{ .created = 4, .empty = 2, .enabled = 1 }));
}

TEST("encounters: a friend's run may leave empty the aliases its owner filled with what our game can't name")
{
	using Fill = Protocol::RadiantFill;
	using NoneNamed = std::span<const std::pair<std::uint32_t, std::uint32_t>>;
	constexpr std::uint8_t  REF = 0, LOC = 1;
	constexpr std::uint32_t CP_TRIGGER = 0x00169C68;  // the checkpoint's trigger, a plugin's reference both games have
	constexpr std::uint32_t PLACE = 0x0001A2B3;
	// A checkpoint run as the owner's game sent it: its trigger, its place, two attackers (one copy placed here, one not
	// yet), Defender01 an actor their game doesn't share (their defender, made by their Init quest), a reinforcement
	// their game left empty, and a plugin's reference our game doesn't have.
	const std::array fills{ Fill{ 0, REF, CP_TRIGGER }, Fill{ 1, LOC, PLACE }, Fill{ 2, REF, SPAWN }, Fill{ 3, REF, SPAWN2 }, Fill{ 4, REF, Protocol::RADIANT_UNSHARED_REF },
		Fill{ 5, REF, 0 }, Fill{ 6, REF, 0x00ABCDEF } };
	// Our game forces the trigger, the place and the attacker whose copy is placed.
	const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> named{ { { 0, CP_TRIGGER }, { 1, PLACE }, { 2, OURS } } };
	// The rest the owner filled may go empty here; the one empty there is no reason to loosen anything.
	CHECK((Encounters::LoosenOf(fills, named) == std::vector<std::uint32_t>{ 3, 4, 6 }));
	// Everything named: nothing to loosen; nothing filled there: nothing either.
	const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> all{ { { 0, CP_TRIGGER }, { 1, PLACE }, { 2, OURS }, { 3, OURS2 }, { 4, 0x00111111 }, { 6, 0x00ABCDEF } } };
	CHECK(Encounters::LoosenOf(fills, all).empty());
	const std::array none{ Fill{ 0, REF, 0 }, Fill{ 1, LOC, 0 } };
	CHECK(Encounters::LoosenOf(none, NoneNamed{}).empty());
	// An alias named twice in what came is loosened once.
	const std::array twice{ Fill{ 4, REF, Protocol::RADIANT_UNSHARED_REF }, Fill{ 4, REF, Protocol::RADIANT_UNSHARED_REF } };
	CHECK((Encounters::LoosenOf(twice, NoneNamed{}) == std::vector<std::uint32_t>{ 4 }));
}

TEST("encounters: an actor of our run is offered only once it has stood alive a moment, unless it fights")
{
	// RESceneCC_DN101's three dead raiders are made alive and killed by their own script as they load: offered at once, they
	// went out alive and hostile, a friend's game stopped its fight for them, and they died (the rig, 2026-11-12).
	Encounters::ShareSettle settle;
	const auto              t0 = Encounters::Clock::now();
	// The scan looks four times a second.
	CHECK(!settle.Ready(SPAWN, t0, false));
	CHECK(!settle.Ready(SPAWN, t0 + 1s, false));
	CHECK(!settle.Ready(SPAWN, t0 + 2s, false));
	CHECK(!settle.Ready(SPAWN, t0 + Encounters::SHARE_SETTLE - 1ms, false));
	CHECK(settle.Ready(SPAWN, t0 + Encounters::SHARE_SETTLE, false));
	CHECK(settle.Begun() == 1);
	// Each actor waits from its own first sighting.
	CHECK(!settle.Ready(SPAWN2, t0 + 1s, false));
	CHECK(!settle.Ready(SPAWN2, t0 + 2s, false));
	CHECK(!settle.Ready(SPAWN2, t0 + Encounters::SHARE_SETTLE, false));
	CHECK(settle.Ready(SPAWN2, t0 + 1s + Encounters::SHARE_SETTLE, false));
	// One fighting already goes at once, and its wait is over.
	CHECK(!settle.Ready(SPAWN3, t0, false));
	CHECK(settle.Ready(SPAWN3, t0 + 1ms, true));
	CHECK(!settle.Ready(SPAWN3, t0 + 2ms, false));  // seen afresh: a new wait
	// Offered: forgotten, and seen again it waits afresh (a fresh roll of the same id later).
	settle.Forget(SPAWN);
	CHECK(!settle.Ready(SPAWN, t0 + 10s, false));
	CHECK(settle.Size() == 3);
	CHECK(settle.Begun() == 5);
	// Not looked at for more than GAP (it left, or the id names another actor now): seen again, it waits afresh, so a
	// corpse made under a reused id never goes out on the old actor's wait.
	CHECK(!settle.Ready(SPAWN, t0 + 10s + Encounters::ShareSettle::GAP + 1ms, false));
	CHECK(settle.Begun() == 6);
	CHECK(!settle.Ready(SPAWN, t0 + 14s, false));
	CHECK(settle.Ready(SPAWN, t0 + 10s + Encounters::ShareSettle::GAP + 1ms + Encounters::SHARE_SETTLE, false));
	CHECK(!settle.Ready(SPAWN, t0 + 70s, false));  // looked at again after a long gap: afresh
	// A wait not looked at for KEEP is of an actor gone without being offered (the corpse, dead by then): it goes.
	settle.Expire(t0 + 4s + Encounters::ShareSettle::KEEP - 1ms);
	CHECK(settle.Expired() == 1);  // SPAWN3, last looked at t0 + 2 ms
	settle.Expire(t0 + 4s + Encounters::ShareSettle::KEEP);
	CHECK(settle.Expired() == 2);  // and SPAWN2, last looked at t0 + 4 s; SPAWN was at t0 + 70 s
	CHECK(settle.Size() == 1);
	settle.Clear();
	CHECK(settle.Size() == 0);
}
