// A friend's gear as their stand-in here is dressed in it (game/StandInGear.h). What their game reports goes through a
// real server; what reaches our game is fed, frame by frame, to StandInGear::Dress, the function RemotePlayers asks, and
// what it says is done to a stand-in played by the test: what it wears, its power armor frame and skeleton, the engine
// putting the NPC's own outfit back on it, and its gun put away and drawn again.

#include "ServerFixture.h"

#include "game/AttackKind.h"
#include "game/StandInGear.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using ArmorFrames::Dress;
	using StandInGear::Put;

	constexpr std::uint32_t PISTOL = 0x0000DF42;
	constexpr std::uint32_t RIFLE = 0x0000E27B;
	constexpr std::uint32_t VAULT_SUIT = 0x0001EED7;
	constexpr std::uint32_t HAT = 0x000F4D22;
	constexpr std::uint32_t GLASSES = 0x0018F5B3;
	constexpr std::uint32_t FRAG = 0x0000E56B;         // a grenade: counted as worn by its owner's game while they have one
	constexpr std::uint32_t T60_HELMET = 0x00154ABD;   // a power armor piece
	constexpr std::uint32_t T60_TORSO = 0x00154AC1;
	constexpr std::uint32_t FRAME = 0x00031CE5;        // the frame itself as an item
	constexpr std::uint32_t PLAIN_FRAME = 0x0003E577;    // the frame item the engine picks for an actor in a vault suit
	constexpr std::uint32_t ATOMCAT_FRAME = 0x001909D6;  // the engine's default frame item
	constexpr std::uint32_t SETTLER_RAGS = 0x0001C2B9;  // the stand-in's own outfit, which the engine can put back on it

	// Mods on her pistol (OMOD forms), each in its attach point (the index the engine keeps with it).
	constexpr Protocol::ItemMod STANDARD_RECEIVER{ 0x0004F21D, 0, 0 };
	constexpr Protocol::ItemMod SHORT_BARREL{ 0x0004F21E, 1, 0 };
	constexpr Protocol::ItemMod LONG_BARREL{ 0x0004F21F, 1, 0 };
	constexpr Protocol::ItemMod IRON_SIGHTS{ 0x0004F220, 2, 0 };
	constexpr Protocol::ItemMod REFLEX_SIGHT{ 0x0004F221, 2, 1 };
	// Mods from a plugin Bob's game hasn't (another load order, or one the host ignores).
	constexpr Protocol::ItemMod CUSTOM_BARREL{ 0x0A000F01, 1, 0 };
	constexpr Protocol::ItemMod CUSTOM_BARREL_LONG{ 0x0A000F02, 1, 0 };
	constexpr Protocol::ItemMod CUSTOM_LINING{ 0x0A000F03, 0, 0 };
	// What a copy given with nothing on record is built with: the item's default combination (stands for its mods).
	const std::vector<Protocol::ItemMod> DEFAULT_COMBINATION{ { 0x00DEFA17, 0, 0 } };

	bool InHand(std::uint32_t a_item)
	{
		return a_item == PISTOL || a_item == RIFLE;
	}

	ArmorFrames::Item KindOf(std::uint32_t a_item)
	{
		if (a_item == PISTOL || a_item == RIFLE || a_item == FRAG) {
			return ArmorFrames::Item::kWeapon;
		}
		if (a_item == T60_HELMET || a_item == T60_TORSO) {
			return ArmorFrames::Item::kPiece;
		}
		return a_item == FRAME || a_item == PLAIN_FRAME || a_item == ATOMCAT_FRAME ? ArmorFrames::Item::kFrame : ArmorFrames::Item::kOther;
	}

	// What a stand-in dressed so wears of a list (RemotePlayers' Kept), and what of that an NPC's inventory shows as worn
	// (Wearable: a grenade is held, not worn).
	std::vector<std::uint32_t> Kept(const std::vector<std::uint32_t>& a_items, Dress a_dress)
	{
		std::vector<std::uint32_t> out;
		std::ranges::copy_if(a_items, std::back_inserter(out), [&](std::uint32_t a_item) { return ArmorFrames::Wears(a_dress, KindOf(a_item)); });
		return out;
	}

	std::vector<std::uint32_t> Wearable(std::vector<std::uint32_t> a_items)
	{
		std::erase(a_items, FRAG);
		std::ranges::sort(a_items);
		return a_items;
	}

	// About how long a stand-in takes to put its gun away and draw it again (Puppets::RedrawWeapon).
	constexpr auto DRAW_TIME = 500ms;

	// Alice's stand-in in Bob's game, and the engine's side of it.
	struct StandIn
	{
		std::vector<std::uint32_t> wears{ SETTLER_RAGS };  // as placed: the NPC's own outfit
		bool                       has3D = true;
		bool                       inFrame = false;
		bool                       armorSkeleton = false;  // its 3D rebuilt on the power armor skeleton
		std::vector<std::uint32_t> on;                     // what the engine has it in: an equip shows to it a frame later
		Clock::time_point          gunOutAt{};             // its gun is out from then on
		// The copies of her items it carries, by item, and the mods each was built with.
		std::map<std::uint32_t, std::vector<Protocol::ItemMod>> copies;
	};

	// Bob's game, dressing Alice's stand-in each frame as StandInGear::Dress says, the way RemotePlayers::Update does.
	// Counts what an onlooker would see: a stand-in emptied and dressed again, gear put on over what it wears, and its gun
	// put away and drawn again.
	struct Ours
	{
		StandInGear::Gear gear;
		StandIn           standIn;
		Clock::time_point now = Clock::time_point{} + 1h;
		int               applies = 0;  // emptied and dressed
		int               wears = 0;    // put on over what it wears, out of a frame
		int               inArmor = 0;  // pieces and weapons put on in its frame
		int               changes = 0;
		int               redraws = 0;
		int               refits = 0;   // found in something else, logged and dressed afresh
		int               keyPuts = 0;  // items put on for the engine's frame pick
		int               gives = 0;     // copies given (built with her mods, or the default combination)
		int               replaces = 0;  // copies taken away for carrying other mods than hers
		std::set<std::uint32_t> missing;  // her mods Bob's game hasn't

		bool Here(std::uint32_t a_mod) const { return !missing.contains(a_mod); }

		void Receive(const std::optional<Protocol::Equipment>& a_report)
		{
			REQUIRE(a_report);
			StandInGear::Report(gear, a_report->items, a_report->mods);
		}

		// What Equipment::PutOn does with the stand-in's copy of one of her items, as StandInGear::ChooseCopy says, and
		// Equipment::Give builds it: with those of her mods that are here, or else the default combination.
		void PutOn(std::uint32_t a_item)
		{
			const auto buildable = StandInGear::Buildable(StandInGear::ModsOf(gear.mods, a_item), [&](std::uint32_t a_mod) { return Here(a_mod); });
			const auto carried = standIn.copies.find(a_item);
			const auto ours = carried != standIn.copies.end() ? std::optional{ carried->second } : std::nullopt;
			switch (StandInGear::ChooseCopy(buildable ? &*buildable : nullptr, ours)) {
			case StandInGear::Copy::kKeep:
				break;
			case StandInGear::Copy::kReplace:
				++replaces;
				// The gun in its hand taken away, and the new copy equipped but holstered until it is drawn again.
				if (InHand(a_item)) {
					standIn.gunOutAt = Clock::time_point::max();
				}
				[[fallthrough]];
			case StandInGear::Copy::kGive:
				++gives;
				standIn.copies[a_item] = buildable ? buildable->mods : DEFAULT_COMBINATION;
				break;
			}
		}

		void PutOn(const std::vector<std::uint32_t>& a_items)
		{
			for (const auto item : a_items) {
				PutOn(item);
			}
		}

		// A new stand-in for her (a door, a load, out of her frame): wearing its own outfit, its 3D not in yet.
		void Place()
		{
			standIn = StandIn{};
			standIn.has3D = false;
			StandInGear::Placed(gear);
		}

		// The engine's frame pick (Equipment::FramePick) by what the stand-in has on as the engine sees it: the plain
		// frame item for a vault suit, else the default.
		std::uint32_t EnginePick() const { return std::ranges::contains(standIn.on, VAULT_SUIT) ? PLAIN_FRAME : ATOMCAT_FRAME; }

		// The switch-in, due this frame (ArmorFrames::ChooseSwitch says kIn), as RemotePlayers::PickReady holds it back:
		// whether it went in. The engine puts its pick on it, in the body slot (the suit comes off).
		bool SwitchIn()
		{
			if (!pickSince) {
				pickSince = now;
			}
			const auto theirs = std::ranges::find_if(gear.items, [](std::uint32_t a_item) { return KindOf(a_item) == ArmorFrames::Item::kFrame; });
			const bool named = theirs != gear.items.end();
			const auto key = named && *theirs == PLAIN_FRAME ? VAULT_SUIT : 0;  // Equipment::FrameKey
			auto       on = standIn.on;
			auto       dressedIn = Wearable(Kept(gear.worn, Dress::kClothes));
			std::ranges::sort(on);
			ArmorFrames::PickFacts facts{
				.named = named,
				.picksTheirs = named && EnginePick() == *theirs,
				.key = key != 0,
				.keyOn = key != 0 && std::ranges::contains(on, key),
				.dressed = gear.dressed && std::ranges::includes(Wearable(on), dressedIn),
				.waited = now - *pickSince,
			};
			if (keyPutAt) {
				facts.sincePut = now - *keyPutAt;
			}
			switch (ArmorFrames::ChoosePick(facts)) {
			case ArmorFrames::Pick::kPutOn:
				++keyPuts;
				keyPutAt = now;
				standIn.wears.push_back(key);
				return false;
			case ArmorFrames::Pick::kWait:
				return false;
			case ArmorFrames::Pick::kSwitch:
			case ArmorFrames::Pick::kGiveUp:
				break;
			}
			const auto frame = EnginePick();
			std::erase(standIn.wears, VAULT_SUIT);
			standIn.wears.push_back(frame);
			standIn.inFrame = true;
			standIn.armorSkeleton = true;  // the rebuild, a frame later at most: not what this looks at
			return true;
		}

		std::optional<Clock::time_point> pickSince;
		std::optional<Clock::time_point> keyPutAt;

		void Frame()
		{
			now += 16ms;
			standIn.on = standIn.wears;  // what was put on last frame is on to the engine now
			const auto dress = ArmorFrames::ChooseDress(standIn.inFrame, standIn.inFrame && standIn.armorSkeleton);
			const auto shown = dress == Dress::kArmor ? Kept(standIn.wears, dress) : standIn.wears;
			const auto dressing = StandInGear::Dress(gear, dress, standIn.has3D, now, InHand,
				[&](const std::vector<std::uint32_t>& a_worn) { return Wearable(shown) == Wearable(Kept(a_worn, dress)); });
			const auto outfit = Kept(gear.items, dress);
			switch (dressing.put) {
			case Put::kApply:
				++applies;
				standIn.wears = outfit;
				standIn.copies.clear();  // emptied first
				PutOn(outfit);
				break;
			case Put::kWear:
				++wears;
				standIn.wears = outfit;
				PutOn(outfit);
				break;
			case Put::kWearInArmor:
				{
					++inArmor;
					// Their pieces and weapons on; the frame the engine put on it stays.
					std::erase_if(standIn.wears, [&](std::uint32_t a_item) { return ArmorFrames::Wears(Dress::kArmor, KindOf(a_item)); });
					standIn.wears.insert(standIn.wears.end(), outfit.begin(), outfit.end());
					PutOn(outfit);
					break;
				}
			case Put::kNone:
				break;
			}
			changes += dressing.change;
			refits += dressing.refit;
			if (dressing.redraw) {
				++redraws;
				standIn.gunOutAt = now + DRAW_TIME;
			}
		}

		void Frames(int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				Frame();
			}
		}

		std::vector<std::uint32_t> Wears() const { return Wearable(standIn.wears); }

		// One of her shots, as WeaponFire::PlayShot replays it on the stand-in.
		AttackKind::Replay Shot() const
		{
			const bool holdsGun = std::ranges::any_of(standIn.wears, InHand);
			return AttackKind::ReplayShot(holdsGun, gear.dressed && now >= standIn.gunOutAt);
		}
	};

	// Bob (peer 1, whose game is looked at) and Alice (peer 2), both in.
	struct Session
	{
		Fixture       f;
		std::uint32_t alice = 0;

		Session()
		{
			REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
			const auto welcome = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
			REQUIRE(welcome);
			alice = welcome->playerId;
			REQUIRE(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
			f.fake->Take(1);
		}

		// Alice's game reports her gear, and the mods on it; what Bob's game is told, if anything.
		std::optional<Protocol::Equipment> Report(std::vector<std::uint32_t> a_items, std::chrono::milliseconds a_wait = 2000ms, std::vector<Protocol::ItemMods> a_mods = {})
		{
			f.fake->Deliver(2, Protocol::Encode(Protocol::Equipment{ 0, std::move(a_items), std::move(a_mods) }, MT::kEquipment));
			const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerEquipment), a_wait);
			const auto report = packet ? Protocol::DecodeEquipment(*packet) : std::nullopt;
			if (report) {
				CHECK(report->playerId == alice);
			}
			return report;
		}
	};

	std::vector<std::uint32_t> Sorted(std::vector<std::uint32_t> a_items)
	{
		std::ranges::sort(a_items);
		return a_items;
	}
}

