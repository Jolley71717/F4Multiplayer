#pragma once

#include "BenchNumbersBook.h"
#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// The host's settlement figures, shown to friends (the wire's Protocol::BenchNumbers; docs/DESIGN.md "The host's
// settlement figures").
//
// A settlement's figures (power, food, water, defense, beds, people, happiness) are actor values on its workbench, each
// world's own: a friend's game at the host's settlement showed what its own save's copy of that settlement counts, with
// the worth of the copies of the host's pieces put on top (ResourceMirror.h). The host's game is the only authority for
// its settlement, so now:
// - The host's game, while its shared workshop is on, reads once a second what its own workshop bar would show at every
//   settlement its player owns (the engine's Workshop getters, whole numbers; loaded or not) and tells the session the
//   ones that changed, all in one message (Reporter). After its welcome and after each load it first says "forget them
//   all", then tells every one again. A settlement it no longer owns is told once as not its own.
// - The server keeps the last figures of each (BenchNumbersBook.h) and tells the others, a joiner all of them.
// - A friend's game keeps what it was told (Heard). While it holds figures for a workbench, that settlement is the
//   host's: its workshop bar there shows the host's figures (Shown: the bar asks the engine's getters, which answer
//   from the figures; nothing is written to the friend's workbench, so nothing of them can reach the friend's save and
//   nothing local can overwrite them), no worth of copies is put on that workbench (WithoutHosts), and entering workshop
//   mode there says them once (Announcer).
// - With no figures (the first second after a join, the host away, the rule off): the friend's game shows its own world's
//   figures as it did before, the copies' worth is put on as before, and nothing is said.
//
// Kept free of the game so it can be tested.
namespace WorkshopNumbers
{
	using Clock = std::chrono::steady_clock;
	using Figures = Protocol::BenchFigures;

	// How often the host's game reads its settlements, so no more than one message a second goes out.
	inline constexpr auto POLL_EVERY = std::chrono::seconds(1);

	// How often the host's game tells every settlement again, changed or not: a message the server's event cap dropped
	// (a busy second) is made up for by the next. The server passes on only what differs from what it holds, so a friend's
	// game hears nothing of it.
	inline constexpr auto RETELL_EVERY = std::chrono::seconds(30);

	// What the engine's getters say of a workbench (an int each, as the workshop bar reads them).
	struct Read
	{
		int power = 0;      // Workshop::GetGeneratedPower
		int powerUsed = 0;  // the workbench's power needed
		int food = 0;
		int water = 0;
		int defense = 0;
		int beds = 0;
		int population = 0;
		int happiness = 0;
		int happinessChange = 0;  // Workshop::GetHappinessTarget: the target less the happiness now (the bar's arrow)
		int foodRating = 0;       // each rating: 0 fine, below 0 short (the bar shows the figure red)
		int waterRating = 0;
		int defenseRating = 0;
		int bedsRating = 0;
		int powerRating = 0;
	};

	[[nodiscard]] constexpr std::uint16_t Count(int a_value)
	{
		return static_cast<std::uint16_t>(std::clamp(a_value, 0, 65535));
	}

	[[nodiscard]] constexpr std::uint8_t Percent(int a_value)
	{
		return static_cast<std::uint8_t>(std::clamp(a_value, 0, 100));
	}

	// The figures to tell of a settlement the host owns: whole numbers, none below 0, happiness within 0 to 100.
	[[nodiscard]] constexpr Figures Of(std::uint32_t a_bench, const Read& a_read)
	{
		Figures out;
		out.bench = a_bench;
		out.flags = static_cast<std::uint8_t>(Figures::kHosts | (a_read.foodRating < 0 ? Figures::kFoodShort : 0) | (a_read.waterRating < 0 ? Figures::kWaterShort : 0) |
											  (a_read.defenseRating < 0 ? Figures::kDefenseShort : 0) | (a_read.bedsRating < 0 ? Figures::kBedsShort : 0) |
											  (a_read.powerRating < 0 ? Figures::kPowerShort : 0));
		out.power = Count(a_read.power);
		out.powerUsed = Count(a_read.powerUsed);
		out.food = Count(a_read.food);
		out.water = Count(a_read.water);
		out.defense = Count(a_read.defense);
		out.beds = Count(a_read.beds);
		out.population = Count(a_read.population);
		out.happiness = Percent(a_read.happiness);
		out.happinessTarget = Percent(a_read.happiness + a_read.happinessChange);
		return out;
	}

	// What the workshop bar asks the engine for, one getter each (the thirteen calls of its fill).
	enum class Bar : std::uint8_t
	{
		kPower,
		kFood,
		kWater,
		kBeds,
		kDefense,
		kPopulation,
		kHappiness,
		kFoodRating,
		kWaterRating,
		kDefenseRating,
		kBedsRating,
		kHappinessChange,
		kPowerRating,
	};
	inline constexpr std::size_t BARS = 13;

