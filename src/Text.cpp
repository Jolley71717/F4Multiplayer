#include "Text.h"

#include "Config.h"
#include "IniFile.h"
#include "Platform.h"
#include "net/Identity.h"

namespace Text
{
	namespace
	{
		constexpr auto LANG_FOLDER = "Data/F4SE/Plugins/F4Multiplayer/lang"sv;

		std::optional<std::string> ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream in{ a_path, std::ios::binary };
			if (!in) {
				return std::nullopt;
			}
			return std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
		}

		// The language the game is set to. The game reads Fallout4_Default.ini (next to the exe), then Fallout4.ini and
		// Fallout4Custom.ini in Documents\My Games\Fallout4, each over the last, so the last that has the key decides.
		std::string GameLanguage()
		{
			std::vector<std::filesystem::path> files{ Platform::GamePath("Fallout4_Default.ini") };
			if (const auto folder = Identity::DataFolder()) {
				files.push_back(folder->parent_path() / "Fallout4.ini");
				files.push_back(folder->parent_path() / "Fallout4Custom.ini");
			}
			std::string language;
			for (const auto& file : files) {
				if (const auto text = ReadFile(file)) {
					if (const auto value = IniFile::GetValue(*text, "sLanguage", true); value && !value->empty()) {
						language = *value;
					}
				}
			}
			return language;
		}
	}

	void Load()
	{
		const auto& configured = Config::Get().language;
		const bool  fromIni = !configured.empty();
		const auto  asked = fromIni ? configured : GameLanguage();
		auto        code = asked.empty() ? std::string{ "en" } : CodeFor(asked);
		if (!IsFileCode(code)) {
			REX::WARN("Text: '{}' is not a language code; using English", asked);
			code = "en";
		}

		Table      table;
		const auto path = Platform::GamePath(LANG_FOLDER) / (code + ".txt");
		if (const auto text = ReadFile(path)) {
			table = Parse(*text);
			// A key the English table lacks is a typo in the file (or a text from a newer version): it is never used.
			std::size_t unknown = 0;
			for (const auto& [key, text2] : table) {
				if (std::ranges::none_of(English(), [&](const Entry& a_entry) { return a_entry.key == key; })) {
					if (++unknown <= 3) {
						REX::WARN("Text: {}.txt has a key the mod does not use: '{}'", code, key);
					}
				}
			}
			REX::INFO("Text: language {} ({} texts from {}.txt; the rest are English)", code, table.size(), code);
		} else if (code == "en") {
			REX::INFO("Text: language en (built in)");
		} else {
			REX::WARN("Text: no {}.txt in {} for {} '{}'; using English", code, LANG_FOLDER, fromIni ? "sLanguage in F4Multiplayer.ini" : "the game's language", asked);
			code = "en";
		}
		Use(code, std::move(table));
	}
}
