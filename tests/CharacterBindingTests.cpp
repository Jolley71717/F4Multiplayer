// CharacterBinding's rules on their own (CharacterSessionTests plays them against a real server).

#include "Test.h"

#include "game/CharacterBinding.h"
#include "game/Story.h"

#include <chrono>
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

TEST("character binding: unnamed only after a New Game, until the ID changes or any save loads")
{
	CharacterBinding::Tracker   tracker;
	const CharacterBinding::Link alone{ false, false, false };
	CHECK(!tracker.Unnamed(COOP));
	CHECK(!tracker.UnnamedId());

	// New Game: the ID is still the last character's until the name is confirmed.
	CHECK(tracker.OnLoad(alone, true, COOP) == Step::kNone);
	CHECK(tracker.Unnamed(COOP));
	CHECK(!tracker.Unnamed(NEW));
	CHECK(tracker.Check(alone, { COOP, true, false }) == Step::kNone);
	CHECK(tracker.Unnamed(COOP));
	// At the main menu nothing is named: an ID seen there is not taken.
	CHECK(tracker.Check(alone, { NEW, false, false }) == Step::kNone);
	CHECK(tracker.UnnamedId() == COOP);
	// Named, wherever the player is.
	CHECK(tracker.Check(alone, InWorld(NEW)) == Step::kNone);
	CHECK(!tracker.Unnamed(NEW));
	CHECK(!tracker.Unnamed(COOP));
	CHECK(!tracker.UnnamedId());

	// Any load ends it: the save's ID is its character's, even the same ID.
	tracker.OnLoad(alone, true, NEW);
	CHECK(tracker.Unnamed(NEW));
	tracker.OnLoad(alone, false, NEW);
	CHECK(!tracker.Unnamed(NEW));
	CHECK(!tracker.UnnamedId());
}

TEST("character binding: a new character named outside the opening is the one that joined, whether it joined before the New Game or after")
{
	const CharacterBinding::Link joining{ false, false, true };
	// Joined with the co-op character, then a New Game with an alternate start.
	CharacterBinding::Tracker before;
	before.OnHello(COOP);
	CHECK(before.OnLoad(joining, true, COOP) == Step::kNone);
	CHECK(before.Check(joining, InWorld(COOP)) == Step::kNone);
	CHECK(before.Check(joining, InWorld(NEW)) == Step::kNone);  // named in the mod's room
	CHECK(before.OnLoad(joining, false, NEW) == Step::kNone);   // its own save
	CHECK(before.OnLoad(joining, false, COOP) == Step::kLeave);

	// Joined after the New Game, before naming: the hello said the last character's ID.
	CharacterBinding::Tracker after;
	CHECK(after.OnLoad({ false, false, false }, true, COOP) == Step::kNone);
	after.OnHello(COOP);
	CHECK(after.Check(joining, InWorld(NEW)) == Step::kNone);
	CHECK(after.OnLoad(joining, false, NEW) == Step::kNone);
	CHECK(after.OnLoad(joining, false, SOLO) == Step::kLeave);
}

TEST("character binding: a load's ID change is never taken for naming")
{
	const CharacterBinding::Link joining{ false, false, true };
	CharacterBinding::Tracker   tracker;
	tracker.OnHello(COOP);
	CHECK(tracker.OnLoad(joining, true, COOP) == Step::kNone);  // New Game
	CHECK(tracker.OnLoad(joining, false, SOLO) == Step::kLeave);  // then the solo save, before naming
	CHECK(tracker.Check(joining, InWorld(SOLO)) == Step::kNone);
	CHECK(!tracker.UnnamedId());
	CHECK(tracker.OnLoad(joining, false, SOLO) == Step::kLeave);  // still the co-op character that joined
	CHECK(tracker.OnLoad(joining, false, COOP) == Step::kNone);
	// Outside the opening, with no New Game, a changed ID rebinds nothing either.
	CHECK(tracker.Check(joining, InWorld(NEW)) == Step::kNone);
	CHECK(tracker.OnLoad(joining, false, NEW) == Step::kLeave);
}

TEST("character binding: a New Game that reaches Vault 111's opening is held only as it always was, whenever the game gives the ID")
{
	CharacterBinding::Tracker   tracker;
	const CharacterBinding::Link waiting{ true, false, false };
	// Were the new ID given before the new game's message, naming would change nothing: the vault ends the hold.
	tracker.OnLoad(waiting, true, NEW);
	CHECK(tracker.Unnamed(NEW));
	tracker.OnVaultOpening();
	CHECK(!tracker.Unnamed(NEW));
	CHECK(tracker.Check(waiting, InOpening(NEW)) == Step::kNone);
	CHECK(tracker.Check(waiting, InWorld(NEW)) == Step::kStartHosting);  // out of the vault: as before

	// The next new game is its own.
	tracker.OnLoad(waiting, true, NEW);
	CHECK(tracker.Unnamed(NEW));
}