	// What that getter answers on a friend's game at the host's settlement: the host's figure; a rating 0 (fine) or -1
	// (short, as the host's bar shows it); the happiness arrow's change the host's target less the host's happiness.
	[[nodiscard]] constexpr int Shown(const Figures& a_figures, Bar a_what)
	{
		const auto rating = [&](std::uint8_t a_flag) { return (a_figures.flags & a_flag) != 0 ? -1 : 0; };
		switch (a_what) {
		case Bar::kPower:
			return a_figures.power;
		case Bar::kFood:
			return a_figures.food;
		case Bar::kWater:
			return a_figures.water;
		case Bar::kBeds:
			return a_figures.beds;
		case Bar::kDefense:
			return a_figures.defense;
		case Bar::kPopulation:
			return a_figures.population;
		case Bar::kHappiness:
			return a_figures.happiness;
		case Bar::kFoodRating:
			return rating(Figures::kFoodShort);
		case Bar::kWaterRating:
			return rating(Figures::kWaterShort);
		case Bar::kDefenseRating:
			return rating(Figures::kDefenseShort);
		case Bar::kBedsRating:
			return rating(Figures::kBedsShort);
		case Bar::kHappinessChange:
			return static_cast<int>(a_figures.happinessTarget) - static_cast<int>(a_figures.happiness);
		case Bar::kPowerRating:
			return rating(Figures::kPowerShort);
		}
		return 0;
	}

	// The host's game: what it last told the session of each settlement it owns, so it tells only what changed.
	class Reporter
	{
	public:
		// Whether the settlements are to be read now: once POLL_EVERY has passed since the last time it said so. Every
		// RETELL_EVERY the Update that follows tells them all again.
		[[nodiscard]] bool Due(Clock::time_point a_now)
		{
			if (a_now < next) {
				return false;
			}
			next = a_now + POLL_EVERY;
			if (a_now >= retellAt) {
				retellAt = a_now + RETELL_EVERY;
				retell = true;
			}
			return true;
		}

		// a_owned: the figures of every settlement our player owns now (Of), each workbench once (a repeat, or workbench 0,
		// is left out). The messages to send, in order: after Forget, one with no entries (forget them all); then one with
		// the settlements we no longer own (each once, without kHosts) and those whose figures differ from what was last
		// told, at most Protocol::MAX_BENCH_NUMBERS entries; the rest wait for the next call, where they come first.
		// When Due said it is time to tell them all again, everything counts as differing, and the settlements told as
		// no longer ours are told so again.
		[[nodiscard]] std::vector<Protocol::BenchNumbers> Update(std::span<const Figures> a_owned)
		{
			std::vector<Protocol::BenchNumbers> out;
			if (fresh) {
				fresh = false;
				retell = false;
				for (const auto& [bench, figures] : sent) {
					Lost(bench);  // not ours after the load, and the word to forget them all dropped: told so at the next retelling
				}
				sent.clear();
				gone.clear();
				out.emplace_back();
				++reports;
			}
			if (std::exchange(retell, false)) {
				sent.clear();
				gone.insert(lost.begin(), lost.end());
			}
			std::vector<Figures> owned;
			owned.reserve(a_owned.size());
			for (auto figures : a_owned) {
				figures.flags |= Figures::kHosts;
				if (figures.bench != 0 && std::ranges::none_of(owned, [&](const Figures& a_other) { return a_other.bench == figures.bench; })) {
					owned.push_back(figures);
				}
			}
			const auto owns = [&](std::uint32_t a_bench) { return std::ranges::any_of(owned, [&](const Figures& a_other) { return a_other.bench == a_bench; }); };
			for (auto it = sent.begin(); it != sent.end();) {
				if (!owns(it->first)) {
					gone.insert(it->first);
					it = sent.erase(it);
				} else {
					++it;
				}
			}
			Protocol::BenchNumbers word;
			for (auto it = gone.begin(); it != gone.end();) {
				if (owns(*it)) {
					it = gone.erase(it);  // ours again before it was told: its figures go as any change
				} else if (word.benches.size() < Protocol::MAX_BENCH_NUMBERS) {
					word.benches.push_back(Figures{ .bench = *it });
					Lost(*it);
					it = gone.erase(it);
				} else {
					++it;
				}
			}
			// From the one that waited last time, so a full message never starves the settlements after it.
			const auto first = owned.empty() ? 0 : turn % owned.size();
			bool       waited = false;
			for (std::size_t i = 0; i < owned.size(); ++i) {
				const auto  index = (first + i) % owned.size();
				const auto& figures = owned[index];
				if (const auto it = sent.find(figures.bench); it != sent.end() && it->second == figures) {
					continue;
				}
				if (word.benches.size() >= Protocol::MAX_BENCH_NUMBERS) {
					if (!waited) {
						waited = true;
						turn = index;
					}
					continue;
				}
				sent[figures.bench] = figures;
				word.benches.push_back(figures);
			}
			if (!waited) {
				turn = 0;
			}
			if (!word.benches.empty()) {
				out.push_back(std::move(word));
				++reports;
			}
			return out;
		}

