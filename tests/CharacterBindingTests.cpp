// CharacterBinding's rules on their own (CharacterSessionTests plays them against a real server).

#include "Test.h"

#include "game/CharacterBinding.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using CharacterBinding::Legacy;
using CharacterBinding::Step;

namespace
{
	constexpr std::uint64_t COOP = 0x00A1B2C3D4E5F601;  // the character played in co-op
	constexpr std::uint64_t SOLO = 0x00A1B2C3D4E5F602;  // another character of the same player
	constexpr std::uint64_t NEW = 0x00A1B2C3D4E5F603;   // one started later

	constexpr CharacterBinding::Game InWorld(std::uint64_t a_character) { return { a_character, true, false }; }
	constexpr CharacterBinding::Game InOpening(std::uint64_t a_character) { return { a_character, true, true }; }
}

TEST("character binding: with bHost, hosting waits for the world and the end of the opening, never the main menu")
{
	CharacterBinding::Tracker tracker;
	const CharacterBinding::Link waiting{ true, false, false };
	CHECK(tracker.Check(waiting, { COOP, false, false }) == Step::kNone);  // the main menu: a friend must not be first
	CHECK(tracker.Check(waiting, InOpening(COOP)) == Step::kNone);
	CHECK(tracker.Check({ false, false, false }, InWorld(COOP)) == Step::kNone);  // bHost off
	CHECK(tracker.Check(waiting, InWorld(COOP)) == Step::kStartHosting);
}

TEST("character binding: a character with ID 0 (a coc from the main menu, many older saves) is hosted like any other")
{
	CharacterBinding::Tracker tracker;
	tracker.OnHostingStarted(0);
	const CharacterBinding::Link hosting{ true, true, false };
	CHECK(tracker.Check(hosting, InWorld(0)) == Step::kNone);
	CHECK(tracker.OnLoad(hosting, false, 0) == Step::kNone);
	CHECK(tracker.Check(hosting, InWorld(COOP)) == Step::kStopHosting);
}

TEST("character binding: a friend who joined and then reconnects stays the character that joined, until another server is chosen")
{
	CharacterBinding::Tracker   tracker;
	const CharacterBinding::Link joining{ false, false, true };
	tracker.OnHello(COOP);
	tracker.OnHello(SOLO);  // a reconnect's hello can't change who joined (a load would have left first)
	CHECK(tracker.OnLoad(joining, false, SOLO) == Step::kLeave);
	CHECK(tracker.OnLoad(joining, false, COOP) == Step::kNone);
	CHECK(tracker.OnLoad({ false, false, false }, false, SOLO) == Step::kNone);  // not joining: nothing to leave

	tracker.OnServerChanged();  // the menu's Join, with the solo character loaded
	CHECK(tracker.OnLoad(joining, false, NEW) == Step::kNone);  // no hello yet: nobody has joined
	tracker.OnHello(SOLO);
	CHECK(tracker.OnLoad(joining, false, COOP) == Step::kLeave);
}

TEST("character binding: a welcome back into the session we joined is taken; into another one it isn't, unless we chose that server again")
{
	CharacterBinding::Tracker tracker;
	CHECK(tracker.OnWelcome(false, 0x5E55) == Step::kNone);
	CHECK(tracker.OnWelcome(false, 0x5E55) == Step::kNone);
	CHECK(tracker.OnWelcome(false, 0x0717) == Step::kLeave);
	tracker.OnServerChanged();
	CHECK(tracker.OnWelcome(false, 0x0717) == Step::kNone);
	CHECK(tracker.OnWelcome(true, 0x0B0B) == Step::kNone);  // our own server: whatever it holds is ours
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

	// The test rig (dev channel) keeps files of its own, and never looks at the old one.
	CHECK(CharacterBinding::PrepareFile(folder, SOLO, true, 0x5E55, say) == folder / CharacterBinding::FileName(SOLO, true));
	CHECK(log.empty());

	std::error_code error;
	std::filesystem::remove_all(folder, error);
}

