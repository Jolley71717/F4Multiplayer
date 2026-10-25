#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// How a destructible object's damage is shared (src/game/Destructibles.h does it in the game). Kept free of the game so
// the rules can be tested.
//
// The engine keeps an object's damage as its health (ExtraHealth on the reference; none while it was never damaged),
// and works out from it which of its base's destruction stages it has passed: a car at 80% burns, at 50% it goes off
// and becomes a hulk. Each stage is passed once: its explosion, debris and model come when the health first falls past
// it, never again. So a game that takes a friend's health sets off what the friend's game set off only for the stages
// its own copy had not passed yet, and a game that passed them first (its own shot, an explosion of its own) sets off
// nothing more.
namespace DestructionRules
{
	using Clock = std::chrono::steady_clock;

	// DestructibleObjectStage::flags.
	inline constexpr std::uint8_t STAGE_CAP_DAMAGE = 0x1;
	inline constexpr std::uint8_t STAGE_DISABLE = 0x2;  // the object goes away (an exploding barrel)
	inline constexpr std::uint8_t STAGE_DESTROY = 0x4;  // the object counts as destroyed
	inline constexpr std::uint8_t STAGE_IGNORE_EXTERNAL_DAMAGE = 0x8;

	// What a stage does, of what matters here (the engine's DestructibleObjectStage).
	struct Stage
	{
		int           healthPercent = 0;   // passed when the object's health falls to this percent of its maximum
		std::uint8_t  flags = 0;
		std::uint32_t explosion = 0;       // the BGSExplosion it sets off, 0 none
		bool          replacesModel = false;  // it puts a model of its own in the object's place (a car's hulk)
	};

	// The engine's percent of a health: rounded up (GetCurrentDestructionStage). None while never damaged (no health:
	// the engine's -1) is 100.
	[[nodiscard]] inline int Percent(float a_health, std::uint32_t a_maximum)
	{
		if (a_health < 0.0f || a_maximum == 0) {
			return 100;
		}
		return static_cast<int>(std::ceil(a_health * 100.0f / static_cast<float>(a_maximum)));
	}

	// How many of the stages an object has passed at a health: the engine's first stage whose percent is below the
	// object's is the next to come, and those before it are passed (a record's stages go from the most health down).
	[[nodiscard]] inline std::size_t Passed(std::span<const Stage> a_stages, float a_health, std::uint32_t a_maximum)
	{
		if (a_health < 0.0f) {
			return 0;
		}
		const int percent = Percent(a_health, a_maximum);
		for (std::size_t i = 0; i < a_stages.size(); ++i) {
			if (a_stages[i].healthPercent < percent) {
				return i;
			}
		}
		return a_stages.size();
	}

	// An object as a game has it.
	struct Copy
	{
		float health = -1.0f;  // -1: never damaged
		bool  destroyed = false;
	};

	// What a game does to its copy with a friend's report (or the session's, catching up).
	struct Plan
	{
		bool                       apply = false;  // nothing at all when false: our copy is as far gone already
		float                      health = -1.0f;
		bool                       destroyed = false;
		std::size_t                from = 0;  // stages passed before
		std::size_t                to = 0;    // and after
		std::vector<std::uint32_t> explosions;    // the newly passed stages' explosions, in order, to set off here
		bool                       replaceModel = false;  // a newly passed stage replaces the model: its 3D loads again
		bool                       disable = false;       // a newly passed stage takes it away
	};

	[[nodiscard]] inline Plan PlanApply(std::span<const Stage> a_stages, std::uint32_t a_maximum, const Copy& a_copy, const Protocol::Destruction& a_report)
	{
		Plan plan;
		const bool lower = a_copy.health < 0.0f || a_report.health < a_copy.health;
		const bool newlyDestroyed = a_report.destroyed && !a_copy.destroyed;
		if (!lower && !newlyDestroyed) {
			return plan;
		}
		plan.apply = true;
		plan.health = lower ? a_report.health : a_copy.health;
		plan.destroyed = a_copy.destroyed || a_report.destroyed;
		plan.from = Passed(a_stages, a_copy.health, a_maximum);
		plan.to = (std::max)(plan.from, Passed(a_stages, plan.health, a_maximum));
		for (std::size_t i = plan.from; i < plan.to; ++i) {
			const auto& stage = a_stages[i];
			if (a_report.explode && stage.explosion != 0) {
				plan.explosions.push_back(stage.explosion);
			}
			plan.replaceModel = plan.replaceModel || stage.replacesModel;
			plan.disable = plan.disable || (stage.flags & STAGE_DISABLE) != 0;
			plan.destroyed = plan.destroyed || (stage.flags & STAGE_DESTROY) != 0;
		}
		return plan;
	}

