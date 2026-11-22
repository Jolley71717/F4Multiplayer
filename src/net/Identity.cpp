#include "net/Identity.h"

#include "net/IdentityFile.h"

#include <ShlObj.h>

#include <random>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace Identity
{
	std::optional<std::filesystem::path> DataFolder()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return std::nullopt;
		}
		std::filesystem::path path{ docs };
		CoTaskMemFree(docs);
		return path / "My Games" / "Fallout4" / "F4SE";
	}

	namespace
	{
		std::optional<std::filesystem::path> FilePath()
		{
			const auto folder = DataFolder();
			return folder ? std::optional{ *folder / "F4Multiplayer_player.id" } : std::nullopt;
		}

		std::uint64_t Load()
		{
			const auto path = FilePath();
			if (!path) {
				return 0;
			}
			// The file work is IdentityFile::LoadOrCreate (game-free, which the tests run): a file that can't be read now
			// is never written over. Only the file's name goes in the log, not the folder: it holds the Windows user name.
			std::random_device rd;
			const auto         loaded = IdentityFile::LoadOrCreate(*path, [&] { return (static_cast<std::uint64_t>(rd()) << 32) | rd(); });
			switch (loaded.what) {
			case IdentityFile::Kept::Unreadable:
				REX::WARN("Identity: {} can't be read right now; it is left as it is, and this run of the game uses an identity of its own", path->filename().string());
				break;
			case IdentityFile::Kept::NotCreated:
				REX::WARN("Identity: couldn't write {}", path->filename().string());
				break;
			case IdentityFile::Kept::Read:
			case IdentityFile::Kept::Created:
				break;
			}
			return loaded.id;
		}
	}

	std::uint64_t Get()
	{
		static const std::uint64_t id = Load();
		return id;
	}
}
