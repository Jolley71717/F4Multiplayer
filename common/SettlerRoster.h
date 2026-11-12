#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// The settlers of the host's settlements, as the host's game tells them (the shared workshop, bSharedWorkshop; the wire's
// Protocol::BenchSettlers).
//
// Workshop mode offers Command on an actor only when the player's own game holds it as a workshop NPC (the engine's
// TESObjectREFR::IsWorkshopNPC: the reference carries one of the workshop's allow keywords, which the workshop's scripts
// add in each save). A friend at the host's settlement saw the host's settlers (NpcSync mirrors them), but its own save
// never made them settlers, so nothing offered Command on them and no job for one could be asked (SettlerRequests.h).
// Now the host's game says which of its settlers each of its settlements has, and a friend's game in workshop mode there
// answers "yes" for those to the engine's question of the command keyword. Who is a settler is the host's word; what a
// job does stays the host's judgement too (its game checks each request).
//
// - The host's game (Reporter): for each settlement its player owns, the settlers it has loaded there (alive, linked to
//   the workbench as the workshop's own, named as the session knows them), told when the list changes, and an empty list
//   once none are loaded; everything again after a welcome, and "forget them all" after a load.
// - The server (Book): the latest list per workbench, from the host's own game alone and only under the rule; each change
//   to the others, all of them to a joiner, forgotten when the host's game leaves.
// - A friend's game (Book, then Offered): while its workshop mode is open at a settlement the host's game runs, the
//   settlers the host named there are offered Command.
//
// Kept free of the game so it can be tested.
namespace SettlerRoster
{
	// Workbenches whose lists are kept (the host owns far fewer settlements; a list past this is not kept).
	inline constexpr std::size_t MAX_BENCHES = 256;
	// How long the host's game keeps naming a settler it no longer has loaded in high process: a settler at the edge of a
	// large settlement drops out of it and comes back, and each change is a report, a relay and Command lost on the friend's
	// side for a moment. One gone for good goes from the list after this.
	inline constexpr auto GRACE = std::chrono::seconds(10);

	// The settlers as they go on the wire: no 0, no repeats, no more than MAX_BENCH_SETTLERS, in ascending order (so the same
	// settlers in another order are no change).
	[[nodiscard]] inline std::vector<std::uint32_t> Normal(std::vector<std::uint32_t> a_settlers)
	{
		std::erase(a_settlers, 0u);
		std::ranges::sort(a_settlers);
		a_settlers.erase(std::unique(a_settlers.begin(), a_settlers.end()), a_settlers.end());
		if (a_settlers.size() > Protocol::MAX_BENCH_SETTLERS) {
			a_settlers.resize(Protocol::MAX_BENCH_SETTLERS);
		}
		return a_settlers;
	}

	// The lists of the host's settlements: the server's, and a friend's game's copy of it.
	class Book
	{
	public:
		// One list, as the host's game (or the server) told it. Whether it changed what is kept: a list the same as the one
		// kept, an empty one for a workbench with none, or a new workbench past MAX_BENCHES change nothing. Workbench 0 (no
		// settlers) forgets them all.
		bool Take(const Protocol::BenchSettlers& a_msg)
		{
			if (a_msg.bench == 0) {
				return Clear();
			}
			auto       settlers = Normal(a_msg.settlers);
			const auto it = lists.find(a_msg.bench);
			if (settlers.empty()) {
				if (it == lists.end()) {
					return false;
				}
				lists.erase(it);
				return true;
			}
			if (it != lists.end()) {
				if (it->second == settlers) {
					return false;
				}
				it->second = std::move(settlers);
				return true;
			}
			if (lists.size() >= MAX_BENCHES) {
				return false;
			}
			lists.emplace(a_msg.bench, std::move(settlers));
			return true;
		}

		// Whether there was anything to forget.
		bool Clear()
		{
			const bool had = !lists.empty();
			lists.clear();
			return had;
		}

		// Every list kept (a joiner's catch-up), in no particular order.
		[[nodiscard]] std::vector<Protocol::BenchSettlers> All() const
		{
			std::vector<Protocol::BenchSettlers> out;
			out.reserve(lists.size());
			for (const auto& [bench, settlers] : lists) {
				out.push_back({ bench, settlers });
			}
			return out;
		}

		// The settlers the host's game named at that workbench (none: it named none, or nothing of it).
		[[nodiscard]] std::span<const std::uint32_t> Of(std::uint32_t a_bench) const
		{
			const auto it = lists.find(a_bench);
			return it != lists.end() ? std::span<const std::uint32_t>{ it->second } : std::span<const std::uint32_t>{};
		}

