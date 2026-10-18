#pragma once

#include "Protocol.h"
#include "Text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A joiner's mods compared with the host's, by name. Whether a joiner is let in is the hello's load order hash (the
// server's check, unchanged); this only tells the player what differs, since the mod can't download mods for them:
// the game loads plugins only at startup, and Nexus files can't be passed on. The server sends the session's load
// order (Protocol::LoadOrder) before refusing a joiner for theirs, before letting in one whose list differs only in
// plugins the host lets differ, and when a player asks (kRequestLoadOrder). Session.cpp compares it with its own and
// tells the player (a HUD notice, the F1 menu's Mods page, every name in the log). Free of the game, so it is unit
// tested (tests/ModDiffTests.cpp).
namespace ModDiff
{
	// A plugin of a load order (Protocol::ComparedLoadOrder: the full ones, Protocol::LIGHT_PLUGINS, the light ones).
	struct Plugin
	{
		std::string name;
		bool        light = false;
	};

	// A plugin both games load, but at another place among its kind, or as the other kind (light in one game, full in
	// the other): its form IDs differ either way.
	struct Moved
	{
		std::string name;              // as the host's list writes it
		bool        lightHere = false;
		bool        lightThere = false;  // in the host's game

		bool operator==(const Moved&) const = default;
	};

	struct Difference
	{
		std::vector<std::string> missing;  // in the host's game, not ours: in the host's order
		std::vector<std::string> extra;    // in ours, not the host's: in our order
		std::vector<Moved>       moved;    // in both, out of the host's order: in the host's order
		std::size_t              mine = 0;    // plugins compared of ours (the light mark left out)
		std::size_t              theirs = 0;  // and of the host's
		bool                     cut = false;  // the host's list was cut at Protocol::MAX_PLUGINS: ours was compared as far

		[[nodiscard]] std::size_t Count() const { return missing.size() + extra.size() + moved.size(); }
		[[nodiscard]] bool        Empty() const { return Count() == 0; }

		bool operator==(const Difference&) const = default;
	};

	namespace detail
	{
		inline std::string Lower(std::string_view a_name)
		{
			std::string out{ a_name };
			for (char& c : out) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return out;
		}

		// Which of a_a's entries are not in a longest common subsequence of a_a and a_b (both of distinct names).
		inline std::vector<bool> OutOfOrder(const std::vector<std::string>& a_a, const std::vector<std::string>& a_b)
		{
			const auto n = a_a.size();
			const auto m = a_b.size();
			// lcs[i][j]: the longest common subsequence of a_a[i..] and a_b[j..]. At most 512 by 512.
			std::vector<std::uint16_t> lcs((n + 1) * (m + 1), 0);
			const auto                 at = [&](std::size_t i, std::size_t j) -> std::uint16_t& { return lcs[i * (m + 1) + j]; };
			for (std::size_t i = n; i-- > 0;) {
				for (std::size_t j = m; j-- > 0;) {
					at(i, j) = a_a[i] == a_b[j] ? static_cast<std::uint16_t>(at(i + 1, j + 1) + 1) : (std::max)(at(i + 1, j), at(i, j + 1));
				}
			}
			std::vector<bool> out(n, true);
			for (std::size_t i = 0, j = 0; i < n && j < m;) {
				if (a_a[i] == a_b[j]) {
					out[i] = false;
					++i, ++j;
				} else if (at(i + 1, j) >= at(i, j + 1)) {
					++i;
				} else {
					++j;
				}
			}
			return out;
		}
	}

	// A load order's plugins with their kind. A name twice (no game loads one twice) counts once, where it first is.
	[[nodiscard]] inline std::vector<Plugin> Plugins(const std::vector<std::string>& a_order)
	{
		std::vector<Plugin>             out;
		std::unordered_set<std::string> seen;
		bool                            light = false;
		for (const auto& name : a_order) {
			if (name == Protocol::LIGHT_PLUGINS) {
				light = true;
			} else if (!name.empty() && seen.insert(detail::Lower(name)).second) {
				out.push_back({ name, light });
			}
		}
		return out;
	}