TEST("character binding: a quickload of the same character is the same character, whatever high half the game gives its ID")
{
	// The rig's host: a coc character from the main menu (0 in memory) loaded its own quicksave
	// (Quicksave0_00000000M_...) and the game said 0000000100000000. A character first played before any mod is
	// 0x0000000012345678 in memory and 0x0000000112345678 from its saves made since ('M').
	CHECK(CharacterBinding::Character(0x0000000100000000) == CharacterBinding::Character(0));
	CHECK(CharacterBinding::Character(0x0000000112345678) == 0x12345678);
	CHECK(CharacterBinding::Character(0x0000000012345678) == 0x12345678);
	CHECK(CharacterBinding::Character(0x0000000112345678) != CharacterBinding::Character(0x0000000112345679));

	CharacterBinding::Tracker tracker;
	const CharacterBinding::Link hosting{ true, true, false };
	tracker.OnHostingStarted(CharacterBinding::Character(0));
	CHECK(tracker.OnLoad(hosting, false, CharacterBinding::Character(0x0000000100000000)) == Step::kNone);
	CHECK(tracker.Check(hosting, InWorld(CharacterBinding::Character(0x0000000100000000))) == Step::kNone);
	CHECK(tracker.OnLoad(hosting, false, CharacterBinding::Character(0x00000001D8F04348)) == Step::kStopHosting);  // another character

	// A friend joined with the character, then quickloads it: still in the session.
	CharacterBinding::Tracker   friendTracker;
	const CharacterBinding::Link joining{ false, false, true };
	friendTracker.OnHello(CharacterBinding::Character(0x0000000012345678));
	CHECK(friendTracker.OnLoad(joining, false, CharacterBinding::Character(0x0000000112345678)) == Step::kNone);
	CHECK(friendTracker.OnLoad(joining, false, CharacterBinding::Character(0x0000000187654321)) == Step::kLeave);
}

TEST("character binding: a session file named after the whole ID (up to 0.9.0-rc2) becomes the character's file")
{
	const auto folder = std::filesystem::temp_directory_path() / std::format("f4mp-bind-{:016X}", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
	std::filesystem::create_directories(folder);
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	const auto               write = [&](const std::string& a_name, std::string_view a_text) {
		std::ofstream out{ folder / a_name, std::ios::binary };
		out << a_text;
	};
	constexpr std::uint64_t CHARACTER = 0xD8F04348;
	write(CharacterBinding::FileName(0x00000001D8F04348, false), "the co-op session");
	write(CharacterBinding::FileName(0x00000001D8F04349, false), "another character's");
	write(CharacterBinding::FileName(0x00000001D8F04348, true), "the rig's");

	const auto path = CharacterBinding::PrepareFile(folder, CHARACTER, false, 0, say);
	CHECK(path == folder / CharacterBinding::FileName(CHARACTER, false));
	CHECK(std::filesystem::exists(path) && std::filesystem::file_size(path) == std::string_view{ "the co-op session" }.size());
	CHECK(!std::filesystem::exists(folder / CharacterBinding::FileName(0x00000001D8F04348, false)));
	CHECK(std::filesystem::exists(folder / CharacterBinding::FileName(0x00000001D8F04349, false)));  // not this character's
	CHECK(std::filesystem::exists(folder / CharacterBinding::FileName(0x00000001D8F04348, true)));   // the rig's stay apart
	CHECK(log.size() == 1);

	// Once it has its own file, an older one under a whole ID is left alone.
	write(CharacterBinding::FileName(0x00000002D8F04348, false), "older");
	CHECK(CharacterBinding::PrepareFile(folder, CHARACTER, false, 0, say) == path);
	CHECK(std::filesystem::file_size(path) == std::string_view{ "the co-op session" }.size());
	CHECK(log.size() == 1);

	// The rig's files move the same way, among its own.
	CHECK(CharacterBinding::PrepareFile(folder, CHARACTER, true, 0, say) == folder / CharacterBinding::FileName(CHARACTER, true));
	CHECK(std::filesystem::exists(folder / CharacterBinding::FileName(CHARACTER, true)));
	CHECK(log.size() == 2);

	// The ID 0 (a coc character): its quickload's file (0000000100000000) is taken back too.
	write(CharacterBinding::FileName(0x0000000100000000, true), "the rig's coc character");
	CHECK(CharacterBinding::PrepareFile(folder, 0, true, 0, say) == folder / CharacterBinding::FileName(0, true));
	CHECK(std::filesystem::exists(folder / CharacterBinding::FileName(0, true)));

	std::error_code error;
	std::filesystem::remove_all(folder, error);
}
