// Security gates run by the game's two-state script, their open state shared as a door's (src/game/SharedGates.h):
// which gates, what a game reads of one, when it sets a friend's through the script, and the state's way through the
// session server to a friend at the gate, a friend elsewhere and a late joiner. The gate's script is modelled as
// Default2StateActivator runs it: SetOpenNoWait sets a timer, the timer calls SetOpen, which waits out a busy gate, then
// goes busy, plays the animation and only then sets isOpen.

#include "ServerFixture.h"

#include "game/DoorShare.h"
#include "game/SharedGates.h"

#include <algorithm>
#include <map>
#include <set>

using namespace TestServer;

namespace
{
	using RS = Protocol::RefState;
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Clock = SharedGates::Clock;
	using SharedGates::Apply;
	using SharedGates::ScriptState;
	using namespace std::chrono_literals;

	constexpr std::uint32_t TERMINAL_GATE = 0x000FB323;  // SecurityGateSm01A at Malden Drainage, opened from its terminal
	constexpr std::uint32_t CARD_GATE = 0x000ED773;      // SecurityGateSm01A at Wilson Atomatoys HQ, behind two ID card readers
	constexpr std::uint32_t CHAINED_GATE = 0x000FB324;   // a second gate the first one links (a terminal's chain)

	// One gate's Default2StateActivator.
	struct Script
	{
		bool                isOpen = false;
		bool                busy = false;
		std::optional<bool> timer;  // SetOpenNoWait's shouldOpenNext, waiting for the timer
		int                 animations = 0;

		ScriptState State() const { return { isOpen, busy }; }

		// waiting.onActivate: a toggle. busy.onActivate does nothing (AllowInterrupt is false on the gates).
		void Activate()
		{
			if (!busy) {
				Begin(!isOpen);
			}
		}

		void SetOpenNoWait(bool a_open) { timer = a_open; }

		// The VM runs: the timer fires (SetOpen waits while the gate is busy), or the animation finishes.
		void Tick()
		{
			if (busy) {
				busy = false;
				isOpen = !isOpen;
				return;
			}
			if (timer) {
				const bool open = *timer;
				timer.reset();
				Begin(open);
			}
		}

		void Settle()
		{
			for (int i = 0; i < 6; ++i) {
				Tick();
			}
		}

	private:
		void Begin(bool a_open)
		{
			if (a_open != isOpen) {
				busy = true;
				++animations;
			}
		}
	};

	// One game's gates, judged as WorldSync judges them: what it reads (ReadRefState), what its watcher reports
	// (WatchRefStates, with SharedGates::Applied), and what it takes from the server (OfferRefState, TryApplyRefState).
	struct GateGame
	{
		std::map<std::uint32_t, Script>            gates;
		std::set<std::uint32_t>                    loaded;
		std::map<std::uint32_t, Clock::time_point> loadedAt;
		std::map<std::uint32_t, RS>                lastSent;
		std::map<std::uint32_t, RS>                pending;
		DoorShare::Book                            book;
		SharedGates::Applied                       applied;
		Clock::time_point                          now{};
		Clock::time_point                          inWorldSince{};
		int                                        set = 0;

		GateGame()
		{
			for (const auto ref : { TERMINAL_GATE, CARD_GATE, CHAINED_GATE }) {
				gates[ref];
				loaded.insert(ref);
			}
		}

		RS Read(std::uint32_t a_ref) const { return RS{ a_ref, SharedGates::Target(gates.at(a_ref).State()) ? RS::kYes : RS::kNo, RS::kUnknown }; }

		// The watcher's report of a gate it watches, if it says something new.
		std::optional<std::vector<std::uint8_t>> Report(std::uint32_t a_ref)
		{
			if (!loaded.contains(a_ref)) {
				return std::nullopt;
			}
			const auto state = Read(a_ref);
			if (applied.Holds(a_ref, state.open == RS::kYes, now)) {
				return std::nullopt;
			}
			if (lastSent.contains(a_ref) && state.SameState(lastSent[a_ref])) {
				return std::nullopt;
			}
			lastSent[a_ref] = state;
			book.Sent(state);
			return Protocol::Encode(state, MT::kReportRefState);
		}

		bool TryApply(const RS& a_state)
		{
			if (!loaded.contains(a_state.refId)) {
				return false;
			}
			const auto at = loadedAt.find(a_state.refId);
			const bool settled = SharedGates::Settled(at != loadedAt.end() ? std::optional{ at->second } : std::nullopt, inWorldSince, now);
			const bool open = a_state.open == RS::kYes;
			switch (SharedGates::Decide(open, gates.at(a_state.refId).State(), settled)) {
			case Apply::kWait:
				return false;
			case Apply::kSet:
				gates.at(a_state.refId).SetOpenNoWait(open);
				applied.Set(a_state.refId, open, now);
				++set;
				break;
			case Apply::kNothing:
				break;
			}
			return true;
		}

