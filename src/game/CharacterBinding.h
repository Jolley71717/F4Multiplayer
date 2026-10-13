#pragma once

#include "SessionFile.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

	// The file of a_character's session. The dev channel's test rig uses files of its own, so its test
	// kills never reach a real session (and it never reads one).
	[[nodiscard]] inline std::string FileName(std::uint64_t a_character, bool a_dev)
	{
		return std::format("F4Multiplayer.{}{:016X}.session", a_dev ? "dev." : "", a_character);
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
		bool          inOpening = false;  // Vault 111's opening (Story::InOpening)
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
			if (a_link.hosting) {
				return a_game.character != hosted ? Step::kStopHosting : Step::kNone;
			}
			// A joined game's character changes with no load only when a new character's name is confirmed, in the
			// opening: that character plays in the session from now on. (Anywhere else a change is a load: OnLoad.)
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

	private:
		std::uint64_t                hosted = 0;  // the character whose session our server holds
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
		const auto path = a_folder / FileName(a_character, a_dev);
		if (a_dev) {
			return path;  // the rig never reads a real session
		}
		std::error_code error;
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
