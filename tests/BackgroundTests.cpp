#include "Test.h"

#include "Background.h"

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
	};
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
