#pragma once

#include "Protocol.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// The host's settlement figures as last told (Protocol::BenchNumbers; src/game/WorkshopNumbers.h): what the workshop bar
// shows at each settlement the host owns. The server keeps one for the session (from the host's game alone, never saved:
// the host's game tells them again after its welcome and each load) and each friend's game keeps one of what the server
// told it. Only what changes is passed on.
//
// Kept free of the game and the network so it can be tested.
namespace BenchNumbersBook
{
	class Book
	{
	public:
		// The settlements whose figures are kept. Past it a new one is not taken (the game has some 40; a mod's more).
		static constexpr std::size_t MAX_BENCHES = 256;

		// A word of the host's settlements. No entries: all of them are forgotten. An entry with kHosts: that settlement's
		// figures now. One without: it isn't the host's any more, its figures are forgotten.
		// What changed here, to pass on as it is (none: nothing did; no entries in it: everything was forgotten).
		std::optional<Protocol::BenchNumbers> Take(const Protocol::BenchNumbers& a_word)
		{
			if (a_word.benches.empty()) {
				if (figures.empty()) {
					return std::nullopt;
				}
				figures.clear();
				return Protocol::BenchNumbers{};
			}
			Protocol::BenchNumbers changed;
			for (const auto& entry : a_word.benches) {
				const auto it = figures.find(entry.bench);
				if ((entry.flags & Protocol::BenchFigures::kHosts) == 0) {
					if (it != figures.end()) {
						figures.erase(it);
						changed.benches.push_back(Protocol::BenchFigures{ .bench = entry.bench });
					}
				} else if (it != figures.end()) {
					if (it->second != entry) {
						it->second = entry;
						changed.benches.push_back(entry);
					}
				} else if (figures.size() < MAX_BENCHES) {
					figures.emplace(entry.bench, entry);
					changed.benches.push_back(entry);
				}
			}
			if (changed.benches.empty()) {
				return std::nullopt;
			}
			return changed;
		}

		// Everything is forgotten (the host left, a new session). Whether anything was kept.
		bool Clear()
		{
			const bool had = !figures.empty();
			figures.clear();
			return had;
		}

		// All of them, for a joiner: in messages of at most Protocol::MAX_BENCH_NUMBERS entries, by workbench.
		[[nodiscard]] std::vector<Protocol::BenchNumbers> All() const
		{
			std::vector<Protocol::BenchNumbers> out;
			for (const auto& [bench, entry] : figures) {
				if (out.empty() || out.back().benches.size() >= Protocol::MAX_BENCH_NUMBERS) {
					out.emplace_back();
				}
				out.back().benches.push_back(entry);
			}
			return out;
		}

		// The host's figures at that settlement; none: none told (not the host's, or not told yet).
		[[nodiscard]] std::optional<Protocol::BenchFigures> Of(std::uint32_t a_bench) const
		{
			const auto it = figures.find(a_bench);
			return it == figures.end() ? std::nullopt : std::optional{ it->second };
		}

		[[nodiscard]] bool Holds(std::uint32_t a_bench) const { return figures.contains(a_bench); }
		[[nodiscard]] std::size_t Benches() const { return figures.size(); }

	private:
		std::map<std::uint32_t, Protocol::BenchFigures> figures;  // only entries with kHosts
	};
}
