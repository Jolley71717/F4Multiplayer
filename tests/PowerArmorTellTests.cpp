// A friend's power armor as it travels: what the wearer's game makes of what it sits in (ArmorFrames::OwnWear, as
// Party sends it), through the real server, to what each friend's game keeps of it (ArmorFrames::Hear, as
// RemotePlayers keeps it) and logs once a frame (ArmorFrames::UnknownFrame). A wear whose frame is not known leaves
// a line on the side that lacks it, once a wear; the first status of every wear, which names no frame, leaves none.

#include "ServerFixture.h"
#include "game/ArmorFrames.h"

#include <chrono>
#include <format>
#include <map>
#include <set>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t VANILLA_FRAME = 0x0017F6E4;  // a frame every game has
	constexpr std::uint32_t PLUGIN_FRAME = 0x0A001F2C;   // a frame from a plugin only some of the friends have
	constexpr std::uint32_t TERMINAL = 0x0016E3F1;       // furniture that is no frame
	constexpr std::uint32_t MADE_HERE = 0xFF001234;      // a frame reference the wearer's game made (a dropped suit)
	constexpr std::uint32_t PRE_PLACED = 0x0001F3D9;     // a frame reference every game has (the museum's suit)

	// The wearer's game: what it sits in, what OwnWear makes of it, sent as Party sends it.
	struct Wearer
	{
		PeerId                peer;
		ArmorFrames::OwnWear  wear;
		bool                  on = false;
		ArmorFrames::Seat     seat;
		ArmorFrames::Seat     held;  // the frame the engine keeps as worn (Equipment::PowerArmorFrameOf)
		std::uint8_t          health = 100;
		int                   logged = 0;

		void Send(Fixture& a_f, Clock::time_point a_now)
		{
			const auto told = wear.Update(on, [&] { return ArmorFrames::WornSeat(seat, held); }, a_now);
			logged += told.unknown;
			Protocol::PlayerStatus status;
			status.location = 0x1000;
			status.level = 12;
			status.health = health;
			status.powerArmor = on;
			status.powerArmorFrame = told.frame;
			status.powerArmorFrameRef = told.frameRef;
			a_f.fake->Deliver(peer, Protocol::Encode(status, MT::kReportStatus));
		}

		// Its game's frames between statuses (it logs on a frame, not only as it sends).
		void Tick(Clock::time_point a_now) { logged += wear.Update(on, [&] { return ArmorFrames::WornSeat(seat, held); }, a_now).unknown; }
	};

	// One friend's game hearing the session: the frames it has (what RemotePlayers' WornFrame finds), and per player
	// what it keeps of their wear. It takes the server's messages in the order they came, as Session dispatches them:
	// a player who left is forgotten (RemotePlayers::Remove), a status kept (RemotePlayers::SetPowerArmor).
	struct Hearer
	{
		struct Friend
		{
			ArmorFrames::Heard        heard;
			ArmorFrames::UnknownFrame unknown;
		};

		PeerId                           peer;
		std::set<std::uint32_t>          framesHere;
		std::map<std::uint32_t, Friend>  friends;
		int                              logged = 0;
		std::vector<std::string>         order;  // what came, in order: "left <id>", "status <id>"

		// Takes what the server sent, in order, through the next status from a_from.
		bool HearStatus(Fixture& a_f, std::uint32_t a_from)
		{
			for (int i = 0; i < 20; ++i) {
				const auto packets = a_f.fake->TakeThrough(peer, Type(MT::kPlayerStatus));
				if (packets.empty()) {
					return false;
				}
				std::uint32_t last = 0;
				for (const auto& packet : packets) {
					if (packet[0] == Type(MT::kPlayerLeft)) {
						const auto gone = Protocol::DecodePlayerLeft(packet);
						REQUIRE(gone);
						order.push_back(std::format("left {}", gone->playerId));
						friends.erase(gone->playerId);
					} else if (packet[0] == Type(MT::kPlayerStatus)) {
						const auto status = Protocol::DecodePlayerStatus(packet);
						REQUIRE(status);
						order.push_back(std::format("status {}", status->playerId));
						auto& f = friends[status->playerId];
						f.heard = ArmorFrames::Hear(f.heard, status->powerArmor, status->powerArmorFrame, status->powerArmorFrameRef);
						last = status->playerId;
					}
				}
				if (last == a_from) {
					return true;
				}
			}
			return false;
		}

		// Its game's frames: each stand-in's wear looked at (RemotePlayers::Update).
		void Tick(Clock::time_point a_now)
		{
			for (auto& [id, f] : friends) {
				logged += f.unknown.Update(f.heard.on, f.heard.on && framesHere.contains(f.heard.frame), a_now);
			}
		}

		const ArmorFrames::Heard& Of(std::uint32_t a_id) { return friends[a_id].heard; }
	};

	// Both games run their frames from a_from to a_to.
	void Run(Wearer& a_wearer, Hearer& a_hearer, Clock::time_point a_from, Clock::time_point a_to)
	{
		for (auto t = a_from; t <= a_to; t += 100ms) {
			a_wearer.Tick(t);
			a_hearer.Tick(t);
		}
	}
}

