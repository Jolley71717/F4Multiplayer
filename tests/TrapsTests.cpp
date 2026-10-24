// World traps shared (src/game/TrapRules.h, src/game/Traps.h): what a trap's script state says, what a game reports of
// a trap, what another game does with the report, and the report's way through the session server: relayed to the
// others, kept in the world state for whoever joins later, and in the session file through a restart.

#include "ServerFixture.h"

#include "SessionFile.h"
#include "game/TrapRules.h"

#include <cstring>
#include <filesystem>
#include <map>

using namespace TestServer;

namespace
{
	using TS = Protocol::TrapState;
	using TrapRules::Action;
	using TrapRules::Kind;
	using TrapRules::Phase;
	using Clock = TrapRules::Clock;
	using namespace std::chrono_literals;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t MINE = 0x00181819;      // a frag mine placed in the Commonwealth
	constexpr std::uint32_t TRIPWIRE = 0x0010BE7F;  // a tripwire, linked to a grenade bouquet
	constexpr std::uint32_t BOUQUET = 0x0010BE80;
	constexpr std::uint32_t BEAR = 0x020385EB;  // bear traps in a Far Harbor quarry
	constexpr std::uint32_t BEAR2 = 0x020385EA;
	constexpr std::uint64_t HOST_IDENTITY = 0x7AB5ull;

	std::vector<std::uint8_t> StateAt(std::uint32_t a_sequence, float a_x, float a_y)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.worldspace = COMMONWEALTH;
		state.x = a_x, state.y = a_y;
		return Protocol::Encode(state);
	}

	TS Trap(std::uint32_t a_ref, std::uint8_t a_state, float a_x = 1000.0f, float a_y = 2000.0f)
	{
		return TS{ 0, a_ref, a_state, 0, COMMONWEALTH, a_x, a_y, 0.0f };
	}

	std::vector<Protocol::TrapStates> TrapLists(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kTraps, [](auto a_data) { return Protocol::DecodeTrapStates(a_data); });
	}

	// One game's view of its traps, run by the rules as Traps runs them: what it reports, and what it does with the
	// session's states.
	struct Game
	{
		struct Here
		{
			Kind  kind;
			Phase phase = Phase::kArmed;
			Phase last = Phase::kArmed;
		};
		std::map<std::uint32_t, Here>                            traps;
		std::unordered_map<std::uint32_t, TrapRules::Known>      known;
		std::vector<TS>                                          reports;
		std::map<std::uint32_t, Action>                          done;

		// The watcher's look: a report for each change, as Traps::Poll makes it.
		void Look(bool a_takenByUs = false, bool a_settled = true)
		{
			for (auto& [id, trap] : traps) {
				const auto it = known.find(id);
				const auto session = it != known.end() ? std::optional{ it->second.state.state } : std::nullopt;
				if (const auto state = a_settled ? TrapRules::Change(trap.kind, trap.last, trap.phase, a_takenByUs, session) : std::nullopt) {
					reports.push_back(Trap(id, *state));
					TrapRules::Take(known, reports.back(), false, Clock::now());
					known[id].applied = true;
				}
				trap.last = trap.phase;
			}
		}

		// What the session's states do here: a fire set off live goes off, quiet ones are spent.
		void Apply(Clock::time_point a_now, bool a_settled = true)
		{
			for (auto& [id, entry] : known) {
				const auto here = traps.find(id);
				if (entry.applied || here == traps.end()) {
					continue;
				}
				const bool live = TrapRules::Live(entry.live, entry.arrived, a_now, 0.0f);
				const auto action = TrapRules::Decide(entry.state.state, here->second.phase, live, a_settled);
				if (action == Action::kWait) {
					continue;
				}
				entry.applied = true;
				done[id] = action;
				auto& phase = here->second.phase;
				switch (action) {
				case Action::kFireLive:
				case Action::kFireQuiet:
					phase = here->second.kind == Kind::kMine ? Phase::kGone : Phase::kFired;
					break;
				case Action::kDisarm:
					phase = here->second.kind == Kind::kMine ? Phase::kGone : Phase::kDisarmed;
					break;
				case Action::kTake:
					phase = Phase::kGone;
					break;
				default:
					break;
				}
			}
		}
	};

	Server::Options Hosted(std::string a_file = {})
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		options.worldStateRefill = std::chrono::milliseconds(1);
		options.sessionFile = std::move(a_file);
		return options;
	}

	std::uint32_t JoinAsHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		return JoinAs(a_f, a_peer, "Host", HOST_IDENTITY);
	}
}

