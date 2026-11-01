#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// One roll per leveled NPC for the whole session, the same in every game, made before any game builds the NPC
// (docs/SHARED-ROLLS.md).
//
// Fallout 4 puts a leveled NPC together the first time a game needs it: a raider placed from a leveled list is rolled at
// that game's player's level, and so is its chance of being legendary. Five players walking into one place made five
// rolls, and a report from the game that runs an NPC came seconds after every other game had built its own. Here the
// roll is a pure function of the session (its id, kept in the session file), the reference (its plugin and its id there,
// the same in every game whatever the load order) and the session's NPC level: every game makes the same one without
// waiting for anybody. The engine's own roll (TESActorBaseData's resolver, 1.11.240 +309B10) is let run and its result
// replaced, slot by slot, before the NPC is put together from it (src/game/RollHook.cpp).
//
// Pick is the engine's resolver over the game's data as Look gives it, with our dice: the legendary chance first (the
// NPC's legendary chance global, times the difficulty's multiplier; the engine's cooldown between legendaries is not
// kept), then each of the 13 template slots from its source (the legendary template for every slot of a legendary, else
// the slot's own template or the default one, where the slot's use flag is set), a leveled list rolled down to an NPC.
// What it cannot roll whole (a list with nothing at that level, an entry that is no NPC) is nothing: the engine's own
// roll stands then.
//
// Pure, so it can be tested without the game: tests/SharedRollTests.cpp.
namespace SharedRoll
{
	using Templates = std::array<std::uint32_t, Protocol::IDENTITY_TEMPLATES>;

	// ---- Seeds ---------------------------------------------------------------------------------------------------

	[[nodiscard]] constexpr std::uint64_t Mix(std::uint64_t a_x)
	{
		a_x += 0x9E3779B97F4A7C15ull;
		a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
		a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
		return a_x ^ (a_x >> 31);
	}

	[[nodiscard]] constexpr std::uint64_t Combine(std::uint64_t a_seed, std::uint64_t a_value)
	{
		return Mix(a_seed ^ Mix(a_value));
	}

	// A form's name in every game: its plugin's file name (any case) and its id within that plugin, whatever the load
	// order puts the plugin at. Form IDs on the wire are the session's (common/FormIds.h), but a hook on the engine's
	// thread has only ours; this needs no table.
	[[nodiscard]] constexpr std::uint64_t PluginHash(std::string_view a_plugin)
	{
		std::uint64_t hash = 0xCBF29CE484222325ull;
		for (const char c : a_plugin) {
			const auto lower = static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
			hash = (hash ^ lower) * 0x100000001B3ull;
		}
		return hash;
	}

	[[nodiscard]] constexpr std::uint64_t StableKey(std::uint64_t a_pluginHash, std::uint32_t a_localId)
	{
		return Combine(a_pluginHash, a_localId);
	}

	[[nodiscard]] constexpr std::uint64_t StableKey(std::string_view a_plugin, std::uint32_t a_localId)
	{
		return StableKey(PluginHash(a_plugin), a_localId);
	}

	// A form ID's plugin slot and its id within that plugin: a light plugin's (FE) by its 12-bit index and 12-bit id, a
	// full one's by its byte and 24-bit id. Nothing for a runtime form (FF).
	struct Local
	{
		bool          light = false;
		std::uint32_t index = 0;  // the load order's byte, or a light plugin's index
		std::uint32_t id = 0;
	};

	[[nodiscard]] constexpr std::optional<Local> LocalOf(std::uint32_t a_formId)
	{
		const auto top = a_formId >> 24;
		if (top == 0xFF) {
			return std::nullopt;
		}
		if (top == 0xFE) {
			return Local{ true, (a_formId >> 12) & 0xFFF, a_formId & 0xFFF };
		}
		return Local{ false, top, a_formId & 0xFFFFFF };
	}

	// What is rolled, keyed: a plugin's reference by its own key; a runtime one (a random encounter's raider, an ambush:
	// every game's quest makes its own at the same markers) by the NPC it was placed as, the cell or worldspace, where it
	// stands on a SPOT_GRID and the how-many-th of the same there within ORDINAL_WINDOW (a group placed together at one
	// marker), so two games' raiders at one marker are each other's roll.
	inline constexpr float SPOT_GRID = 256.0f;
	inline constexpr float MAX_SPOT = 1.0e6f;