TEST("character binding: only the game's own Vault 111 start is a vault start; an unnamed alternate start in the vault is held for its name")
{
	using Story::Opening;
	// The game's own start (Out of Time running; the pre-war world alone is not, as an alternate start may make the
	// character at its mirror and move on unnamed): the vault holds it, named or not, and it ends
	// the unnamed hold.
	CHECK(Story::Decide(true, true, true).opening == Opening::kVault);
	CHECK(Story::Decide(true, true, true).vaultStart);
	CHECK(Story::Decide(true, true, false).opening == Opening::kVault);
	CHECK(Story::Decide(true, true, false).vaultStart);
	// An alternate start's walk into the vault's places: never a vault start (its unnamed hold would end for good).
	CHECK(!Story::Decide(true, false, true).vaultStart);
	CHECK(Story::Decide(true, false, true).opening == Opening::kUnnamed);
	CHECK(!Story::Decide(true, false, false).vaultStart);
	CHECK(Story::Decide(true, false, false).opening == Opening::kVault);  // named: paused there, as before
	// Elsewhere.
	CHECK(Story::Decide(false, false, true).opening == Opening::kUnnamed);
	CHECK(Story::Decide(false, false, false).opening == Opening::kNone);
	CHECK(!Story::Decide(false, true, true).vaultStart);  // the start's quest with the player out of its places
}

TEST("character binding: an unnamed alternate start that walks through Vault 111 is still unnamed once out, as Story decides it")
{
	CharacterBinding::Tracker    tracker;
	const CharacterBinding::Link waiting{ true, false, false };
	tracker.OnLoad(waiting, true, COOP);  // New Game with an alternate start: the last character's ID
	// Session's frame: Story's hold, then the vault start passed on.
	const auto frame = [&](bool a_vaultPlaces, bool a_ownStart) {
		const auto hold = Story::Decide(a_vaultPlaces, a_ownStart, tracker.Unnamed(COOP));
		if (hold.vaultStart) {
			tracker.OnVaultOpening();
		}
		return tracker.Check(waiting, { COOP, true, hold.opening != Story::Opening::kNone });
	};
	CHECK(frame(false, false) == Step::kNone);
	CHECK(frame(true, false) == Step::kNone);  // in the vault
	CHECK(tracker.Unnamed(COOP));
	CHECK(frame(false, false) == Step::kNone);  // out: still held, no hosting under the last character's ID
	CHECK(tracker.Unnamed(COOP));
	CHECK(frame(false, false) == Step::kNone);
}