TEST("traps: a script's state reads as the trap's phase, whatever its case")
{
	CHECK(TrapRules::PhaseOf(Kind::kTripwire, "Active") == Phase::kArmed);
	CHECK(TrapRules::PhaseOf(Kind::kTripwire, "busy") == Phase::kGoing);
	CHECK(TrapRules::PhaseOf(Kind::kTripwire, "Inactive") == Phase::kFired);
	CHECK(TrapRules::PhaseOf(Kind::kTripwire, "DISARMED") == Phase::kDisarmed);
	CHECK(TrapRules::PhaseOf(Kind::kTension, "Waiting") == Phase::kArmed);
	CHECK(TrapRules::PhaseOf(Kind::kTension, "triggered") == Phase::kFired);
	CHECK(TrapRules::PhaseOf(Kind::kTrap, "Idle") == Phase::kArmed);
	CHECK(TrapRules::PhaseOf(Kind::kTrap, "warn") == Phase::kFired);
	CHECK(TrapRules::PhaseOf(Kind::kTrap, "TrapFired") == Phase::kFired);
	CHECK(TrapRules::PhaseOf(Kind::kTrap, "Disarm") == Phase::kDisarmed);
	CHECK(TrapRules::PhaseOf(Kind::kBearTrap, "Open") == Phase::kArmed);
	CHECK(TrapRules::PhaseOf(Kind::kBearTrap, "Closed") == Phase::kFired);
	CHECK(TrapRules::PhaseOf(Kind::kBearTrap, "Disarming") == Phase::kDisarmed);
	// The empty state (before its OnLoad), another kind's state name, or a mine (no script): not known.
	CHECK(TrapRules::PhaseOf(Kind::kBearTrap, "") == Phase::kUnknown);
	CHECK(TrapRules::PhaseOf(Kind::kTrap, "Active") == Phase::kUnknown);
	CHECK(TrapRules::PhaseOf(Kind::kMine, "Active") == Phase::kUnknown);
	// Each kind's script is the first listed for it; the base trap's last, under its children's.
	CHECK(TrapRules::ScriptOf(Kind::kTripwire) == "TrapTripwire");
	CHECK(TrapRules::ScriptOf(Kind::kBearTrap) == "DLC03:TrapBear");
	CHECK(TrapRules::SCRIPTS.back().kind == Kind::kTrap);
	CHECK(TrapRules::ScriptOf(Kind::kMine).empty());
}

TEST("traps: an activator with no trap script bound yet is looked at again until it has been loaded a while")
{
	const auto t0 = Clock::now();
	CHECK(!TrapRules::ClassFinal(Kind::kNone, true, t0, t0));  // its cell just loaded: its scripts may not be bound yet
	CHECK(!TrapRules::ClassFinal(Kind::kNone, true, t0, t0 + 9s));
	CHECK(TrapRules::ClassFinal(Kind::kNone, true, t0, t0 + 10s));  // long enough: no trap
	CHECK(TrapRules::ClassFinal(Kind::kTrap, true, t0, t0));        // a trap is known at once
	CHECK(TrapRules::ClassFinal(Kind::kNone, false, t0, t0));       // not an activator: never a scripted trap
	CHECK(TrapRules::ClassFinal(Kind::kMine, false, t0, t0));
}