	[[nodiscard]] inline std::optional<std::uint64_t> SpotKey(std::uint64_t a_original, std::uint64_t a_place, float a_x, float a_y)
	{
		if (!std::isfinite(a_x) || !std::isfinite(a_y) || std::fabs(a_x) > MAX_SPOT || std::fabs(a_y) > MAX_SPOT || (a_x == 0.0f && a_y == 0.0f)) {
			return std::nullopt;  // not placed yet: no spot to agree on
		}
		const auto gx = static_cast<std::int64_t>(std::floor(a_x / SPOT_GRID));
		const auto gy = static_cast<std::int64_t>(std::floor(a_y / SPOT_GRID));
		return Combine(Combine(Combine(a_original, a_place), static_cast<std::uint64_t>(gx)), static_cast<std::uint64_t>(gy));
	}

	// The seed of one roll: the session's, the reference's key, and an ordinal: for a plugin's reference, its life in the
	// session (how many times it died: the server's count, kRollLives), so one a cell reset brings back is another roll;
	// for a runtime one, the how-many-th of a group at one spot (Ordinals).
	[[nodiscard]] constexpr std::uint64_t SeedOf(std::uint64_t a_session, std::uint64_t a_key, std::uint32_t a_ordinal)
	{
		return Combine(Combine(a_session, a_key), a_ordinal);
	}

	// Our dice: the same seed rolls the same in every game (and every build: no std:: distribution, whose results the
	// standard leaves to the library).
	class Rng
	{
	public:
		explicit constexpr Rng(std::uint64_t a_seed) :
			state(a_seed)
		{}

		constexpr std::uint64_t Next()
		{
			state += 0x9E3779B97F4A7C15ull;
			auto z = state;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}

		// In [0, a_n); 0 for none.
		constexpr std::uint32_t Below(std::uint32_t a_n)
		{
			return a_n == 0 ? 0 : static_cast<std::uint32_t>(((Next() >> 32) * a_n) >> 32);
		}

	private:
		std::uint64_t state;
	};

	// ---- The game's data, as Pick sees it ----------------------------------------------------------------------

	struct Entry
	{
		std::uint32_t form = 0;
		std::uint16_t level = 1;
	};

	struct Form
	{
		enum class Kind
		{
			kNpc,
			kList,   // a leveled NPC list
			kOther,  // anything else: not something a slot can be
		};
		Kind kind = Kind::kOther;

		// An NPC (TESActorBaseData).
		std::uint16_t useFlags = 0;      // TEMPLATE_USE_FLAG: the slots its templates fill
		std::uint32_t baseTemplate = 0;  // the default template
		Templates     slotTemplates{};   // a slot's own template, 0 for the default
		std::uint32_t legendTemplate = 0;
		float         legendChance = 0;  // percent: its legendary chance global's value (0 without one)
		bool          eachSlot = false;  // ACTOR_BASE_DATA flag 1 << 9: each slot rolls its list on its own (the engine rolls a list once per resolve otherwise)

		// A leveled list (TESLeveledList).
		std::vector<Entry> entries;
		std::uint8_t       listFlags = 0;  // kAllLevels: every entry up to the level, not only the highest level's
	};

	inline constexpr std::uint8_t ALL_LEVELS = 1 << 0;  // "calculate from all levels <= the player's level"

	// How the reference's level modifier (ExtraLevCreaModifier) asks the lists to be read: as each list says, every entry
	// up to the level always, or never (the engine's LeveledListAllBelowForce).
	enum class AllBelow : std::int8_t
	{
		kNever = -1,
		kDefault = 0,
		kAlways = 1,
	};

	inline constexpr std::size_t MAX_DEPTH = 8;     // lists within lists (vanilla nests two or three)
	inline constexpr std::uint16_t MIN_LEVEL = 1;
	inline constexpr std::uint16_t MAX_LEVEL = Protocol::MAX_IDENTITY_LEVEL;

	struct Picked
	{
		Templates     templates{};
		std::uint16_t mask = 0;
		bool          legendary = false;
	};

	enum class Miss
	{
		kNone,
		kNotLeveled,  // the NPC has no template slot to fill, or isn't an NPC here
		kEmptyList,   // a list gave nothing at that level
		kNotNpc,      // a list gave something that isn't an NPC
		kTooDeep,     // lists nested past MAX_DEPTH
	};

	struct Result
	{
		std::optional<Picked> picked;
		Miss                  miss = Miss::kNone;
	};

