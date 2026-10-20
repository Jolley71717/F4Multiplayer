// No input while the game's window is not in front, with the game running on (src/game/InputGate.h), driven
// against a model of the game's input manager: its polling switch, which the game's own SetActive turns on at
// every focus change while bAlwaysActive is on, and the mouse state its window procedure latches from raw input.

#include "Test.h"

#include "game/InputGate.h"

namespace
{
	struct Game
	{
		InputGate::Gate gate;
		bool            alwaysActive = true;  // bRunInBackground in a session
		bool            front = true;
		bool            polling = true;   // BSInputDeviceManager::pollingEnabled
		bool            latched = false;  // the mouse device's button state, set by a click's raw input
		int             clears = 0;
		int             attacks = 0;      // what the player's controls read

		// The window goes out of front or comes back: the game's Main::SetActive.
		void Focus(bool a_front)
		{
			front = a_front;
			polling = a_front || alwaysActive;
		}

		// One frame as main.cpp runs it, then the game's own input poll.
		void Frame(bool a_buttonDown)
		{
			const auto step = gate.Frame(alwaysActive, front, a_buttonDown);
			if (step.polling) {
				polling = *step.polling;
			}
			if (step.clear) {
				latched = false;
				++clears;
			}
			if (polling && latched) {
				++attacks;
			}
		}

		// A click: the raw input reaches the game's window procedure and latches the button, gate or not.
		void Click() { latched = true; }
	};
}

TEST("input gate: in front, the game reads its input as it always did")
{
	Game game;
	for (int i = 0; i < 10; ++i) {
		game.Frame(false);
	}
	CHECK(game.polling);
	CHECK(game.clears == 0);
	game.Click();
	game.Frame(true);
	CHECK(game.attacks == 1);
	CHECK(game.gate.closes == 0);
}

TEST("input gate: alt-tabbed with the game running on, clicks in another window never reach the player's controls")
{
	Game game;
	game.Frame(false);
	game.Focus(false);  // alt-tab: SetActive keeps polling on (bAlwaysActive)
	CHECK(game.polling);
	game.Frame(false);
	CHECK(!game.polling);
	CHECK(game.gate.Now() == InputGate::Gate::State::kClosed);
	CHECK(game.clears == 1);
	for (int i = 0; i < 20; ++i) {
		game.Click();
		game.Frame(i % 2 == 0);
	}
	CHECK(game.attacks == 0);
	CHECK(game.gate.closes == 1);
}

TEST("input gate: a focus change the game makes while out of front does not turn input back on")
{
	Game game;
	game.Focus(false);
	game.Frame(false);
	game.polling = true;  // the game's SetActive again (bAlwaysActive), e.g. another window activated and gone
	game.Click();
	game.Frame(false);
	CHECK(!game.polling);
	CHECK(game.attacks == 0);
}

TEST("input gate: back in front by keyboard, input goes again with nothing from meanwhile pressed")
{
	Game game;
	game.Focus(false);
	game.Frame(false);
	game.Click();  // latched while away
	game.Focus(true);
	game.Frame(false);
	CHECK(game.polling);
	CHECK(game.gate.Now() == InputGate::Gate::State::kOpen);
	CHECK(game.attacks == 0);
	CHECK(game.clears == 2);  // as it closed and as it opened
	game.Click();
	game.Frame(true);
	CHECK(game.attacks == 1);
}

TEST("input gate: the click that brings the game back is kept from it until the button is up")
{
	Game game;
	game.Focus(false);
	game.Frame(false);
	game.Focus(true);  // clicked on the game's window
	game.Click();
	for (int i = 0; i < 5; ++i) {
		game.Frame(true);  // still held
	}
	CHECK(game.gate.Now() == InputGate::Gate::State::kWaitRelease);
	CHECK(!game.polling);
	CHECK(game.attacks == 0);
	game.Frame(false);  // released
	CHECK(game.gate.Now() == InputGate::Gate::State::kOpen);
	CHECK(game.polling);
	CHECK(game.attacks == 0);
	CHECK(game.gate.swallowedClicks == 1);
	game.Click();
	game.Frame(true);
	CHECK(game.attacks == 1);  // the next click is the player's
}

TEST("input gate: out of front again while waiting for the release closes it as before")
{
	Game game;
	game.Focus(false);
	game.Frame(false);
	game.Focus(true);
	game.Frame(true);
	game.Focus(false);
	game.Frame(true);
	CHECK(game.gate.Now() == InputGate::Gate::State::kClosed);
	CHECK(!game.polling);
	game.Focus(true);
	game.Frame(false);
	CHECK(game.gate.Now() == InputGate::Gate::State::kOpen);
	CHECK(game.gate.swallowedClicks == 1);
}

TEST("input gate: playing alone (bAlwaysActive off) the game pauses and stops its input itself; the gate does nothing")
{
	Game game;
	game.alwaysActive = false;
	game.Focus(false);
	game.Frame(false);
	CHECK(!game.polling);  // the game's own
	CHECK(game.clears == 0);
	CHECK(game.gate.closes == 0);
	game.Focus(true);
	game.Frame(true);
	CHECK(game.polling);
	CHECK(game.gate.swallowedClicks == 0);
}

TEST("input gate: the session ending while alt-tabbed hands input back to the game's own handling")
{
	Game game;
	game.Focus(false);
	game.Frame(false);
	CHECK(!game.polling);
	game.alwaysActive = false;  // out of the session: Background puts the player's setting back
	game.Frame(false);
	CHECK(game.gate.Now() == InputGate::Gate::State::kOpen);
	CHECK(game.polling);  // as the game left it at the last focus change
	game.Focus(true);
	game.Frame(false);
	CHECK(game.polling);
}

TEST("input gate: it says what it is doing")
{
	InputGate::Gate gate;
	CHECK(gate.Describe() == "input gate: open (closed 0 time(s), 0 click(s) bringing the game back kept from it)");
	gate.Frame(true, false, false);
	CHECK(gate.Describe() == "input gate: closed (closed 1 time(s), 0 click(s) bringing the game back kept from it)");
	gate.Frame(true, true, true);
	CHECK(gate.Describe() == "input gate: waiting for the click's release (closed 1 time(s), 1 click(s) bringing the game back kept from it)");
}
