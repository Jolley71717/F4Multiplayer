#pragma once

#include "SessionFile.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// A saved session belongs to the host's character: the game gives every character an ID (made when the
// player confirms the name in the opening, and kept in each of its saves), and the host's server keeps
// that character's session in a file of its own. Another character (a new one, or the solo save) never
// takes in the co-op character's dead NPCs, loot and quest stages. A friend who loads another character
// while joined leaves the session. These are the rules, kept free of the game so they can be tested;
// Session applies them.
namespace CharacterBinding
{
	// Before sessions were per character, one file held the session whatever the host played.
	inline constexpr std::string_view LEGACY_FILE = "F4Multiplayer.session";

	// The character a game's ID (BGSSaveLoadManager::currentPlayerID) names: its low 32 bits. Only they are the
	// character's: a save's file name holds them and one letter after them (Quicksave0_D8F04348M4A6F..., the name in
	// hex after the letter), '_' before the game had a plugin of a mod and 'M' since, and a load of that save gives the
	// ID back with the letter in the high half ('M': 1). The game in memory keeps the high half it had: a character
	// first loaded from a save made before its first mod is 0 there, and so is a coc character from the main menu, yet
	// their quicksaves load back as 1. Comparing the whole ID took a quickload for another character (seen on the rig,
	// 0 hosted, 0000000100000000 loaded) and restarted hosting, into a fresh session file.
	[[nodiscard]] constexpr std::uint64_t Character(std::uint64_t a_playerId) { return a_playerId & 0xFFFFFFFFull; }

	// Which character's world a game plays, for the items players give each other (Drops' record and the server's book,
	// common/DropStore.h): only that character's saves hold what its world gave and took, so a new character doesn't get
	// back what an older one took, while that older one's own save from before the take still does. The engine's ID can't
	// say which world it is: every coc character from the main menu has the same one (0), and a new game has the last
	// character's until its name is confirmed. So each world has a key of its own, kept in its saves (the co-save's 'CHAR'
	// record) and said in its record to the server (Protocol::DropRecord::character). It gets one as it starts: a new game
	// a fresh one; a loaded save the one it kept, or, a save from before keys, one made from its character's ID (the same for
	// all of that character's older saves); a world no load and no new game started (coc from the main menu) a fresh one
	// the first time it is asked. Free of the game so it can be tested; Drops keeps one (any thread, under its lock).
	class World
	{
	public:
		// F4SE's revert, before a load and as a new game starts: nothing is known of the world to come.
		void Revert()
		{
			key = 0;
			saved = 0;
		}

		// The loaded save's co-save record: its key. One that isn't a key is no record (a save from before keys).
		void Loaded(std::span<const std::uint8_t> a_bytes)
		{
			std::uint64_t value = 0;
			if (a_bytes.size() == sizeof(value)) {
				std::memcpy(&value, a_bytes.data(), sizeof(value));
			}
			saved = value;
		}

		// The load (or, a_newGame, the new game) is done. a_character: the loaded character's ID (Character); a_random: a
		// random number, for a fresh key.
		void Started(bool a_newGame, std::uint64_t a_character, std::uint64_t a_random)
		{
			key = a_newGame ? Fresh(a_random) : saved != 0 ? saved : FromCharacter(a_character);
			saved = 0;
		}

		// The world's key, given now if nothing started it (a_random: as Started's).
		[[nodiscard]] std::uint64_t Key(std::uint64_t a_random)
		{
			if (key == 0) {
				key = Fresh(a_random);
			}
			return key;
		}

		// The co-save's record for a save made now.
		[[nodiscard]] std::vector<std::uint8_t> Save(std::uint64_t a_random)
		{
			const auto                value = Key(a_random);
			std::vector<std::uint8_t> out(sizeof(value));
			std::memcpy(out.data(), &value, sizeof(value));
			return out;
		}

