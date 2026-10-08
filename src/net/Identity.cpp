#include "net/Identity.h"

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
			std::uint64_t id = 0;
			if (std::ifstream in{ *path }; in && (in >> std::hex >> id) && id != 0) {
				return id;
			}
			std::random_device rd;
			while (id == 0) {
				id = (static_cast<std::uint64_t>(rd()) << 32) | rd();
			}
			std::ofstream out{ *path, std::ios::trunc };
			out << std::format("{:016x}", id);
			if (!out) {
				REX::WARN("Identity: couldn't write {}", path->string());
			}
			return id;
		}
	}

	std::uint64_t Get()
	{
		static const std::uint64_t id = Load();
		return id;
	}
}