TEST("power armor tell: getting into a frame twice in a session logs nothing on either side, though each wear's first status names no frame")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer wearer{ .peer = 1 };
	Hearer bob{ .peer = 2, .framesHere = { VANILLA_FRAME } };
	const auto t0 = Clock::now();

	for (int wear = 0; wear < 2; ++wear) {
		const auto start = t0 + std::chrono::seconds(wear * 10);
		// The armor is on a moment before the furniture is known: her first status names no frame.
		wearer.on = true;
		wearer.seat = {};
		wearer.Send(f, start);
		REQUIRE(bob.HearStatus(f, alice->playerId));
		CHECK(bob.Of(alice->playerId).on);
		Run(wearer, bob, start, start + 300ms);
		// Then she sits in a dropped suit of her own game's: the frame goes to Bob, the reference does not.
		wearer.seat = { .ref = MADE_HERE, .base = VANILLA_FRAME, .frame = true };
		wearer.Send(f, start + 400ms);
		REQUIRE(bob.HearStatus(f, alice->playerId));
		CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);
		CHECK(bob.Of(alice->playerId).frameRef == 0u);
		// She uses a terminal in the armor: what she sits in changes, the frame told does not.
		wearer.seat = { .ref = 0xFF002000, .base = TERMINAL, .frame = false };
		wearer.health = 90;
		wearer.Send(f, start + 2s);
		REQUIRE(bob.HearStatus(f, alice->playerId));
		CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);
		Run(wearer, bob, start + 400ms, start + 5s);
		CHECK(wearer.logged == 0);
		CHECK(bob.logged == 0);

		wearer.on = false;
		wearer.seat = {};
		wearer.Send(f, start + 6s);
		REQUIRE(bob.HearStatus(f, alice->playerId));
		CHECK(!bob.Of(alice->playerId).on);
		CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);  // kept for the start of her next wear
		Run(wearer, bob, start + 6s, start + 9s);
	}
	CHECK(wearer.logged == 0);
	CHECK(bob.logged == 0);
}

