#include "Test.h"

#include "game/VoiceSteps.h"

#include <array>
#include <string>

using namespace VoiceSteps;

TEST("voice steps: volume and range step round, and a value from the ini joins the steps")
{
	CHECK(Next(VOLUMES, 100u) == 125u);
	CHECK(Next(VOLUMES, 150u) == 25u);
	CHECK(Next(VOLUMES, 90u) == 100u);
	CHECK(Next(VOLUMES, 999u) == 25u);
	CHECK(Next(RANGES, 60.0f) == 120.0f);
	CHECK(Next(RANGES, 120.0f) == 0.0f);
	CHECK(Next(RANGES, 0.0f) == 30.0f);
	CHECK(Next(RANGES, 45.0f) == 60.0f);
}

TEST("voice steps: the output device steps from the default through the names and back; one gone goes to the default")
{
	const std::array<std::string, 2> names{ "Speakers", "Headset" };
	CHECK(NextOutput(names, "") == "Speakers");
	CHECK(NextOutput(names, "Speakers") == "Headset");
	CHECK(NextOutput(names, "Headset") == "");
	CHECK(NextOutput(names, "Unplugged") == "");
	CHECK(NextOutput({}, "") == "");
}

TEST("voice steps: louder samples stop at full scale instead of wrapping round")
{
	CHECK(Scaled(1000, 0.5f) == 500);
	CHECK(Scaled(30000, 1.5f) == 32767);
	CHECK(Scaled(-30000, 1.5f) == -32768);
}

TEST("voice steps: the level bar runs from -60 dB to 0 dB")
{
	CHECK(Bar(-120.0f).empty());
	CHECK(Bar(-60.0f).empty());
	CHECK(Bar(-30.0f, 24).size() == 12);
	CHECK(Bar(0.0f, 24).size() == 24);
	CHECK(Bar(6.0f, 24).size() == 24);
}

TEST("voice steps: a device's name comes over to UTF-8")
{
	CHECK(Utf8(u"Speakers") == "Speakers");
	CHECK(Utf8(u"Haut-parleurs é") == "Haut-parleurs \xC3\xA9");
	CHECK(Utf8(u"マイク") == "\xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF");
	CHECK(Utf8(u"\U0001F3A4") == "\xF0\x9F\x8E\xA4");
}
