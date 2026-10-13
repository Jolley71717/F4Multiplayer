// A mirrored NPC's gear: the owner's game reports what its copy wears, the server relays it, and each other
// game dresses its own copy from that copy's own inventory (GearPlan). The NPC is a real one of that game and
// goes into its saves, so nothing may be given to it or taken off it.

#include "ServerFixture.h"

#include "game/GearPlan.h"

#include <set>

using namespace TestServer;

namespace
{
	using GearPlan::Slots;

	constexpr std::uint32_t SETTLER = 0x0002F001;
	constexpr std::uint32_t RAIDER = 0x0002F002;

	// Items, with the slots they fill.
	constexpr std::uint32_t SETTLER_CLOTHES = 0x00011001;  // body
	constexpr std::uint32_t COMBAT_SUIT = 0x00011002;      // body: what the host dressed its settler in
	constexpr std::uint32_t HAT = 0x00011003;              // head
	constexpr std::uint32_t GLASSES = 0x00011004;          // eyes
	constexpr std::uint32_t PIPE_PISTOL = 0x00011005;      // hands
	constexpr std::uint32_t COMBAT_RIFLE = 0x00011006;     // hands: what the host gave its settler
	constexpr std::uint32_t FRAG_GRENADE = 0x00011007;     // the grenade slot
	constexpr std::uint32_t T45_HELMET = 0x00011008;       // a power armor piece
	constexpr std::uint32_t T45_TORSO = 0x00011009;        // a power armor piece

	Slots SlotsOf(std::uint32_t a_item)
	{
		switch (a_item) {
		case SETTLER_CLOTHES:
		case COMBAT_SUIT:
			return Slots{ 1 } << 3;
		case HAT:
		case T45_HELMET:
			return Slots{ 1 } << 0;
		case GLASSES:
			return Slots{ 1 } << 17;
		case T45_TORSO:
			return Slots{ 1 } << 11;
		case PIPE_PISTOL:
		case COMBAT_RIFLE:
			return GearPlan::WEAPON_SLOT;
		case FRAG_GRENADE:
			return GearPlan::THROWABLE_SLOT;
		default:
			return 0;
		}
	}

	bool IsPiece(std::uint32_t a_item) { return a_item == T45_HELMET || a_item == T45_TORSO; }

	// One game's copy of an NPC, as the engine treats it: equipping an item takes off whatever it wears in the
	// same slots, and an item it does not carry cannot be equipped.
	struct NpcCopy
	{
		std::multiset<std::uint32_t> carried;
		std::vector<std::uint32_t>   worn;
		bool                         inFrame = false;

		std::vector<std::uint32_t> Plan(const std::vector<std::uint32_t>& a_wanted) const
		{
			return GearPlan::Equip(worn, a_wanted, inFrame, [&](std::uint32_t a_item) { return carried.contains(a_item); }, IsPiece, SlotsOf);
		}

		void Equip(std::uint32_t a_item)
		{
			REQUIRE(carried.contains(a_item));
			std::erase_if(worn, [&](std::uint32_t a_worn) { return (SlotsOf(a_worn) & SlotsOf(a_item)) != 0; });
			worn.push_back(a_item);
		}

		// What Equipment::Wear does with a report: the plan's equips, nothing else.
		void Dress(const std::vector<std::uint32_t>& a_wanted)
		{
			for (const auto item : Plan(a_wanted)) {
				Equip(item);
			}
		}

		bool Wears(std::uint32_t a_item) const { return std::ranges::contains(worn, a_item); }
	};

	std::vector<std::uint8_t> Claim(std::uint32_t a_ref)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Protocol::ClaimReason::kUnowned } });
	}

	std::vector<std::uint8_t> Gear(std::uint32_t a_ref, std::vector<std::uint32_t> a_items)
	{
		return Protocol::Encode(Protocol::NpcEquipment{ 0, a_ref, std::move(a_items) }, MT::kReportNpcEquipment);
	}

	// The next gear report the server sends a_peer, decoded.
	std::optional<Protocol::NpcEquipment> NextGear(Fixture& a_f, PeerId a_peer, std::chrono::milliseconds a_wait = std::chrono::milliseconds(2000))
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kNpcEquipment), a_wait);
		return packet ? Protocol::DecodeNpcEquipment(*packet) : std::nullopt;
	}
}

TEST("npc gear: a settler the host dressed keeps the host's armor and gun while a friend's game runs it, and is given nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	// Bob walked up to the settler first: his game runs it, in its own roll's clothes, hat, glasses and pistol.
	f.fake->Deliver(2, Claim(SETTLER));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	f.fake->Deliver(2, Gear(SETTLER, { SETTLER_CLOTHES, HAT, GLASSES, PIPE_PISTOL }));

	// The host's settler wears what the host gave it; its own clothes, pistol and glasses are in its pack.
	NpcCopy hosts{ .carried = { SETTLER_CLOTHES, COMBAT_SUIT, COMBAT_RIFLE, PIPE_PISTOL, GLASSES }, .worn = { COMBAT_SUIT, COMBAT_RIFLE } };
	const auto carriedBefore = hosts.carried;
	const auto report = NextGear(f, 1);
	REQUIRE(report);
	CHECK(report->playerId == bob->playerId);
	hosts.Dress(report->items);

	CHECK(hosts.Wears(COMBAT_SUIT));   // not swapped for its own clothes
	CHECK(hosts.Wears(COMBAT_RIFLE));  // nor its rifle for its pistol
	CHECK(hosts.Wears(GLASSES));       // a free slot takes what both copies have
	CHECK(!hosts.Wears(HAT));          // it has no hat, and is not given Bob's
	CHECK(hosts.carried == carriedBefore);

	// Bob's game reports again (its copy drew and holstered, a load re-registered the mirror): nothing more to do.
	f.fake->Deliver(2, Gear(SETTLER, { SETTLER_CLOTHES, HAT, GLASSES, PIPE_PISTOL }));
	const auto again = NextGear(f, 1);
	REQUIRE(again);
	CHECK(hosts.Plan(again->items).empty());
}

