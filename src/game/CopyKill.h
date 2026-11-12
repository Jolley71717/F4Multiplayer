#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// A death another player's game reports, applied to our reference of that NPC (Spawns::ApplyDeath: our copy of a friend's
// runtime NPC, or our own one a friend killed first). Kept free of the game so it can be tested.
//
// Protected: only the player may kill it. The engine's kill (Actor::KillImpl, 1.11.240 +CA00C0) asks whether the killer
// may kill this actor (CanKillMe +C5B410): an essential or a child never, a protected one only by the player (Actor::boolFlags
// kProtected, which the kill works out again first, at +CA0297, from the reference's override, its base and its aliases);
// otherwise it is only knocked down. The console's `kill` names no killer, so a
// protected one stood on, though the report was taken as applied: a Railroad job's courser (RRR07's CourserTarget, a
// protected alias) the host killed stood alive in the other game, where its alias holds our copy (rig, 2026-11-12). Naming
// our player as the killer would make it our player's kill (experience, a crime); the reference's protection is overridden
// for the kill instead and put back right after it (WorldSync::KillNamingNoKiller: clearing the flag alone was undone by
// that working out; read from the 1.11.240 disassembly, not yet seen in a game). A kill that leaves it standing is tried again.
//
// And a kill made when it can't land (a loading screen, the copy not loaded yet) was lost for good: the report marks the
// spawn dead at once (SpawnBook::Theirs::Died), so nothing tried again. Each death waits here until our reference lies
// dead or is gone, tried again every RETRY_AFTER while it stands loaded in the world, at most TRIES times.
namespace CopyKill
{
	using Clock = std::chrono::steady_clock;

	constexpr auto          RETRY_AFTER = std::chrono::seconds(2);
	constexpr std::uint32_t TRIES = 4;
	constexpr std::size_t   MAX = 256;

	// Whether its protection is lifted for the kill: protected, but not essential (an essential one is only knocked down
	// whoever kills it: WorldSync holds those deaths).
	constexpr bool LiftsProtection(bool a_protected, bool a_essential)
	{
		return a_protected && !a_essential;
	}

	struct Facts
	{
		bool there = false;     // our reference exists and is not deleted
		bool deadHere = false;  // it lies dead here
		bool inWorld = false;   // our player is in the world (WorldSync::InWorld: not a loading screen or the main menu)
		bool loaded = false;    // it has its 3D
	};

	enum class Verdict
	{
		kNone,    // no death waits for it
		kDone,    // dead here, or gone: forgotten
		kWait,    // not now (out of the world, not loaded, or a try made less than RETRY_AFTER ago)
		kKill,    // kill it now (a try is counted)
		kGiveUp,  // TRIES tries left it standing: forgotten (said once, by the caller)
	};

	class Book
	{
	public:
		// Its NPC died in another player's game. Known already: kept as it is (its tries go on).
		void Add(std::uint32_t a_local)
		{
			if (a_local == 0 || waiting.contains(a_local)) {
				return;
			}
			if (waiting.size() >= MAX) {
				waiting.erase(waiting.begin());
			}
			waiting.emplace(a_local, Entry{});
		}

		Verdict Judge(std::uint32_t a_local, const Facts& a_facts, Clock::time_point a_now)
		{
			const auto it = waiting.find(a_local);
			if (it == waiting.end()) {
				return Verdict::kNone;
			}
			if (!a_facts.there || a_facts.deadHere) {
				waiting.erase(it);
				return Verdict::kDone;
			}
			if (!a_facts.inWorld || !a_facts.loaded) {
				return Verdict::kWait;
			}
			auto& entry = it->second;
			if (entry.tries > 0 && a_now - entry.triedAt < RETRY_AFTER) {
				return Verdict::kWait;
			}
			if (entry.tries >= TRIES) {
				waiting.erase(it);
				return Verdict::kGiveUp;
			}
			++entry.tries;
			entry.triedAt = a_now;
			return Verdict::kKill;
		}

		// The tries made on it so far (0: none, or no death waits for it).
		[[nodiscard]] std::uint32_t Tries(std::uint32_t a_local) const
		{
			const auto it = waiting.find(a_local);
			return it != waiting.end() ? it->second.tries : 0;
		}

		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(waiting.size());
			for (const auto& [id, entry] : waiting) {
				ids.push_back(id);
			}
			return ids;
		}

		[[nodiscard]] std::size_t Size() const { return waiting.size(); }
		void                      Clear() { waiting.clear(); }

	private:
		struct Entry
		{
			std::uint32_t     tries = 0;
			Clock::time_point triedAt{};
		};
		std::unordered_map<std::uint32_t, Entry> waiting;
	};
}
