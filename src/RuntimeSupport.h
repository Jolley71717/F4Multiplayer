#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Which Fallout 4 the plugin is in, and what that game's F4SE offers. One DLL serves every runtime it supports, chosen
// at load: old-gen 1.10.163 (F4SE 0.6.23), next-gen 1.10.980/984 (F4SE 0.7.2; not supported yet) and the Anniversary
// update 1.11.x (F4SE 0.7.9). The game-side half (the Address Library lookups) is Runtime.h. No game needed.
namespace RuntimeSupport
{
	// The order of CommonLibF4's runtime index (REX::FModule::Runtime), and of the slots in REL::ID{ og, ng, ae }.
	enum class Kind : std::uint8_t
	{
		kOG = 0,
		kNG = 1,
		kAE = 2,
	};

	struct Version
	{
		std::uint16_t major = 0;
		std::uint16_t minor = 0;
		std::uint16_t build = 0;
		std::uint16_t sub = 0;

		constexpr auto operator<=>(const Version&) const = default;
	};

	// F4SE hands versions over packed: major in the top byte, then minor, then 12 bits of build and 4 of sub.
	constexpr Version Unpack(std::uint32_t a_packed)
	{
		return { static_cast<std::uint16_t>((a_packed >> 24) & 0xFF), static_cast<std::uint16_t>((a_packed >> 16) & 0xFF),
			static_cast<std::uint16_t>((a_packed >> 4) & 0xFFF), static_cast<std::uint16_t>(a_packed & 0xF) };
	}

	inline constexpr Version OLD_GEN{ 1, 10, 163, 0 };

	// The runtime a game version belongs to (the versions CommonLibF4 knows), or none.
	constexpr std::optional<Kind> KindOf(const Version& a_game)
	{
		constexpr std::array NEXT_GEN{ Version{ 1, 10, 980, 0 }, Version{ 1, 10, 984, 0 } };
		constexpr std::array ANNIVERSARY{ Version{ 1, 11, 137, 0 }, Version{ 1, 11, 159, 0 }, Version{ 1, 11, 169, 0 }, Version{ 1, 11, 191, 0 },
			Version{ 1, 11, 221, 0 }, Version{ 1, 11, 240, 0 } };
		if (a_game == OLD_GEN) {
			return Kind::kOG;
		}
		if (std::ranges::find(NEXT_GEN, a_game) != NEXT_GEN.end()) {
			return Kind::kNG;
		}
		if (std::ranges::find(ANNIVERSARY, a_game) != ANNIVERSARY.end()) {
			return Kind::kAE;
		}
		return std::nullopt;
	}

	constexpr std::string_view Name(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kOG:
			return "old-gen";
		case Kind::kNG:
			return "next-gen";
		default:
			return "Anniversary";
		}
	}

	// F4SE 0.6.x (old-gen) loads a plugin through F4SEPlugin_Query and never reads F4SEPlugin_Version; 0.7.x reads only
	// the version data. The query says yes on old-gen 1.10.163 only, never in the editor: the other runtimes are taken
	// or refused by the version data.
	constexpr bool QueryAccepts(const Version& a_game, bool a_editor)
	{
		return !a_editor && a_game == OLD_GEN;
	}

	// The save folder's name (My Games\<name>) comes from F4SE from 0.7.1 on; older ones have no such call, and their
	// game always uses "Fallout4".
	constexpr bool HasSaveFolderName(const Version& a_f4se)
	{
		return a_f4se >= Version{ 0, 7, 1, 0 };
	}

	inline constexpr std::string_view DEFAULT_SAVE_FOLDER = "Fallout4";

	// F4SE's task interface has AddTaskPermanent from F4SE 0.7.0 on. Its interface version does not say so: 0.6.23's is
	// 2 as well (F4SETaskInterface::kInterfaceVersion, with AddTask and AddUITask only), so calling AddTaskPermanent there
	// would jump through whatever follows its interface. Asked of F4SE's own version instead.
	constexpr bool HasPermanentTasks(const Version& a_f4se)
	{
		return a_f4se >= Version{ 0, 7, 0, 0 };
	}

	// One entry of F4SE's Address Library (format 0): an ID and its offset from the game's image, sorted by ID.
	struct Mapping
	{
		std::uint64_t id;
		std::uint64_t offset;
	};

	// The offset of a_id, or none when the table has no such ID. Exact: an ID missing from the middle of the table is
	// missing (the library's own lookup before the Dear-Modding fork handed back the next ID's offset), and 0 is never
	// an ID (it marks a runtime with no value in REL::ID{ og, ng, ae }).
	constexpr std::optional<std::uint64_t> Find(std::span<const Mapping> a_table, std::uint64_t a_id)
	{
		if (a_id == 0) {
			return std::nullopt;
		}
		const auto it = std::ranges::lower_bound(a_table, a_id, {}, &Mapping::id);
		if (it == a_table.end() || it->id != a_id) {
			return std::nullopt;
		}
		return it->offset;
	}
}