		// A save from before keys: its character's ID, marked so no fresh key is ever the same.
		[[nodiscard]] static constexpr std::uint64_t FromCharacter(std::uint64_t a_character) { return OLD | Character(a_character); }
		[[nodiscard]] static constexpr std::uint64_t Fresh(std::uint64_t a_random)
		{
			const auto value = a_random & ~OLD;
			return value != 0 ? value : 1;
		}

	private:
		static constexpr std::uint64_t OLD = 1ull << 63;

		std::uint64_t key = 0;
		std::uint64_t saved = 0;  // the loaded save's (0: none), until Started
	};

	// The file of a_character's session. The dev channel's test rig uses files of its own, so its test
	// kills never reach a real session (and it never reads one).
	[[nodiscard]] inline std::string FileName(std::uint64_t a_character, bool a_dev)
	{
		return std::format("F4Multiplayer.{}{:016X}.session", a_dev ? "dev." : "", a_character);
	}

	// Up to 0.9.0-rc2 a session file was named after the whole ID, high half too: such a file of a_character (the same low
	// half, another high half), if there is one and a_character has no file under its own name yet. The one of the session
	// the loaded save was played in (a_saveSession, its co-save record's) if one is: a quickload that restarted hosting left
	// a fresh file beside the real one, written last. Else the newest.
	[[nodiscard]] inline std::optional<std::filesystem::path> FileUnderWholeId(const std::filesystem::path& a_folder, std::uint64_t a_character, bool a_dev,
		std::uint64_t a_saveSession = 0)
	{
		const auto own = FileName(a_character, a_dev);
		const auto prefix = std::format("F4Multiplayer.{}", a_dev ? "dev." : "");
		const auto suffix = std::format("{:08X}.session", Character(a_character));
		std::optional<std::filesystem::path>   found;
		std::filesystem::file_time_type        newest{};
		bool                                   saves = false;  // found is the save's session's
		std::error_code                        error;
		for (std::filesystem::directory_iterator it{ a_folder, error }, end; !error && it != end; it.increment(error)) {
			const auto name = it->path().filename().string();
			if (name.size() != prefix.size() + 16 + std::string_view{ ".session" }.size() || name == own || !name.starts_with(prefix) || !name.ends_with(suffix)) {
				continue;
			}
			const auto high = std::string_view{ name }.substr(prefix.size(), 8);
			if (high.find_first_not_of("0123456789ABCDEF") != std::string_view::npos || !it->is_regular_file(error)) {
				continue;
			}
			if (saves) {
				continue;
			}
			if (a_saveSession != 0) {
				if (const auto file = SessionFile::Load(it->path()); file && file->sessionId == a_saveSession) {
					found = it->path();
					saves = true;
					continue;
				}
			}
			const auto time = it->last_write_time(error);
			if (!error && (!found || time > newest)) {
				found = it->path();
				newest = time;
			}
			error.clear();
		}
		return found;
	}

	// What Session is to do.
	enum class Step
	{
		kNone,
		kStartHosting,  // host the loaded character's session
		kStopHosting,   // another character is loaded, or a new game started: hosting starts again with its session
		kLeave,         // leave the session we joined (or are trying to join)
	};

	// The loaded game, as Session sees it.
	struct Game
	{
		std::uint64_t character = 0;  // the loaded character's ID (0 is an ordinary ID: many saves have it)
		bool          inWorld = false;
		bool          inOpening = false;  // Vault 111's opening, or a new character with no name yet (Story::InOpening)
	};

	// Session's side of it.
	struct Link
	{
		bool wantHost = false;  // bHost
		bool hosting = false;   // our own server runs
		bool joining = false;   // set to reach a friend's server: joined, connecting, or waiting to try again
	};

