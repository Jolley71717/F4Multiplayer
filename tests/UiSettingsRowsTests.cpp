#include "Test.h"

#include "Text.h"
#include "game/UiSettingsRows.h"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>

namespace
{
	// The English of a key, or "" if the table lacks it.
	std::string_view EnglishOf(std::string_view a_key)
	{
		const auto english = Text::English();
		const auto entry = std::ranges::find_if(english, [&](const auto& a_entry) { return a_entry.key == a_key; });
		return entry == english.end() ? std::string_view{} : entry->text;
	}
}

TEST("settings rows: the host's rules are on Session options and a player's own switches on Settings, as each row's own words say")
{
	// What a row says of itself decides its page: one that is the host's setting is a rule of the session hosted.
	for (const auto& toggle : UiSettingsRows::TOGGLES) {
		const auto what = EnglishOf(toggle.what);
		if (what.empty() || EnglishOf(toggle.name).empty()) {
			Test::Fail(__FILE__, __LINE__, std::format("the switch '{}' has no English name or description", toggle.name));
			continue;
		}
		const bool saysHost = what.find("host's setting") != std::string_view::npos;
		if (saysHost != toggle.host) {
			Test::Fail(__FILE__, __LINE__, std::format("the switch '{}' says it {} the host's setting but is on {}", toggle.name, saysHost ? "is" : "isn't", toggle.host ? "Session options" : "Settings"));
		}
	}
	// Neither page lost a switch to the other: the three rules a host sets from the menu, and the six that are a player's own.
	CHECK(UiSettingsRows::Count(true) == 3);
	CHECK(UiSettingsRows::Count(false) == 6);
	CHECK(UiSettingsRows::Count(true) + UiSettingsRows::Count(false) == UiSettingsRows::TOGGLES.size());
	// The way to the rules and what the page says of them have their words.
	CHECK(EnglishOf("menu.session_options") == "Session options");
	CHECK(EnglishOf("menu.session_options_what").find("session you host") != std::string_view::npos);
}

TEST("settings rows: every switch writes its own ini line and flips the setting it shows")
{
	std::set<std::string> keys;
	std::set<std::string> names;
	for (const auto& toggle : UiSettingsRows::TOGGLES) {
		CHECK(keys.insert(toggle.key).second);
		CHECK(names.insert(toggle.name).second);
		CHECK(std::string_view{ toggle.onValue } != std::string_view{ toggle.offValue });
		// One switch moves one setting: turning it on changes nothing any other switch shows.
		Config::Settings off{};
		for (const auto& other : UiSettingsRows::TOGGLES) {
			off.*other.value = false;
		}
		auto on = off;
		on.*toggle.value = true;
		for (const auto& other : UiSettingsRows::TOGGLES) {
			CHECK((on.*other.value) == (&other == &toggle));
		}
	}
}
