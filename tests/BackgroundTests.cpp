#include "Test.h"

#include "Background.h"
#include "game/InputGate.h"

namespace
{
	// The game's bAlwaysActive as main.cpp drives it: each frame the switch is asked, and what it says is set.
	struct Game
	{
		bool               alwaysActive = false;
		Background::Switch background;
		int                changes = 0;

		void Frame(bool a_enabled, bool a_session)
		{
			if (const auto value = background.Update(a_enabled, a_session, alwaysActive)) {
				alwaysActive = *value;
				++changes;
			}
		}

		// As main.cpp asks it: why the game runs on now, from the session's status and the developer channel.
		void Frame(bool a_enabled, const Background::Now& a_now) { Frame(a_enabled, Background::RunOn(a_now) != Background::Reason::kNone); }
	};

	constexpr Background::Now ALONE{};
	constexpr Background::Now UNDER_WAY{ .underWay = true };  // an invite or join-by-address accepted, or bHost waiting
	constexpr Background::Now JOINED{ .session = true, .underWay = true };  // a join's link stays set once it is up
	constexpr Background::Now DEV{ .devChannel = true };

	// The game's input as main.cpp's GateInput drives it, with the setting Background left: polling as the game's own
	// SetActive leaves it at a focus change (on whatever the focus with bAlwaysActive on), then the gate's step.
	struct Input
	{
		InputGate::Gate gate;
		bool            polling = true;  // BSInputDeviceManager::pollingEnabled
		int             reads = 0;       // frames the keyboard and mouse were read

		void Focus(bool a_alwaysActive, bool a_front) { polling = a_front || a_alwaysActive; }

		void Frame(bool a_alwaysActive, bool a_front)
		{
			if (const auto step = gate.Frame(a_alwaysActive, a_front, false); step.polling) {
				polling = *step.polling;
			}
			if (polling) {
				++reads;
			}
		}
	};
}

static_assert(Background::RunOn(ALONE) == Background::Reason::kNone);
static_assert(Background::RunOn(UNDER_WAY) == Background::Reason::kUnderWay);
static_assert(Background::RunOn(JOINED) == Background::Reason::kSession);
static_assert(Background::RunOn(DEV) == Background::Reason::kDevChannel);
static_assert(Background::RunOn({ .session = true, .devChannel = true }) == Background::Reason::kSession);
static_assert(Background::RunOn({ .underWay = true, .devChannel = true }) == Background::Reason::kUnderWay);

TEST("background: a join accepted at the main menu runs on from then, through the load, and the player's setting is back when it ends")
{
	Game game;
	for (int i = 0; i < 3; ++i) {
		game.Frame(true, ALONE);  // the main menu, nothing pending
	}
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 0);

	game.Frame(true, UNDER_WAY);  // an invite accepted: the save loads and the link is tried
	CHECK(game.alwaysActive);
	game.Frame(true, UNDER_WAY);
	game.Frame(true, JOINED);  // welcomed: no change, it is on already
	game.Frame(true, UNDER_WAY);  // the link dropped and is tried again: still on
	CHECK(game.alwaysActive);
	CHECK(game.changes == 1);

	game.Frame(true, ALONE);  // left
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 2);

	// A join refused and left before it was ever up puts the setting back too.
	game.Frame(true, UNDER_WAY);
	CHECK(game.alwaysActive);
	game.Frame(true, ALONE);
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 4);
}

TEST("background: with the developer channel on the game runs on from the first frame, in and out of sessions, unless bRunInBackground is off")
{
	Game game;
	game.Frame(true, DEV);  // the main menu
	CHECK(game.alwaysActive);
	game.Frame(true, { .underWay = true, .devChannel = true });
	game.Frame(true, { .session = true, .underWay = true, .devChannel = true });
	game.Frame(true, DEV);  // the session over: the rig still drives the game
	CHECK(game.alwaysActive);
	CHECK(game.changes == 1);

	// bRunInBackground off puts the player's setting back at once, the developer channel or not.
	game.Frame(false, DEV);
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 2);

	Game off;
	off.Frame(false, DEV);
	off.Frame(false, UNDER_WAY);
	CHECK(!off.alwaysActive);
	CHECK(off.changes == 0);

	// A player (or rig) with the game set to run on anyway keeps it after.
	Game on{ .alwaysActive = true };
	on.Frame(true, DEV);
	on.Frame(true, ALONE);
	CHECK(on.alwaysActive);
	CHECK(on.changes == 0);
}

TEST("background: in every case it runs on, nothing reaches the game from the keyboard or mouse while it is not in front")
{
	for (const auto& now : { UNDER_WAY, JOINED, DEV }) {
		Game  game;
		Input input;
		game.Frame(true, ALONE);
		input.Frame(game.alwaysActive, true);
		game.Frame(true, now);
		REQUIRE(game.alwaysActive);
		input.Frame(game.alwaysActive, true);
		const int before = input.reads;

		input.Focus(game.alwaysActive, false);  // clicked elsewhere: with bAlwaysActive on the game's SetActive keeps polling on
		for (int i = 0; i < 10; ++i) {
			game.Frame(true, now);
			input.Frame(game.alwaysActive, false);
			input.Focus(game.alwaysActive, false);  // a focus change the game makes meanwhile turns it back on: closed again
		}
		CHECK(input.reads == before);
		CHECK(input.gate.Now() == InputGate::Gate::State::kClosed);

		input.Focus(game.alwaysActive, true);
		game.Frame(true, now);
		input.Frame(game.alwaysActive, true);
		CHECK(input.gate.Now() == InputGate::Gate::State::kOpen);
		CHECK(input.reads == before + 1);
	}

	// Alone at the main menu, nothing pending: the game pauses and stops its input itself, as it always did.
	Game  game;
	Input input;
	game.Frame(true, ALONE);
	input.Focus(game.alwaysActive, false);
	input.Frame(game.alwaysActive, false);
	CHECK(!input.polling);
	CHECK(input.gate.closes == 0);
}

TEST("background: playing alone, alt-tabbing pauses the game as it always did; in a session it runs on, and after it the player's setting is back")
{
	Game game;
	for (int i = 0; i < 3; ++i) {
		game.Frame(true, false);
	}
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 0);

	game.Frame(true, true);  // hosting, or joined
	CHECK(game.alwaysActive);
	game.Frame(true, true);
	CHECK(game.changes == 1);

	game.Frame(true, false);  // left, or stopped hosting
	CHECK(!game.alwaysActive);
	CHECK(game.changes == 2);

	// The next session turns it on again.
	game.Frame(true, true);
	CHECK(game.alwaysActive);
}

TEST("background: a player who has the game run on anyway keeps it on after a session, and bRunInBackground off changes nothing")
{
	Game game{ .alwaysActive = true };
	game.Frame(true, true);
	game.Frame(true, false);
	CHECK(game.alwaysActive);
	CHECK(game.changes == 0);

	Game off;
	off.Frame(false, true);
	off.Frame(false, false);
	CHECK(!off.alwaysActive);
	CHECK(off.changes == 0);
}

TEST("background: turning bRunInBackground off during a session puts the player's setting back at once")
{
	Game game;
	game.Frame(true, true);
	CHECK(game.alwaysActive);
	game.Frame(false, true);
	CHECK(!game.alwaysActive);
	game.Frame(false, false);
	CHECK(game.changes == 2);
}