	// Our load order against the host's (both as hellos carry them: Protocol::ComparedLoadOrder), the plugins the host
	// lets differ (a_ignored: sIgnoredPlugins, which kLoadOrder carries) left out as the server leaves them out
	// (Protocol::SameLoadOrderIgnoring). Names in any case.
	// A plugin in both games is out of order when, among the other plugins of its kind both games have, it is not in
	// the longest run the two orders share (one moved plugin is one name, not every plugin it passed), or when it is
	// light in one game and full in the other.
	// A plugin the host lets differ is named only where it moves another plugin's form IDs: where a plugin both games
	// have, in its place in both, has another number of those of its kind before it in each game (one only the host
	// has is missing, one only we have extra, one both have but elsewhere out of order). One that loads after the rest
	// of its kind, or in its place in both games, is no difference.
	[[nodiscard]] inline Difference Compare(std::vector<std::string> a_mine, const std::vector<std::string>& a_hosts, const std::vector<std::string>& a_ignored = {})
	{
		Difference out;
		if (a_hosts.size() >= Protocol::MAX_PLUGINS && a_mine.size() > a_hosts.size()) {
			a_mine.resize(a_hosts.size());  // what the host's list left off isn't known: ours is compared as far
			out.cut = true;
		}
		const auto mine = Plugins(a_mine);
		const auto hosts = Plugins(a_hosts);
		out.mine = mine.size();
		out.theirs = hosts.size();

		std::unordered_set<std::string> ignored;
		for (const auto& name : a_ignored) {
			ignored.insert(detail::Lower(name));
		}
		std::unordered_map<std::string, const Plugin*> mineByName;
		std::unordered_map<std::string, const Plugin*> hostsByName;
		for (const auto& plugin : mine) {
			mineByName.emplace(detail::Lower(plugin.name), &plugin);
		}
		for (const auto& plugin : hosts) {
			hostsByName.emplace(detail::Lower(plugin.name), &plugin);
		}
		// What differs, by name in lower case: listed at the end in the host's order (ours for the extra ones).
		std::unordered_set<std::string> missing;
		std::unordered_set<std::string> extra;
		std::unordered_set<std::string> moved;
		for (const auto& [name, plugin] : hostsByName) {
			if (!ignored.contains(name) && !mineByName.contains(name)) {
				missing.insert(name);
			}
		}
		for (const auto& [name, plugin] : mineByName) {
			if (!ignored.contains(name) && !hostsByName.contains(name)) {
				extra.insert(name);
			}
		}

		for (const bool light : { false, true }) {
			// The plugins of this kind that both games load as this kind (the ones the host lets differ left out), in
			// each game's order; and for each, the ones the host lets differ of this kind that load before it there.
			using Before = std::unordered_map<std::string, std::vector<std::string>>;
			std::vector<std::string> theirs;
			std::vector<std::string> ours;
			Before                   ignoredBeforeThere;
			Before                   ignoredBeforeHere;
			const auto kept = [&](const std::vector<Plugin>& a_list, const std::unordered_map<std::string, const Plugin*>& a_other, std::vector<std::string>& a_order, Before& a_before) {
				std::vector<std::string> before;
				for (const auto& plugin : a_list) {
					if (plugin.light != light) {
						continue;
					}
					auto       name = detail::Lower(plugin.name);
					const auto other = a_other.find(name);
					if (ignored.contains(name)) {
						before.push_back(std::move(name));
					} else if (other != a_other.end() && other->second->light == light) {
						a_before.emplace(name, before);
						a_order.push_back(std::move(name));
					}
				}
			};
			kept(hosts, mineByName, theirs, ignoredBeforeThere);
			kept(mine, hostsByName, ours, ignoredBeforeHere);
			const auto outOfOrder = detail::OutOfOrder(theirs, ours);
			for (std::size_t i = 0; i < theirs.size(); ++i) {
				if (outOfOrder[i]) {
					moved.insert(theirs[i]);
				}
			}
			// A plugin in its place in both orders that is another place among its kind in each game: the ones the
			// host lets differ that are before it in one game and not the other moved it.
			for (std::size_t i = 0; i < theirs.size(); ++i) {
				const auto& there = ignoredBeforeThere.at(theirs[i]);
				const auto& here = ignoredBeforeHere.at(theirs[i]);
				if (outOfOrder[i] || there.size() == here.size()) {
					continue;
				}
				for (const auto& name : there) {
					if (std::ranges::find(here, name) == here.end()) {
						(mineByName.contains(name) ? moved : missing).insert(name);
					}
				}
				for (const auto& name : here) {
					if (std::ranges::find(there, name) == there.end()) {
						(hostsByName.contains(name) ? moved : extra).insert(name);
					}
				}
			}
		}
		// A plugin the host doesn't let differ that is light in one game and full in the other.
		for (const auto& [name, plugin] : hostsByName) {
			const auto here = mineByName.find(name);
			if (!ignored.contains(name) && here != mineByName.end() && here->second->light != plugin->light) {
				moved.insert(name);
			}
		}

		for (const auto& plugin : hosts) {
			const auto name = detail::Lower(plugin.name);
			if (missing.contains(name)) {
				out.missing.push_back(plugin.name);
			} else if (moved.contains(name)) {
				out.moved.push_back({ plugin.name, mineByName.at(name)->light, plugin.light });
			}
		}
		for (const auto& plugin : mine) {
			if (extra.contains(detail::Lower(plugin.name))) {
				out.extra.push_back(plugin.name);
			}
		}
		return out;
	}