TEST("traps: only a trap seen armed is reported, once, and a mine gone went off unless our player picked it up")
{
	using TrapRules::Change;
	const std::optional<std::uint8_t> none;
	CHECK(Change(Kind::kTripwire, Phase::kArmed, Phase::kFired, false, none) == TS::kFired);
	CHECK(Change(Kind::kTripwire, Phase::kGoing, Phase::kDisarmed, true, none) == TS::kDisarmed);
	CHECK(!Change(Kind::kTripwire, Phase::kArmed, Phase::kGoing, false, none));  // on its way: the next look says which
	CHECK(!Change(Kind::kTripwire, Phase::kArmed, Phase::kArmed, false, none));
	// First seen spent: spent before we came by.
	CHECK(!Change(Kind::kTripwire, Phase::kUnknown, Phase::kFired, false, none));
	CHECK(!Change(Kind::kTripwire, Phase::kFired, Phase::kFired, false, none));
	// A mine is there or not.
	CHECK(Change(Kind::kMine, Phase::kArmed, Phase::kGone, false, none) == TS::kFired);
	CHECK(Change(Kind::kMine, Phase::kArmed, Phase::kGone, true, none) == TS::kTaken);
	// A bear trap that snapped is taken by whoever takes it; one gone otherwise (a quest's script) isn't news.
	CHECK(Change(Kind::kBearTrap, Phase::kFired, Phase::kGone, true, none) == TS::kTaken);
	CHECK(!Change(Kind::kBearTrap, Phase::kFired, Phase::kGone, false, none));
	CHECK(!Change(Kind::kTripwire, Phase::kArmed, Phase::kGone, false, none));
	// What the session holds already isn't sent again (what a friend's report did here isn't reported back); a take
	// still is after a disarm, never after a spring (that stands).
	CHECK(!Change(Kind::kTripwire, Phase::kArmed, Phase::kFired, false, TS::kFired));
	CHECK(!Change(Kind::kMine, Phase::kArmed, Phase::kGone, false, TS::kFired));
	CHECK(!Change(Kind::kBearTrap, Phase::kArmed, Phase::kFired, false, TS::kDisarmed));
	CHECK(!Change(Kind::kBearTrap, Phase::kFired, Phase::kGone, true, TS::kFired));
	CHECK(Change(Kind::kMine, Phase::kDisarmed, Phase::kGone, true, TS::kDisarmed) == TS::kTaken);
	CHECK(!Change(Kind::kMine, Phase::kArmed, Phase::kGone, true, TS::kTaken));
}

TEST("traps: a friend's state is done to an armed trap once it has settled, live only just after and where it went off")
{
	using TrapRules::Decide;
	CHECK(Decide(TS::kFired, Phase::kArmed, true, false) == Action::kWait);  // only just loaded
	CHECK(Decide(TS::kFired, Phase::kUnknown, true, true) == Action::kWait);
	CHECK(Decide(TS::kFired, Phase::kGoing, true, true) == Action::kWait);
	CHECK(Decide(TS::kFired, Phase::kArmed, true, true) == Action::kFireLive);
	CHECK(Decide(TS::kFired, Phase::kArmed, false, true) == Action::kFireQuiet);
	CHECK(Decide(TS::kDisarmed, Phase::kArmed, true, true) == Action::kDisarm);
	CHECK(Decide(TS::kFired, Phase::kFired, true, true) == Action::kNothing);  // ours went off too
	CHECK(Decide(TS::kDisarmed, Phase::kFired, false, true) == Action::kNothing);
	CHECK(Decide(TS::kTaken, Phase::kFired, false, true) == Action::kTake);  // a bear trap ours snapped too, taken there
	CHECK(Decide(TS::kTaken, Phase::kArmed, false, true) == Action::kTake);
	CHECK(Decide(TS::kTaken, Phase::kGone, false, true) == Action::kNothing);

	const auto now = Clock::now();
	CHECK(TrapRules::Live(true, now - 1s, now, 10.0f));
	CHECK(!TrapRules::Live(true, now - 4s, now, 10.0f));                         // it waited for the trap to load here
	CHECK(!TrapRules::Live(true, now, now, TrapRules::LIVE_SLACK + 1.0f));       // not where theirs was
	CHECK(!TrapRules::Live(false, now, now, 0.0f));                               // a world state's

	const auto loaded = now - 1s;
	CHECK(!TrapRules::Settled(loaded, now - 10s, now));
	CHECK(TrapRules::Settled(loaded, now - 10s, now + 1s));
	CHECK(!TrapRules::Settled(now - 10s, now - 1s, now));  // our player only just in the world
}