TEST("character binding: a load whose end never comes is never taken for naming")
{
	const CharacterBinding::Link joining{ false, false, true };
	CharacterBinding::Tracker   tracker;
	tracker.OnHello(COOP);
	CHECK(tracker.OnLoad(joining, true, COOP) == Step::kNone);  // New Game
	tracker.OnLoadStarting();  // the solo save, before naming; frames go on after the hold's limit with no OnLoad
	CHECK(!tracker.Unnamed(COOP));
	CHECK(tracker.Check(joining, InWorld(SOLO)) == Step::kNone);
	CHECK(tracker.OnLoad(joining, false, SOLO) == Step::kLeave);  // still the co-op character that joined
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

TEST("character binding: of two files under whole IDs, the one of the session the loaded save was played in becomes the character's, however old")
{
	const auto folder = std::filesystem::temp_directory_path() / std::format("f4mp-bind-{:016X}", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
	std::filesystem::create_directories(folder);
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	const auto               save = [&](std::uint64_t a_wholeId, std::uint64_t a_session) {
		SessionFile::Snapshot snapshot;
		snapshot.sessionId = a_session;
		REQUIRE(SessionFile::Save(folder / CharacterBinding::FileName(a_wholeId, false), snapshot));
	};
	// 0.9.0 hosted the character under one whole ID (the friends' progress, session 5E55); a quickload restarted
	// hosting under another, into a fresh file (session 7777), written last.
	save(0x00000001D8F04348, 0x5E55);
	save(0x00000002D8F04348, 0x7777);
	const auto now = std::filesystem::file_time_type::clock::now();
	std::filesystem::last_write_time(folder / CharacterBinding::FileName(0x00000001D8F04348, false), now - std::chrono::hours{ 2 });
	std::filesystem::last_write_time(folder / CharacterBinding::FileName(0x00000002D8F04348, false), now);

	// The loaded save names 5E55: that one is the character's.
	auto whole = CharacterBinding::FileUnderWholeId(folder, 0xD8F04348, false, 0x5E55);
	REQUIRE(whole);
	CHECK(whole->filename() == CharacterBinding::FileName(0x00000001D8F04348, false));
	// A save with no record, or one of neither: the newest, as before.
	whole = CharacterBinding::FileUnderWholeId(folder, 0xD8F04348, false, 0);
	REQUIRE(whole);
	CHECK(whole->filename() == CharacterBinding::FileName(0x00000002D8F04348, false));
	whole = CharacterBinding::FileUnderWholeId(folder, 0xD8F04348, false, 0x1234);
	REQUIRE(whole);
	CHECK(whole->filename() == CharacterBinding::FileName(0x00000002D8F04348, false));

	// PrepareFile goes by the save's: the older file is the one renamed.
	const auto path = CharacterBinding::PrepareFile(folder, 0xD8F04348, false, 0x5E55, say);
	const auto kept = SessionFile::Load(path);
	REQUIRE(kept);
	CHECK(kept->sessionId == 0x5E55);
	CHECK(std::filesystem::exists(folder / CharacterBinding::FileName(0x00000002D8F04348, false)));

	std::error_code error;
	std::filesystem::remove_all(folder, error);
}

// ---- whose world it is (CharacterBinding::World: the key a drop record says, common/DropStore.h) ----------------

namespace
{
	// A world's key as the co-save keeps it ('CHAR').
	std::vector<std::uint8_t> SavedKey(CharacterBinding::World& a_world) { return a_world.Save(0x5151); }

	// a_world after loading a save whose co-save holds a_saved (empty: a save from before keys), of character a_character.
	std::uint64_t AfterLoad(CharacterBinding::World& a_world, const std::vector<std::uint8_t>& a_saved, std::uint64_t a_character, std::uint64_t a_random)
	{
		a_world.Revert();
		a_world.Loaded(a_saved);
		a_world.Started(false, a_character, a_random);
		return a_world.Key(0x7777);
	}
}

TEST("world key: two characters with the engine's same ID (coc from the main menu: 0) are two worlds; each keeps its key in its saves")
{
	CharacterBinding::World first;
	const auto              a = first.Key(0x1234);  // no load and no new game: given the first time it is asked
	CHECK(a != 0);
	CHECK(first.Key(0x9999) == a);  // and kept
	const auto saveOfFirst = SavedKey(first);

	// The game restarted, a fresh coc character: the same ID, nothing loaded.
	CharacterBinding::World second;
	const auto              b = second.Key(0x4321);
	CHECK(b != 0);
	CHECK(b != a);

	// The first character's save, loaded in either game, is the first character's world again.
	CHECK(AfterLoad(second, saveOfFirst, 0, 0x5555) == a);
	CHECK(AfterLoad(first, saveOfFirst, 0, 0x6666) == a);
}

TEST("world key: a new game is a world of its own, even with the last character's ID; its saves load back as it")
{
	CharacterBinding::World world;
	world.Started(false, COOP, 0x1111);  // a save of COOP from before keys
	const auto coop = world.Key(0);
	world.Revert();
	world.Started(true, COOP, 0x2222);  // a new game: the ID is still COOP's until the name is confirmed
	const auto fresh = world.Key(0);
	CHECK(fresh != coop);
	CHECK(fresh != 0);
	const auto save = SavedKey(world);
	CHECK(AfterLoad(world, save, NEW, 0x3333) == fresh);  // its saves have the ID it was named with by then
}

TEST("world key: saves from before keys are their character's world, the same for all of them, and never a fresh key")
{
	CharacterBinding::World world;
	const auto              coop = AfterLoad(world, {}, COOP, 0x1);
	CHECK(coop == CharacterBinding::World::FromCharacter(COOP));
	CHECK(AfterLoad(world, {}, COOP | 0x0000000100000000ull, 0x2) == coop);  // another high half: the same character
	CHECK(AfterLoad(world, {}, SOLO, 0x3) != coop);
	// A save made since keeps that key, so the character's older and newer saves agree.
	const auto save = SavedKey(world);  // SOLO's
	CHECK(AfterLoad(world, save, SOLO, 0x4) == CharacterBinding::World::FromCharacter(SOLO));
	// No random number makes a key one of those (nor 0).
	for (const std::uint64_t random : { 0ull, 1ull << 63, CharacterBinding::World::FromCharacter(COOP), ~0ull }) {
		const auto key = CharacterBinding::World::Fresh(random);
		CHECK(key != 0);
		CHECK((key >> 63) == 0);
	}
	// A record that isn't a key is none.
	CHECK(AfterLoad(world, { 1, 2, 3 }, COOP, 0x5) == coop);
}

TEST("world key: a load whose co-save has no key, after one that had: the save's character, never the world before's")
{
	CharacterBinding::World world;
	const auto              first = world.Key(0xAAAA);
	const auto              save = SavedKey(world);
	CHECK(AfterLoad(world, save, 0, 0xB) == first);
	// F4SE's revert, then a save with no 'CHAR' record: no key carries over from the world before.
	CHECK(AfterLoad(world, {}, SOLO, 0xC) == CharacterBinding::World::FromCharacter(SOLO));
	// Started without a revert (should one ever be missed) still forgets the save before's key.
	world.Loaded(save);
	world.Started(false, SOLO, 0xD);
	CHECK(world.Key(0) == first);
	world.Started(false, SOLO, 0xE);
	CHECK(world.Key(0) == CharacterBinding::World::FromCharacter(SOLO));
}