		[[nodiscard]] bool Has(std::uint32_t a_bench, std::uint32_t a_settler) const { return std::ranges::binary_search(Of(a_bench), a_settler); }
		[[nodiscard]] bool Any(std::uint32_t a_bench) const { return lists.contains(a_bench); }
		[[nodiscard]] std::size_t Benches() const { return lists.size(); }

	private:
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> lists;  // never an empty one
	};

	// The host's game: what it last told of each settlement, so it tells only what changed.
	class Reporter
	{
	public:
		using Clock = std::chrono::steady_clock;

		// a_loaded: every settlement of ours with a settler loaded now, and those settlers as the session knows them. A
		// settler stays named for GRACE after it was last seen loaded. The lists to send: each that differs from the last one
		// sent for its workbench, and an empty one for each workbench a list was sent for that has none now. A settlement
		// with none, never told, is told nothing.
		std::vector<Protocol::BenchSettlers> Round(const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& a_loaded, Clock::time_point a_at)
		{
			for (const auto& [bench, settlers] : a_loaded) {
				if (bench == 0 || !Protocol::IsShareableRef(bench) || (!seen.contains(bench) && seen.size() >= MAX_BENCHES)) {
					continue;
				}
				auto& times = seen[bench];
				for (const auto settler : settlers) {
					if (settler != 0 && (times.contains(settler) || times.size() < Protocol::MAX_BENCH_SETTLERS * 2)) {
						times[settler] = a_at;
					}
				}
			}
			std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> now;
			for (auto it = seen.begin(); it != seen.end();) {
				std::erase_if(it->second, [&](const auto& a_entry) { return a_at - a_entry.second >= GRACE; });
				if (it->second.empty()) {
					it = seen.erase(it);
					continue;
				}
				auto& list = now[it->first];
				for (const auto& [settler, at] : it->second) {
					list.push_back(settler);
				}
				++it;
			}
			std::vector<Protocol::BenchSettlers> out;
			for (const auto& [bench, settlers] : now) {
				auto       list = Normal(settlers);
				const auto it = sent.find(bench);
				if (list.empty() || (it != sent.end() && it->second == list)) {
					continue;
				}
				if (it == sent.end() && sent.size() >= MAX_BENCHES) {
					continue;
				}
				sent[bench] = list;
				out.push_back({ bench, std::move(list) });
			}
			for (auto it = sent.begin(); it != sent.end();) {
				if (!now.contains(it->first)) {
					out.push_back({ it->first, {} });
					it = sent.erase(it);
				} else {
					++it;
				}
			}
			std::ranges::sort(out, {}, &Protocol::BenchSettlers::bench);
			reports += static_cast<std::uint32_t>(out.size());
			return out;
		}

		// Everything is told again at the next round, from what is loaded then (a welcome: the server's lists may be another
		// session's or none; a load: the settlers seen were the old world's).
		void Forget()
		{
			sent.clear();
			seen.clear();
		}

		[[nodiscard]] std::size_t Benches() const { return sent.size(); }
		[[nodiscard]] std::uint32_t Reports() const { return reports; }

	private:
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>                                   sent;  // never an empty one
		std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, Clock::time_point>> seen;  // per workbench, when each was last loaded
		std::uint32_t                                                                           reports = 0;
	};

	// ---- A friend's game: who workshop mode offers Command on --------------------------------------------------

	// Whether workshop mode open at a_openBench offers Command on the host's settlers there: the rule hands requests to a
	// host's game (with the give-command hook in, or a job could not be asked), workshop mode is open, and that settlement
	// is one the host's game runs (SettlerRequests::HostRuns). Then the settlers the host's game named there are offered
	// (Book::Of, each found in our game: an NPC NpcSync knows under that session id).
	[[nodiscard]] constexpr bool Offers(bool a_handsOver, bool a_hooked, std::uint32_t a_openBench, bool a_hostRuns)
	{
		return a_handsOver && a_hooked && a_openBench != 0 && a_hostRuns;
	}

	// The engine asks a reference whether it carries a keyword (HasKeyword, on any thread). The answer for an actor of our
	// game: what the engine found, or yes when that is no but the keyword is workshop mode's command keyword and the actor
	// is one of the host's settlers offered now. Nothing else is ever answered otherwise.
	[[nodiscard]] constexpr bool Answer(bool a_engine, bool a_commandKeyword, bool a_offered)
	{
		return a_engine || (a_commandKeyword && a_offered);
	}
}