	// Which character our hosting and our joining belong to, through loads, new games, reconnects and the menu.
	// Session tells it what happens and does the steps it returns.
	class Tracker
	{
	public:
		// Once a second. With bHost, hosting starts once the game is in the world and out of the opening, never at the
		// main menu: the first arrival decides the session (its load order), so a friend must not find a server before
		// our own game can be first. A new game's character ID comes with its name, in the opening, and nothing of the
		// opening's world is the session's anyway. Once hosting, the opening only pauses sharing (a host with an
		// alternate start who walks into Vault 111 hosts on); another character stops it.
		Step Check(const Link& a_link, const Game& a_game)
		{
			// Since a new game, the ID is the one the game had before (the last character's) until the name is confirmed.
			// A change with no load in between is that (frames are held across a load until OnLoad has run), wherever it
			// happens: an alternate start names the character outside the opening. A joined game plays that character from
			// now on. (Only in the world: at the main menu nothing is named.)
			if (unnamed && a_game.inWorld && a_game.character != *unnamed) {
				if (a_link.joining && bound) {
					bound = a_game.character;
				}
				unnamed.reset();
			}
			if (a_link.hosting) {
				return a_game.character != hosted ? Step::kStopHosting : Step::kNone;
			}
			// In the opening too (a save made there before the name was confirmed leaves nothing for the above to see): a
			// joined game's character changes there with no load only when a new character's name is confirmed.
			if (a_link.joining && bound && a_game.inWorld && a_game.inOpening) {
				bound = a_game.character;
			}
			return a_link.wantHost && a_game.inWorld && !a_game.inOpening ? Step::kStartHosting : Step::kNone;
		}

		// A save was loaded, or (a_newGame) a new game started. Another character's save, or a new game, isn't the hosted
		// character's world. A joining game that loads another character's save than the one that joined leaves, welcomed
		// yet or not (a friend waiting for the host who loads their solo save meanwhile would join with it): the session's
		// changes would go into a world they don't belong to, and be saved there. A new game doesn't leave: a fresh
		// character for co-op is what INSTALL.md suggests, and Check takes it once it has its name.
		Step OnLoad(const Link& a_link, bool a_newGame, std::uint64_t a_character)
		{
			unnamed = a_newGame ? std::optional{ a_character } : std::nullopt;  // a save's ID is its character's
			vaultSeen = false;
			if (a_link.hosting) {
				return a_newGame || a_character != hosted ? Step::kStopHosting : Step::kNone;
			}
			return a_link.joining && !a_newGame && bound && *bound != a_character ? Step::kLeave : Step::kNone;
		}

		void OnHostingStarted(std::uint64_t a_character) { hosted = a_character; }

		// Our game said hello to the server. The first hello after choosing it says which character joins; a reconnect's
		// is still that character's.
		void OnHello(std::uint64_t a_character)
		{
			if (!bound) {
				bound = a_character;
			}
		}

		// The server let us in (a_hosting: our own). Let back in after a drop into another session than the one we
		// joined (the host now plays another character, or started over), we leave: that session's dead and loot
		// would go into this character's world and saves without the player choosing it.
		Step OnWelcome(bool a_hosting, std::uint64_t a_session)
		{
			if (!a_hosting && joined && *joined != a_session) {
				return Step::kLeave;
			}
			joined = a_session;
			return Step::kNone;
		}

		// Another server was chosen, or none (leaving, a menu join, hosting started or stopped): no character has joined it.
		void OnServerChanged()
		{
			bound.reset();
			joined.reset();
		}

		[[nodiscard]] std::uint64_t Hosted() const { return hosted; }

		// A new game's character with no name yet: since a new game, with no load since, and a_character (the loaded ID)
		// still the one the game had then, the last character's. Its world is no session's yet (Story counts it as the
		// opening): hosting would open the last character's session file. Not once the new game has been in Vault 111's
		// opening (OnVaultOpening): that holds it as it always did, and a start there is held no longer than before
		// whenever the game gives the ID (were it given before the new game's message, naming would change nothing).
		[[nodiscard]] bool Unnamed(std::uint64_t a_character) const { return unnamed && !vaultSeen && *unnamed == a_character; }
		// The ID a new game started under, until Check sees the name confirmed or a save loads.
		[[nodiscard]] std::optional<std::uint64_t> UnnamedId() const { return unnamed; }