	// What the log says of a comparison, in English, every name (plugin file names are not personal).
	[[nodiscard]] inline std::vector<std::string> LogLines(const Difference& a_diff)
	{
		const auto join = [](const auto& a_names, auto a_name) {
			std::string s;
			for (const auto& entry : a_names) {
				s += (s.empty() ? "" : ", ") + a_name(entry);
			}
			return s;
		};
		const auto plain = [](const std::string& a_name) { return a_name; };
		std::vector<std::string> lines;
		lines.push_back(std::format("the host's load order has {} plugins, ours {}: {}", a_diff.theirs, a_diff.mine,
			a_diff.Empty() ? std::string{ "the same mods in the same order" } : std::format("{} differ", a_diff.Count())));
		if (!a_diff.missing.empty()) {
			lines.push_back(std::format("missing on our side ({}): {}", a_diff.missing.size(), join(a_diff.missing, plain)));
		}
		if (!a_diff.extra.empty()) {
			lines.push_back(std::format("extra on our side ({}): {}", a_diff.extra.size(), join(a_diff.extra, plain)));
		}
		if (!a_diff.moved.empty()) {
			lines.push_back(std::format("in a different order ({}): {}", a_diff.moved.size(), join(a_diff.moved, [](const Moved& a_moved) {
				return a_moved.lightHere == a_moved.lightThere ? a_moved.name : std::format("{} ({} here, {} for the host)", a_moved.name, a_moved.lightHere ? "light" : "full", a_moved.lightThere ? "light" : "full");
			})));
		}
		if (a_diff.cut) {
			lines.push_back(std::format("the host's list stops at {} plugins: ours was compared as far", Protocol::MAX_PLUGINS));
		}
		return lines;
	}

	// Names on one page of the F1 menu's Mods page (the game's message box shows a dozen lines or so).
	inline constexpr std::size_t NAMES_PER_PAGE = 10;

	// The Mods page's texts, in the player's language: an introduction, then each list under its heading, NAMES_PER_PAGE
	// names a page (a list that goes on to the next page has its heading again there). At least one page.
	[[nodiscard]] inline std::vector<std::string> Pages(const Difference& a_diff, std::size_t a_perPage = NAMES_PER_PAGE)
	{
		a_perPage = (std::max<std::size_t>)(a_perPage, 1);
		const auto intro = a_diff.Empty() ? Text::Get("mods.same") : Text::Format("mods.page_intro", { { "n", a_diff.Count() } });
		struct Line
		{
			std::string_view heading;
			std::string name;
		};
		std::vector<Line> lines;
		for (const auto& name : a_diff.missing) {
			lines.push_back({ "mods.missing", name });
		}
		for (const auto& name : a_diff.extra) {
			lines.push_back({ "mods.extra", name });
		}
		for (const auto& moved : a_diff.moved) {
			const auto name = moved.lightHere == moved.lightThere ? moved.name : Text::Format(moved.lightHere ? "mods.light_here" : "mods.full_here", { { "name", moved.name } });
			lines.push_back({ "mods.order", name });
		}
		std::vector<std::string> pages;
		for (std::size_t first = 0; first < lines.size() || pages.empty(); first += a_perPage) {
			std::string page = intro;
			std::string_view heading;
			for (std::size_t i = first; i < lines.size() && i < first + a_perPage; ++i) {
				if (lines[i].heading != heading) {
					heading = lines[i].heading;
					page += "\n\n" + Text::Get(heading);
				}
				page += "\n" + lines[i].name;
			}
			pages.push_back(std::move(page));
			if (lines.empty()) {
				break;
			}
		}
		return pages;
	}