	// What the session knows of an object, as far as our game can tell: what we reported, or took from a friend.
	struct Known
	{
		std::size_t passed = 0;
		bool        destroyed = false;
	};

	// Our copy's damage as a report, when it is news: a stage the session hasn't seen passed, or destroyed when it
	// wasn't. explode: one of the stages it passed since sets off an explosion here (the others set it off too, if the
	// server lets it through).
	[[nodiscard]] inline std::optional<Protocol::Destruction> Due(std::uint32_t a_ref, std::span<const Stage> a_stages, std::uint32_t a_maximum, const Copy& a_copy,
		const Known& a_known)
	{
		const auto passed = Passed(a_stages, a_copy.health, a_maximum);
		if (passed <= a_known.passed && (!a_copy.destroyed || a_known.destroyed)) {
			return std::nullopt;
		}
		Protocol::Destruction out{ a_ref, (std::max)(a_copy.health, 0.0f), a_copy.destroyed, false };
		for (std::size_t i = a_known.passed; i < passed; ++i) {
			out.explode = out.explode || a_stages[i].explosion != 0;
		}
		return out;
	}

	// Two reports of one object as one: the lower health, destroyed if either is. (What waits for an object not loaded
	// here: by the time it is, any explosion is long over, so none is kept.)
	[[nodiscard]] inline Protocol::Destruction Merge(const Protocol::Destruction& a_old, const Protocol::Destruction& a_new)
	{
		return { a_new.refId, (std::min)(a_old.health, a_new.health), a_old.destroyed || a_new.destroyed, false };
	}

	// The objects our game watches closely (every tenth of a second): those our player hit, and those whose stage changed
	// near us, each for WATCH_FOR after its last change. At most MAX of them: the one watched longest goes first.
	class Watch
	{
	public:
		static constexpr auto        WATCH_FOR = std::chrono::seconds(30);  // a car set burning goes off some 10 s later
		static constexpr std::size_t MAX = 64;

		void Add(std::uint32_t a_ref, Clock::time_point a_now)
		{
			if (!until.contains(a_ref) && until.size() >= MAX) {
				const auto oldest = std::ranges::min_element(until, {}, [](const auto& a_entry) { return a_entry.second; });
				until.erase(oldest);
			}
			until[a_ref] = a_now + WATCH_FOR;
		}

		// Our copy of it changed: watched on from now.
		void Changed(std::uint32_t a_ref, Clock::time_point a_now)
		{
			if (const auto it = until.find(a_ref); it != until.end()) {
				it->second = a_now + WATCH_FOR;
			}
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(until, [&](const auto& a_entry) { return a_now >= a_entry.second; });
		}

		[[nodiscard]] bool Contains(std::uint32_t a_ref) const { return until.contains(a_ref); }
		[[nodiscard]] std::size_t Size() const { return until.size(); }

		[[nodiscard]] std::vector<std::uint32_t> Refs() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(until.size());
			for (const auto& [ref, when] : until) {
				out.push_back(ref);
			}
			return out;
		}

		void Clear() { until.clear(); }

	private:
		std::unordered_map<std::uint32_t, Clock::time_point> until;
	};

	// What waits for objects not in our world yet (a friend wrecked a car in a cell we haven't loaded): one report each,
	// merged. At most MAX: a flood of reports for objects we never load starts it over rather than grow.
	class Waiting
	{
	public:
		static constexpr std::size_t MAX = 20000;

		void Add(const Protocol::Destruction& a_report)
		{
			if (const auto it = reports.find(a_report.refId); it != reports.end()) {
				it->second = Merge(it->second, a_report);
				return;
			}
			if (reports.size() >= MAX) {
				reports.clear();
			}
			auto calm = a_report;
			calm.explode = false;
			reports.emplace(a_report.refId, calm);
		}

		[[nodiscard]] const Protocol::Destruction* Find(std::uint32_t a_ref) const
		{
			const auto it = reports.find(a_ref);
			return it == reports.end() ? nullptr : &it->second;
		}

		void Erase(std::uint32_t a_ref) { reports.erase(a_ref); }
		[[nodiscard]] bool Empty() const { return reports.empty(); }
		[[nodiscard]] std::size_t Size() const { return reports.size(); }

		[[nodiscard]] std::vector<std::uint32_t> Refs() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(reports.size());
			for (const auto& [ref, report] : reports) {
				out.push_back(ref);
			}
			return out;
		}

		void Clear() { reports.clear(); }

	private:
		std::unordered_map<std::uint32_t, Protocol::Destruction> reports;
	};
}