TEST("stand-in gear: a friend's hat and their last grenade never make their stand-in put its gun away; a gun swap does, once")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT, FRAG }));
	bob.Frames(2);
	CHECK(bob.applies == 1);
	CHECK(bob.redraws == 1);  // a new stand-in draws the gun it was given
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frames(70);

	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAG }));  // hat off mid-fight
	bob.Frames(70);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));  // last grenade thrown
	bob.Frames(70);
	CHECK(bob.changes == 2);
	CHECK(bob.wears == 2);
	CHECK(bob.redraws == 1);  // the gun stayed out
	CHECK(bob.Shot() == AttackKind::Replay::kFire);

	bob.Receive(session.Report({ RIFLE, VAULT_SUIT }));
	bob.Frames(70);
	CHECK(bob.changes == 3);
	CHECK(bob.redraws == 2);
	CHECK(bob.applies == 1);  // never stripped and dressed again for a change
	CHECK(bob.refits == 0);   // and the gear check never took a change for the wrong outfit
	CHECK(bob.Wears() == Sorted({ RIFLE, VAULT_SUIT }));
}

TEST("stand-in gear: a friend's game sending its gear again, or in another order, changes nothing on their stand-in")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }));
	bob.Frames(70);
	// Their game forgets what it sent each time it loads a save and sends it again: the server keeps it to itself.
	for (int i = 0; i < 3; ++i) {
		CHECK(!session.Report({ PISTOL, VAULT_SUIT, GLASSES }, 300ms));
	}
	// The same items in another order (only a modified game sends that) get past the server, but are no change here.
	bob.Receive(session.Report({ GLASSES, VAULT_SUIT, PISTOL }));
	bob.Frames(400);  // past two gear checks
	CHECK(bob.applies == 1);
	CHECK(bob.changes == 0);
	CHECK(bob.redraws == 1);
	CHECK(bob.refits == 0);
}

