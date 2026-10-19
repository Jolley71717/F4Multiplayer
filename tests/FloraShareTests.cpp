// Plants a player picks are picked in every game (src/game/FloraShare.h): the harvest goes to the session as a pickup
// of the plant, and every other game, a joiner's too, marks its plant harvested instead of taking anything out of it.

#include "ServerFixture.h"

#include "game/FloraShare.h"

#include <map>

using namespace TestServer;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t TATO = 0x0001F0C5;     // a tato plant placed in the game's data
	constexpr std::uint32_t MUTFRUIT = 0x0001F0C6;
	constexpr std::uint32_t GROWN = 0xFF001234;    // a plant a settlement grew at runtime: only its builder's game has it
	constexpr std::uint32_t ITEM = 0x0001F0C7;     // a loose item

	// One game's plants and loose items, as WorldSync reads and changes them.
	struct PlantGame
	{
		struct Ref
		{
			bool          flora = true;
			std::uint32_t formFlags = 0;
			bool          disabled = false;
		};
		std::map<std::uint32_t, Ref> refs;
		int                          harvested = 0;  // SetHarvested runs
		int                          taken = 0;      // loose items taken out of the world

		PlantGame()
		{
			refs[TATO] = {};
			refs[MUTFRUIT] = {};
			refs[ITEM] = { .flora = false };
		}

		// Our player picks a plant: what goes to the server.
		std::optional<std::vector<std::uint8_t>> Pick(std::uint32_t a_ref, bool a_byOurPlayer = true)
		{
			if (auto it = refs.find(a_ref); it != refs.end()) {
				it->second.formFlags |= FloraShare::HARVESTED_FLAG;
			}
			if (!FloraShare::Reports(a_byOurPlayer, Protocol::IsShareableRef(a_ref))) {
				return std::nullopt;
			}
			return Protocol::Encode(Protocol::RefPickedUp{ a_ref }, MT::kReportPickup);
		}

		void Offer(std::uint32_t a_ref)
		{
			const auto it = refs.find(a_ref);
			if (it == refs.end()) {
				return;
			}
			auto& ref = it->second;
			switch (FloraShare::OnPickup(ref.flora, FloraShare::Harvested(ref.formFlags))) {
			case FloraShare::Pickup::kHarvest:
				ref.formFlags |= FloraShare::HARVESTED_FLAG;
				++harvested;
				return;
			case FloraShare::Pickup::kHarvested:
				return;
			case FloraShare::Pickup::kItem:
				if (!ref.disabled) {
					ref.disabled = true;
					++taken;
				}
				return;
			}
		}

		bool Picked(std::uint32_t a_ref) const { return FloraShare::Harvested(refs.at(a_ref).formFlags); }

		void Take(const Packets& a_packets)
		{
			for (const auto& pickup : Decoded(a_packets, MT::kRefPickedUp, Protocol::DecodeRefPickedUp)) {
				Offer(pickup.refId);
			}
			for (const auto& world : WorldStates(a_packets)) {
				for (const auto ref : world.pickedUp) {
					Offer(ref);
				}
			}
		}
	};
}

TEST("flora share: only our player's pick of a plant every game has is reported")
{
	CHECK(FloraShare::Reports(true, true));
	CHECK(!FloraShare::Reports(false, true));  // an NPC (a settler) picked it
	CHECK(!FloraShare::Reports(true, false));  // grown at runtime
	CHECK(FloraShare::OnPickup(true, false) == FloraShare::Pickup::kHarvest);
	CHECK(FloraShare::OnPickup(true, true) == FloraShare::Pickup::kHarvested);
	CHECK(FloraShare::OnPickup(false, false) == FloraShare::Pickup::kItem);
	CHECK(FloraShare::Harvested(0x2000));
	CHECK(!FloraShare::Harvested(0x0800));  // initially disabled, not harvested
}

TEST("flora share: a plant the host picks is picked in the friend's game, and nothing is taken out of the world")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	PlantGame host, friendGame;

	const auto report = host.Pick(TATO);
	REQUIRE(report);
	f.fake->Deliver(1, *report);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.Picked(TATO));
	CHECK(!friendGame.Picked(MUTFRUIT));
	CHECK(friendGame.harvested == 1);
	CHECK(friendGame.taken == 0);
	CHECK(!friendGame.refs[TATO].disabled);  // the plant stays, harvested: it grows back
	CHECK(Count(Received(f, 1), MT::kRefPickedUp) == 0);  // not echoed to the picker
}

TEST("flora share: the friend's pick reaches the host, and a plant picked in both games is harvested once")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	PlantGame host, friendGame;

	f.fake->Deliver(2, *friendGame.Pick(MUTFRUIT));
	host.Take(Received(f, 1));
	CHECK(host.Picked(MUTFRUIT));
	CHECK(host.harvested == 1);

	// The host picked it too before the news came: its report is dropped by the server, and would change nothing.
	f.fake->Deliver(1, *host.Pick(MUTFRUIT));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.harvested == 0);
	host.Offer(MUTFRUIT);
	CHECK(host.harvested == 1);
}

TEST("flora share: a joiner and a game that reloads find the session's picked plants picked")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	PlantGame host;
	f.fake->Deliver(1, *host.Pick(TATO));
	CHECK(!host.Pick(GROWN));  // the settlement's own crop stays in the builder's game
	Clear(f, { 1, 2 });

	PlantGame joiner;
	JoinAs(f, 3, "Carol");
	joiner.Take(Received(f, 3));
	CHECK(joiner.Picked(TATO));
	CHECK(!joiner.Picked(MUTFRUIT));
	CHECK(joiner.harvested == 1);

	// Bob reloads a save from before the pick: the world state he asks for carries it.
	PlantGame reloaded;
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	reloaded.Take(Received(f, 2));
	CHECK(reloaded.Picked(TATO));
}

TEST("flora share: a loose item's pickup still takes the item out of the world")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	PlantGame friendGame;
	f.fake->Deliver(1, Protocol::Encode(Protocol::RefPickedUp{ ITEM }, MT::kReportPickup));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.taken == 1);
	CHECK(friendGame.harvested == 0);
}
