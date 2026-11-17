#include "Test.h"

#include "game/UiDevHudText.h"

#include <string>

namespace
{
	UiDevHudText::Facts Joined()
	{
		UiDevHudText::Facts facts;
		facts.link = UiDevHudText::Link::kJoined;
		facts.linkDetail = "127.0.x.x:7779";
		facts.roundTripMs = 9;
		facts.version = "0.20.0";
		facts.protocol = 84;
		facts.location = "Sanctuary Hills";
		facts.cell = 0xDD60;
		facts.x = -80352.4f;
		facts.y = 89600.0f;
		facts.players = {
			{ .id = 3, .me = true, .age = 0.02f, .menu = "talking" },
			{ .id = 1, .host = true, .metres = 12.2f, .age = 0.1f },
			{ .id = 5, .metres = 8.0f, .age = 1.4f, .menu = "pipboy" },
			{ .id = 6, .age = 6.0f, .menu = "crafting" },
			{ .id = 7, .unreachable = true, .age = 75.0f },
		};
		facts.fps = 59.6f;
		facts.frameMeanMs = 16.7f;
		facts.frameWorstMs = 33.0f;
		facts.hitches = 3;
		facts.lastHitch = 12.0f;
		facts.oursP50Ms = 0.12f;
		facts.oursP99Ms = 0.4f;
		return facts;
	}
}

TEST("dev hud: the link, where we are, a row for each player and the frame numbers")
{
	CHECK(UiDevHudText::Format(Joined()).text ==
		  "JOINED 127.0.x.x:7779 (ping 9ms) \xE2\x80\x93 F4MP v0.20.0 p84\n"
		  "Sanctuary Hills 0000DD60 (-80352, 89600)\n"
		  "3 (me)               0.0s  @talking\n"
		  "1 (host)      12m    0.1s\n"
		  "5             8m     1.4s  @pipboy\n"
		  "6             away   6.0s  @crafting\n"
		  "7             ??     75s\n"
		  "[60 fps \xE2\x80\x93 16.7/33ms \xE2\x80\x93 3 hitches (12s ago) \xE2\x80\x93 f4mp 0.12/0.40ms]");
}

TEST("dev hud: each row says its line and where its age is, and how old that is")
{
	const auto shown = UiDevHudText::Format(Joined());
	CHECK(shown.rows.size() == 5);
	// Flash counts the en dash as one character: the offsets are characters, not bytes.
	const auto& host = shown.rows[1];
	CHECK(host.id == 1 && host.line == 3);
	CHECK(host.ageTo - host.ageFrom == 4);
	CHECK(host.ageLevel == 0);
	CHECK(shown.rows[2].ageLevel == 1);
	CHECK(shown.rows[3].ageLevel == 2);
	// The first row's age starts 21 characters into its line, after two lines that hold one en dash between them.
	const auto lineStart = UiDevHudText::Characters(shown.text.substr(0, shown.text.find("3 (me)")));
	CHECK(shown.rows[0].ageFrom == lineStart + 21);
	CHECK(lineStart == shown.text.find("3 (me)") - 2);  // the dash's three bytes are one character
}

TEST("dev hud: none of the numbers yet, no session and no hitches")
{
	const auto text = UiDevHudText::Format({}).text;
	CHECK(text.find("DORMANT (ping -) ") == 0);
	CHECK(text.find("F4MP v? p0\n- 00000000 (0, 0)\n[") != std::string::npos);
	CHECK(text.find("0 hitches \xE2\x80\x93") != std::string::npos);
	CHECK(text.find("ago") == std::string::npos);
}

TEST("dev hud: one hitch, a dedicated server, the version as About shows it")
{
	auto facts = Joined();
	facts.hitches = 1;
	facts.lastHitch = 4.0f;
	facts.dedicated = true;
	const auto text = UiDevHudText::Format(facts).text;
	CHECK(text.find("JOINED 127.0.x.x:7779 dedicated (ping 9ms)") == 0);
	CHECK(text.find("\xE2\x80\x93 1 hitch (4.0s ago) \xE2\x80\x93") != std::string::npos);
	CHECK(UiDevHudText::ShortVersion("0.20.0.0") == "0.20.0");
	CHECK(UiDevHudText::ShortVersion("0.20.1") == "0.20.1");
	facts.lastHitch = 130.0f;
	CHECK(UiDevHudText::Format(facts).text.find("1 hitch (2 min ago)") != std::string::npos);
}