	// A key as the player knows it: F1 for iKeyMenu's 0x70.
	[[nodiscard]] inline std::string KeyName(std::uint32_t a_vk)
	{
		return a_vk >= 0x70 && a_vk <= 0x7B ? std::format("F{}", a_vk - 0x6F) : std::format("key {:X}", a_vk);
	}

	// What the joining game knows of the host's mods, and when to tell the player. The server sends its list at every
	// hello that differs (a refused joiner tries again every minute), just before the refusal or the welcome. A refusal
	// is told as the difference (one message, at every refusal, as any refusal is). Otherwise the HUD hears of a
	// difference once the welcome is in, once, and again only when it changes, or when the player asked.
	class Tracker
	{
	public:
		using Clock = std::chrono::steady_clock;

		// The server answers a player's ask at most this often, counted from the last list it sent them (its
		// LOAD_ORDER_INTERVAL, common/Server.cpp): asked sooner, it says nothing, so we don't ask.
		static constexpr auto ASK_INTERVAL = std::chrono::seconds(5);
		// An ask unanswered this long was lost (or dropped): the next list is no longer its answer.
		static constexpr auto ASK_TIMEOUT = std::chrono::seconds(10);

		// Our hello went out on a new connection.
		void OnConnected()
		{
			listThisConnection = false;
			in = false;
			pending = false;
			asked = false;
			lastList.reset();
			lastAsk.reset();
		}

		// The connection is gone: an ask on it gets no answer.
		void OnDisconnected()
		{
			in = false;
			pending = false;
			asked = false;
		}

		// The player wants to ask the host for its list (kRequestLoadOrder): whether to send the ask now (false: one
		// is out, or the server would not answer yet; ASK_INTERVAL). Its answer is told whatever it says.
		bool OnAsk(Clock::time_point a_now)
		{
			Expire(a_now);
			const auto recent = [&](const std::optional<Clock::time_point>& a_when) { return a_when && a_now - *a_when < ASK_INTERVAL; };
			if (asked || recent(lastList) || recent(lastAsk)) {
				return false;
			}
			asked = true;
			lastAsk = a_now;
			return true;
		}

		// The host's list came, compared with ours: whether to tell the player now (before the welcome or a refusal,
		// not yet: OnWelcome, OnRefused).
		bool OnList(Difference a_diff, Clock::time_point a_now)
		{
			Expire(a_now);
			listThisConnection = true;
			lastList = a_now;
			const bool tell = std::exchange(asked, false) || (!a_diff.Empty() && told != a_diff);
			if (tell) {
				told = a_diff;
			}
			known = std::move(a_diff);
			if (!in) {
				pending = tell;
				return false;
			}
			return tell;
		}

		// Let in: whether to tell the player of the list that came before the welcome now. Without a list on this
		// connection our load order is the host's (the server sends one only to a hello that differs): one from an
		// earlier connection is old news.
		bool OnWelcome()
		{
			in = true;
			if (!listThisConnection) {
				known.reset();
				told.reset();
			}
			return std::exchange(pending, false);
		}

		// Refused for our load order (Protocol::REJECT_LOAD_ORDER): whether the refusal is told as the difference the
		// list just before it names (Differing()), in place of the refusal's own text.
		bool OnRefused()
		{
			pending = false;
			return listThisConnection && Differing() > 0;
		}

		// Set to reach another server (or none): what we knew was the last host's.
		void Reset() { *this = Tracker{}; }

		[[nodiscard]] const std::optional<Difference>& Known() const { return known; }
		// Mods that differ from the host's, as far as known (0: none, or nothing known).
		[[nodiscard]] std::size_t Differing() const { return known ? known->Count() : 0; }
		// An ask is out, sent less than ASK_TIMEOUT ago.
		[[nodiscard]] bool Asking(Clock::time_point a_now) const { return asked && lastAsk && a_now - *lastAsk < ASK_TIMEOUT; }

	private:
		void Expire(Clock::time_point a_now) { asked = Asking(a_now); }

		std::optional<Difference>        known;
		std::optional<Difference>        told;
		std::optional<Clock::time_point> lastList;  // on this connection
		std::optional<Clock::time_point> lastAsk;   // on this connection
		bool                             listThisConnection = false;
		bool                             in = false;       // welcomed on this connection
		bool                             pending = false;  // the list before the welcome is news (OnWelcome)
		bool                             asked = false;
	};
}
