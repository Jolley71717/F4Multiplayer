#include "ServerFixture.h"
#include "Test.h"

#include "Protocol.h"
#include "game/EnemyList.h"

#include <cstdint>
#include <optional>
#include <vector>

// The `enemies` dev command (src/game/EnemyList.h), as the co-op follower script uses it: the host's game runs a
// raider (it claimed it through the real server) and has an enemy of its own made at runtime; the joiner's game has
// a copy of the raider and its own spawned enemy of the same kind. The script reads both games' answers and has to
// tell which enemies fight either player, and which actor in the joiner's game is the one the host's game lists.
namespace
{
	using namespace TestServer;

	constexpr std::uint32_t RAIDER = 0x0001A4D7;
	constexpr std::uint32_t RAIDER_BASE = 0x000F5B6B;
	constexpr std::uint32_t GHOUL_BASE = 0x0002A4E1;

	EnemyList::Seen Actor(std::uint32_t a_id, std::uint32_t a_base, const char* a_name, float a_x, float a_y, float a_distance)
	{
		EnemyList::Seen seen;
		seen.id = a_id;
		seen.base = a_base;
		seen.name = a_name;
		seen.x = a_x;
		seen.y = a_y;
		seen.z = 120.0f;
		seen.distance = a_distance;
		return seen;
	}

	// Who runs an NPC in one game, from the owners the server sent it (as NpcSync::ApplyOwners keeps them).
	std::uint32_t OwnerFrom(Fixture& a_f, PeerId a_peer, std::uint32_t a_refId)
	{
		std::uint32_t owner = 0;
		for (const auto& packet : Received(a_f, a_peer)) {
			if (!packet.empty() && packet[0] == Type(MT::kActorOwners)) {
				const auto owners = Protocol::DecodeOwners(packet);
				REQUIRE(owners);
				for (const auto& entry : *owners) {
					if (entry.refId == a_refId) {
						owner = entry.playerId;
					}
				}
			}
		}
		return owner;
	}

	const EnemyList::Seen* Find(const std::vector<EnemyList::Seen>& a_list, std::uint32_t a_id)
	{
		for (const auto& seen : a_list) {
			if (seen.id == a_id) {
				return &seen;
			}
		}
		return nullptr;
	}
}

TEST("enemies: the follower reads what an enemy fights from the game that runs it, as a session player, and finds a spawned one in its own game")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto joiner = f.Join(2, Fixture::MakeHello("Joiner"));
	REQUIRE(joiner);
	f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { RAIDER, Protocol::ClaimReason::kUnowned } }));
	const auto ownerAtHost = OwnerFrom(f, 1, RAIDER);
	const auto ownerAtJoiner = OwnerFrom(f, 2, RAIDER);
	REQUIRE(ownerAtHost == host->playerId);
	REQUIRE(ownerAtJoiner == host->playerId);

	// The host's game: the raider it runs shoots at the joiner's stand-in there (a form ID only that game has), and its
	// own spawned ghoul goes for the host. The stand-in itself and a settler nearby are never listed.
	constexpr std::uint32_t JOINER_STAND_IN_AT_HOST = 0xFF000E21;
	std::vector<EnemyList::Seen> atHost;
	{
		auto raider = Actor(RAIDER, RAIDER_BASE, "Raider", 1000.0f, 2000.0f, 900.0f);
		raider.hostile = true;
		raider.inCombat = true;
		raider.target = JOINER_STAND_IN_AT_HOST;
		raider.targetPlayer = joiner->playerId;  // RemotePlayers::PlayerIdFor the stand-in
		raider.lineOfSight = true;
		raider.owner = ownerAtHost;
		raider.runsHere = EnemyList::RunsHere(ownerAtHost, host->playerId);
		atHost.push_back(raider);
		auto ghoul = Actor(0xFF000A10, GHOUL_BASE, "Feral Ghoul", 1500.0f, 1800.0f, 400.0f);
		ghoul.hostile = true;
		ghoul.inCombat = true;
		ghoul.target = Protocol::PLAYER_REF_ID;
		ghoul.targetPlayer = host->playerId;
		ghoul.runsHere = EnemyList::RunsHere(0, host->playerId);
		atHost.push_back(ghoul);
		auto standIn = Actor(JOINER_STAND_IN_AT_HOST, 0x0001A4D8, "Joiner", 1200.0f, 1900.0f, 100.0f);
		standIn.standIn = true;
		atHost.push_back(standIn);
		atHost.push_back(Actor(0x0002F2A6, 0x00020593, "Settler", 1100.0f, 1700.0f, 200.0f));
	}
	const auto hostAnswer = EnemyList::List(atHost);
	const auto fromHost = EnemyList::Parse(hostAnswer);
	REQUIRE(fromHost.size() == 2);
	CHECK(fromHost[0].id == 0xFF000A10);  // nearest first
	CHECK(fromHost[1].id == RAIDER);
	CHECK(hostAnswer.find("made=true") != std::string::npos);

	// The joiner's game: its copy of the raider has its AI off and fights nothing there (whatever its stale target), so its
	// line says to ask the host; its own spawned ghoul stands about where the host's does, with another form ID.
	std::vector<EnemyList::Seen> atJoiner;
	{
		auto raider = Actor(RAIDER, RAIDER_BASE, "Raider", 1010.0f, 1990.0f, 300.0f);
		raider.hostile = true;
		raider.inCombat = true;
		raider.target = 0x00000014;
		raider.targetPlayer = joiner->playerId;
		raider.owner = ownerAtJoiner;
		raider.runsHere = EnemyList::RunsHere(ownerAtJoiner, joiner->playerId);
		atJoiner.push_back(raider);
		auto ghoul = Actor(0xFF0007C3, GHOUL_BASE, "Feral Ghoul", 1530.0f, 1760.0f, 700.0f);
		ghoul.hostile = true;
		atJoiner.push_back(ghoul);
		atJoiner.push_back(Actor(0xFF0007C9, GHOUL_BASE, "Feral Ghoul", 2600.0f, 1760.0f, 1400.0f));  // another, further off
	}
	const auto fromJoiner = EnemyList::Parse(EnemyList::List(atJoiner));
	const auto copy = Find(fromJoiner, RAIDER);
	REQUIRE(copy);
	CHECK(!copy->runsHere);
	CHECK(copy->owner == host->playerId);
	CHECK(copy->target == 0);
	CHECK(copy->targetPlayer == 0);

	// So the follower asks the owner's game: there the raider fights the joiner, by the number both games know.
	const auto owned = Find(fromHost, copy->id);
	REQUIRE(owned);
	CHECK(owned->runsHere);
	CHECK(owned->targetPlayer == joiner->playerId);
	CHECK(owned->lineOfSight);

	// The host's ghoul has no form ID in the joiner's game: the joiner's own ghoul at that spot is the one to shoot.
	const auto& ghoulAtHost = fromHost[0];
	CHECK(!Find(fromJoiner, ghoulAtHost.id));
	const auto mine = EnemyList::Like(atJoiner, ghoulAtHost.base, ghoulAtHost.x, ghoulAtHost.y);
	REQUIRE(mine);
	CHECK(mine->id == 0xFF0007C3);
	// Too far from anything of its kind: none (the host's game alone has it).
	CHECK(!EnemyList::Like(atJoiner, ghoulAtHost.base, ghoulAtHost.x + 5000.0f, ghoulAtHost.y));
	// A leveled enemy can roll another base in each game: "any" takes the nearest live one.
	CHECK(EnemyList::Like(atJoiner, 0, 1000.0f, 2000.0f)->id == RAIDER);
}

