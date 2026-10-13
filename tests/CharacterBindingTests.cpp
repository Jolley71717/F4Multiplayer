// CharacterBinding's rules on their own (SessionLifeTests runs a real server on the files they pick).

#include "Test.h"

#include "game/CharacterBinding.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using CharacterBinding::HostStep;
using CharacterBinding::Legacy;

namespace
{
	constexpr std::uint64_t COOP = 0x00A1B2C3D4E5F601;  // the character played in co-op
	constexpr std::uint64_t SOLO = 0x00A1B2C3D4E5F602;  // another character of the same player
}

TEST("character binding: the host's game doesn't host at the main menu, in Vault 111's opening, or with bHost off")
{
	// a_wantHost, a_hosting, a_inWorld, a_inOpening, hosted, current
	CHECK(CharacterBinding::ForHost(true, false, false, false, 0, COOP) == HostStep::kNone);  // main menu
	CHECK(CharacterBinding::ForHost(true, false, true, true, 0, COOP) == HostStep::kNone);    // the opening
	CHECK(CharacterBinding::ForHost(false, false, true, false, 0, COOP) == HostStep::kNone);  // bHost off
	CHECK(CharacterBinding::ForHost(true, false, true, false, 0, COOP) == HostStep::kStart);  // out in the world
}

TEST("character binding: hosting stops when another character is loaded or a new game's opening starts, and carries on otherwise")
{
	CHECK(CharacterBinding::ForHost(true, true, true, false, COOP, COOP) == HostStep::kNone);
	CHECK(CharacterBinding::ForHost(true, true, true, false, COOP, SOLO) == HostStep::kStop);
	CHECK(CharacterBinding::ForHost(true, true, true, true, COOP, COOP) == HostStep::kStop);
	// A character with ID 0 (a coc from the main menu, many older saves) is a character like any other.
	CHECK(CharacterBinding::ForHost(true, true, true, false, 0, 0) == HostStep::kNone);
	CHECK(CharacterBinding::ForHost(true, true, true, false, 0, COOP) == HostStep::kStop);
	// Then, with the other character in the world, it starts again (with that character's file).
	CHECK(CharacterBinding::ForHost(true, false, true, false, COOP, SOLO) == HostStep::kStart);
}

TEST("character binding: the host reloading their own character's save keeps hosting; another character's save or a new game stops it")
{
	CHECK(!CharacterBinding::HostStopsOnLoad(true, false, COOP, COOP));  // a death reload, an autosave
	CHECK(CharacterBinding::HostStopsOnLoad(true, false, COOP, SOLO));
	CHECK(CharacterBinding::HostStopsOnLoad(true, true, COOP, COOP));  // New game: the ID is still the last character's
	CHECK(!CharacterBinding::HostStopsOnLoad(false, false, COOP, SOLO));  // not hosting: nothing to stop
}

TEST("character binding: a friend reloading after a death stays joined; loading their solo character leaves the session")
{
	CHECK(!CharacterBinding::JoinerLeavesOnLoad(true, false, COOP, COOP));
	CHECK(CharacterBinding::JoinerLeavesOnLoad(true, false, COOP, SOLO));
	// Starting a new character for co-op while joined is fine: its ID comes with its name, later.
	CHECK(!CharacterBinding::JoinerLeavesOnLoad(true, true, COOP, COOP));
	CHECK(!CharacterBinding::JoinerLeavesOnLoad(false, false, COOP, SOLO));  // not joined
}

TEST("character binding: each character's session has a file of its own, and the test rig's never share a name with a player's")
{
	CHECK(CharacterBinding::FileName(COOP, false) != CharacterBinding::FileName(SOLO, false));
	CHECK(CharacterBinding::FileName(COOP, false) != CharacterBinding::FileName(COOP, true));
	CHECK(CharacterBinding::FileName(0, false) != CharacterBinding::LEGACY_FILE);
	CHECK(CharacterBinding::FileName(COOP, false).ends_with(".session"));
}

TEST("character binding: the old shared session goes only to a save that was played in it")
{
	// a_characterFileExists, a_legacyReadable, legacy session, the save's session
	CHECK(CharacterBinding::ForLegacy(false, true, 0x5E55, 0x5E55) == Legacy::kAdopt);
	CHECK(CharacterBinding::ForLegacy(false, true, 0x5E55, 0x0717) == Legacy::kLeave);  // another session's save
	CHECK(CharacterBinding::ForLegacy(false, true, 0x5E55, 0) == Legacy::kLeave);       // a save never in co-op
	CHECK(CharacterBinding::ForLegacy(false, true, 0, 0) == Legacy::kLeave);            // "no session" matches nothing
	CHECK(CharacterBinding::ForLegacy(true, true, 0x5E55, 0x5E55) == Legacy::kNone);    // the character has its own
	CHECK(CharacterBinding::ForLegacy(false, false, 0, 0x5E55) == Legacy::kNone);       // unreadable: not touched
}

TEST("character binding: an old shared file that can't be read is left alone, and a character with its own file keeps it")
{
	const auto folder = std::filesystem::temp_directory_path() / std::format("f4mp-bind-{:016X}", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
	std::filesystem::create_directories(folder);
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	{
		std::ofstream garbage{ folder / CharacterBinding::LEGACY_FILE, std::ios::binary };
		garbage << "not a session";
	}
	const auto path = CharacterBinding::PrepareFile(folder, COOP, false, 0x5E55, say);
	CHECK(path == folder / CharacterBinding::FileName(COOP, false));
	CHECK(std::filesystem::exists(folder / CharacterBinding::LEGACY_FILE));
	CHECK(!std::filesystem::exists(path));
	CHECK(log.empty());

	{
		std::ofstream own{ path, std::ios::binary };
		own << "the character's own";
	}
	CHECK(CharacterBinding::PrepareFile(folder, COOP, false, 0x5E55, say) == path);
	CHECK(std::filesystem::file_size(path) == std::string_view{ "the character's own" }.size());
	CHECK(log.empty());

	std::error_code error;
	std::filesystem::remove_all(folder, error);
}