TEST("power armor tell: a wear whose frame is never known, or not in a friend's game, is logged once a wear on the side that lacks it, however many statuses repeat it")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer wearer{ .peer = 1 };
	Hearer bob{ .peer = 2, .framesHere = { VANILLA_FRAME } };
	const auto t0 = Clock::now();

	// In power armor and sitting in something that is no frame, the whole wear: her game logs it once, a second in;
	// Bob's, which has no frame of hers from before, likewise. Her health changing sends her status again and again.
	wearer.on = true;
	wearer.seat = { .ref = 0xFF002000, .base = TERMINAL, .frame = false };
	wearer.Send(f, t0);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	Run(wearer, bob, t0, t0 + 900ms);
	CHECK(wearer.logged == 0);
	CHECK(bob.logged == 0);
	for (int i = 0; i < 3; ++i) {
		wearer.health = static_cast<std::uint8_t>(90 - 10 * i);
		const auto at = t0 + 1s + std::chrono::seconds(i);
		wearer.Send(f, at);
		REQUIRE(bob.HearStatus(f, alice->playerId));
		Run(wearer, bob, at, at + 900ms);
	}
	CHECK(wearer.logged == 1);
	CHECK(bob.logged == 1);

	// Out, and into a plugin's frame Bob's game lacks: her game knows its frame and says nothing; Bob's logs once.
	wearer.on = false;
	wearer.Send(f, t0 + 4s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	Run(wearer, bob, t0 + 4s, t0 + 5s);
	wearer.on = true;
	wearer.seat = { .ref = 0xFF003000, .base = PLUGIN_FRAME, .frame = true };
	wearer.Send(f, t0 + 5s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	Run(wearer, bob, t0 + 5s, t0 + 9s);
	CHECK(wearer.logged == 1);
	CHECK(bob.logged == 2);

	// Two friends join late: the server tells each her last status. The one with the plugin logs nothing; the one
	// without it logs it once, as Bob did.
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	REQUIRE(f.Join(4, Fixture::MakeHello("Dave")));
	Hearer carol{ .peer = 3, .framesHere = { VANILLA_FRAME, PLUGIN_FRAME } };
	Hearer dave{ .peer = 4, .framesHere = { VANILLA_FRAME } };
	REQUIRE(carol.HearStatus(f, alice->playerId));
	REQUIRE(dave.HearStatus(f, alice->playerId));
	CHECK(carol.Of(alice->playerId).frame == PLUGIN_FRAME);
	for (auto t = t0 + 9s; t <= t0 + 12s; t += 100ms) {
		carol.Tick(t);
		dave.Tick(t);
	}
	CHECK(carol.logged == 0);
	CHECK(dave.logged == 1);
}

TEST("power armor tell: a frame reference made in the wearer's game is never sent, a pre-placed one is, and a status naming a made one anyway is refused")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	Hearer alice{ .peer = 1, .framesHere = { VANILLA_FRAME } };
	Wearer wearer{ .peer = 2 };
	const auto t0 = Clock::now();

	// Bob gets into the museum's suit, which every game has: Alice's game hears which one, to hide her own copy.
	wearer.on = true;
	wearer.seat = { .ref = PRE_PLACED, .base = VANILLA_FRAME, .frame = true };
	wearer.Send(f, t0);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	CHECK(alice.Of(bob->playerId).frameRef == PRE_PLACED);

	// Out: the reference goes, the frame base stays for his next wear's first status.
	wearer.on = false;
	wearer.Send(f, t0 + 1s);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	CHECK(alice.Of(bob->playerId).frameRef == 0u);
	CHECK(alice.Of(bob->playerId).frame == VANILLA_FRAME);

	// Into a suit his own game dropped: its id names another reference, or none, in Alice's, and is never sent.
	wearer.on = true;
	wearer.seat = { .ref = MADE_HERE, .base = VANILLA_FRAME, .frame = true };
	wearer.Send(f, t0 + 2s);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	CHECK(alice.Of(bob->playerId).frameRef == 0u);

	// A game that sends one anyway that is no spawn id either (a spawn id names a friend's frame lent to him: FrameLoan.h)
	// is refused by the server: Alice hears nothing, and
	// keeps what she had. His next good status, of a plugin frame she lacks, gets through and is logged once.
	Protocol::PlayerStatus bad;
	bad.level = 12;
	bad.health = 100;
	bad.powerArmor = true;
	bad.powerArmorFrame = VANILLA_FRAME;
	bad.powerArmorFrameRef = 0xFF000000;  // the runtime range's first id, which no spawn has
	f.fake->Deliver(2, Protocol::Encode(bad, MT::kReportStatus));
	CHECK(!f.fake->WaitFor(1, Type(MT::kPlayerStatus), 200ms));
	CHECK(alice.Of(bob->playerId).frameRef == 0u);
	wearer.on = false;
	wearer.Send(f, t0 + 3s);
	wearer.on = true;
	wearer.seat = { .ref = 0xFF004000, .base = PLUGIN_FRAME, .frame = true };
	wearer.Send(f, t0 + 4s);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	REQUIRE(alice.HearStatus(f, bob->playerId));
	CHECK(alice.Of(bob->playerId).frame == PLUGIN_FRAME);
	Run(wearer, alice, t0 + 4s, t0 + 6s);
	CHECK(alice.logged == 1);
	CHECK(wearer.logged == 0);
}

TEST("power armor tell: a friend whose game restarts and comes back in the same suit is logged again, as their leaving reaches us before their new status")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob", 0xB0B));
	REQUIRE(bob);
	Hearer alice{ .peer = 1, .framesHere = { VANILLA_FRAME } };
	Wearer first{ .peer = 2 };
	const auto t0 = Clock::now();
	first.on = true;
	first.seat = { .ref = 0xFF004000, .base = PLUGIN_FRAME, .frame = true };
	first.Send(f, t0);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	Run(first, alice, t0, t0 + 2s);
	CHECK(alice.logged == 1);

	// His game restarts and joins again before the server noticed the old connection was gone: the server drops
	// that one, he keeps his id, and his new game (in the same suit after loading) sends its first status.
	const auto again = f.Join(5, Fixture::MakeHello("Bob", 0xB0B));
	REQUIRE(again);
	CHECK(again->playerId == bob->playerId);
	Wearer second{ .peer = 5 };
	second.on = true;
	second.seat = first.seat;
	second.Send(f, t0 + 3s);
	REQUIRE(alice.HearStatus(f, bob->playerId));
	const std::vector<std::string> expected{ std::format("status {}", bob->playerId), std::format("left {}", bob->playerId), std::format("status {}", bob->playerId) };
	CHECK(alice.order == expected);
	Run(second, alice, t0 + 3s, t0 + 5s);
	CHECK(alice.logged == 2);
}

