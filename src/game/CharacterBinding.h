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

	enum class HostStep
	{
		kNone,
		kStart,  // start hosting this character's session
		kStop,   // stop: another character is loaded (hosting starts again with its session), or the opening
	};

	// What the host's game does about hosting, checked once a second. A new game is hosted only once its
	// opening is over: its character ID comes with the name, so before that it is still the last
	// character's, and nothing of the opening's world is the session's anyway.
	[[nodiscard]] constexpr HostStep ForHost(bool a_wantHost, bool a_hosting, bool a_inWorld, bool a_inOpening, std::uint64_t a_hostedCharacter,
		std::uint64_t a_currentCharacter)
	{
		if (a_hosting) {
			return a_inOpening || a_currentCharacter != a_hostedCharacter ? HostStep::kStop : HostStep::kNone;
		}
		return a_wantHost && a_inWorld && !a_inOpening ? HostStep::kStart : HostStep::kNone;
	}

	// The host's game after a load (a_newGame: a new game started): another character's save, or a new game,
	// isn't the hosted character's world.
	[[nodiscard]] constexpr bool HostStopsOnLoad(bool a_hosting, bool a_newGame, std::uint64_t a_hostedCharacter, std::uint64_t a_currentCharacter)
	{
		return a_hosting && (a_newGame || a_currentCharacter != a_hostedCharacter);
	}

	// A joined friend's game after a load: a save of another character than the one that joined leaves the
	// session (the session's changes would be applied to a world they don't belong to, and saved there). A new
	// game doesn't: starting a fresh character for co-op is what INSTALL.md suggests, and its ID comes later,
	// with its name, and Session then takes that character as the one that joined.
	[[nodiscard]] constexpr bool JoinerLeavesOnLoad(bool a_joined, bool a_newGame, std::uint64_t a_boundCharacter, std::uint64_t a_currentCharacter)
	{
		return a_joined && !a_newGame && a_currentCharacter != a_boundCharacter;
	}

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
