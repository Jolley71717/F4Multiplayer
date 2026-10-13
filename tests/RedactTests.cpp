#include "Test.h"

#include "Redact.h"

#include <string>

namespace
{
	constexpr std::uint64_t STEAM_ID = 76561198012345678ull;
	constexpr std::uint64_t OTHER_ID = 76561198087654321ull;

	bool IsAliasShape(const std::string& a_alias)
	{
		if (a_alias.size() != 7 || a_alias[0] != '~') {
			return false;
		}
		for (std::size_t i = 1; i < a_alias.size(); ++i) {
			const char c = a_alias[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				return false;
			}
		}
		return true;
	}
}

TEST("Redact: an alias is a tilde and six hex digits, the same every time in one run")
{
	const auto alias = Redact::Alias(STEAM_ID);
	CHECK(IsAliasShape(alias));
	CHECK(Redact::Alias(STEAM_ID) == alias);
	CHECK(Redact::Alias(STEAM_ID, Redact::Salt()) == alias);
	CHECK(Redact::Salt() == Redact::Salt());
}

TEST("Redact: two accounts get two aliases, and another salt gives another alias")
{
	CHECK(Redact::Alias(STEAM_ID, 1) != Redact::Alias(OTHER_ID, 1));
	CHECK(Redact::Alias(STEAM_ID, 1) != Redact::Alias(STEAM_ID, 2));
	CHECK(Redact::Alias(STEAM_ID, 1) == Redact::Alias(STEAM_ID, 1));
}

TEST("Redact: an alias gives nothing of the id away")
{
	for (const std::uint64_t salt : { 0ull, 1ull, 0xDEADBEEFull }) {
		const auto alias = Redact::Alias(STEAM_ID, salt);
		CHECK(IsAliasShape(alias));
		CHECK(alias.find("5678") == std::string::npos);
		CHECK(alias.find("7656") == std::string::npos);
	}
}

TEST("Redact: Scrub replaces a SteamID64 wherever it is in the line")
{
	const auto alias = Redact::Alias(STEAM_ID);
	CHECK(Redact::Scrub("76561198012345678 joined") == alias + " joined");
	CHECK(Redact::Scrub("host is 76561198012345678") == "host is " + alias);
	CHECK(Redact::Scrub("can't connect to '76561198012345678'") == "can't connect to '" + alias + "'");
	CHECK(Redact::Scrub("Connection: connecting to steam:76561198012345678") == "Connection: connecting to steam:" + alias);
	CHECK(Redact::Scrub("(76561198012345678)") == "(" + alias + ")");
	CHECK(Redact::Scrub("76561198012345678") == alias);
	CHECK(Redact::Scrub("a 76561198012345678 and 76561198087654321") == "a " + alias + " and " + Redact::Alias(OTHER_ID));
}

TEST("Redact: Scrub leaves lobby ids, form ids, hashes and other numbers alone")
{
	for (const std::string_view line : {
			 "Steam: hosting lobby 109775241234567890",
			 "weather 0001E0F2 hash 2C42CB18",
			 "player 7 killed 0002F4C5",
			 "7656119801234567 is 16 digits",
			 "765611980123456789 is 18 digits",
			 "86561198012345678 starts wrong",
			 "F4Multiplayer v0.8.6 rc1",
			 "1.11.240.0.1 is a version",
			 "",
		 }) {
		CHECK(Redact::Scrub(line) == line);
	}
}

TEST("Redact: Scrub masks IPv4 addresses but not loopback")
{
	CHECK(Redact::Scrub("server: player 1 joined from udp 198.51.100.7:4321 (1 online)") ==
		  "server: player 1 joined from udp 198.51.100.x:4321 (1 online)");
	CHECK(Redact::Scrub("can't connect to '203.0.113.250'") == "can't connect to '203.0.113.x'");
	CHECK(Redact::Scrub("from udp 127.0.0.1:7779") == "from udp 127.0.0.1:7779");
	CHECK(Redact::Scrub("10.0.0.2 and 192.168.1.20.") == "10.0.0.x and 192.168.1.x.");
	// Not addresses: an octet over 255, too few parts, glued to a word.
	CHECK(Redact::Scrub("1.2.3.256") == "1.2.3.256");
	CHECK(Redact::Scrub("1.2.3") == "1.2.3");
	CHECK(Redact::Scrub("v1.11.240.0") == "v1.11.240.0");
}

TEST("Redact: MaskAddress masks the machine part and agrees with Scrub")
{
	CHECK(Redact::MaskAddress("198.51.100.7", 4321) == "198.51.100.x:4321");
	CHECK(Redact::MaskAddress("127.0.0.1", 7779) == "127.0.0.1:7779");
	CHECK(Redact::MaskAddress("2001:db8:85a3:8d3:1319:8a2e:370:7348", 7779) == "[2001:db8:85a3:8d3:x]:7779");
	CHECK(Redact::MaskAddress("2001:db8::1", 7779) == "[2001:db8:x]:7779");
	CHECK(Redact::MaskAddress("::1", 7779) == "[::1]:7779");
	CHECK(Redact::MaskAddress("not an address", 7779) == "?:7779");
	// Scrub gives the same, and never masks twice.
	const auto masked = Redact::MaskAddress("198.51.100.7", 4321);
	CHECK(Redact::Scrub("198.51.100.7:4321") == masked);
	CHECK(Redact::Scrub(masked) == masked);
	const auto alias = Redact::Alias(STEAM_ID);
	CHECK(Redact::Scrub("steam " + alias) == "steam " + alias);
}