TEST("traps: the explosions of a trap gone off here, or at a mine we watch, are the trap's own")
{
	const auto now = Clock::now();
	const std::vector<TrapRules::Fired> fired{ { { 1000.0f, 2000.0f, 0.0f }, now - 2s } };
	const std::vector<TrapRules::Spot>  mines{ { -5000.0f, 300.0f, 10.0f } };
	CHECK(TrapRules::Claims(fired, mines, { 1500.0f, 2500.0f, 0.0f }, now));    // a bouquet's grenade, a little off
	CHECK(!TrapRules::Claims(fired, mines, { 3000.0f, 2000.0f, 0.0f }, now));   // too far: somebody's own
	CHECK(!TrapRules::Claims(fired, mines, { 1000.0f, 2000.0f, 0.0f }, now + 10s));
	CHECK(TrapRules::Claims(fired, mines, { -5100.0f, 300.0f, 10.0f }, now));
	CHECK(!TrapRules::Claims(fired, mines, { -5400.0f, 300.0f, 10.0f }, now));
	CHECK(!TrapRules::Claims({}, {}, { 0.0f, 0.0f, 0.0f }, now));
}

TEST("traps: the book keeps the first state, and a take after a disarm, never after a spring")
{
	std::unordered_map<std::uint32_t, TrapRules::Known> book;
	const auto                                           now = Clock::now();
	CHECK(TrapRules::Take(book, Trap(TRIPWIRE, TS::kFired), true, now));
	CHECK(!TrapRules::Take(book, Trap(TRIPWIRE, TS::kDisarmed), true, now));
	CHECK(!TrapRules::Take(book, Trap(TRIPWIRE, TS::kFired), false, now));  // a world state's repeat
	CHECK(book.at(TRIPWIRE).live);
	CHECK(!TrapRules::Take(book, Trap(TRIPWIRE, TS::kTaken), false, now));  // what went off stands
	CHECK(book.at(TRIPWIRE).state.state == TS::kFired);
	CHECK(TrapRules::Take(book, Trap(MINE, TS::kDisarmed), true, now));
	CHECK(TrapRules::Take(book, Trap(MINE, TS::kTaken), false, now));
	CHECK(book.at(MINE).state.state == TS::kTaken);
	CHECK(!TrapRules::Take(book, Trap(MINE, TS::kTaken), false, now));
	CHECK(!TrapRules::Take(book, Trap(MINE, TS::kFired), false, now));
	CHECK(Protocol::TrapSupersedes(TS::kTaken, std::nullopt));
	CHECK(!Protocol::TrapSupersedes(TS::kFired, TS::kTaken));
}

TEST("traps: a tripwire the host trips sets off the friend's, its bouquet goes too, and nothing comes back")
{
	Fixture f{ Hosted() };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, StateAt(1, 1100.0f, 2000.0f));
	f.fake->Deliver(2, StateAt(1, 1200.0f, 2000.0f));
	Clear(f, { 1, 2 });

	Game host, bob;
	for (auto* game : { &host, &bob }) {
		game->traps[TRIPWIRE] = { Kind::kTripwire };
		game->traps[BOUQUET] = { Kind::kTrap };
		game->Look();
	}
	// The host walks into the wire: it trips, and the bouquet it is linked to goes off.
	host.traps[TRIPWIRE].phase = Phase::kFired;
	host.traps[BOUQUET].phase = Phase::kFired;
	host.Look();
	REQUIRE(host.reports.size() == 2);
	for (const auto& report : host.reports) {
		f.fake->Deliver(1, Protocol::Encode(report));
	}
	const auto lists = TrapLists(Received(f, 2));
	REQUIRE(lists.size() == 2);
	for (const auto& list : lists) {
		CHECK(list.live);
		REQUIRE(list.traps.size() == 1);
		CHECK(list.traps[0].playerId != 0);
		TrapRules::TakeList(bob.known, list, Clock::now());
	}
	CHECK(TrapLists(Received(f, 1)).empty());  // not echoed to the host

	bob.Apply(Clock::now());
	CHECK(bob.done.at(TRIPWIRE) == Action::kFireLive);
	CHECK(bob.done.at(BOUQUET) == Action::kFireLive);
	// What that did in Bob's game is no news.
	bob.Look();
	CHECK(bob.reports.empty());
}

TEST("traps: what a trap does in its first moments loaded is its script setting it up, not news")
{
	Game host;
	host.traps[BEAR] = { Kind::kBearTrap };
	host.Look(false, false);  // found open
	host.traps[BEAR].phase = Phase::kFired;
	host.Look(false, false);  // closed by its OnLoad a moment later
	CHECK(host.reports.empty());
	host.Look();  // settled: closed is how it was
	CHECK(host.reports.empty());

	// Once settled, a snap is reported.
	host.traps[BEAR2] = { Kind::kBearTrap };
	host.Look();
	host.traps[BEAR2].phase = Phase::kFired;
	host.Look();
	REQUIRE(host.reports.size() == 1);
	CHECK(host.reports[0].refId == BEAR2);
	CHECK(host.reports[0].state == TS::kFired);
}

