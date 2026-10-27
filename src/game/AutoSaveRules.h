#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The mod's own autosaves, what the player is told a death does, and what the log says of each save and load, kept
// free of the game so it can be tested (AutoSave.cpp and Downed.cpp do what this answers).
//
// When the player dies, Fallout 4 loads the newest save of that character. In a session that save was often ten
// minutes old or more: the teleport key moves the player with coc or cow, which the engine does not count as travel,
// so the autosave a load door or fast travel makes never came. A bleed-out then took the player back that far, while
// the world, caught up from the session after the load, looked current: to the player, "the mod gave me an old save".
// So:
// - landing after a teleport into a friend's cell or worldspace, or across the worldspace both are in, is travel: an
//   autosave, when the player's own game setting for travel autosaves (bSaveOnTravel, Settings > Gameplay) is on;
// - with the host's bSaveOnDown, going down makes an autosave too, so a bleed-out loads the moment the player went
//   down (a save made while down loads fine: Downed::OnGameLoaded stands the player up as if helped);
// - never in Survival, where the game saves only in a bed, and none within MIN_GAP of the last save of any kind, so a
//   string of teleports does not stop the game for a save each time.
namespace AutoSaveRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto MIN_GAP = 60s;      // no autosave of ours within this long of the last save began
	inline constexpr auto ANSWER_TIME = 10s;  // a save that begins within this long of our request is ours
	// A move within the worldspace the player is in (no coc or cow, no loading screen): the autosave waits this long,
	// for the cells around the new place to load first.
	inline constexpr auto LANDING_SETTLE = 3s;
	inline constexpr float TRAVEL_DISTANCE = 4096.0f;  // an exterior cell: a move within one worldspace this far is travel

	// Whether a teleport to a friend is travel, as a load door or fast travel would be: into another cell or worldspace
	// (a coc or cow), or across the worldspace both are in, farther than an exterior cell (most teleports: both outside).
	// A hop within one interior, or to a friend a few steps away outside, is not.
	constexpr bool TeleportIsTravel(bool a_sameSpace, bool a_interior, float a_distance)
	{
		return !a_sameSpace || (!a_interior && a_distance >= TRAVEL_DISTANCE);
	}

	enum class Why
	{
		kNone,      // not one of ours: the player's, or the game's own
		kTeleport,  // landing after the teleport key's coc or cow
		kDown,      // going down, with the host's bSaveOnDown
	};

	// Whose a save is, for the log.
	constexpr std::string_view WhyName(Why a_why)
	{
		switch (a_why) {
		case Why::kTeleport:
			return "the mod's autosave after a teleport";
		case Why::kDown:
			return "the mod's autosave on going down";
		default:
			return "the player's or the game's";
		}
	}

	// What the game shows when an autosave of ours could be made.
	struct Facts
	{
		bool survival = false;      // Survival difficulty: the game saves only in a bed
		bool saveOnTravel = false;  // the player's own setting for travel autosaves (bSaveOnTravel)
		bool saveOnDown = false;    // the host's rule (bSaveOnDown)
	};

	// Whether the game should make an autosave for a_why, by its settings and the rules (the gap is the Book's).
	constexpr bool Wanted(Why a_why, const Facts& a_facts)
	{
		if (a_facts.survival) {
			return false;
		}
		switch (a_why) {
		case Why::kTeleport:
			return a_facts.saveOnTravel;
		case Why::kDown:
			return a_facts.saveOnDown;
		default:
			return false;
		}
	}

	enum class Asked
	{
		kYes,        // ask the game for an autosave now
		kNotWanted,  // the setting or the rule is off, or Survival
		kTooSoon,    // a save began within MIN_GAP
		kWaiting,    // one asked for a moment ago has not begun yet
	};

	// The saves this game began, and the autosave of ours asked for and not begun yet.
	class Book
	{
	public:
		Asked Ask(Why a_why, const Facts& a_facts, Clock::time_point a_now)
		{
			if (!Wanted(a_why, a_facts)) {
				return Asked::kNotWanted;
			}
			if (asked != Why::kNone && a_now - askedAt < ANSWER_TIME) {
				return Asked::kWaiting;
			}
			if (lastSave && a_now - *lastSave < MIN_GAP) {
				++tooSoon;
				return Asked::kTooSoon;
			}
			asked = a_why;
			askedAt = a_now;
			++asks;
			return Asked::kYes;
		}

		// A save begins: ours (and why), when we asked for one within ANSWER_TIME; otherwise the player's or the game's.
		Why Began(Clock::time_point a_now)
		{
			lastSave = a_now;
			const auto ours = asked != Why::kNone && a_now - askedAt < ANSWER_TIME ? asked : Why::kNone;
			asked = Why::kNone;
			if (ours != Why::kNone) {
				++made;
			}
			return ours;
		}

		// Our request that the game answered with no save within ANSWER_TIME, once (for the log); kNone otherwise.
		Why Unanswered(Clock::time_point a_now)
		{
			if (asked == Why::kNone || a_now - askedAt < ANSWER_TIME) {
				return Why::kNone;
			}
			++unanswered;
			return std::exchange(asked, Why::kNone);
		}

		// How long ago the last save began; nullopt before the first.
		[[nodiscard]] std::optional<Clock::duration> SinceSave(Clock::time_point a_now) const
		{
			return lastSave ? std::optional<Clock::duration>{ a_now - *lastSave } : std::nullopt;
		}

		std::uint32_t asks = 0;        // autosaves asked for
		std::uint32_t made = 0;        // of those, begun
		std::uint32_t unanswered = 0;  // of those, never begun
		std::uint32_t tooSoon = 0;     // not asked for: a save had just been made

	private:
		std::optional<Clock::time_point> lastSave;
		Why                              asked = Why::kNone;
		Clock::time_point                askedAt{};
	};

	// A save's name as the log may hold it. The engine names a save
	// "<kind><n>_<character ID>_<0 or 1>_<character name in hex>_<location>_<play time>_<date and time>_<n>_<n>" (with
	// ".fos" on its file): the character's name is the player's own choice, often their own name, so the log keeps the
	// kind, the location and the date only: "Autosave3 (Concord, 2026-10-26 21:14:05)". a_raw is F4SE's message data, a
	// name, a file name or a whole path, maybe ending in NULs. Of a name in another form only its length is told.
	inline std::string Label(std::string_view a_raw)
	{
		auto name = a_raw.substr(0, a_raw.find('\0'));
		if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
			name.remove_prefix(slash + 1);
		}
		if (name.size() >= 4) {
			auto extension = std::string{ name.substr(name.size() - 4) };
			std::ranges::transform(extension, extension.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			if (extension == ".fos") {
				name.remove_suffix(4);
			}
		}
		if (name.empty()) {
			return "(no name)";
		}
		std::vector<std::string_view> parts;
		for (std::size_t start = 0;;) {
			const auto end = name.find('_', start);
			parts.push_back(name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
			if (end == std::string_view::npos) {
				break;
			}
			start = end + 1;
		}
		const auto all = [](std::string_view a_part, auto a_test) {
			return !a_part.empty() && std::ranges::all_of(a_part, [&](char a_c) { return a_test(static_cast<unsigned char>(a_c)) != 0; });
		};
		const auto hex = [](unsigned char a_c) { return std::isxdigit(a_c); };
		const auto digit = [](unsigned char a_c) { return std::isdigit(a_c); };
		// The kind: Save, Autosave, Quicksave or Exitsave, and its number. A name in another form (another mod's) could
		// hold anything, the player's name too, so none of it is logged.
		const auto kind = parts[0];
		const auto letters = kind.substr(0, kind.find_first_of("0123456789"));
		const auto number = kind.substr(letters.size());
		std::string lower{ letters };
		std::ranges::transform(lower, lower.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
		const bool known = (lower == "save" || lower == "autosave" || lower == "quicksave" || lower == "exitsave") && (number.empty() || all(number, digit));
		if (!known || parts.size() < 9 || !all(parts[1], hex) || !all(parts[3], hex)) {
			return std::format("a save named in another form ({} characters)", name.size());
		}
		const auto when = parts[parts.size() - 3];
		std::string location;
		for (std::size_t i = 4; i + 4 < parts.size(); ++i) {
			location += (location.empty() ? "" : "_") + std::string{ parts[i] };
		}
		if (location.empty()) {
			location = "no location";
		}
		if (when.size() != 14 || !all(when, digit)) {
			return std::format("{} ({})", kind, location);
		}
		return std::format("{} ({}, {}-{}-{} {}:{}:{})", kind, location, when.substr(0, 4), when.substr(4, 2), when.substr(6, 2), when.substr(8, 2),
			when.substr(10, 2), when.substr(12, 2));
	}

	// The player's own setting for travel autosaves, which the game keeps in Fallout4Prefs.ini under [MAIN] next to
	// bSaveOnRest, bSaveOnWait and bSaveOnPause ("bSaveOnTravel=1"), not under [SaveGame]. Its collections key a
	// setting "name:Section", and a lookup that guesses the section finds nothing: the first build asked for
	// "bSaveOnTravel:SaveGame", took the null for "off" and never made a travel autosave. So the setting is found by
	// its name in any section, [MAIN] first, and one that is not there at all is the game's default (on), not off.
	inline constexpr std::string_view TRAVEL_SETTING = "bSaveOnTravel";
	inline constexpr std::string_view TRAVEL_SECTION = "MAIN";
	inline constexpr bool             TRAVEL_DEFAULT = true;  // the game's own: all four bSaveOn... settings ship on

	// Which of a_keys ("name:Section" as the game keys an ini setting) is a_name: the one in a_section if there is one,
	// else the first in another section. Name and section compare without regard to case; a key with no section, or
	// only a name that merely starts with a_name, is not it. nullopt: no such setting.
	inline std::optional<std::size_t> FindSetting(const std::vector<std::string_view>& a_keys, std::string_view a_name, std::string_view a_section)
	{
		const auto same = [](std::string_view a_a, std::string_view a_b) {
			return std::ranges::equal(a_a, a_b, [](unsigned char a_x, unsigned char a_y) { return std::tolower(a_x) == std::tolower(a_y); });
		};
		std::optional<std::size_t> other;
		for (std::size_t i = 0; i < a_keys.size(); ++i) {
			const auto colon = a_keys[i].find(':');
			if (colon == std::string_view::npos || !same(a_keys[i].substr(0, colon), a_name)) {
				continue;
			}
			if (same(a_keys[i].substr(colon + 1), a_section)) {
				return i;
			}
			if (!other) {
				other = i;
			}
		}
		return other;
	}

	// What the player is told of what a death does (Fallout 4 loads their newest save, which in a session can be many
	// minutes old), at the moments it is near: they just went down and bleed out if nobody comes; they were helped up
	// for the last time for now, or into the host's cooldown, so the next lethal hit kills; or they joined a session
	// whose host turned revives off. The text key; empty, nothing to tell.
	struct Moment
	{
		bool wentDown = false;     // "You're down!"
		bool lastRevive = false;   // helped up, and that was the last revive for now
		int  cooldownSeconds = -1; // helped up, and for this long a lethal hit kills
		bool reviveOff = false;    // welcomed into a session with the host's bRevive off
	};

	constexpr std::string_view DeathWarning(const Moment& a_moment)
	{
		if (a_moment.wentDown) {
			return "down.bleed_out_loads_save";
		}
		if (a_moment.lastRevive || a_moment.cooldownSeconds > 0 || a_moment.reviveOff) {
			return "down.death_loads_save";
		}
		return {};
	}
}