	namespace detail
	{
		template <class Look>
		std::optional<std::uint32_t> RollList(std::uint32_t a_list, std::uint16_t a_level, AllBelow a_allBelow, Rng& a_rng, Look& a_look, std::size_t a_depth, Miss& a_miss)
		{
			if (a_depth >= MAX_DEPTH) {
				a_miss = Miss::kTooDeep;
				return std::nullopt;
			}
			const std::optional<Form> list = a_look(a_list);
			if (!list || list->kind != Form::Kind::kList) {
				a_miss = Miss::kNotNpc;
				return std::nullopt;
			}
			const bool             all = a_allBelow == AllBelow::kAlways || (a_allBelow == AllBelow::kDefault && (list->listFlags & ALL_LEVELS));
			std::uint16_t          top = 0;
			bool                   any = false;
			for (const auto& entry : list->entries) {
				if (entry.form != 0 && entry.level <= a_level) {
					top = any ? (std::max)(top, entry.level) : entry.level;
					any = true;
				}
			}
			if (!any) {
				a_miss = Miss::kEmptyList;
				return std::nullopt;
			}
			std::vector<std::uint32_t> candidates;
			for (const auto& entry : list->entries) {
				if (entry.form != 0 && entry.level <= a_level && (all || entry.level == top)) {
					candidates.push_back(entry.form);
				}
			}
			const auto chosen = candidates[a_rng.Below(static_cast<std::uint32_t>(candidates.size()))];
			const std::optional<Form> form = a_look(chosen);
			if (!form || form->kind == Form::Kind::kOther) {
				a_miss = Miss::kNotNpc;
				return std::nullopt;
			}
			if (form->kind == Form::Kind::kList) {
				return RollList(chosen, a_level, a_allBelow, a_rng, a_look, a_depth + 1, a_miss);
			}
			return chosen;
		}
	}

	// The levels the lists are read at (ListLevel): a plain roll's with the reference's modifier and how it reads the lists,
	// a legendary one's without them (the engine reads no modifier for a legendary roll).
	struct Levels
	{
		std::uint16_t plain = 1;
		std::uint16_t legendary = 1;
		AllBelow      allBelow = AllBelow::kDefault;
		bool          legendaryAllowed = true;  // the engine makes none of a reference dead or deleted as it rolls
	};

	inline constexpr std::uint64_t LEGENDARY_STREAM = 0x4C4547454E440000ull;  // "LEGEND": the legendary draw's dice

	// a_original: the NPC the reference was placed as. a_legendaryMultiplier: the difficulty's (1 on Normal). a_look(id):
	// the Form, or nothing for one that isn't there.
	template <class Look>
	Result Pick(std::uint32_t a_original, const Levels& a_levels, std::uint64_t a_seed, float a_legendaryMultiplier, Look a_look)
	{
		Result                    result;
		const std::optional<Form> npc = a_look(a_original);
		if (!npc || npc->kind != Form::Kind::kNpc || (npc->useFlags & ((1u << Protocol::IDENTITY_TEMPLATES) - 1)) == 0) {
			result.miss = Miss::kNotLeveled;
			return result;
		}
		Rng rng{ a_seed };

		// The legendary chance, on dice of its own (the seed and the level the lists are read at for a legendary): the slots
		// roll the same whether or not the NPC can be one, and the draw is not one number for every level.
		Rng        legendRng{ Combine(a_seed, LEGENDARY_STREAM ^ a_levels.legendary) };
		const auto draw = legendRng.Below(100);
		const auto chance = npc->legendChance * (std::isfinite(a_legendaryMultiplier) ? a_legendaryMultiplier : 1.0f);
		Picked     picked;
		picked.legendary = a_levels.legendaryAllowed && npc->legendTemplate != 0 && npc->legendChance > 0.0f && static_cast<float>(draw) < chance;
		const auto level = std::clamp<std::uint16_t>(picked.legendary ? a_levels.legendary : a_levels.plain, MIN_LEVEL, MAX_LEVEL);
		const auto allBelow = picked.legendary ? AllBelow::kDefault : a_levels.allBelow;

		std::unordered_map<std::uint32_t, std::uint32_t> rolled;  // a list rolled once per resolve, as the engine does
		const bool                                       cache = !npc->eachSlot || picked.legendary;
		for (std::size_t slot = 0; slot < Protocol::IDENTITY_TEMPLATES; ++slot) {
			std::uint32_t source = 0;
			if (picked.legendary) {
				source = npc->legendTemplate;
			} else if (npc->useFlags & (1u << slot)) {
				source = npc->slotTemplates[slot] != 0 ? npc->slotTemplates[slot] : npc->baseTemplate;
			}
			if (source == 0) {
				continue;
			}
			const std::optional<Form> form = a_look(source);
			if (!form || form->kind == Form::Kind::kOther) {
				result.miss = Miss::kNotNpc;
				return result;
			}
			std::uint32_t out = source;
			if (form->kind == Form::Kind::kList) {
				if (const auto it = rolled.find(source); cache && it != rolled.end()) {
					out = it->second;
				} else {
					const auto got = detail::RollList(source, level, allBelow, rng, a_look, 0, result.miss);
					if (!got) {
						return result;
					}
					out = *got;
					if (cache) {
						rolled.emplace(source, out);
					}
				}
			}
			picked.templates[slot] = out;
			picked.mask = static_cast<std::uint16_t>(picked.mask | (1u << slot));
		}
		if (picked.mask == 0) {
			result.miss = Miss::kNotLeveled;
			return result;
		}
		result.picked = picked;
		return result;
	}

