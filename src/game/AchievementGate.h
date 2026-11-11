#pragma once

#include "game/AchievementStages.h"
#include "game/StageRules.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Which Game.AddAchievement calls are a caught-up stage's, and so dropped (Achievements.cpp's hook on the Papyrus
// native asks). A friend's stage set here runs its script as it did in their game, Game.AddAchievement included; the
// maintainer's rule: a stage that happens live while both are in the session awards both players, one caught up for a
// joiner (done before they joined; the welcome's stages) awards nothing. A stage's fragment runs on the Papyrus VM a
// moment after the stage is set, on the VM's own threads, so nothing scoped around the setstage can tell the two apart:
// QuestSync marks each caught-up stage it sets (CaughtUp), and the hook judges each call by the quest fragment it was
// made from (the frame nearest the native whose script is a QF_ fragment: Fragment), dropping it while that stage is
// marked. A mark expires after MARK_FOR, so it can never outlive the catch-up by more than that: a stage is done once in
// a game, and the only stage a mark can meet later is one a lagging Papyrus queue ran late. Kept free of the game so it
// can be tested.
namespace AchievementGate
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// How long a caught-up stage's award stays dropped after the stage was set here. Its fragment runs a moment later; a
	// lagging Papyrus queue can run it late (the price waiver's 5 minutes, StageRules::PriceWaiver). Past this the game's
	// own call runs again, whatever made it.
	inline constexpr auto MARK_FOR = 5min;

	// How many frames up from the native the quest fragment is looked for: the fragment itself (previousFrame), or a
	// function of the quest's own script it called.
	inline constexpr std::size_t FRAMES = 16;

	struct Key
	{
		std::uint8_t  file;   // StageRules::File
		std::uint32_t quest;  // form ID in its file
		std::uint16_t stage;

		auto operator<=>(const Key&) const = default;
	};

	// The game file a quest fragment's script namespace names: "DLC01:" Automatron, "DLC03:" Far Harbor, "DLC04:"
	// Nuka-World, none Fallout4.esm (the add-ons' fragments are compiled under their own namespace).
	inline std::optional<StageRules::File> FileOfNamespace(std::string_view a_prefix)
	{
		if (a_prefix.empty()) {
			return StageRules::File::kFallout4;
		}
		if (a_prefix == "DLC01") {
			return StageRules::File::kRobot;
		}
		if (a_prefix == "DLC03") {
			return StageRules::File::kCoast;
		}
		if (a_prefix == "DLC04") {
			return StageRules::File::kNukaWorld;
		}
		return std::nullopt;
	}

	// A Papyrus frame's script and function parsed as a quest stage fragment of one of the game's masters: the script
	// "[DLCnn:]Fragments:Quests:QF_<editor id>_<8 hex digits, the form ID as the file was compiled>" and the function
	// "Fragment_Stage_<stage>_Item_<n>". Anything else (another script, a fragment of a mod's quest, a scene's or a
	// terminal's fragment) is nothing: the game's call runs.
	inline std::optional<Key> Fragment(std::string_view a_script, std::string_view a_function)
	{
		constexpr std::string_view FRAGMENTS = "Fragments:Quests:QF_";
		constexpr std::string_view STAGE = "Fragment_Stage_";
		std::string_view           prefix;
		if (const auto colon = a_script.find(':'); colon != std::string_view::npos && !a_script.substr(colon + 1).starts_with("Quests:")) {
			prefix = a_script.substr(0, colon);
			a_script.remove_prefix(colon + 1);
		}
		const auto file = FileOfNamespace(prefix);
		if (!file || !a_script.starts_with(FRAGMENTS) || a_script.size() < FRAGMENTS.size() + 1 + 8) {
			return std::nullopt;
		}
		const auto    hex = a_script.substr(a_script.size() - 8);
		std::uint32_t id = 0;
		const auto [idEnd, idErr] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
		if (idErr != std::errc{} || idEnd != hex.data() + hex.size() || a_script[a_script.size() - 9] != '_') {
			return std::nullopt;
		}
		if (!a_function.starts_with(STAGE)) {
			return std::nullopt;
		}
		const auto    digits = a_function.substr(STAGE.size());
		std::uint32_t stage = 0;
		const auto [stageEnd, stageErr] = std::from_chars(digits.data(), digits.data() + digits.size(), stage, 10);
		if (stageErr != std::errc{} || stageEnd == digits.data() || stage > 0xFFFF || !std::string_view{ stageEnd, digits.data() + digits.size() }.starts_with("_Item_")) {
			return std::nullopt;
		}
		return Key{ static_cast<std::uint8_t>(*file), id & 0x00FFFFFF, static_cast<std::uint16_t>(stage) };
	}

	// The award a caught-up stage reaches by stage sets alone (AchievementStages: its own fragment's, or that of a stage
	// its fragment sets, of its own quest or another), or nothing.
	inline std::optional<Key> Reaches(Key a_key)
	{
		const auto key = std::tuple{ a_key.file, a_key.quest, a_key.stage };
		const auto it = std::ranges::lower_bound(AchievementStages::REACHES, key, {}, [](const AchievementStages::Reach& a_reach) { return std::tuple{ a_reach.file, a_reach.quest, a_reach.stage }; });
		if (it == std::end(AchievementStages::REACHES) || std::tuple{ it->file, it->quest, it->stage } != key) {
			return std::nullopt;
		}
		return Key{ it->awardFile, it->awardQuest, it->awardStage };
	}

	// The caught-up stages whose awards are dropped, each until MARK_FOR after it was set.
	class Marks
	{
	public:
		// A friend's stage was set here caught up, not live: its own award is dropped, and the one it reaches by setting
		// another stage (Reaches), whose fragment the game runs from this one's. How many marks were added (0: both known).
		std::size_t CaughtUp(Key a_key, Clock::time_point a_now)
		{
			std::size_t added = Add(a_key, a_now) ? 1 : 0;
			if (const auto award = Reaches(a_key); award && *award != a_key) {
				added += Add(*award, a_now) ? 1 : 0;
			}
			return added;
		}

		// Whether an award made from this stage's fragment is dropped: the stage is marked and the mark is younger than
		// MARK_FOR. How long ago it was marked, for the log, or nothing.
		std::optional<Clock::duration> Drops(Key a_key, Clock::time_point a_now) const
		{
			const auto it = std::ranges::find(marks, a_key, &Mark::key);
			if (it == marks.end() || a_now - it->at >= MARK_FOR) {
				return std::nullopt;
			}
			return a_now - it->at;
		}

		// Marks older than MARK_FOR are forgotten.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(marks, [&](const Mark& a_mark) { return a_now - a_mark.at >= MARK_FOR; });
		}

		// A load: the Papyrus queue is the save's, nothing set before it runs any more.
		void Clear() { marks.clear(); }

		std::size_t Size() const { return marks.size(); }

	private:
		struct Mark
		{
			Key               key;
			Clock::time_point at;
		};

		// A stage marked again keeps its first mark: the award's clock runs from when it was set.
		bool Add(Key a_key, Clock::time_point a_now)
		{
			if (std::ranges::find(marks, a_key, &Mark::key) != marks.end()) {
				return false;
			}
			marks.push_back(Mark{ a_key, a_now });
			return true;
		}

		std::vector<Mark> marks;
	};
}