TEST("stand-in gear: a friend's game flipping between two outfits as fast as it can redresses their stand-in at most once a second")
{
	Session session;
	Ours    bob;
	const std::vector<std::uint32_t> one{ PISTOL, VAULT_SUIT, HAT };
	const std::vector<std::uint32_t> other{ RIFLE, VAULT_SUIT };
	bob.Receive(session.Report(one));
	bob.Frame();
	// Each report differs from the one before, so the server passes every one on; we read one a frame.
	for (int i = 0; i < 40; ++i) {
		bob.Receive(session.Report(i % 2 ? one : other));
		bob.Frame();
	}
	CHECK(bob.changes == 0);  // within a second of being dressed: put on together, later
	// It ends on what the stand-in has on already: nothing to put on at all.
	bob.Frames(70);
	CHECK(bob.changes == 0);
	CHECK(bob.redraws == 1);
	CHECK(bob.applies == 1);
	CHECK(bob.Wears() == Sorted(one));
	// Once more, more than a second on and ending on the other: the first flip goes on at once (its gun swapped), and
	// the forty after it, within the second, end on what it then wears.
	for (int i = 0; i < 41; ++i) {
		bob.Receive(session.Report(i % 2 ? one : other));
		bob.Frame();
	}
	bob.Frames(70);
	CHECK(bob.changes == 1);
	CHECK(bob.redraws == 2);
	CHECK(bob.Wears() == Sorted(other));
	CHECK(bob.refits == 0);
}

