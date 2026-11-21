#include "Config.h"

#include "ConfigRead.h"
#include "IniFile.h"
#include "Log.h"
#include "Platform.h"
#include "SessionLevel.h"
#include "StorySetting.h"

namespace Config
{
	namespace
	{
		constexpr auto PATH = "Data/F4SE/Plugins/F4Multiplayer.ini"sv;
		constexpr auto FOLDER = "Data/F4SE/Plugins"sv;

		// dist/F4Multiplayer.ini, compiled in (xmake.lua, utils.bin2c): a new install's settings file is written from
		// it, and an older file gets the keys it lacks from it, each with its explanation. The zip ships no live ini
		// (0.21.0 and earlier did, and a mod manager or an extract over Data replaced the player's with it on every
		// update), so a player's values are never replaced.
		constexpr unsigned char DEFAULTS_BYTES[] = {
#include "F4Multiplayer.ini.h"
		};

		std::string_view Defaults()
		{
			std::string_view text{ reinterpret_cast<const char*>(DEFAULTS_BYTES), sizeof(DEFAULTS_BYTES) };
			while (text.ends_with('\0')) {  // bin2c's terminator
				text.remove_suffix(1);
			}
			return text;
		}

		// Heads the keys an update appends to a player's file (IniFile::AddMissing).
		constexpr auto NOTE = "# --- Settings added by an update of the mod, each at its default ---"sv;

		Settings settings;

		// The settings file, or where it goes when there is none yet: Platform::GamePath finds a file that is there;
		// a new one goes beside the plugin, in the Plugins folder it found.
		std::filesystem::path IniPath()
		{
			auto            path = Platform::GamePath(PATH);
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				path = Platform::GamePath(FOLDER) / "F4Multiplayer.ini";
			}
			return path;
		}

		// The settings to read: the player's file with the keys this version adds (appended to it), or, with no file
		// yet, the defaults, written as the file (IniFile::LoadOrCreate). A file that can't be read, or one saved as
		// UTF-16 (the reader takes UTF-8 or plain text), is left alone.
		std::string SettingsText()
		{
			auto        loaded = IniFile::LoadOrCreate(IniPath(), Defaults(), NOTE, WHEN_ABSENT);
			std::string names;
			for (const auto& key : loaded.added) {
				names += names.empty() ? key : ", " + key;
			}
			switch (loaded.what) {
			case IniFile::Kept::Created:
				REX::INFO("Config: {} was not there; written with every setting at its default", PATH);
				break;
			case IniFile::Kept::NotCreated:
				REX::WARN("Config: {} is not there and could not be written; using the defaults", PATH);
				break;
			case IniFile::Kept::Unreadable:
				REX::WARN("Config: {} could not be read; using the defaults", PATH);
				break;
			case IniFile::Kept::Extended:
				REX::INFO("Config: added {} setting(s) this version has and {} lacked, at their defaults: {}", loaded.added.size(), PATH, names);
				break;
			case IniFile::Kept::NotExtended:
				REX::WARN("Config: {} lacks {} setting(s) of this version ({}) and could not be written to; they stay at their defaults", PATH, loaded.added.size(), names);
				break;
			case IniFile::Kept::Utf16:
				REX::WARN("Config: {} is saved as UTF-16, which the mod can't read, so it is left as it is and the defaults are used; save it as UTF-8 (Notepad: Save as, Encoding)", PATH);
				break;
			case IniFile::Kept::Complete:
				break;
			}
			return std::move(loaded.text);
		}
	}

	void Load()
	{
		// The reading itself is Config::Read (ConfigRead.h, which the tests run on the defaults); what it says goes to the log.
		Read(SettingsText(), settings, [](bool a_warn, const std::string& a_text) {
			if (a_warn) {
				REX::WARN("{}", a_text);
			} else {
				REX::INFO("{}", a_text);
			}
		});

		if (!Log::SetLevel(settings.logLevel)) {
			REX::WARN("Config: sLogLevel '{}' is not info or debug; keeping {}", settings.logLevel, Log::Level());
		}
		// Whether the story is shared decides what a host's session shares; said here so a log shows it without hosting.
		REX::INFO("Config: loaded {} (sStory: {}, sQuests: {})", PATH, StorySetting::Name(settings.sharedStory), StorySetting::QuestsName(settings.safeQuests));
	}

	const Settings& Get()
	{
		return settings;
	}

	Settings& Mutable()
	{
		return settings;
	}

	bool Write(std::string_view a_key, std::string_view a_value)
	{
		// The whole file is read and written back (IniFile::SaveValue): it's a few kilobytes, and keeping the player's
		// comments and ordering matters more than speed here (a menu click, not a frame).
		// The password never goes in the log (players share their logs): only whether one is set.
		const std::string_view shown = a_key == "sPassword" ? (a_value.empty() ? "(none)" : "(a password)") : a_value;
		switch (IniFile::SaveValue(IniPath() /* the one Load read or wrote */, a_key, a_value, Defaults())) {
		case IniFile::Saved::Written:
			REX::INFO("Config: wrote {} = {}", a_key, shown);
			return true;
		case IniFile::Saved::Unreadable:
			// Written back now, the file would hold this one line in place of every setting the player had.
			REX::WARN("Config: {} can't be read right now (another program may have it open), so {} = {} is not written to it; it holds until the game closes", PATH, a_key, shown);
			return false;
		case IniFile::Saved::Utf16:
			// Not a file to put a UTF-8 line in (Load said so in the log and left it alone too).
			REX::WARN("Config: {} is saved as UTF-16; {} is not written to it", PATH, a_key);
			return false;
		case IniFile::Saved::NotWritten:
			break;
		}
		REX::WARN("Config: could not write {} = {} to {}", a_key, shown, PATH);
		return false;
	}
}
