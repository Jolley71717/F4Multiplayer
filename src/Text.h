#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TextEnglish.h"

// Everything the player reads that the plugin wrote: the HUD's notices, the F1 menu's pages and buttons, the
// server's refusals as the joining game shows them. Each is a key with an English text compiled in (TextEnglish.h);
// a language file (Data/F4SE/Plugins/F4Multiplayer/lang/<code>.txt, "key = text" lines) replaces the texts it has, and
// a key it lacks shows the English, so a missing or half-done file never blanks a message.
//
// A text holds named placeholders, {player}, {n}, {place}: Format fills them in from the arguments. One the
// arguments lack stays in the text as written ("{player}"), so a typo in a translation shows up where it is read
// rather than as a gap. In a file, "\n" is a line break and "\\" a backslash.
//
// This header is free of the game, so the tests run all of it; Text.cpp picks the language (the game's own
// setting, or sLanguage in F4Multiplayer.ini) and reads the file.
namespace Text
{
	// One placeholder's value: Arg{ "n", 3 }, Arg{ "player", name }.
	struct Arg
	{
		std::string name;
		std::string value;

		Arg(std::string_view a_name, std::string_view a_value) :
			name(a_name), value(a_value)
		{}
		Arg(std::string_view a_name, const char* a_value) :
			name(a_name), value(a_value ? a_value : "")
		{}
		Arg(std::string_view a_name, const std::string& a_value) :
			name(a_name), value(a_value)
		{}
		template <std::integral T>
		Arg(std::string_view a_name, T a_value) :
			name(a_name), value(std::to_string(a_value))
		{}
	};

	// The compiled-in English: every key there is, with its text (TextEnglish.h).
	[[nodiscard]] inline std::span<const Entry> English()
	{
		return ENGLISH_TABLE;
	}

	using Table = std::map<std::string, std::string, std::less<>>;

	namespace detail
	{
		inline bool IsNameChar(char a_c)
		{
			return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_';
		}

		inline std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			return a_str.substr(first, a_str.find_last_not_of(" \t\r\n") - first + 1);
		}

		struct State
		{
			Table table;                  // the language file's texts, by key
			std::string code = "en";      // the language they are in
			std::unordered_map<std::string_view, std::string_view> english;  // built on first use
		};

		inline State& Current()
		{
			static State state;
			return state;
		}