TEST("enemies: who is listed")
{
	EnemyList::Seen raider;
	raider.id = RAIDER;
	raider.hostile = true;
	CHECK(EnemyList::Listed(raider));
	auto dead = raider;
	dead.dead = true;
	CHECK(!EnemyList::Listed(dead));
	auto companion = raider;
	companion.companion = true;
	CHECK(!EnemyList::Listed(companion));
	auto standIn = raider;
	standIn.standIn = true;
	CHECK(!EnemyList::Listed(standIn));

	// Not hostile, but fighting us (a settler we shot) or a friend: listed while its AI runs here.
	EnemyList::Seen settler;
	settler.id = 0x0002F2A6;
	CHECK(!EnemyList::Listed(settler));
	settler.inCombat = true;
	settler.target = Protocol::PLAYER_REF_ID;  // outside a session there is no player number
	CHECK(EnemyList::Listed(settler));
	settler.target = 0xFF000E21;
	CHECK(!EnemyList::Listed(settler));  // fighting someone who is no player
	settler.targetPlayer = 2;
	CHECK(EnemyList::Listed(settler));
	settler.runsHere = false;  // a copy: what it fights here means nothing
	CHECK(!EnemyList::Listed(settler));

	CHECK(EnemyList::RunsHere(0, 0));
	CHECK(EnemyList::RunsHere(0, 3));
	CHECK(EnemyList::RunsHere(3, 3));
	CHECK(!EnemyList::RunsHere(1, 3));
	CHECK(EnemyList::MadeHere(0xFF000A10));
	CHECK(!EnemyList::MadeHere(RAIDER));
	CHECK(!EnemyList::MadeHere(Protocol::PLAYER_REF_ID));
}

TEST("enemies: the answer stays readable: nearest first, capped, a name's apostrophe kept out of the quotes")
{
	std::vector<EnemyList::Seen> seen;
	for (std::uint32_t i = 0; i < 40; ++i) {
		EnemyList::Seen raider;
		raider.id = 0x00100000 + i;
		raider.base = RAIDER_BASE;
		raider.name = i == 27 ? "Raider's Dog" : "Raider";
		raider.hostile = true;
		raider.distance = static_cast<float>(4000 - i * 50);
		raider.x = -1234.0f;
		raider.y = static_cast<float>(i);
		seen.push_back(raider);
	}
	const auto answer = EnemyList::List(seen);
	const auto back = EnemyList::Parse(answer);
	REQUIRE(back.size() == EnemyList::MAX_LISTED);
	CHECK(back.front().id == 0x00100000 + 39);
	for (std::size_t i = 1; i < back.size(); ++i) {
		CHECK(back[i - 1].distance <= back[i].distance);
	}
	const auto dog = Find(back, 0x0010001B);
	REQUIRE(dog);
	CHECK(dog->name == "Raider`s Dog");
	CHECK(dog->base == RAIDER_BASE);
	CHECK(dog->x == -1234.0f);
	CHECK(dog->hostile);
	CHECK(dog->runsHere);

	CHECK(EnemyList::List({}) == "none");
	CHECK(EnemyList::Parse("none").empty());
	CHECK(EnemyList::Parse("error: not in game").empty());
}
