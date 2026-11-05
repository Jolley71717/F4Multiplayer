#include "Test.h"

#include "game/UiDevHudText.h"

#include <string>

TEST("dev hud: says the link, ping and frame numbers")
{
	UiDevHudText::Facts facts;
	facts.link = UiDevHudText::Link::kJoined;
	facts.steam = true;
	facts.linkDetail = "1.2.x.x:7779";
	facts.friends = { { .id = 2, .metres = 84.4f }, { .id = 3 }, { .id = 4, .unreachable = true } };
	facts.roundTripMs = 48;
	facts.fps = 59.6f;
	facts.frameMeanMs = 16.7f;
	facts.frameWorstMs = 41.0f;
	facts.hitches = 1;
	facts.oursP50Ms = 0.25f;
	facts.oursP99Ms = 1.5f;
	facts.location = "Sanctuary Hills";
	facts.cell = 0x3C;
	facts.x = -512.4f;
	facts.y = 2204.0f;
	facts.update = 7;
	CHECK(UiDevHudText::Format(facts) ==
		  "JOINED steam  1.2.x.x:7779  |  ping 48 ms  |  players 4\n"
		  "player 2  84 m\n"
		  "player 3  elsewhere\n"
		  "player 4  unreachable\n"
		  "60 fps  |  frame 16.7 / 41 ms  |  hitches 1\n"
		  "mod 0.25 / 1.50 ms  |  update 7\n"
		  "Sanctuary Hills 0000003C  (-512, 2204)");
}

TEST("dev hud: a ping not measured yet and no location show as a dash")
{
	const auto text = UiDevHudText::Format({});
	CHECK(text.find("DORMANT") == 0);
	CHECK(text.find("ping -  |  players 1\n") != std::string::npos);
	CHECK(text.find("\n- 00000000") != std::string::npos);
}
