#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
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
// adds each linked settlement's workbench (Workshop::GetWorkshopContainer), in the order of the engine's list of
// workshops, and then, unless told not to, the player.
// Workshop mode's list (Workshop::RefillSharedWorkshopContainer), a build's takes (Workshop::RemoveItemFromWorkshopContainers,
// in that order) and a crafting menu's gather (WorkbenchMenuBase's, from the station's settlement) all come from it.
//
// Those links are each world's own (a save's caravans), so a friend's game at the host's settlement found its own supply
// lines' workbenches, holding its own stash. Now:
// - The host's game, for each of its settlements' workbenches it keeps, asks its own engine which workbenches its supply
//   lines reach from there (AddOtherContainers itself), in the engine's order, lists those of settlements it owns too,
//   and tells the session (Reporter: again only when they change).
// - A friend's game viewing the host's workbench views those too, the first Views::MAX of them, one at a time (Views:
//   each one shared storage the host keeps, the true copy shown in their copy, their own stash there set aside), and the
//   engine's gather for that settlement finds the ones viewed, in the host's order, ahead of its own world's
//   (Gathered). A build takes from them as the host's game would, each take a move at that workbench checked by the
//   server as any. The player still comes last.
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
		// a_links: the workbenches linked to a_bench now, in the order the host's engine takes from them (a_bench itself,
		// nothing (0) and repeats left out, the first of each kept). The report to send when they differ from the last one
		// sent for it, or come in another order (none sent yet counts as none linked).
		std::optional<Protocol::BenchLinks> Update(std::uint32_t a_bench, std::vector<std::uint32_t> a_links)
		{
			std::vector<std::uint32_t> kept;
			for (const auto link : a_links) {
				if (link != 0 && link != a_bench && std::ranges::find(kept, link) == kept.end()) {
					kept.push_back(link);
				}
			}
			a_links = std::move(kept);
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
	public:
		static constexpr std::size_t MAX_BENCHES = 4096;  // the server's table holds no more (StorageBook::Book::MAX_CONTAINERS)

	private:

		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> links;
	};

	// The engine's gather of a settlement's other workbenches (AddOtherContainers) with the keeper's supply lines in, for a
	// friend's game viewing the keeper's workbench there. a_have: what the gather's list held before the call (workshop
	// mode's and a build's: the workbench, Workshop::CollectLinkedWorkshopContainers puts it first; the crafting menus':
	// nothing, a fresh list the gather copies into the menu's after), a_own: the workbenches our own world's supply lines
	// reach (what the engine found), a_keepers: the keeper's we view (Views::Viewed). What goes after a_have, in order: the
	// keeper's first, in the keeper's engine's order, then our own world's that aren't among them (our own stash there, or
	// our copy of one the session shares that we don't view: our own things either way); none already in the list, none
	// twice. The player, when the gather adds it, comes after these, as the engine's last.
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

	// A friend's game: which of the keeper's linked workbenches to view with the keeper's workbench, and when. The server
	// sends a player at most a burst of 16 views' contents, then 8 a second (Server.cpp STORAGE_VIEW_BURST); one past that
	// is answered closed. So no more than MAX are viewed (the first in the keeper's order; the rest stay our own, as our
	// world's supply lines are), a new one opens no sooner than EVERY after the last, and one the server closed (or
	// answered closed) is tried again after RETRY, doubling each time up to RETRY_MAX until its contents come.
	class Views
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr std::size_t MAX = 8;  // with the workbench's own view and the views asked afresh, well under 16
		static constexpr Clock::duration EVERY = std::chrono::milliseconds{ 250 };  // 4 a second: under the server's 8
		static constexpr Clock::duration RETRY = std::chrono::seconds{ 5 };
		static constexpr Clock::duration RETRY_MAX = std::chrono::seconds{ 60 };

		struct Step
		{
			std::vector<std::uint32_t>   close;   // viewed, no longer linked (or past MAX): to end
			std::optional<std::uint32_t> open;    // to open now
			std::vector<std::uint32_t>   wanted;  // the links to view, in the keeper's order (at most MAX)
		};

		// a_linked: the keeper's links (Heard::With), a_viewed: the containers we view besides the keeper's workbench.
		Step Next(std::span<const std::uint32_t> a_linked, std::span<const std::uint32_t> a_viewed, Clock::time_point a_now)
		{
			Step step;
			step.wanted.assign(a_linked.begin(), a_linked.begin() + static_cast<std::ptrdiff_t>((std::min)(a_linked.size(), MAX)));
			for (const auto viewed : a_viewed) {
				if (std::ranges::find(step.wanted, viewed) == step.wanted.end()) {
					step.close.push_back(viewed);
				}
			}
			std::erase_if(retries, [&](const auto& a_entry) { return std::ranges::find(step.wanted, a_entry.first) == step.wanted.end(); });
			if (a_now < openAt) {
				return step;
			}
			for (const auto link : step.wanted) {
				if (std::ranges::find(a_viewed, link) != a_viewed.end()) {
					continue;
				}
				auto& retry = retries[link];
				if (a_now < retry.at) {
					continue;
				}
				// Tried again after the backoff unless its contents come first (Shown).
				retry.at = a_now + retry.backoff;
				retry.backoff = (std::min)(retry.backoff * 2, RETRY_MAX);
				openAt = a_now + EVERY;
				step.open = link;
				break;
			}
			return step;
		}

		// Its contents came: it is tried again (after a close) at the first interval.
		void Shown(std::uint32_t a_link)
		{
			if (const auto it = retries.find(a_link); it != retries.end()) {
				it->second.backoff = RETRY;
			}
		}

		// The server closed its view: not tried again before the backoff.
		void Closed(std::uint32_t a_link, Clock::time_point a_now)
		{
			auto& retry = retries[a_link];
			retry.at = (std::max)(retry.at, a_now + retry.backoff);
			retry.backoff = (std::min)(retry.backoff * 2, RETRY_MAX);
		}

		// The keeper's workbench isn't viewed any more (the menu closed, a load, the connection).
		void Clear()
		{
			retries.clear();
			openAt = {};
		}

	private:
		struct Retry
		{
			Clock::time_point at{};
			Clock::duration   backoff = RETRY;
		};

		std::unordered_map<std::uint32_t, Retry> retries;
		Clock::time_point                        openAt{};
	};
}