TEST("traps: a mine a friend set off while we were elsewhere is spent quietly when we come by, and stays so after a load")
{
	Game bob;
	const auto arrived = Clock::now();
	TrapRules::TakeList(bob.known, Protocol::TrapStates{ true, { Trap(MINE, TS::kFired) } }, arrived);
	bob.Apply(arrived + 1s);  // not loaded here yet: nothing to do
	CHECK(bob.done.empty());

	bob.traps[MINE] = { Kind::kMine };
	bob.Look();
	bob.Apply(arrived + 30s, false);  // just loaded
	CHECK(bob.done.empty());
	bob.Apply(arrived + 32s);
	CHECK(bob.done.at(MINE) == Action::kFireQuiet);
	CHECK(bob.traps[MINE].phase == Phase::kGone);
	bob.Look();
	CHECK(bob.reports.empty());

	// A load brings the mine back: the session's state spends it again (Traps::OnGameLoaded looks at it afresh).
	bob.done.clear();
	bob.traps[MINE] = { Kind::kMine };
	for (auto& [id, entry] : bob.known) {
		entry.applied = false;
		entry.live = false;
	}
	bob.Look();
	bob.Apply(arrived + 60s);
	CHECK(bob.done.at(MINE) == Action::kFireQuiet);
	CHECK(bob.reports.empty());
}

TEST("traps: the server keeps the first report, a take after a disarm, and only from a player near the trap")
{
	Fixture f{ Hosted() };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// Nowhere known yet: dropped.
	f.fake->Deliver(1, Protocol::Encode(Trap(MINE, TS::kFired)));
	CHECK(TrapLists(Received(f, 2)).empty());

	f.fake->Deliver(1, StateAt(1, 1100.0f, 2000.0f));
	f.fake->Deliver(2, StateAt(1, 90000.0f, 2000.0f));  // Bob is across the map
	Clear(f, { 1, 2 });
	// A trap far from its reporter: dropped.
	f.fake->Deliver(2, Protocol::Encode(Trap(MINE, TS::kFired)));
	CHECK(TrapLists(Received(f, 1)).empty());
	// One in another space, at the same numbers: dropped.
	auto inside = Trap(MINE, TS::kFired);
	inside.cell = 0x0001F00F, inside.worldspace = 0;
	f.fake->Deliver(1, Protocol::Encode(inside));
	CHECK(TrapLists(Received(f, 2)).empty());

	f.fake->Deliver(1, Protocol::Encode(Trap(MINE, TS::kFired)));
	auto lists = TrapLists(Received(f, 2));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].traps[0].refId == MINE);
	CHECK(lists[0].traps[0].state == TS::kFired);

	// Another spring, a disarm or a take of it isn't news: what went off stands.
	f.fake->Deliver(1, Protocol::Encode(Trap(MINE, TS::kFired)));
	f.fake->Deliver(1, Protocol::Encode(Trap(MINE, TS::kDisarmed)));
	f.fake->Deliver(1, Protocol::Encode(Trap(MINE, TS::kTaken)));
	CHECK(TrapLists(Received(f, 2)).empty());

	// A disarm and a take are done standing at the trap: 1500 units off, dropped; a spring that far is believed.
	f.fake->Deliver(1, Protocol::Encode(Trap(BEAR, TS::kTaken, 1000.0f, 3500.0f)));
	f.fake->Deliver(1, Protocol::Encode(Trap(BEAR, TS::kDisarmed, 1000.0f, 3500.0f)));
	CHECK(TrapLists(Received(f, 2)).empty());
	f.fake->Deliver(1, Protocol::Encode(Trap(BEAR, TS::kFired, 1000.0f, 3500.0f)));
	lists = TrapLists(Received(f, 2));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].traps[0].state == TS::kFired);

	// A disarm, then a take: news. A known trap is where the session has it, whatever a report says: Bob's take of it
	// "at his feet" across the map is dropped.
	f.fake->Deliver(1, Protocol::Encode(Trap(TRIPWIRE, TS::kDisarmed)));
	lists = TrapLists(Received(f, 2));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].traps[0].state == TS::kDisarmed);
	f.fake->Deliver(2, Protocol::Encode(Trap(TRIPWIRE, TS::kTaken, 90000.0f, 2000.0f)));
	CHECK(TrapLists(Received(f, 1)).empty());
	f.fake->Deliver(1, Protocol::Encode(Trap(TRIPWIRE, TS::kTaken, 90000.0f, 2000.0f)));  // the host stands at it
	lists = TrapLists(Received(f, 2));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].traps[0].state == TS::kTaken);
	CHECK(lists[0].traps[0].x == 1000.0f);  // where it is stays where it was

	// A joiner gets the list (as no news) before the world state that ends their catching up.
	const auto welcome = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(welcome);
	const auto packets = Received(f, 3);
	lists = TrapLists(packets);
	REQUIRE(lists.size() == 1);
	CHECK(!lists[0].live);
	REQUIRE(lists[0].traps.size() == 3);
	std::map<std::uint32_t, std::uint8_t> states;
	for (const auto& trap : lists[0].traps) {
		states[trap.refId] = trap.state;
	}
	CHECK(states[MINE] == TS::kFired);
	CHECK(states[BEAR] == TS::kFired);
	CHECK(states[TRIPWIRE] == TS::kTaken);
	std::size_t trapsAt = packets.size(), worldAt = 0;
	for (std::size_t i = 0; i < packets.size(); ++i) {
		const auto type = Protocol::PeekType(packets[i]);
		if (type == MT::kTraps && trapsAt == packets.size()) {
			trapsAt = i;
		}
		if (type == MT::kWorldState) {
			worldAt = i;
		}
	}
	CHECK(trapsAt < worldAt);
}