		void Offer(const RS& a_state)
		{
			if (!gates.contains(a_state.refId) || !book.Take(a_state)) {
				return;
			}
			if (lastSent.contains(a_state.refId)) {
				lastSent[a_state.refId] = a_state;
			}
			if (TryApply(a_state)) {
				pending.erase(a_state.refId);
			} else {
				pending[a_state.refId] = a_state;
			}
		}

		void Take(const Packets& a_packets)
		{
			for (const auto& state : Decoded(a_packets, MT::kRefStateChanged, Protocol::DecodeRefState)) {
				Offer(state);
			}
			for (const auto& world : WorldStates(a_packets)) {
				for (const auto& state : world.refStates) {
					Offer(state);
				}
			}
		}

		// Time passes: the VM runs, and what waits is tried again (the retry sweep).
		void Pass(Clock::duration a_time)
		{
			now += a_time;
			for (auto& [ref, script] : gates) {
				script.Tick();
			}
			for (auto it = pending.begin(); it != pending.end();) {
				it = TryApply(it->second) ? pending.erase(it) : std::next(it);
			}
		}

		void Unload(std::uint32_t a_ref) { loaded.erase(a_ref); }

		void Load(std::uint32_t a_ref)
		{
			loaded.insert(a_ref);
			loadedAt[a_ref] = now;
		}

		bool Open(std::uint32_t a_ref) const { return gates.at(a_ref).isOpen && !gates.at(a_ref).busy; }
	};

	Server::Options Unhurried()
	{
		Server::Options options;
		options.worldStateRefill = std::chrono::milliseconds(1);
		return options;
	}
}

TEST("shared gates: only an activator running the two-state script alone, and not listed out")
{
	CHECK(SharedGates::IsGateScript("Default2StateActivator"));
	CHECK(SharedGates::IsGateScript("default2stateactivator"));  // a reference's own copy is named so in the game's files
	CHECK(!SharedGates::IsGateScript("DN066_LabDoorScript"));
	CHECK(!SharedGates::IsGateScript("Default1StateActivator"));
	CHECK(!SharedGates::IsGateScript("Default2StateActivatorX"));
	CHECK(!SharedGates::IsGateScript(""));

	CHECK(SharedGates::Shares({ .activator = true, .scripts = 1, .gateScripts = 1 }));
	CHECK(SharedGates::Shares({ .activator = true, .scripts = 2, .gateScripts = 2 }));  // the base's and the reference's
	CHECK(!SharedGates::Shares({ .activator = true, .scripts = 2, .gateScripts = 1 }));  // dn133_SecurityDoorScript beside it
	CHECK(!SharedGates::Shares({ .activator = true, .scripts = 1, .gateScripts = 0 }));  // a script that extends it
	CHECK(!SharedGates::Shares({ .activator = true }));                                    // no script
	CHECK(!SharedGates::Shares({ .activator = false, .scripts = 1, .gateScripts = 1 }));  // a klaxon light (a moveable static)
	CHECK(!SharedGates::Shares({ .activator = true, .scripts = 1, .gateScripts = 1, .listedOut = true }));
	CHECK(!SharedGates::Shares(SharedGates::Facts{}));
}

TEST("shared gates: the lists name Fallout4.esm forms once each, Mass Fusion's gates among the never")
{
	std::set<std::uint32_t> ids;
	for (const auto& form : SharedGates::NOT_A_WAY) {
		CHECK(form.file == "Fallout4.esm");
		CHECK(form.id != 0 && form.id < 0x01000000);
		CHECK(ids.insert(form.id).second);
	}
	for (const auto& form : SharedGates::NEVER) {
		CHECK(form.file == "Fallout4.esm");
		CHECK(form.id != 0 && form.id < 0x01000000);
		CHECK(ids.insert(form.id).second);
	}
	CHECK(ids.contains(0x000F9CDE));  // DN084_ControlRoomDoor
	CHECK(ids.contains(0x001C5E6F));  // a jukebox
	CHECK(!ids.contains(CARD_GATE));
	CHECK(!ids.contains(TERMINAL_GATE));
}

