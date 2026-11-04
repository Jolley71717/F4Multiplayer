#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// The host's supply lines at a friend's builds (shared storage's workbenches, Storage.h; the wire's Protocol::BenchLinks).
//
// The engine (1.11.240, by RE) builds with what a settlement's workbench holds, then what every workbench its supply lines
// reach holds, then the player's: Workshop::CollectLinkedWorkshopContainers (2195116) puts the workbench first and calls
// Workshop::AddOtherContainers (2195029), which walks the settlement location's links of the supply-line keyword
// (BGSLocation::CollectLinkedLocations, 2199664: every location reachable through them, the settlement's own left out),
// adds each linked settlement's workbench (Workshop::GetWorkshopContainer) and then, unless told not to, the player.
// Workshop mode's list (Workshop::RefillSharedWorkshopContainer), a build's takes (Workshop::RemoveItemFromWorkshopContainers,
// in that order) and a crafting menu's gather (WorkbenchMenuBase's, from the station's settlement) all come from it.
//
// Those links are each world's own (a save's caravans), so a friend's game at the host's settlement found its own supply
// lines' workbenches, holding its own stash. Now:
// - The host's game, as it lists one of its settlements' workbenches, asks its own engine which workbenches its supply
//   lines reach from there (AddOtherContainers itself), lists those of settlements it owns too, and tells the session
//   (Reporter: again only when they change).
// - A friend's game viewing the host's workbench views those too (each one shared storage the host keeps: the true copy
//   shown in their copy, their own stash there set aside), and the engine's gather for that settlement finds them,
//   in the host's order, ahead of its own world's (Gathered). A build takes from them as the host's game would, each take a
//   move at that workbench checked by the server as any. The player still comes last.
// - The rest of 0.12.0's rules stand: only settlements the host owns, each item used once (the server's book), and with
//   the host away the views close and the friend builds with their own.
//
// Kept free of the game so it can be tested.
namespace SupplyLines
{
	// The host's game: what it last told the session of each workbench's supply lines, so it tells only what changed.
	class Reporter
	{
	public:
		// a_links: the workbenches linked to a_bench now, in any order (a_bench itself, nothing (0) and repeats left out).
		// The report to send when they differ from the last one sent for it (none sent yet counts as none linked).
		std::optional<Protocol::BenchLinks> Update(std::uint32_t a_bench, std::vector<std::uint32_t> a_links)
		{
			std::erase_if(a_links, [&](std::uint32_t a_link) { return a_link == 0 || a_link == a_bench; });
			std::ranges::sort(a_links);
			a_links.erase(std::ranges::unique(a_links).begin(), a_links.end());
			if (a_links.size() > Protocol::MAX_BENCH_LINKS) {
				a_links.resize(Protocol::MAX_BENCH_LINKS);
			}
			const auto it = sent.find(a_bench);
			if (it != sent.end() ? it->second == a_links : a_links.empty()) {
				if (it == sent.end()) {
					sent.emplace(a_bench, std::move(a_links));
				}
				return std::nullopt;
			}
			sent[a_bench] = a_links;
			++reports;
			return Protocol::BenchLinks{ a_bench, std::move(a_links) };
		}

		// Everything is told again (a welcome: the server's table may be another session's; a load: our world's lines may
		// differ).
		void Forget() { sent.clear(); }

		[[nodiscard]] std::size_t Benches() const { return sent.size(); }
		[[nodiscard]] std::uint32_t Reports() const { return reports; }

	private:
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> sent;
		std::uint32_t                                                 reports = 0;
	};

	// A friend's game: the keepers' supply lines, as the server told them.
	class Heard
	{
	public:
		void Take(const Protocol::BenchLinks& a_links)
		{
			if (a_links.links.empty()) {
				links.erase(a_links.bench);
				return;
			}
			if (links.size() < MAX_BENCHES || links.contains(a_links.bench)) {
				links[a_links.bench] = a_links.links;
			}
		}

		// A welcome (the table comes again after the owners) or a lost connection.
		void Clear() { links.clear(); }

		// The workbenches to view with a_bench: its keeper's supply lines' that a_viewable says our game can view (another
		// game keeps it), in the keeper's order. None when no supply line reaches it, or none have come.
		template <class Viewable>
		[[nodiscard]] std::vector<std::uint32_t> With(std::uint32_t a_bench, Viewable&& a_viewable) const
		{
			std::vector<std::uint32_t> out;
			if (const auto it = links.find(a_bench); it != links.end()) {
				for (const auto link : it->second) {
					if (link != a_bench && a_viewable(link)) {
						out.push_back(link);
					}
				}
			}
			return out;
		}

		[[nodiscard]] std::size_t Benches() const { return links.size(); }

	private:
		static constexpr std::size_t MAX_BENCHES = 4096;  // as the server's book

		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> links;
	};

	// The engine's gather of a settlement's other workbenches (AddOtherContainers) with the keeper's supply lines in, for a
	// friend's game viewing the keeper's workbench there. a_have: what the gather's list held before it (the workbench; a
	// crafting station's own containers), a_own: the workbenches our own world's supply lines reach (what the engine found),
	// a_keepers: the keeper's (Heard::With). What goes after a_have, in order: the keeper's first, as the keeper's game takes
	// from them right after the workbench, then our own world's that aren't among them (our own stash there, or our copy of
	// one the session shares that we don't view: our own things either way); none already in the list, none twice. The
	// player, when the gather adds it, comes after these, as the engine's last.
	[[nodiscard]] inline std::vector<std::uint32_t> Gathered(std::span<const std::uint32_t> a_have, std::span<const std::uint32_t> a_own,
		std::span<const std::uint32_t> a_keepers)
	{
		std::vector<std::uint32_t> out;
		const auto                 listed = [&](std::uint32_t a_id) {
            return a_id == 0 || std::ranges::find(a_have, a_id) != a_have.end() || std::ranges::find(out, a_id) != out.end();
		};
		for (const auto list : { a_keepers, a_own }) {
			for (const auto id : list) {
				if (!listed(id)) {
					out.push_back(id);
				}
			}
		}
		return out;
	}
}
