#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Form IDs across load orders (protocol 36). A form ID's top byte is its plugin's place among the full plugins of the
// game that loaded it (FE and the next 12 bits: its place among the light ones; FF: made at runtime), so two games with
// other mods, or the same mods in another order, give a mod's NPC or item another ID. The wire carries every form ID
// in the session's numbering instead: the session's plugin table (Protocol::PluginTable), the host's load order with
// the plugins later joiners brought added after it (Extend). Each game translates at its edge (FormTranslate.h): its
// own IDs to the session's on the way out, the session's to its own on the way in, by plugin file name. A form of a
// plugin the other side lacks has no ID there (nullopt), and what it was in is skipped. Free of the game, so it is unit
// tested (tests/FormIdsTests.cpp).
namespace FormIds
{
	inline constexpr std::uint32_t LIGHT_TOP = 0xFE;
	inline constexpr std::uint32_t RUNTIME_TOP = 0xFF;
	// Places a form ID can name: full plugins 00 to FD (FE and FF mean light and runtime), light ones 000 to FFF.
	inline constexpr std::size_t MAX_FULL = 0xFE;
	inline constexpr std::size_t MAX_LIGHT = 0x1000;
	// The part of a form ID its plugin numbers: 24 bits for a full plugin's, 12 for a light one's.
	inline constexpr std::uint32_t FULL_LOCAL = 0x00FFFFFF;
	inline constexpr std::uint32_t LIGHT_LOCAL = 0x00000FFF;

	// A plugin's place in a load order: among the full ones, or among the light ones.
	struct Place
	{
		bool          light = false;
		std::uint16_t index = 0;

		bool operator==(const Place&) const = default;
	};

	// The place a form ID's plugin has in the load order that gave it the ID; none for 0 and runtime forms (FF), which
	// no plugin numbers.
	[[nodiscard]] constexpr std::optional<Place> PlaceOf(std::uint32_t a_id)
	{
		const auto top = a_id >> 24;
		if (a_id == 0 || top == RUNTIME_TOP) {
			return std::nullopt;
		}
		if (top == LIGHT_TOP) {
			return Place{ true, static_cast<std::uint16_t>((a_id >> 12) & 0xFFF) };
		}
		return Place{ false, static_cast<std::uint16_t>(top) };
	}

	// The form's number within its plugin.
	[[nodiscard]] constexpr std::uint32_t LocalPart(std::uint32_t a_id)
	{
		return (a_id >> 24) == LIGHT_TOP ? (a_id & LIGHT_LOCAL) : (a_id & FULL_LOCAL);
	}

	// The form ID of a plugin's form a_local at a_place; none where no ID can say it: a light plugin numbers 12 bits
	// only (a plugin full in one game and light in the other, with a form past them), and the places run out.
	[[nodiscard]] constexpr std::optional<std::uint32_t> Compose(Place a_place, std::uint32_t a_local)
	{
		if (a_place.light) {
			if (a_place.index >= MAX_LIGHT || a_local > LIGHT_LOCAL) {
				return std::nullopt;
			}
			return (LIGHT_TOP << 24) | (static_cast<std::uint32_t>(a_place.index) << 12) | a_local;
		}
		if (a_place.index >= MAX_FULL || a_local > FULL_LOCAL) {
			return std::nullopt;
		}
		return (static_cast<std::uint32_t>(a_place.index) << 24) | a_local;
	}

	// A load order split into its kinds, from the layout hellos carry (Protocol::ComparedLoadOrder: the full plugins,
	// Protocol::LIGHT_PLUGINS, the light ones; a list without the mark is all full). A name twice (no game loads one
	// twice) counts where it first is; past the places a form ID can name, the rest has none.
	struct Table
	{
		std::vector<std::string> full;
		std::vector<std::string> light;

		[[nodiscard]] static Table Of(const std::vector<std::string>& a_order)
		{
			Table out;
			bool  light = false;
			for (const auto& name : a_order) {
				if (name == Protocol::LIGHT_PLUGINS) {
					light = true;
				} else if (!name.empty() && !out.Find(name)) {
					auto& kind = light ? out.light : out.full;
					if (kind.size() < (light ? MAX_LIGHT : MAX_FULL)) {
						kind.push_back(name);
					}
				}
			}
			return out;
		}

		// Back to the hello layout.
		[[nodiscard]] std::vector<std::string> Order() const
		{
			std::vector<std::string> out = full;
			out.emplace_back(Protocol::LIGHT_PLUGINS);
			out.insert(out.end(), light.begin(), light.end());
			return out;
		}

		[[nodiscard]] bool Empty() const { return full.empty() && light.empty(); }
		[[nodiscard]] std::size_t Size() const { return full.size() + light.size(); }

		// A plugin's place, by name in any case.
		[[nodiscard]] std::optional<Place> Find(std::string_view a_name) const
		{
			for (std::size_t i = 0; i < full.size(); ++i) {
				if (Protocol::SamePluginName(full[i], a_name)) {
					return Place{ false, static_cast<std::uint16_t>(i) };
				}
			}
			for (std::size_t i = 0; i < light.size(); ++i) {
				if (Protocol::SamePluginName(light[i], a_name)) {
					return Place{ true, static_cast<std::uint16_t>(i) };
				}
			}
			return std::nullopt;
		}

		// The plugin at a place, or nothing.
		[[nodiscard]] const std::string* At(Place a_place) const
		{
			const auto& kind = a_place.light ? light : full;
			return a_place.index < kind.size() ? &kind[a_place.index] : nullptr;
		}