TEST("shared gates: a gate is read as where it is going, and set only once settled and somewhere else")
{
	CHECK(!SharedGates::Target({ .open = false, .busy = false }));
	CHECK(SharedGates::Target({ .open = false, .busy = true }));  // opening: isOpen is set once the animation has played
	CHECK(SharedGates::Target({ .open = true, .busy = false }));
	CHECK(!SharedGates::Target({ .open = true, .busy = true }));  // closing

	CHECK(SharedGates::Decide(true, std::nullopt, true) == Apply::kWait);  // no script object to read yet
	CHECK(SharedGates::Decide(true, ScriptState{ false, false }, false) == Apply::kWait);
	CHECK(SharedGates::Decide(true, ScriptState{ false, false }, true) == Apply::kSet);
	CHECK(SharedGates::Decide(true, ScriptState{ false, true }, true) == Apply::kNothing);  // on its way already
	CHECK(SharedGates::Decide(true, ScriptState{ true, true }, true) == Apply::kSet);       // closing: opened again after
	CHECK(SharedGates::Decide(false, ScriptState{ false, false }, true) == Apply::kNothing);

	const Clock::time_point start{};
	CHECK(!SharedGates::Settled(std::nullopt, start, start + 1s));
	CHECK(SharedGates::Settled(std::nullopt, start, start + SharedGates::LOAD_SETTLE));
	CHECK(!SharedGates::Settled(start + 10s, start, start + 11s));  // loaded a second ago
	CHECK(SharedGates::Settled(start + 10s, start, start + 12s));
	CHECK(!SharedGates::Settled(start, start + 10s, start + 11s));  // loaded during the loading screen, in the world a second
}

TEST("shared gates: a gate set from a friend's state isn't reported back while it gets there")
{
	SharedGates::Applied applied;
	const Clock::time_point start{};
	CHECK(!applied.Holds(TERMINAL_GATE, false, start));  // never set: news as ever
	applied.Set(TERMINAL_GATE, true, start);
	CHECK(applied.Holds(TERMINAL_GATE, false, start + 1s));  // still reads shut
	CHECK(applied.Size() == 1);
	CHECK(!applied.Holds(TERMINAL_GATE, true, start + 2s));  // there: forgotten
	CHECK(applied.Size() == 0);
	CHECK(!applied.Holds(TERMINAL_GATE, false, start + 3s));  // closed afterwards: news

	applied.Set(CARD_GATE, true, start);
	CHECK(applied.Holds(CARD_GATE, false, start + SharedGates::Applied::HOLD - 1s));
	CHECK(!applied.Holds(CARD_GATE, false, start + SharedGates::Applied::HOLD + 1s));  // never got there: what it says goes
	CHECK(applied.Size() == 0);
}

TEST("shared gates: a gate opened from the host's terminal opens in the friend's game, through its script, and back")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	GateGame host, friendGame;
	host.now = friendGame.now = Clock::time_point{} + 1min;
	// The friend used the same terminal a minute ago (and couldn't hack it): their game watches the gate too, and both
	// watchers have said it is shut.
	CHECK(friendGame.Report(TERMINAL_GATE));
	CHECK(host.Report(TERMINAL_GATE));
	CHECK(!friendGame.Report(TERMINAL_GATE));
	Clear(f, { 1, 2 });

	// The host's terminal: SetOpenNoWait(true). Reported as the gate starts to move, before isOpen is set.
	host.gates[TERMINAL_GATE].SetOpenNoWait(true);
	host.Pass(100ms);
	REQUIRE(host.gates[TERMINAL_GATE].busy);
	const auto report = host.Report(TERMINAL_GATE);
	REQUIRE(report);
	f.fake->Deliver(1, *report);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.set == 1);
	CHECK(friendGame.gates[TERMINAL_GATE].timer == std::optional{ true });

	// Until the friend's script runs the gate reads shut there: the friend's watcher doesn't send that back.
	CHECK(!friendGame.Report(TERMINAL_GATE));
	friendGame.Pass(100ms);  // the timer: busy, opening
	CHECK(!friendGame.Report(TERMINAL_GATE));
	friendGame.Pass(2s);  // the animation has played
	CHECK(friendGame.Open(TERMINAL_GATE));
	CHECK(!friendGame.Report(TERMINAL_GATE));
	host.Pass(2s);
	CHECK(!host.Report(TERMINAL_GATE));
	CHECK(Count(Received(f, 1), MT::kRefStateChanged) == 0);

	// The friend closes it with the terminal now unlocked: the host's closes.
	friendGame.gates[TERMINAL_GATE].SetOpenNoWait(false);
	friendGame.Pass(100ms);
	const auto close = friendGame.Report(TERMINAL_GATE);
	REQUIRE(close);
	f.fake->Deliver(2, *close);
	host.Take(Received(f, 1));
	host.Pass(100ms);
	host.Pass(2s);
	CHECK(!host.gates[TERMINAL_GATE].isOpen);
	CHECK(host.gates[TERMINAL_GATE].animations == 2);
	CHECK(!host.Report(TERMINAL_GATE));
}