TEST("npc gear: someone who doesn't run the NPC can't dress it in power armor pieces in the others' games")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Deliver(2, Claim(SETTLER));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kActorOwners)));

	f.fake->Deliver(3, Gear(SETTLER, { T45_HELMET, T45_TORSO }));  // Carol doesn't run it
	CHECK(!NextGear(f, 1, std::chrono::milliseconds(300)));

	// Even from its owner, pieces it happens to carry go on no body: they are a frame's.
	f.fake->Deliver(2, Gear(SETTLER, { T45_HELMET, T45_TORSO, PIPE_PISTOL }));
	const auto report = NextGear(f, 1);
	REQUIRE(report);
	NpcCopy hosts{ .carried = { T45_HELMET, T45_TORSO, PIPE_PISTOL, SETTLER_CLOTHES }, .worn = { SETTLER_CLOTHES } };
	CHECK((hosts.Plan(report->items) == std::vector<std::uint32_t>{ PIPE_PISTOL }));
}

TEST("npc gear: a raider sitting in its power armor in our game is left in its frame, whatever its owner's copy wears")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(2, Claim(RAIDER));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	f.fake->Deliver(2, Gear(RAIDER, { SETTLER_CLOTHES, PIPE_PISTOL, FRAG_GRENADE }));  // in Bob's game it got out
	const auto report = NextGear(f, 1);
	REQUIRE(report);

	NpcCopy hosts{ .carried = { T45_HELMET, T45_TORSO, PIPE_PISTOL, SETTLER_CLOTHES }, .worn = { T45_HELMET, T45_TORSO }, .inFrame = true };
	CHECK(hosts.Plan(report->items).empty());
}

TEST("npc gear: a newcomer dresses its copy from the last report once, and its corpse holds only its own roll")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Deliver(1, Claim(RAIDER));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	f.fake->Deliver(1, Gear(RAIDER, { HAT, COMBAT_RIFLE }));
	f.fake->Deliver(1, Gear(RAIDER, { HAT, PIPE_PISTOL }));  // it dropped the rifle and drew a pistol

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto report = NextGear(f, 2);
	REQUIRE(report);
	CHECK((report->items == std::vector<std::uint32_t>{ HAT, PIPE_PISTOL }));
	CHECK(!NextGear(f, 2, std::chrono::milliseconds(300)));  // one report, the last

	// Bob's copy rolled a pistol and nothing for its head.
	NpcCopy bobs{ .carried = { PIPE_PISTOL }, .worn = {} };
	bobs.Dress(report->items);
	CHECK(bobs.Wears(PIPE_PISTOL));
	CHECK((bobs.carried == std::multiset<std::uint32_t>{ PIPE_PISTOL }));  // looted later: no hat, no rifle, no second gun
}

// --- the rule on its own ---------------------------------------------------------------------------

TEST("gear plan: only what the copy carries and doesn't wear yet, in the owner's order, once each")
{
	NpcCopy copy{ .carried = { GLASSES, PIPE_PISTOL, HAT }, .worn = { HAT } };
	CHECK((copy.Plan({ PIPE_PISTOL, COMBAT_RIFLE, HAT, GLASSES, PIPE_PISTOL }) == std::vector<std::uint32_t>{ PIPE_PISTOL, GLASSES }));
}

TEST("gear plan: nothing it wears that the owner's copy doesn't is pushed out of its slot")
{
	NpcCopy copy{ .carried = { SETTLER_CLOTHES, COMBAT_SUIT, HAT }, .worn = { COMBAT_SUIT } };
	const auto plan = copy.Plan({ SETTLER_CLOTHES, HAT });
	CHECK((plan == std::vector<std::uint32_t>{ HAT }));
	// But the owner's copy wearing the same body piece frees nothing that matters: it is already on.
	NpcCopy same{ .carried = { COMBAT_SUIT, HAT }, .worn = { COMBAT_SUIT } };
	CHECK((same.Plan({ COMBAT_SUIT, HAT }) == std::vector<std::uint32_t>{ HAT }));
}

TEST("gear plan: an owner's copy that took something off leaves ours wearing it")
{
	NpcCopy copy{ .carried = { HAT, PIPE_PISTOL }, .worn = { HAT, PIPE_PISTOL } };
	copy.Dress({ PIPE_PISTOL });
	CHECK(copy.Wears(HAT));
	CHECK(copy.Wears(PIPE_PISTOL));
}