TEST("stand-in gear: a friend whose game restarted is dressed again in the same gear, and a newcomer sees it too")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(2);
	REQUIRE(bob.gear.dressed);

	// Her game restarts and says hello again on the same connection: Bob's game is told she left and joined, and
	// starts her afresh.
	session.f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	REQUIRE(session.f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	REQUIRE(session.f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
	bob = Ours{};
	// The same gear as before the restart: passed on, since it is her first report since.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(2);
	CHECK(bob.applies == 1);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));

	// Carol joins afterwards: told Alice's gear as she joins, with nothing new from Alice.
	REQUIRE(session.f.Join(3, Fixture::MakeHello("Carol", 0xCA201)));
	const auto packet = session.f.fake->WaitFor(3, Type(MT::kPlayerEquipment));
	REQUIRE(packet);
	Ours carol;
	carol.Receive(Protocol::DecodeEquipment(*packet));
	carol.Frames(2);
	CHECK(carol.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
}

TEST("stand-in gear: a stand-in placed again through a door is dressed as its 3D comes in, with nothing new from the server")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frames(70);
	// Through a door: a new stand-in, in its own outfit. Nothing goes on until its 3D is in; then it is dressed in what
	// she last reported, though the server sends nothing new.
	bob.Place();
	CHECK(!session.Report({ PISTOL, VAULT_SUIT, HAT }, 300ms));
	bob.Frames(5);
	CHECK(bob.applies == 1);
	CHECK(bob.Wears() == Sorted({ SETTLER_RAGS }));
	bob.standIn.has3D = true;
	bob.Frame();
	CHECK(bob.applies == 2);
	CHECK(bob.redraws == 2);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT, HAT }));
	// A change reported right after is put on a second after it was dressed, over what it wears.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(30);
	CHECK(bob.changes == 0);
	bob.Frames(40);
	CHECK(bob.changes == 1);
	CHECK(bob.redraws == 2);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
}

TEST("stand-in gear: a friend in power armor gets their pieces only once the stand-in is on the armor skeleton, and a grenade there keeps the gun out")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(70);
	// She gets into her frame: her game reports the frame and pieces. The stand-in is switched into its copy (the engine
	// puts the frame item on it) but its 3D is rebuilt on the power armor skeleton only a few frames later: nothing of
	// hers goes on meanwhile, not even the change she reported, or the pieces would hang off its own body.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAME, T60_HELMET, T60_TORSO }));
	bob.standIn.inFrame = true;
	bob.standIn.wears.push_back(FRAME);
	bob.Frames(120);  // two seconds, past the change gap and a gear check
	CHECK(bob.inArmor == 0);
	CHECK(bob.wears == 0);
	CHECK(bob.refits == 0);
	CHECK(!std::ranges::contains(bob.standIn.wears, T60_HELMET));
	// The rebuild comes: dressed afresh in its frame, its gun drawn again.
	bob.standIn.armorSkeleton = true;
	bob.Frame();
	CHECK(bob.inArmor == 1);
	CHECK(bob.applies == 1);  // never emptied in its frame: the frame item would go too
	CHECK(bob.redraws == 2);
	CHECK(std::ranges::contains(bob.standIn.wears, T60_HELMET));
	CHECK(std::ranges::contains(bob.standIn.wears, FRAME));
	bob.Frames(250);  // past more gear checks: what it wears in its frame is what it was dressed in
	CHECK(bob.refits == 0);
	// A grenade picked up: put on in its frame, the gun stays out.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAME, T60_HELMET, T60_TORSO, FRAG }));
	bob.Frames(70);
	CHECK(bob.inArmor == 2);
	CHECK(bob.changes == 1);
	CHECK(bob.redraws == 2);
	// Out of her frame: the stand-in is replaced (ArmorFrames), and the new one dressed in clothes.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Place();
	bob.standIn.has3D = true;
	bob.Frame();
	CHECK(bob.applies == 2);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
}