		inline const std::unordered_map<std::string_view, std::string_view>& EnglishMap()
		{
			auto& map = Current().english;
			if (map.empty()) {
				for (const auto& entry : English()) {
					map.emplace(entry.key, entry.text);
				}
			}
			return map;
		}
	}

	// a_template with each {name} replaced by the argument of that name. A {name} with no argument, and a brace
	// that opens no name, stay as they are.
	[[nodiscard]] inline std::string Expand(std::string_view a_template, std::span<const Arg> a_args)
	{
		std::string out;
		out.reserve(a_template.size() + 16);
		for (std::size_t i = 0; i < a_template.size(); ++i) {
			if (a_template[i] == '{') {
				const auto close = a_template.find('}', i + 1);
				if (close != std::string_view::npos && close > i + 1 &&
					std::ranges::all_of(a_template.substr(i + 1, close - i - 1), detail::IsNameChar)) {
					const auto name = a_template.substr(i + 1, close - i - 1);
					const auto arg = std::ranges::find_if(a_args, [&](const Arg& a_arg) { return a_arg.name == name; });
					if (arg != a_args.end()) {
						out += arg->value;
						i = close;
						continue;
					}
				}
			}
			out += a_template[i];
		}
		return out;
	}

	[[nodiscard]] inline std::string Expand(std::string_view a_template, std::initializer_list<Arg> a_args)
	{
		return Expand(a_template, std::span<const Arg>{ a_args.begin(), a_args.size() });
	}

	// The text of a language file: "key = text" lines, '#' lines and blank lines skipped, "\n" and "\\" unescaped, a
	// leading UTF-8 byte order mark and the lines' CRs ignored. A line with no '=' or no key is skipped.
	[[nodiscard]] inline Table Parse(std::string_view a_file)
	{
		Table table;
		if (a_file.starts_with("\xEF\xBB\xBF")) {
			a_file.remove_prefix(3);
		}
		while (!a_file.empty()) {
			const auto end = a_file.find('\n');
			const auto line = detail::Trim(a_file.substr(0, end));
			a_file = end == std::string_view::npos ? std::string_view{} : a_file.substr(end + 1);
			if (line.empty() || line.front() == '#') {
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto key = detail::Trim(line.substr(0, eq));
			const auto raw = detail::Trim(line.substr(eq + 1));
			if (key.empty()) {
				continue;
			}
			std::string text;
			text.reserve(raw.size());
			for (std::size_t i = 0; i < raw.size(); ++i) {
				if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'n' || raw[i + 1] == '\\')) {
					text += raw[i + 1] == 'n' ? '\n' : '\\';
					++i;
				} else {
					text += raw[i];
				}
			}
			table[std::string{ key }] = std::move(text);
		}
		return table;
	}

	// A text as a language file holds it (Parse's inverse), for the tests and for writing a file from the table.
	[[nodiscard]] inline std::string Escape(std::string_view a_text)
	{
		std::string out;
		for (const char c : a_text) {
			if (c == '\n') {
				out += "\\n";
			} else if (c == '\\') {
				out += "\\\\";
			} else {
				out += c;
			}
		}
		return out;
	}

	// The names of a text's placeholders, in order of appearance (a translation must have the English text's).
	[[nodiscard]] inline std::vector<std::string> Placeholders(std::string_view a_text)
	{
		std::vector<std::string> names;
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] != '{') {
				continue;
			}
			const auto close = a_text.find('}', i + 1);
			if (close != std::string_view::npos && close > i + 1 && std::ranges::all_of(a_text.substr(i + 1, close - i - 1), detail::IsNameChar)) {
				names.emplace_back(a_text.substr(i + 1, close - i - 1));
				i = close;
			}
		}
		std::ranges::sort(names);
		return names;
	}

	// A language as the game or the ini names it, as the code of a language file ("en", "en-gb", "ru", "es", "ar", "de",
	// "fr", "it", "pl", "pt-br", "ja", "zh-cn", "zh-tw"). The game's own sLanguage is "en", "fr", "de", "it", "es", "pl",
	// "ru", "ptbr", "ja", "cn" (Simplified Chinese) or "zhhant"/"tchinese" (Traditional); "esmx" (Latin American Spanish) is
	// Spanish too. Case, '-' and '_' do not matter, and a language's English name works too. What this does not know
	// comes back lower-cased with '_' as '-', so a file the player adds (lang/ko.txt) is found by its own name.
	[[nodiscard]] inline std::string CodeFor(std::string_view a_name)
	{
		std::string plain;
		std::string kept;
		for (const char c : detail::Trim(a_name)) {
			const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			kept += lower == '_' ? '-' : lower;
			if (lower != '-' && lower != '_' && lower != ' ') {
				plain += lower;
			}
		}
		static const std::pair<std::string_view, std::string_view> KNOWN[] = {
			{ "en", "en" }, { "english", "en" }, { "enus", "en" }, { "americanenglish", "en" },
			{ "engb", "en-gb" }, { "enuk", "en-gb" }, { "britishenglish", "en-gb" },
			{ "ru", "ru" }, { "russian", "ru" },
			{ "es", "es" }, { "spanish", "es" }, { "esmx", "es" }, { "latam", "es" }, { "eses", "es" },
			{ "ar", "ar" }, { "arabic", "ar" },
			{ "de", "de" }, { "german", "de" },
			{ "fr", "fr" }, { "french", "fr" },
			{ "it", "it" }, { "italian", "it" },
			{ "pl", "pl" }, { "polish", "pl" },
			{ "ptbr", "pt-br" }, { "pt", "pt-br" }, { "brazilian", "pt-br" }, { "portuguese", "pt-br" },
			{ "ja", "ja" }, { "jp", "ja" }, { "japanese", "ja" },
			{ "cn", "zh-cn" }, { "zh", "zh-cn" }, { "zhcn", "zh-cn" }, { "zhhans", "zh-cn" }, { "schinese", "zh-cn" }, { "chinese", "zh-cn" },
			{ "tw", "zh-tw" }, { "zhtw", "zh-tw" }, { "zhhant", "zh-tw" }, { "tchinese", "zh-tw" },
		};
		for (const auto& [name, code] : KNOWN) {
			if (plain == name) {
				return std::string{ code };
			}
		}
		return kept;
	}

	// Whether a code can name a file in the lang folder (letters, digits and '-', so it never leaves the folder).
	[[nodiscard]] inline bool IsFileCode(std::string_view a_code)
	{
		return !a_code.empty() && a_code.size() <= 16 && std::ranges::all_of(a_code, [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '-'; });
	}

	// The language the texts are in now, and what its file held (replacing the last). The plugin sets it once at
	// startup; the tests set it to try a language.
	inline void Use(std::string a_code, Table a_table)
	{
		auto& state = detail::Current();
		state.code = std::move(a_code);
		state.table = std::move(a_table);
	}

	[[nodiscard]] inline const std::string& Language()
	{
		return detail::Current().code;
	}

	// Picks the language (sLanguage in F4Multiplayer.ini, else the game's own sLanguage, else English) and reads its
	// file from Data/F4SE/Plugins/F4Multiplayer/lang. After Config::Load, once, at startup (Text.cpp, the plugin only).
	void Load();

	// The English text of a key (the key itself for one that does not exist, so a typo shows where it is read).
	[[nodiscard]] inline std::string GetEnglish(std::string_view a_key)
	{
		const auto& english = detail::EnglishMap();
		const auto  it = english.find(a_key);
		return it != english.end() ? std::string{ it->second } : std::string{ a_key };
	}

	// The text of a key in the current language: the file's, else the English.
	[[nodiscard]] inline std::string Get(std::string_view a_key)
	{
		const auto& table = detail::Current().table;
		if (const auto it = table.find(a_key); it != table.end() && !it->second.empty()) {
			return it->second;
		}
		return GetEnglish(a_key);
	}

	[[nodiscard]] inline std::string Format(std::string_view a_key, std::initializer_list<Arg> a_args)
	{
		return Expand(Get(a_key), a_args);
	}

	// The English text with its placeholders filled: what the log says of a notice, whatever language the player reads.
	[[nodiscard]] inline std::string FormatEnglish(std::string_view a_key, std::initializer_list<Arg> a_args)
	{
		return Expand(GetEnglish(a_key), a_args);
	}

	// A server's refusal as the joining player reads it. The server's texts are English on the wire (any version of the
	// server or a dedicated one sends them); the ones this client knows are shown in its language, and any other text
	// (a changed server's) as it came.
	[[nodiscard]] inline std::string Reject(std::string_view a_reason)
	{
		static constexpr std::pair<std::string_view, std::string_view> WHOLE[] = {
			{ "Not an F4Multiplayer client", "reject.not_client" },
			{ "Wrong password", "reject.password" },
			{ "The host isn't in the game yet", "reject.host_not_ready" },
			{ "Server is full", "reject.full" },
		};
		for (const auto& [english, key] : WHOLE) {
			if (a_reason == english) {
				return Get(key);
			}
		}
		// "Version mismatch: server uses protocol 31, you have 30. Install the same mod version."
		constexpr std::string_view VERSION_START = "Version mismatch: server uses protocol ";
		constexpr std::string_view VERSION_MIDDLE = ", you have ";
		constexpr std::string_view VERSION_END = ". Install the same mod version.";
		if (a_reason.starts_with(VERSION_START) && a_reason.ends_with(VERSION_END)) {
			auto rest = a_reason.substr(VERSION_START.size(), a_reason.size() - VERSION_START.size() - VERSION_END.size());
			if (const auto middle = rest.find(VERSION_MIDDLE); middle != std::string_view::npos) {
				const auto server = rest.substr(0, middle);
				const auto you = rest.substr(middle + VERSION_MIDDLE.size());
				const auto digits = [](std::string_view a_s) { return !a_s.empty() && a_s.size() < 10 && std::ranges::all_of(a_s, [](char a_c) { return a_c >= '0' && a_c <= '9'; }); };
				if (digits(server) && digits(you)) {
					return Format("reject.version", { { "server", server }, { "you", you } });
				}
			}
		}
		// The load order refusal is several sentences (Protocol::LoadOrderDifference) around lists of plugin names.
		static constexpr std::pair<std::string_view, std::string_view> SENTENCES[] = {
			{ "Your load order is different from the other players.", "reject.order" },
			{ "Everyone needs the same mods in the same order.", "reject.order_all" },
			{ "Only you have:", "reject.only_you" },
			{ "You are missing:", "reject.missing" },
			{ "Same mods, different order: sort yours like theirs.", "reject.same_mods" },
			{ "Texture plugins may differ only when they load last.", "reject.texture" },
		};
		std::string out{ a_reason };
		if (!out.starts_with(SENTENCES[0].first)) {
			return out;
		}
		for (const auto& [english, key] : SENTENCES) {
			const auto text = Get(key);
			for (std::size_t at = out.find(english); at != std::string::npos; at = out.find(english, at + text.size())) {
				out.replace(at, english.size(), text);
			}
		}
		return out;
	}
}