		// A message of Update's that could not be sent (the queue was full): what it told is told again at the next Update.
		void Unsent(const Protocol::BenchNumbers& a_word)
		{
			if (a_word.benches.empty()) {
				fresh = true;
				return;
			}
			for (const auto& figures : a_word.benches) {
				if ((figures.flags & Figures::kHosts) != 0) {
					sent.erase(figures.bench);
				} else {
					gone.insert(figures.bench);
				}
			}
		}

		// Everything is told again, after a word to forget them all (a welcome: the server's table may be another session's;
		// a load: our world's settlements may differ).
		void Forget() { fresh = true; }

		[[nodiscard]] std::size_t Benches() const { return sent.size(); }
		[[nodiscard]] std::uint32_t Reports() const { return reports; }
		[[nodiscard]] std::optional<Figures> Told(std::uint32_t a_bench) const
		{
			const auto it = sent.find(a_bench);
			return it == sent.end() ? std::nullopt : std::optional{ it->second };
		}

	private:
		void Lost(std::uint32_t a_bench)
		{
			if (lost.size() < MAX_LOST) {
				lost.insert(a_bench);
			}
		}

		static constexpr std::size_t MAX_LOST = 256;

		std::set<std::uint32_t>                    lost;          // settlements once told as ours: told as not ours again at each retelling, unless ours
		bool                                       retell = false;
		Clock::time_point                          retellAt{};
		std::unordered_map<std::uint32_t, Figures> sent;          // what the session was last told of each settlement we own
		std::set<std::uint32_t>                    gone;          // settlements told as ours that aren't any more, not yet told so
		bool                                       fresh = true;  // nothing told since the welcome or the load
		std::size_t                                turn = 0;
		std::uint32_t                              reports = 0;
		Clock::time_point                          next{};
	};

	// A friend's game: the host's settlements' figures, as the server told them.
	class Heard
	{
	public:
		// Whether anything changed.
		bool Take(const Protocol::BenchNumbers& a_word) { return book.Take(a_word).has_value(); }

		// A welcome (the server tells them again) or a lost connection. Whether anything was held.
		bool Clear() { return book.Clear(); }

		// The host's figures at that settlement; none: it isn't the host's as far as we were told, or nothing has come yet.
		[[nodiscard]] std::optional<Figures> Of(std::uint32_t a_bench) const { return book.Of(a_bench); }

		// Whether the settlement is the host's for the figures: exactly while we hold figures for it.
		[[nodiscard]] bool Hosts(std::uint32_t a_bench) const { return book.Holds(a_bench); }

		// What the workshop bar's getter answers for that workbench: the host's figure, or none: ask the engine, as before
		// (our own world's figure).
		[[nodiscard]] std::optional<int> OnBar(std::uint32_t a_bench, Bar a_what) const
		{
			const auto figures = book.Of(a_bench);
			return figures ? std::optional{ Shown(*figures, a_what) } : std::nullopt;
		}

		[[nodiscard]] std::size_t Benches() const { return book.Benches(); }

	private:
		BenchNumbersBook::Book book;
	};

	// A friend's game: the worth of friends' pieces to put on our workbenches (ResourceMirror::Tally::Totals, by workbench)
	// without the host's settlements: there the figures are the host's game's alone, and what we had put on such a
	// workbench comes off (ResourceMirror::Applier::Apply takes off what the totals no longer name). How many were left out.
	template <class Totals>
	std::size_t WithoutHosts(Totals& a_totals, const Heard& a_heard)
	{
		return std::erase_if(a_totals, [&](const auto& a_entry) { return a_heard.Hosts(a_entry.first); });
	}

	// A friend's game: the host's figures said once as workshop mode is entered at the host's settlement (or as they first
	// come while it is open there), for a player whose bar could not be patched, and so the player knows whose they are.
	class Announcer
	{
	public:
		// a_bench: the workbench workshop mode is open at (0: none known). The figures to say now, if any.
		[[nodiscard]] std::optional<Figures> Update(bool a_inWorkshopMode, std::uint32_t a_bench, const Heard& a_heard)
		{
			if (!a_inWorkshopMode) {
				said = 0;
				return std::nullopt;
			}
			if (a_bench == 0 || a_bench == said) {
				return std::nullopt;
			}
			const auto figures = a_heard.Of(a_bench);
			if (figures) {
				said = a_bench;
			}
			return figures;
		}

	private:
		std::uint32_t said = 0;  // the workbench whose figures this stay in workshop mode said
	};
}