TEST("stand-in gear: a friend in the plain frame gets a stand-in in the plain frame, whether their frame item is told before it goes in or after")
{
	// The 2026-10-14 report: her stand-in was placed and dressed in her vault suit 30 ms before it was switched in, and
	// the engine, which picks the frame item by what it has on, gave it the AtomCat frame. Her frame item came 9 s later,
	// and swapped in then, the engine picked again within seconds (the AtomCat frame, then the Wastelander one).
	{
		Session session;
		Ours    bob;
		bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
		bob.Place();
		bob.standIn.has3D = true;
		bob.Frame();  // dressed now; on to the engine next frame
		REQUIRE(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
		CHECK(!bob.SwitchIn());  // the switch-in is due at once: it waits for her suit to be on
		bob.Frame();
		CHECK(bob.SwitchIn());
		CHECK(std::ranges::contains(bob.standIn.wears, PLAIN_FRAME));
		CHECK(!std::ranges::contains(bob.standIn.wears, ATOMCAT_FRAME));
		CHECK(bob.now - *bob.pickSince < ArmorFrames::PICK_WAIT);
		// Her frame item and pieces come: the pieces go on, the frame item stays the engine's, which is hers.
		bob.Receive(session.Report({ PISTOL, PLAIN_FRAME, T60_HELMET, T60_TORSO }));
		bob.Frames(30 * 1000 / 16);
		CHECK(bob.refits == 0);
		CHECK(bob.keyPuts == 0);
		CHECK(bob.Wears() == Sorted({ PISTOL, PLAIN_FRAME, T60_HELMET, T60_TORSO }));
	}
	// She has been in her frame a while (her stand-in placed again through a door): her report names her frame item, and
	// has no suit (the frame has its body slot). Her suit is put on the new stand-in for the pick, and it goes in in the
	// plain frame.
	{
		Session session;
		Ours    bob;
		bob.Receive(session.Report({ PISTOL, PLAIN_FRAME, T60_HELMET, T60_TORSO }));
		bob.Place();
		bob.standIn.has3D = true;
		bob.Frame();
		REQUIRE(bob.Wears() == Sorted({ PISTOL }));
		CHECK(!bob.SwitchIn());
		CHECK(bob.keyPuts == 1);
		bob.Frame();
		CHECK(bob.SwitchIn());
		CHECK(bob.keyPuts == 1);
		bob.Frames(30 * 1000 / 16);
		CHECK(bob.refits == 0);
		CHECK(bob.Wears() == Sorted({ PISTOL, PLAIN_FRAME, T60_HELMET, T60_TORSO }));
	}
	// In the AtomCat frame herself: the engine's pick is hers at once, nothing put on.
	{
		Session session;
		Ours    bob;
		bob.Receive(session.Report({ PISTOL, ATOMCAT_FRAME, T60_HELMET }));
		bob.Place();
		bob.standIn.has3D = true;
		bob.Frame();
		CHECK(bob.SwitchIn());
		CHECK(bob.keyPuts == 0);
		CHECK(std::ranges::contains(bob.standIn.wears, ATOMCAT_FRAME));
	}
	// Her clothes never come on to the engine (it keeps undressing the stand-in): in all the same after PICK_WAIT.
	{
		Session session;
		Ours    bob;
		bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
		bob.Place();
		bob.standIn.has3D = true;
		int frames = 0;
		for (; frames < 1000; ++frames) {
			bob.Frame();
			bob.standIn.on.clear();
			if (bob.SwitchIn()) {
				break;
			}
		}
		CHECK(bob.standIn.inFrame);
		CHECK(frames * 16ms >= ArmorFrames::PICK_WAIT - 16ms);
		CHECK(frames * 16ms <= ArmorFrames::PICK_WAIT + 32ms);
	}
}

TEST("stand-in gear: a stand-in the engine dresses in its own outfit again is put right at the next check, and one it keeps undressing is left after three tries")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frames(2);
	REQUIRE(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT, HAT }));
	// The engine sets up its inventory late and puts its own outfit back on it.
	bob.standIn.wears = { SETTLER_RAGS };
	bob.Frames(3000 / 16 + 2);
	CHECK(bob.refits == 1);
	CHECK(bob.applies == 2);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frames(400);
	CHECK(bob.refits == 1);  // it stuck

	// Now it undoes every dressing: three more tries in a row, then it is left as it is rather than stripped and
	// dressed again every three seconds for as long as she plays.
	const int before = bob.applies;
	for (int i = 0; i < 30 * 1000 / 16; ++i) {
		bob.Frame();
		bob.standIn.wears = { SETTLER_RAGS };
	}
	CHECK(bob.applies - before == 3);
	CHECK(bob.refits == 4);
	// A change she makes is put on, and the tries start over.
	bob.Receive(session.Report({ RIFLE, VAULT_SUIT }));
	for (int i = 0; i < 30 * 1000 / 16; ++i) {
		bob.Frame();
		bob.standIn.wears = { SETTLER_RAGS };
	}
	CHECK(bob.changes == 1);
	CHECK(bob.refits == 7);
}

TEST("stand-in gear: a change reported just as the gear check falls is put on over what it wears, not taken for the wrong outfit")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frame();
	const auto dressedAt = bob.now;
	// Frames up to the one before the check is due ...
	while (bob.now + 16ms < dressedAt + StandInGear::CHECK_INTERVAL) {
		bob.Frame();
	}
	// ... and her change arrives for the very frame it is.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frame();
	CHECK(bob.wears == 1);
	CHECK(bob.applies == 1);
	CHECK(bob.refits == 0);
	CHECK(bob.redraws == 1);
	bob.Frames(400);
	CHECK(bob.refits == 0);
	CHECK(bob.Wears() == Sorted({ PISTOL, VAULT_SUIT }));
}