TEST("shared gates: a card reader's gate opened while the friend was elsewhere opens when they get there")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	GateGame host, friendGame;
	friendGame.Unload(CARD_GATE);
	host.now = friendGame.now = Clock::time_point{} + 1min;

	// The host's card: the reader activates its gate (a toggle), for the host only.
	host.gates[CARD_GATE].Activate();
	f.fake->Deliver(1, *host.Report(CARD_GATE));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.set == 0);
	CHECK(friendGame.pending.contains(CARD_GATE));

	// The friend walks in: the gate loads, plays its OnLoad, and only then is opened.
	friendGame.Pass(10s);
	friendGame.Load(CARD_GATE);
	friendGame.Pass(1s);
	CHECK(friendGame.set == 0);
	friendGame.Pass(2s);
	CHECK(friendGame.set == 1);
	friendGame.Pass(2s);
	friendGame.Pass(2s);
	CHECK(friendGame.Open(CARD_GATE));
	CHECK(friendGame.pending.empty());
}

TEST("shared gates: a joiner's world state opens the session's gates once, and its repeats change nothing")
{
	Fixture f{ Unhurried() };
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	GateGame host, joiner;
	host.gates[TERMINAL_GATE].SetOpenNoWait(true);
	host.Pass(100ms);
	f.fake->Deliver(1, *host.Report(TERMINAL_GATE));
	host.gates[CARD_GATE].Activate();
	f.fake->Deliver(1, *host.Report(CARD_GATE));
	Clear(f, { 1 });

	joiner.now = Clock::time_point{} + 1h;
	joiner.inWorldSince = joiner.now;  // just in from the loading screen
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	joiner.Take(Received(f, 2));
	CHECK(joiner.set == 0);  // not while the world's scripts are still loading theirs
	CHECK(joiner.pending.size() == 2);
	joiner.Pass(3s);
	joiner.Pass(3s);
	joiner.Pass(3s);
	CHECK(joiner.set == 2);
	CHECK(joiner.Open(TERMINAL_GATE));
	CHECK(joiner.Open(CARD_GATE));

	// Its own NPC closes one; the next world state (a container change missed) leaves it closed.
	joiner.gates[CARD_GATE].SetOpenNoWait(false);
	joiner.Pass(1s);
	joiner.Pass(2s);
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
	joiner.Take(Received(f, 2));
	joiner.Pass(3s);
	CHECK(!joiner.gates[CARD_GATE].isOpen);
	CHECK(joiner.set == 2);
}

TEST("shared gates: a press replayed in the friend's game and the state both arriving open the gate once")
{
	GateGame friendGame;
	friendGame.now = Clock::time_point{} + 1min;

	// A button the host pressed is pressed in the friend's game by the stand-in: it toggles the gate (opening), then
	// the host's state comes. Nothing more to do.
	friendGame.gates[TERMINAL_GATE].Activate();
	friendGame.Offer(RS{ TERMINAL_GATE, RS::kYes, RS::kUnknown, 1 });
	CHECK(friendGame.set == 0);
	friendGame.Pass(2s);
	CHECK(friendGame.Open(TERMINAL_GATE));
	CHECK(friendGame.gates[TERMINAL_GATE].animations == 1);

	// The other way round: the state first, then the replayed toggle finds the gate busy and is ignored.
	friendGame.Offer(RS{ TERMINAL_GATE, RS::kNo, RS::kUnknown, 1 });
	CHECK(friendGame.set == 1);
	friendGame.Pass(100ms);
	friendGame.gates[TERMINAL_GATE].Activate();
	friendGame.Pass(2s);
	CHECK(!friendGame.gates[TERMINAL_GATE].isOpen);

	// A gate still closing when the session says open is opened once the close has played.
	friendGame.gates[CARD_GATE].isOpen = true;
	friendGame.gates[CARD_GATE].busy = true;
	friendGame.Offer(RS{ CARD_GATE, RS::kYes, RS::kUnknown, 1 });
	CHECK(friendGame.set == 2);
	friendGame.gates[CARD_GATE].Settle();
	CHECK(friendGame.Open(CARD_GATE));
}

TEST("shared gates: a gate's state is a door's to the server: kept in the world state and relayed to the others")
{
	Fixture f{ Unhurried() };
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Protocol::Encode(RS{ CHAINED_GATE, RS::kYes, RS::kUnknown }, MT::kReportRefState));
	const auto live = Decoded(Received(f, 2), MT::kRefStateChanged, Protocol::DecodeRefState);
	REQUIRE(live.size() == 1);
	CHECK(live[0].refId == CHAINED_GATE);
	CHECK(live[0].open == RS::kYes);
	CHECK(live[0].playerId != 0);

	const auto welcome = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(welcome);
	const auto worlds = WorldStates(Received(f, 3));
	REQUIRE(worlds.size() == 1);
	const auto& states = worlds[0].refStates;
	const auto it = std::ranges::find(states, CHAINED_GATE, &RS::refId);
	REQUIRE(it != states.end());
	CHECK(it->open == RS::kYes);
	CHECK(it->playerId == 0);
}