		// The game is in Vault 111's opening of its own start (Story::InVaultStart). Never for an alternate start that walks
		// into the vault: out again, it would no longer count as unnamed and would host under the last character's ID.
		void OnVaultOpening() { vaultSeen = true; }

		// A load begins (F4SE's kPreLoadGame): the ID from here on is a save's, never a name. Should the load's end never
		// come (frames go on after SaveLoadHold's limit with no OnLoad), Check must not take the save's ID for naming
		// outside the opening and bind a joined game to it.
		void OnLoadStarting() { unnamed.reset(); }

	private:
		std::uint64_t                hosted = 0;  // the character whose session our server holds
		std::optional<std::uint64_t> unnamed;     // since a new game: the ID it had then, until named (UnnamedId)
		bool                         vaultSeen = false;  // since the last load or new game: Vault 111's start was seen
		std::optional<std::uint64_t> bound;       // the character that joined the server we're set to reach
		std::optional<std::uint64_t> joined;      // the session it joined
	};

	enum class Legacy
	{
		kNone,   // there is a character file already, or no old file: nothing to do
		kAdopt,  // the old file is this character's session: it becomes the character's file
		kLeave,  // the old file is some other world's: left where it is, for the character it belongs to
	};

	// The old shared file is this character's only if the loaded save was played in that session: its
	// co-save record carries the session's ID. Nothing else ties them (the old file says nothing of whose it
	// was), and a save with no record (0) is no one's.
	[[nodiscard]] constexpr Legacy ForLegacy(bool a_characterFileExists, bool a_legacyReadable, std::uint64_t a_legacySession, std::uint64_t a_saveSession)
	{
		if (a_characterFileExists || !a_legacyReadable) {
			return Legacy::kNone;
		}
		return a_legacySession != 0 && a_legacySession == a_saveSession ? Legacy::kAdopt : Legacy::kLeave;
	}

	// The session file for a_character in a_folder, adopting the old shared file when it is this character's
	// (a_saveSession: the session the loaded save's co-save record names). Says what it did through a_log.
	inline std::filesystem::path PrepareFile(const std::filesystem::path& a_folder, std::uint64_t a_character, bool a_dev, std::uint64_t a_saveSession,
		const std::function<void(const std::string&)>& a_log)
	{
		const auto      path = a_folder / FileName(a_character, a_dev);
		std::error_code error;
		if (!std::filesystem::exists(path, error)) {
			if (const auto whole = FileUnderWholeId(a_folder, a_character, a_dev, a_saveSession)) {
				std::filesystem::rename(*whole, path, error);
				a_log(error ? std::format("could not rename {} to {}: {}", whole->filename().string(), path.filename().string(), error.message()) :
				              std::format("continuing the saved co-op session: {} is now {}", whole->filename().string(), path.filename().string()));
				error.clear();
			}
		}
		if (a_dev) {
			return path;  // the rig never reads a real session
		}
		const auto      legacyPath = a_folder / LEGACY_FILE;
		if (!std::filesystem::exists(legacyPath, error)) {
			return path;
		}
		const auto legacy = std::filesystem::exists(path, error) ? std::nullopt : SessionFile::Load(legacyPath);
		switch (ForLegacy(std::filesystem::exists(path, error), legacy.has_value(), legacy ? legacy->sessionId : 0, a_saveSession)) {
		case Legacy::kAdopt:
			std::filesystem::rename(legacyPath, path, error);
			a_log(error ? std::format("could not rename {} to {}: {}", LEGACY_FILE, path.filename().string(), error.message()) :
			              std::format("continuing the saved co-op session: {} is now {}", LEGACY_FILE, path.filename().string()));
			break;
		case Legacy::kLeave:
			a_log(std::format("{} is another save's session; this character starts its own in {}", LEGACY_FILE, path.filename().string()));
			break;
		case Legacy::kNone:
			break;
		}
		return path;
	}
}