// The sound itself (WeaponFire::PlayShot on a stand-in whose gun is put away) needs the game; this covers when the stand-in
// holsters its gun, and so which of her shots show their firing animation and which are heard only.
TEST("stand-in gear: a friend's shots keep their firing animation through gear changes that keep their gun; a new stand-in or a gun swap shows a moment of shots heard only")
{
	using AttackKind::Replay;
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT, FRAG }));
	bob.Frames(70);
	int fired = 0;
	int heardOnly = 0;
	const auto shoot = [&](int a_frames) {
		for (int i = 0; i < a_frames; i += 6) {  // ten shots a second
			bob.Frames(6);
			const auto replay = bob.Shot();
			CHECK(replay != Replay::kNone);
			fired += replay == Replay::kFire;
			heardOnly += replay == Replay::kSoundOnly;
		}
	};
	// Hat off, a grenade thrown, glasses on, mid-fight: every shot shows.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAG }));
	shoot(90);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	shoot(90);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }));
	shoot(90);
	CHECK(heardOnly == 0);
	CHECK(fired == 45);
	// A gun swap: about half a second of shots heard while the new gun comes out.
	bob.Receive(session.Report({ RIFLE, VAULT_SUIT, GLASSES }));
	heardOnly = 0;
	shoot(90);
	CHECK(heardOnly >= 4);
	CHECK(heardOnly <= 6);
	// Unarmed after a door: a punch waits until the new stand-in is dressed and ready (asking for one would draw first).
	bob.Receive(session.Report({ VAULT_SUIT }));
	bob.Frames(70);
	bob.Place();
	bob.Frame();
	CHECK(bob.Shot() == Replay::kNone);
	bob.standIn.has3D = true;
	bob.Frames(40);
	CHECK(bob.Shot() == Replay::kSwing);
}

TEST("stand-in gear: no report yet, nothing put on; a stand-in dressed in what it wears is not dressed again")
{
	StandInGear::Gear gear;
	const auto        now = Clock::time_point{} + 1h;
	CHECK(StandInGear::Next(gear, now) == StandInGear::Step::kNone);  // the NPC's own outfit until their gear comes
	CHECK(StandInGear::Report(gear, { PISTOL }));
	CHECK(StandInGear::Next(gear, now) == StandInGear::Step::kDress);
	StandInGear::Done(gear, StandInGear::Step::kDress, now, InHand);
	CHECK(StandInGear::Next(gear, now + 10s) == StandInGear::Step::kNone);
	CHECK(!StandInGear::Report(gear, { PISTOL, PISTOL }));  // a duplicate entry is the same list
	CHECK(StandInGear::Report(gear, {}));                    // everything off is a change
	CHECK(StandInGear::Next(gear, now + 999ms) == StandInGear::Step::kNone);
	CHECK(StandInGear::Next(gear, now + 1s) == StandInGear::Step::kChange);
	CHECK(StandInGear::Done(gear, StandInGear::Step::kChange, now + 1s, InHand));  // the pistol went: drawn again, empty-handed
}

TEST("stand-in gear: a friend's 10mm reaches their stand-in with their own barrel and sights, not one rolled for this game's level, and a barrel swapped at a workbench swaps its copy once")
{
	Session session;
	Ours    bob;
	const std::vector<Protocol::ItemMods> stock{ { PISTOL, 3, { STANDARD_RECEIVER, SHORT_BARREL, IRON_SIGHTS } } };
	const auto report = session.Report({ PISTOL, VAULT_SUIT }, 2000ms, stock);
	REQUIRE(report);
	CHECK(report->mods == stock);  // through the server as her game sent it
	bob.Receive(report);
	bob.Frames(2);
	CHECK(bob.applies == 1);
	CHECK(bob.standIn.copies[PISTOL] == stock[0].mods);
	CHECK(bob.standIn.copies[VAULT_SUIT] == DEFAULT_COMBINATION);  // nothing on record: the default, never a level roll
	bob.Frames(70);

	// A long barrel and a reflex sight at a workbench: the same items, so only the mods tell the server it is news.
	const std::vector<Protocol::ItemMods> modded{ { PISTOL, 3, { STANDARD_RECEIVER, LONG_BARREL, REFLEX_SIGHT } } };
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, modded));
	bob.Frames(70);
	CHECK(bob.changes == 1);
	CHECK(bob.replaces == 1);  // the old copy taken away, one built with her mods given
	CHECK(bob.standIn.copies[PISTOL] == modded[0].mods);
	CHECK(bob.redraws == 2);   // its new copy drawn
	CHECK(bob.applies == 1);   // never stripped and dressed again for it
	CHECK(bob.refits == 0);

	// Her game sending it again: the server keeps it to itself. The same mods listed in another order get past it, but
	// are the copy it has: nothing given, the gun stays out.
	CHECK(!session.Report({ PISTOL, VAULT_SUIT }, 300ms, modded));
	const std::vector<Protocol::ItemMods> reordered{ { PISTOL, 3, { REFLEX_SIGHT, STANDARD_RECEIVER, LONG_BARREL } } };
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, reordered));
	bob.Frames(70);
	CHECK(bob.replaces == 1);
	CHECK(bob.redraws == 2);

	// A hat on: the pistol's copy is left as it is.
	const int gives = bob.gives;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }, 2000ms, reordered));
	bob.Frames(70);
	CHECK(bob.gives == gives + 1);  // the hat only
	CHECK(bob.redraws == 2);

	// Carol joins afterwards: told Alice's gear with its mods as she joins.
	REQUIRE(session.f.Join(3, Fixture::MakeHello("Carol", 0xCA201)));
	const auto packet = session.f.fake->WaitFor(3, Type(MT::kPlayerEquipment));
	REQUIRE(packet);
	Ours carol;
	carol.Receive(Protocol::DecodeEquipment(*packet));
	carol.Frames(2);
	CHECK(carol.standIn.copies[PISTOL] == reordered[0].mods);  // as her game last listed them
	CHECK(carol.Wears() == Sorted({ PISTOL, VAULT_SUIT, HAT }));
}

