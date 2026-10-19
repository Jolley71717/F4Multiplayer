#include "Platform.h"

#include "LaunchReport.h"

namespace Platform
{
	namespace
	{
		using WineVersion = const char* (*)();
		using WineHostVersion = void (*)(const char**, const char**);

		REX::W32::HMODULE Ntdll()
		{
			return REX::W32::GetModuleHandleW(L"ntdll.dll");
		}

		template <class T>
		T Wine(const char* a_name)
		{
			const auto ntdll = Ntdll();
			return ntdll ? reinterpret_cast<T>(REX::W32::GetProcAddress(ntdll, a_name)) : nullptr;
		}

		std::string Env(const char* a_name)
		{
			char       buffer[4096];
			const auto length = REX::W32::GetEnvironmentVariableA(a_name, buffer, static_cast<std::uint32_t>(std::size(buffer)));
			return length > 0 && length < std::size(buffer) ? std::string{ buffer, length } : std::string{};
		}

		bool WorkingFolderIsGame()
		{
			std::error_code ec;
			const auto      here = std::filesystem::current_path(ec);
			if (ec || GameFolder().empty()) {
				return true;  // can't tell: nothing to say
			}
			return std::filesystem::equivalent(here, GameFolder(), ec) || ec;
		}

		LaunchReport::Facts Gather()
		{
			LaunchReport::Facts facts;
			if (const auto version = Wine<WineVersion>("wine_get_version")) {
				facts.wineVersion = version() ? version() : "?";
				if (const auto build = Wine<WineVersion>("wine_get_build_id"); build && build()) {
					facts.wineBuild = build();
				}
				if (const auto host = Wine<WineHostVersion>("wine_get_host_version")) {
					const char* system = nullptr;
					const char* release = nullptr;
					host(&system, &release);
					facts.hostSystem = system ? system : "";
					facts.hostRelease = release ? release : "";
				}
			}
			facts.steamAppId = Env("SteamAppId");
			facts.steamGameId = Env("SteamGameId");
			facts.overlayModule = REX::W32::GetModuleHandleW(L"GameOverlayRenderer64.dll") != nullptr;
			// Steam starts a Proton game with its overlay library preloaded (Wine passes LD_PRELOAD through as it is).
			facts.overlayPreloaded = Env("LD_PRELOAD").find("gameoverlayrenderer") != std::string::npos;
			facts.steamCompat = !Env("STEAM_COMPAT_CLIENT_INSTALL_PATH").empty();
			facts.workingFolderIsGame = WorkingFolderIsGame();
			return facts;
		}
	}

	bool UnderWine()
	{
		static const bool wine = Wine<WineVersion>("wine_get_version") != nullptr;
		return wine;
	}

	const std::filesystem::path& GameFolder()
	{
		static const std::filesystem::path folder = [] {
			wchar_t    buffer[REX::W32::MAX_PATH * 4];
			const auto length = REX::W32::GetModuleFileNameW(nullptr, buffer, static_cast<std::uint32_t>(std::size(buffer)));
			return length > 0 && length < std::size(buffer) ? std::filesystem::path{ std::wstring_view{ buffer, length } }.parent_path() : std::filesystem::path{};
		}();
		return folder;
	}

	std::filesystem::path GamePath(std::string_view a_relative)
	{
		const std::filesystem::path relative{ a_relative };
		std::error_code             ec;
		if (std::filesystem::exists(relative, ec) || GameFolder().empty()) {
			return relative;
		}
		auto inGame = GameFolder() / relative;
		return std::filesystem::exists(inGame, ec) ? inGame : relative;
	}

	void LogStartup()
	{
		for (const auto& line : LaunchReport::Lines(Gather())) {
			if (line.warn) {
				REX::WARN("{}", line.text);
			} else {
				REX::INFO("{}", line.text);
			}
		}
	}
}