	// An encounter zone's bounds (BGSEncounterZone's data: min +0x31, its flags +0x32, max +0x33) and the game's scale above
	// the minimum (the float the engine's zone level multiplies by, read from its code: RollHook).
	struct Zone
	{
		std::uint8_t min = 0;
		std::uint8_t max = 0;               // 0: none
		bool         matchBelowMin = false;  // ENCOUNTER_ZONE_DATA kMatchPCBelowMin: a player below the minimum is matched
		float        scale = 1.0f;
	};

	// The level a list is read at for a reference: the session's, through its encounter zone as the engine takes the
	// player's (1.11.240 +494EC0: below the minimum, the minimum unless the zone matches the player; above it, the minimum
	// plus the rest times the scale; at most the maximum), then times the reference's level modifier (an "easy" or a
	// "boss" placement, ExtraLevCreaModifier), at least 1. Not the engine's: a zone's level locked at a player's first
	// visit (each game's own), and the small variance the engine cycles through (each game's own count).
	[[nodiscard]] inline std::uint16_t ListLevel(std::uint16_t a_session, float a_modifier, const std::optional<Zone>& a_zone)
	{
		int level = a_session;
		if (a_zone) {
			const int min = a_zone->min;
			if (level < min) {
				level = a_zone->matchBelowMin ? level : min;
			} else if (std::isfinite(a_zone->scale) && a_zone->scale > 0.0f) {
				level = static_cast<int>(static_cast<float>(min) + static_cast<float>(level - min) * a_zone->scale);
			}
			if (a_zone->max > 0) {
				level = (std::min)(level, static_cast<int>(a_zone->max));
			}
		}
		if (std::isfinite(a_modifier) && a_modifier > 0.0f) {
			level = static_cast<int>(std::floor(static_cast<float>(level) * a_modifier));
		}
		return static_cast<std::uint16_t>(std::clamp(level, static_cast<int>(MIN_LEVEL), static_cast<int>(MAX_LEVEL)));
	}

	// An NPC whose level is its player's times a multiplier (ACTOR_BASE_DATA kPCLevelMult: a_mult1000 is the multiplier
	// times 1000, as the record keeps it) at a_session, held within its calculated bounds (a max of 0: none).
	[[nodiscard]] inline std::uint16_t NpcLevel(std::uint16_t a_session, std::uint16_t a_mult1000, std::uint16_t a_min, std::uint16_t a_max)
	{
		const auto mult = a_mult1000 == 0 ? 1.0f : static_cast<float>(a_mult1000) / 1000.0f;
		int        level = static_cast<int>(std::lround(static_cast<float>(a_session) * mult));
		if (a_min > 0) {
			level = (std::max)(level, static_cast<int>(a_min));
		}
		if (a_max > 0) {
			level = (std::min)(level, static_cast<int>(a_max));
		}
		return static_cast<std::uint16_t>(std::clamp(level, static_cast<int>(MIN_LEVEL), static_cast<int>(MAX_LEVEL)));
	}

	// The how-many-th roll of one spot key within ORDINAL_WINDOW of the last: a group placed together at one marker.
	inline constexpr std::uint32_t ORDINAL_WINDOW_MS = 10'000;

	class Ordinals
	{
	public:
		static constexpr std::size_t MAX = 512;

		std::uint32_t Next(std::uint64_t a_key, std::uint64_t a_nowMs)
		{
			if (seen.size() >= MAX) {
				std::erase_if(seen, [&](const auto& a_entry) { return a_nowMs - a_entry.second.lastMs > ORDINAL_WINDOW_MS; });
				if (seen.size() >= MAX) {
					seen.clear();
				}
			}
			auto& entry = seen[a_key];
			if (entry.count != 0 && a_nowMs - entry.lastMs > ORDINAL_WINDOW_MS) {
				entry.count = 0;
			}
			entry.lastMs = a_nowMs;
			return entry.count++;
		}

		void Clear() { seen.clear(); }

	private:
		struct Seen
		{
			std::uint32_t count = 0;
			std::uint64_t lastMs = 0;
		};
		std::unordered_map<std::uint64_t, Seen> seen;
	};

	[[nodiscard]] constexpr const char* Name(Miss a_miss)
	{
		switch (a_miss) {
		case Miss::kNone:
			return "none";
		case Miss::kNotLeveled:
			return "not leveled";
		case Miss::kEmptyList:
			return "a list with nothing at that level";
		case Miss::kNotNpc:
			return "a list entry that is no NPC";
		case Miss::kTooDeep:
			return "lists nested too deep";
		}
		return "?";
	}
}