TEST("stand-in gear: a friend in power armor gets their pieces built with their paint, and a stand-in placed again gets the same copies")
{
	Session session;
	Ours    bob;
	const std::vector<Protocol::ItemMods> painted{ { T60_HELMET, 0, { { 0x0015A3C1, 0, 0 } } }, { PISTOL, 3, { STANDARD_RECEIVER, LONG_BARREL } } };
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, { painted[1] }));
	bob.Frames(70);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAME, T60_HELMET, T60_TORSO }, 2000ms, painted));
	bob.standIn.inFrame = true;
	bob.standIn.armorSkeleton = true;
	bob.standIn.wears.push_back(FRAME);
	bob.Frame();
	CHECK(bob.inArmor == 1);
	CHECK(bob.standIn.copies[T60_HELMET] == painted[0].mods);
	CHECK(bob.standIn.copies[T60_TORSO] == DEFAULT_COMBINATION);
	CHECK(bob.standIn.copies[PISTOL] == painted[1].mods);
	CHECK(bob.replaces == 0);  // the pistol it had was hers already
	// Out of her frame: a new stand-in, given every copy afresh with her mods.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, { painted[1] }));
	bob.Place();
	bob.standIn.has3D = true;
	bob.Frame();
	CHECK(bob.applies == 2);
	CHECK(bob.standIn.copies[PISTOL] == painted[1].mods);
	CHECK(!bob.standIn.copies.contains(T60_HELMET));
}

TEST("stand-in gear: a copy is given when missing, kept when its mods are theirs in any order or none are on record, and replaced when they differ")
{
	using StandInGear::ChooseCopy;
	using StandInGear::Copy;
	const Protocol::ItemMods theirs{ PISTOL, 3, { STANDARD_RECEIVER, SHORT_BARREL } };
	using Mods = std::vector<Protocol::ItemMod>;
	CHECK(ChooseCopy(&theirs, std::nullopt) == Copy::kGive);
	CHECK(ChooseCopy(nullptr, std::nullopt) == Copy::kGive);
	CHECK(ChooseCopy(&theirs, Mods{ SHORT_BARREL, STANDARD_RECEIVER }) == Copy::kKeep);
	CHECK(ChooseCopy(&theirs, Mods{ STANDARD_RECEIVER, LONG_BARREL }) == Copy::kReplace);
	CHECK(ChooseCopy(&theirs, Mods{ STANDARD_RECEIVER }) == Copy::kReplace);
	CHECK(ChooseCopy(&theirs, Mods{}) == Copy::kReplace);  // a copy without an instance extra
	CHECK(ChooseCopy(&theirs, Mods{ STANDARD_RECEIVER, { SHORT_BARREL.mod, SHORT_BARREL.index, 1 } }) == Copy::kReplace);  // another rank
	CHECK(ChooseCopy(nullptr, Mods{ LONG_BARREL }) == Copy::kKeep);  // nothing to check it against

	// A report keeps the mods of the items it lists only, one entry each, and a mod change is a change.
	StandInGear::Gear gear;
	const auto        now = Clock::time_point{} + 1h;
	CHECK(StandInGear::Report(gear, { PISTOL, VAULT_SUIT }, { theirs, { RIFLE, 0, { LONG_BARREL } }, { PISTOL, 0, {} } }));
	REQUIRE(gear.mods.size() == 1);
	CHECK(gear.mods[0] == theirs);
	CHECK(StandInGear::ModsOf(gear.mods, VAULT_SUIT) == nullptr);
	StandInGear::Done(gear, StandInGear::Step::kDress, now, InHand);
	CHECK(!StandInGear::Report(gear, { VAULT_SUIT, PISTOL }, { theirs }));
	CHECK(StandInGear::Report(gear, { PISTOL, VAULT_SUIT }, { { PISTOL, 3, { STANDARD_RECEIVER, LONG_BARREL } } }));
	CHECK(StandInGear::Next(gear, now + 1s) == StandInGear::Step::kChange);
	CHECK(StandInGear::Done(gear, StandInGear::Step::kChange, now + 1s, InHand));  // its gun got a new barrel: drawn again
	// Mods on armor change nothing in the hand.
	CHECK(StandInGear::Report(gear, { PISTOL, VAULT_SUIT }, { { PISTOL, 3, { STANDARD_RECEIVER, LONG_BARREL } }, { VAULT_SUIT, 0, { { 0x0011AAAA, 0, 0 } } } }));
	CHECK(!StandInGear::Done(gear, StandInGear::Step::kChange, now + 2s, InHand));
}

