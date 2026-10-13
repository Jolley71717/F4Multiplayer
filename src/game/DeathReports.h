#pragma once

#include "game/FoundDead.h"
#include "game/RemoteEdits.h"

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// The deaths other players' games report, as our game takes them: each is judged against our copy once
// it is loaded (WorldSync kills it, or leaves a character of our own story alive), and held until then.
// Kept free of the game, with where each player is passed in, so the order things arrive in can be
// tested with what the real server sends.
namespace DeathReports
{
	using Clock = std::chrono::steady_clock;
	using RemoteEdits::Spot;

	// A player's position comes unreliably, a few times a second; a death comes reliably, at once. A kill
	// made right after walking in reaches us before the steps that brought the killer there, so a report
	// that seems to come from nowhere near waits this long for their position to catch up before it is
	// refused.
	constexpr auto CATCH_UP = std::chrono::seconds(2);

	struct Report
	{
		std::uint32_t       killer = 0;                    // the player whose kill it was (their stand-in pushes the corpse), or 0
		float               x = 0.0f, y = 0.0f, z = 0.0f;  // where it fell in the reporter's game (all zero: unknown)
		std::uint32_t       reporter = 0;                  // the player whose game reported it (0: replayed from the session)
		std::optional<Spot> reporterThen;                  // where our game had heard the reporter was when it came
		Clock::time_point   heard{};
	};

	// Our copy, loaded: what kind of character it is, and where it stands.
	struct Copy
	{
		FoundDead::Npc npc;
		Spot           at;
	};

	enum class Verdict
	{
		kKill,
		kWait,       // the reporter's position may still be on its way: judged again later
		kKeepAlive,  // a character of our own story, killed nowhere near the reporter: for good
	};

	// Where our game last heard a player was (nothing if never, or they have gone).
	using Where = std::function<std::optional<Spot>(std::uint32_t)>;

	class Book
	{
	public:
		explicit Book(Where a_where) :
			where(std::move(a_where))
		{}

		// A death as it came off the wire, from the player who reported it (the server names them), with where
		// they were then.
		[[nodiscard]] Report Received(const Protocol::ActorDeath& a_death, Clock::time_point a_now) const
		{
			Report report{ a_death.killed ? a_death.playerId : 0, a_death.x, a_death.y, a_death.z, a_death.playerId };
			report.reporterThen = a_death.playerId ? where(a_death.playerId) : std::nullopt;
			report.heard = a_now;
			return report;
		}

		// A death the session replays (the world state, on joining or after loading a save): nobody is known to
		// have seen it.
		[[nodiscard]] static Report Replayed(Clock::time_point a_now)
		{
			Report report;
			report.heard = a_now;
			return report;
		}

		// Whether the report kills our copy now: anyone else dies; a unique or essential character of our own
		// story only if the reporter could have seen it, from where they were when it came or from where they
		// are now (RemoteEdits::KillsOurCopy). Until CATCH_UP has passed, a report from a player who seems
		// nowhere near waits rather than being refused for good.
		[[nodiscard]] Verdict Judge(bool a_sharedStory, const Report& a_report, const Copy& a_ours, Clock::time_point a_now) const
		{
			const auto now = a_report.reporter ? where(a_report.reporter) : std::nullopt;
			if (RemoteEdits::KillsOurCopy(a_sharedStory, a_ours.npc, a_report.reporterThen, now, a_ours.at)) {
				return Verdict::kKill;
			}
			return a_report.reporter && a_now - a_report.heard < CATCH_UP ? Verdict::kWait : Verdict::kKeepAlive;
		}

		// Held until our copy can be judged (it isn't loaded yet, or the judge said wait).
		void Hold(std::uint32_t a_ref, const Report& a_report) { held[a_ref] = a_report; }
		void Forget(std::uint32_t a_ref) { held.erase(a_ref); }
		[[nodiscard]] bool Holds(std::uint32_t a_ref) const { return held.contains(a_ref); }
		void Clear() { held.clear(); }
		[[nodiscard]] bool        Empty() const { return held.empty(); }
		[[nodiscard]] std::size_t Size() const { return held.size(); }

		// Tries each held report again: a_try(ref, report) returns true once it is done with.
		template <class Try>
		void Retry(Try&& a_try)
		{
			std::erase_if(held, [&](const auto& a_entry) { return a_try(a_entry.first, a_entry.second); });
		}

		// Tries the report held for a_ref again, if there is one (as a_ref loads). True when it was done with.
		template <class Try>
		bool RetryOne(std::uint32_t a_ref, Try&& a_try)
		{
			const auto it = held.find(a_ref);
			if (it == held.end() || !a_try(it->first, it->second)) {
				return false;
			}
			held.erase(it);
			return true;
		}

		// Every reference a report is held for.
		[[nodiscard]] std::vector<std::uint32_t> Refs() const
		{
			std::vector<std::uint32_t> refs;
			refs.reserve(held.size());
			for (const auto& [ref, report] : held) {
				refs.push_back(ref);
			}
			return refs;
		}

	private:
		Where                                      where;
		std::unordered_map<std::uint32_t, Report> held;
	};
}