TEST("power armor tell: the worn frame is what the player sits in while that is a frame, else the one the engine keeps as worn")
{
	const ArmorFrames::Seat none{};
	const ArmorFrames::Seat frame{ .ref = PRE_PLACED, .base = VANILLA_FRAME, .frame = true };
	const ArmorFrames::Seat made{ .ref = MADE_HERE, .base = PLUGIN_FRAME, .frame = true };
	const ArmorFrames::Seat terminal{ .ref = 0xFF002000, .base = TERMINAL, .frame = false };

	// Getting in: what they sit in, whatever the engine keeps.
	CHECK(ArmorFrames::WornSeat(frame, none).ref == PRE_PLACED);
	CHECK(ArmorFrames::WornSeat(frame, made).ref == PRE_PLACED);
	// Loaded in the armor: nothing sat in, the engine's worn frame.
	CHECK(ArmorFrames::WornSeat(none, made).ref == MADE_HERE);
	CHECK(ArmorFrames::WornSeat(none, made).base == PLUGIN_FRAME);
	CHECK(ArmorFrames::WornSeat(none, frame).frame);
	// Loaded in the armor and at a terminal at once: the worn frame, never the terminal.
	CHECK(ArmorFrames::WornSeat(terminal, frame).base == VANILLA_FRAME);
	// Neither a frame: what they sit in, for the log.
	CHECK(ArmorFrames::WornSeat(terminal, none).base == TERMINAL);
	CHECK(!ArmorFrames::WornSeat(terminal, none).frame);
	CHECK(ArmorFrames::WornSeat(none, none).base == 0u);
	// A frame with no base is no frame to tell.
	CHECK(ArmorFrames::WornSeat(none, ArmorFrames::Seat{ .ref = MADE_HERE, .base = 0, .frame = true }).base == 0u);
}

TEST("power armor tell: a player whose game loads with them already in power armor is seen in their frame by a friend there, one joining late, after a reload and after they come back")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Hearer bob{ .peer = 2, .framesHere = { VANILLA_FRAME } };
	const auto t0 = Clock::now();

	// Her save was made in a suit her game dropped: loaded, she sits in nothing, and the engine keeps the frame as worn.
	// Her first status names it; before, every status named none until she got out and in again.
	Wearer wearer{ .peer = 1 };
	wearer.on = true;
	wearer.held = { .ref = MADE_HERE, .base = VANILLA_FRAME, .frame = true };
	wearer.Send(f, t0);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	CHECK(bob.Of(alice->playerId).on);
	CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);
	CHECK(bob.Of(alice->playerId).frameRef == 0u);  // made in her game: never sent
	Run(wearer, bob, t0, t0 + 3s);
	CHECK(wearer.logged == 0);
	CHECK(bob.logged == 0);

	// Carol joins late: the server tells her Alice's last status, frame and all.
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	Hearer carol{ .peer = 3, .framesHere = { VANILLA_FRAME } };
	REQUIRE(carol.HearStatus(f, alice->playerId));
	CHECK(carol.Of(alice->playerId).on);
	CHECK(carol.Of(alice->playerId).frame == VANILLA_FRAME);

	// She reloads a save made in the museum's suit, which every game has: the wear is forgotten (Party::OnGameLoaded)
	// and learnt again from the engine's worn frame, reference and all.
	wearer.wear.Forget();
	wearer.held = { .ref = PRE_PLACED, .base = VANILLA_FRAME, .frame = true };
	wearer.Send(f, t0 + 4s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	CHECK(bob.Of(alice->playerId).on);
	CHECK(bob.Of(alice->playerId).frameRef == PRE_PLACED);
	Run(wearer, bob, t0 + 4s, t0 + 7s);
	CHECK(wearer.logged == 0);
	CHECK(bob.logged == 0);

	// Her game restarts and joins again, loaded in the same suit: a fresh game's wear, known from its first status.
	const auto again = f.Join(5, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == alice->playerId);
	Wearer second{ .peer = 5 };
	second.on = true;
	second.held = wearer.held;
	second.Send(f, t0 + 8s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	CHECK(bob.Of(alice->playerId).on);
	CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);
	CHECK(bob.Of(alice->playerId).frameRef == PRE_PLACED);
	Run(second, bob, t0 + 8s, t0 + 11s);
	CHECK(second.logged == 0);
	CHECK(bob.logged == 0);

	// Out, and in again by the usual way: what she sits in names it, as before.
	second.on = false;
	second.held = {};
	second.Send(f, t0 + 12s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	CHECK(!bob.Of(alice->playerId).on);
	second.on = true;
	second.seat = { .ref = 0xFF004000, .base = VANILLA_FRAME, .frame = true };
	second.held = second.seat;
	second.Send(f, t0 + 13s);
	REQUIRE(bob.HearStatus(f, alice->playerId));
	CHECK(bob.Of(alice->playerId).on);
	CHECK(bob.Of(alice->playerId).frame == VANILLA_FRAME);
	CHECK(bob.Of(alice->playerId).frameRef == 0u);
}