TEST("stand-in gear: a friend's gun with a barrel from a plugin this game hasn't is given its copy once, and keeps it out through a hat, a grenade and a sight swap")
{
	Session session;
	Ours    bob;
	bob.missing = { CUSTOM_BARREL.mod, CUSTOM_BARREL_LONG.mod };
	using Mods = std::vector<Protocol::ItemMod>;
	const std::vector<Protocol::ItemMods> custom{ { PISTOL, 3, { STANDARD_RECEIVER, CUSTOM_BARREL, REFLEX_SIGHT } } };
	const auto report = session.Report({ PISTOL, VAULT_SUIT, HAT, FRAG }, 2000ms, custom);
	REQUIRE(report);
	CHECK(report->mods == custom);  // through the server as her game sent it, the barrel too
	bob.Receive(report);
	bob.Frames(70);
	CHECK((bob.standIn.copies[PISTOL] == Mods{ STANDARD_RECEIVER, REFLEX_SIGHT }));  // built without the barrel it hasn't
	const int gives = bob.gives;

	// Hat off, the last grenade thrown, glasses on: the pistol's copy is the one it can have, and stays out. Each of
	// these used to take the gun away and give it again, holstered, its shots unseen until the next gun swap.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAG }, 2000ms, custom));
	bob.Frames(70);
	CHECK(bob.Shot() == AttackKind::Replay::kFire);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, custom));
	bob.Frames(70);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }, 2000ms, custom));
	bob.Frames(70);
	CHECK(bob.changes == 3);
	CHECK(bob.replaces == 0);
	CHECK(bob.gives == gives + 1);  // the glasses only
	CHECK(bob.redraws == 1);
	CHECK(bob.Shot() == AttackKind::Replay::kFire);
	bob.Frames(400);  // past gear checks
	CHECK(bob.refits == 0);
	CHECK(bob.replaces == 0);

	// Another barrel from that plugin: nothing here to build differently, the copy stays.
	const std::vector<Protocol::ItemMods> longer{ { PISTOL, 3, { STANDARD_RECEIVER, CUSTOM_BARREL_LONG, REFLEX_SIGHT } } };
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }, 2000ms, longer));
	bob.Frames(70);
	CHECK(bob.replaces == 0);
	// A sight this game has swapped at a workbench: its copy is replaced once, and drawn.
	const std::vector<Protocol::ItemMods> iron{ { PISTOL, 3, { STANDARD_RECEIVER, CUSTOM_BARREL_LONG, IRON_SIGHTS } } };
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }, 2000ms, iron));
	bob.Frames(70);
	CHECK(bob.replaces == 1);
	CHECK((bob.standIn.copies[PISTOL] == Mods{ STANDARD_RECEIVER, IRON_SIGHTS }));
	CHECK(bob.Shot() == AttackKind::Replay::kFire);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }, 2000ms, iron));
	bob.Frames(70);
	CHECK(bob.replaces == 1);
	CHECK(bob.Shot() == AttackKind::Replay::kFire);

	// She takes the barrel off: her copy is now the one it has, kept.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }, 2000ms, { { PISTOL, 3, { STANDARD_RECEIVER, IRON_SIGHTS } } }));
	bob.Frames(70);
	CHECK(bob.replaces == 1);
}

TEST("stand-in gear: an item whose mods are all from plugins this game hasn't, none at all, or no template combination is given its default copy once and keeps it")
{
	Session session;
	Ours    bob;
	bob.missing = { CUSTOM_BARREL.mod };
	const std::vector<Protocol::ItemMods> odd{
		{ PISTOL, 3, { CUSTOM_BARREL } },  // nothing of it here
		{ VAULT_SUIT, StandInGear::NO_TEMPLATE, {} },  // an instance extra with nothing on it
		{ HAT, StandInGear::NO_TEMPLATE, { { 0x0015A3C1, 0, 0 } } },  // mods, but no template combination named
	};
	const auto report = session.Report({ PISTOL, VAULT_SUIT, HAT }, 2000ms, odd);
	REQUIRE(report);
	CHECK(report->mods == odd);
	bob.Receive(report);
	bob.Frames(70);
	CHECK(bob.standIn.copies[PISTOL] == DEFAULT_COMBINATION);
	CHECK(bob.standIn.copies[VAULT_SUIT] == DEFAULT_COMBINATION);
	CHECK(bob.standIn.copies[HAT] == DEFAULT_COMBINATION);
	CHECK(bob.gives == 3);
	// A grenade and glasses: nothing of hers is given again.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT, FRAG }, 2000ms, odd));
	bob.Frames(70);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT, GLASSES }, 2000ms, odd));
	bob.Frames(70);
	CHECK(bob.changes == 2);
	CHECK(bob.replaces == 0);
	CHECK(bob.gives == 5);
	CHECK(bob.redraws == 1);
	CHECK(bob.Shot() == AttackKind::Replay::kFire);
}

TEST("stand-in gear: the mods a copy is built with here are theirs less the ones this game hasn't, and none without a template combination")
{
	using StandInGear::Buildable;
	using Mods = std::vector<Protocol::ItemMod>;
	const auto here = [](std::uint32_t a_mod) { return (a_mod >> 24) != 0x0A; };
	const Protocol::ItemMods theirs{ PISTOL, 3, { STANDARD_RECEIVER, CUSTOM_BARREL, REFLEX_SIGHT } };
	const auto built = Buildable(&theirs, here);
	REQUIRE(built);
	CHECK(built->item == PISTOL);
	CHECK(built->templateIndex == 3);
	CHECK((built->mods == Mods{ STANDARD_RECEIVER, REFLEX_SIGHT }));
	CHECK(StandInGear::ChooseCopy(&*built, Mods{ REFLEX_SIGHT, STANDARD_RECEIVER }) == StandInGear::Copy::kKeep);
	CHECK(StandInGear::ChooseCopy(&theirs, Mods{ REFLEX_SIGHT, STANDARD_RECEIVER }) == StandInGear::Copy::kReplace);  // what it used to compare
	const auto all = [](std::uint32_t) { return true; };
	CHECK(Buildable(&theirs, all) == theirs);
	CHECK(!Buildable(nullptr, all));
	const Protocol::ItemMods none{ PISTOL, 3, {} };
	CHECK(!Buildable(&none, all));
	const Protocol::ItemMods untemplated{ PISTOL, StandInGear::NO_TEMPLATE, { STANDARD_RECEIVER } };
	CHECK(!Buildable(&untemplated, all));
	const Protocol::ItemMods missing{ PISTOL, 3, { CUSTOM_BARREL, CUSTOM_LINING } };
	CHECK(!Buildable(&missing, here));
}