		// The same plugins in the same places, names in any case.
		[[nodiscard]] bool Same(const Table& a_other) const
		{
			const auto same = [](const std::vector<std::string>& a_a, const std::vector<std::string>& a_b) {
				return a_a.size() == a_b.size() && std::ranges::equal(a_a, a_b, [](const std::string& a, const std::string& b) { return Protocol::SamePluginName(a, b); });
			};
			return same(full, a_other.full) && same(light, a_other.light);
		}
	};

	// One game's form IDs and the session's, both ways. Without both tables (the session's not come yet, or a peer that
	// sent no list) it is the identity, as before protocol 36; with the same plugins in the same places too, and then
	// nothing is translated at all.
	class Mapper
	{
	public:
		Mapper() = default;

		Mapper(const std::vector<std::string>& a_local, const std::vector<std::string>& a_session) :
			local(Table::Of(a_local)),
			session(Table::Of(a_session))
		{
			if (local.Empty() || session.Empty() || local.Same(session)) {
				return;
			}
			identity = false;
			const auto link = [](const Table& a_from, const Table& a_to, std::vector<std::optional<Place>>& a_full, std::vector<std::optional<Place>>& a_light) {
				for (const auto& name : a_from.full) {
					a_full.push_back(a_to.Find(name));
				}
				for (const auto& name : a_from.light) {
					a_light.push_back(a_to.Find(name));
				}
			};
			link(local, session, localFull, localLight);
			link(session, local, sessionFull, sessionLight);
		}

		// Nothing to translate: every form ID is the same in both numberings.
		[[nodiscard]] bool Identity() const { return identity; }

		// Ours as the session numbers it; none for a plugin the session's table lacks.
		[[nodiscard]] std::optional<std::uint32_t> ToSession(std::uint32_t a_id) const
		{
			return identity ? std::optional{ a_id } : Map(a_id, localFull, localLight);
		}

		// The session's as we number it; none for a plugin our game lacks.
		[[nodiscard]] std::optional<std::uint32_t> ToLocal(std::uint32_t a_id) const
		{
			return identity ? std::optional{ a_id } : Map(a_id, sessionFull, sessionLight);
		}

		// The plugin a form ID of ours, or of the session's, is from (for the log): empty when unknown (0, runtime, or a
		// place the table has no plugin at).
		[[nodiscard]] std::string LocalPlugin(std::uint32_t a_id) const { return NameIn(local, a_id); }
		[[nodiscard]] std::string SessionPlugin(std::uint32_t a_id) const { return NameIn(session, a_id); }

		// Plugins of ours the session's table lacks (their forms are not shared), and the session's that we lack.
		[[nodiscard]] std::size_t OursOnly() const { return Count(localFull) + Count(localLight); }
		[[nodiscard]] std::size_t SessionsOnly() const { return Count(sessionFull) + Count(sessionLight); }
		[[nodiscard]] const Table& Local() const { return local; }
		[[nodiscard]] const Table& Session() const { return session; }

	private:
		static std::optional<std::uint32_t> Map(std::uint32_t a_id, const std::vector<std::optional<Place>>& a_full, const std::vector<std::optional<Place>>& a_light)
		{
			const auto place = PlaceOf(a_id);
			if (!place) {
				return a_id;  // 0 and runtime forms (and spawn ids) are nobody's plugin's: the same everywhere
			}
			const auto& kind = place->light ? a_light : a_full;
			if (place->index >= kind.size() || !kind[place->index]) {
				return std::nullopt;
			}
			return Compose(*kind[place->index], LocalPart(a_id));
		}

		static std::string NameIn(const Table& a_table, std::uint32_t a_id)
		{
			const auto place = PlaceOf(a_id);
			const auto name = place ? a_table.At(*place) : nullptr;
			return name ? *name : std::string{};
		}

		static std::size_t Count(const std::vector<std::optional<Place>>& a_links)
		{
			return static_cast<std::size_t>(std::ranges::count_if(a_links, [](const std::optional<Place>& a_link) { return !a_link; }));
		}

		Table                             local;
		Table                             session;
		std::vector<std::optional<Place>> localFull;  // our full plugin i's place in the session's table
		std::vector<std::optional<Place>> localLight;
		std::vector<std::optional<Place>> sessionFull;  // the session's full plugin i's place in ours
		std::vector<std::optional<Place>> sessionLight;
		bool                              identity = true;
	};

	// Adds the plugins of a_order (a hello's list) that a session's table (the same layout) lacks, each after the rest
	// of its kind, so the IDs the table gave before stay what they were. As far as places last (MAX_FULL, MAX_LIGHT, and
	// Protocol::MAX_PLUGINS names in all, the mark included). How many were added.
	inline std::size_t Extend(std::vector<std::string>& a_table, const std::vector<std::string>& a_order)
	{
		auto       table = Table::Of(a_table);
		const auto theirs = Table::Of(a_order);
		std::size_t added = 0;
		const auto  add = [&](const std::vector<std::string>& a_names, std::vector<std::string>& a_kind, std::size_t a_max) {
			for (const auto& name : a_names) {
				if (!table.Find(name) && a_kind.size() < a_max && table.Size() + 2 < Protocol::MAX_PLUGINS) {
					a_kind.push_back(name);
					++added;
				}
			}
		};
		add(theirs.full, table.full, MAX_FULL);
		add(theirs.light, table.light, MAX_LIGHT);
		if (added != 0 || a_table.empty() != table.Empty()) {
			a_table = table.Order();
		}
		return added;
	}
}