TEST("traps: the session's spent traps are in its file, through the host's restart")
{
	const auto path = Test::TempPath("f4mp-traps", ".session").string();
	std::filesystem::remove(path);
	{
		Fixture f{ Hosted(path) };
		JoinAsHost(f, 1);
		f.fake->Deliver(1, StateAt(1, 1100.0f, 2000.0f));
		f.fake->Deliver(1, Protocol::Encode(Trap(TRIPWIRE, TS::kDisarmed)));
		Clear(f, { 1 });
	}
	{
		Fixture f{ Hosted(path) };
		JoinAsHost(f, 1);
		JoinAs(f, 2, "Bob");
		const auto lists = TrapLists(Received(f, 2));
		REQUIRE(lists.size() == 1);
		REQUIRE(lists[0].traps.size() == 1);
		CHECK(lists[0].traps[0].refId == TRIPWIRE);
		CHECK(lists[0].traps[0].state == TS::kDisarmed);
	}
	std::filesystem::remove(path);
}

TEST("session file: spent traps round trip under layout 34, and are damage in a file of 33")
{
	SessionFile::Snapshot in;
	in.contentHash = 0xF672F384;
	in.sessionId = 7;
	in.nextId = 2;
	in.traps = { Trap(MINE, TS::kFired), Trap(TRIPWIRE, TS::kTaken, -300.0f, 5.0f) };
	in.traps[0].playerId = 1;
	CHECK(!in.Empty());
	const auto bytes = SessionFile::Encode(in);
	const auto out = SessionFile::Decode(bytes);
	REQUIRE(out);
	CHECK(out->traps == in.traps);

	auto as33 = bytes;
	const std::uint16_t layout = SessionFile::ALL_THREE;
	std::memcpy(as33.data() + 4, &layout, sizeof(layout));
	CHECK(!SessionFile::Decode(as33));

	// A file of 33 without traps still loads, with drops, wires and the plugin table as before.
	SessionFile::Snapshot bare = in;
	bare.traps.clear();
	auto bare33 = SessionFile::Encode(bare);
	std::memcpy(bare33.data() + 4, &layout, sizeof(layout));
	const auto old = SessionFile::Decode(bare33);
	REQUIRE(old);
	CHECK(old->traps.empty());
	CHECK(SessionFile::HasDrops(SessionFile::FORMAT) && SessionFile::HasWires(SessionFile::FORMAT) && SessionFile::HasPluginTable(SessionFile::FORMAT));
	CHECK(!SessionFile::HasTraps(SessionFile::ALL_THREE));
}
