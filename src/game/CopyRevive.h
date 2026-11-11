#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Our copy of an NPC another player's game runs lies dead here while that NPC lives there: raised (WorldSync's
// RaiseCopies, the engine's Actor::Resurrect), so it is mirrored again. A copy dies of what only our game saw: a throw's
// blast its runner's game deals itself (CopyWatch's OwnersToDeal), where the NPC stands elsewhere there (on the rig, a
// friend's grenade killed three copies left where the host's game had them before it restarted; the raiders stood
// elsewhere in the host's game and lived on), or a drop no hit explains (a GitHub report: a copy at -185 health, no
// killer). The server rightly drops that death (its runner's game is the judge), and up to 0.16.1 the copy lay dead for
// good, until its cell reset: the friend saw a corpse where the others fought a raider. Kept free of the game so it can
// be tested.
namespace CopyRevive
{
	using Clock = std::chrono::steady_clock;

	// The copy has lain dead this long, all the facts holding, before it is raised: its runner's death report (sent as
	// the NPC begins to fall) ends the ownership long before this.
	constexpr auto DEAD_FOR = std::chrono::seconds(3);
	// A try that left the copy dead (the engine refused) is tried again this much later, at most TRIES times.
	constexpr auto          RETRY_AFTER = std::chrono::seconds(10);
	constexpr std::uint32_t TRIES = 3;
	constexpr std::size_t   MAX_COPIES = 256;
	constexpr auto          FORGET_AFTER = std::chrono::minutes(2);
	// A copy is raised only with a state of it from its runner's game this recent at that moment (Facts::heardNow).
	// Heard within NpcPlace::LOST_AFTER (3 s: Facts::heardAlive) is too loose for the raise itself: an NPC that began to
	// fall there just before it sends no more states but still counts as heard, and the copy would be raised in the
	// latency gap before that death and its owner message arrive, then killed again moments later. A runner sends 10 to
	// 20 states a second.
	constexpr auto HEARD_NOW = std::chrono::seconds(1);

	struct Facts
	{
		bool deadHere = false;       // our copy lies dead (not just down: SessionDeaths::GetsUp), loaded
		bool deadForUs = false;      // the session's death as our game knows it (SessionDeaths::Ledger::Contains)
		bool runsElsewhere = false;  // another player's game runs it (NpcSync::Owner)
		bool heardAlive = false;     // a state of it came from that game lately (NpcSync::HeardDead): it lives there
		bool heardNow = false;       // and one came within HEARD_NOW: alive there now, not just lately (needed for the raise)
		bool inMenu = false;         // our player has a menu open on it (the container menu: looting it); no raise meanwhile
		bool story = false;          // a character of a story (FoundDead::IsStoryCharacter): left as it lies
	};

	enum class Verdict
	{
		kNo,      // not a copy dead while its NPC lives (or a story character's): nothing to do, and its clock starts over
		kWait,    // not yet (DEAD_FOR, RETRY_AFTER), or due but held (no state within HEARD_NOW, or a menu open on it)
		kRaise,   // raise it now (a try is counted)
		kGiveUp,  // TRIES tries left it dead: left as it lies (said once)
	};

	class Book
	{
	public:
		// Whether to raise the copy now. Its tries are kept until Prune forgets it, so a copy raised that dies again soon
		// after (whatever killed it here is still there) is tried TRIES times in all, not forever.
		Verdict Judge(std::uint32_t a_id, const Facts& a_facts, Clock::time_point a_now)
		{
			auto       it = entries.find(a_id);
			const bool holds = a_facts.deadHere && !a_facts.deadForUs && a_facts.runsElsewhere && a_facts.heardAlive && !a_facts.story;
			if (!holds) {
				if (it != entries.end()) {
					it->second.judged = a_now;
					it->second.since.reset();  // its clock starts over
				}
				return Verdict::kNo;
			}
			if (it == entries.end()) {
				if (entries.size() >= MAX_COPIES) {
					return Verdict::kNo;
				}
				it = entries.emplace(a_id, Entry{}).first;
			}
			auto& entry = it->second;
			entry.judged = a_now;
			if (!entry.since) {
				entry.since = a_now;
			}
			if (entry.gaveUp) {
				return Verdict::kNo;
			}
			if (a_now - *entry.since < DEAD_FOR || (entry.tries > 0 && a_now - entry.tried < RETRY_AFTER)) {
				return Verdict::kWait;
			}
			if (entry.tries >= TRIES) {
				entry.gaveUp = true;
				++gaveUp;
				return Verdict::kGiveUp;
			}
			// Due, but held without spending a try: its runner's NPC may be falling there right now (no state within
			// HEARD_NOW), or our player is looting it (a raise would change the body under the open menu). It is raised at
			// the first look after a fresh state comes and the menu closes, its wait (since) kept.
			if (!a_facts.heardNow) {
				++heldUnheard;
				return Verdict::kWait;
			}
			if (a_facts.inMenu) {
				++heldInMenu;
				return Verdict::kWait;
			}
			++entry.tries;
			entry.tried = a_now;
			++raised;
			return Verdict::kRaise;
		}

		// Forgets the copies not judged since a_before (no longer heard of: raised and mirrored, unloaded, or dead there).
		// WorldSync forgets after FORGET_AFTER, so tries add up over a copy that keeps dying.
		void Prune(Clock::time_point a_before)
		{
			std::erase_if(entries, [&](const auto& a_entry) { return a_entry.second.judged < a_before; });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::uint32_t Raised() const { return raised; }  // tries
		[[nodiscard]] std::uint32_t GaveUp() const { return gaveUp; }
		[[nodiscard]] std::uint32_t HeldUnheard() const { return heldUnheard; }  // looks that held a due raise for a fresh state
		[[nodiscard]] std::uint32_t HeldInMenu() const { return heldInMenu; }    // looks that held a due raise for a menu on it
		[[nodiscard]] std::size_t   Size() const { return entries.size(); }

	private:
		struct Entry
		{
			std::optional<Clock::time_point> since;  // when the facts last began to hold together
			Clock::time_point judged{};              // the last Judge
			Clock::time_point tried{};
			std::uint32_t     tries = 0;
			bool              gaveUp = false;
		};

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint32_t                            raised = 0;
		std::uint32_t                            gaveUp = 0;
		std::uint32_t                            heldUnheard = 0;
		std::uint32_t                            heldInMenu = 0;
	};
}
