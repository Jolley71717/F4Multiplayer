#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// Settlement attacks are the host's (docs/DESIGN.md, "Settlement attacks"). WorkshopScript.DailyUpdate rolls an attack on
// a settlement its player owns; WorkshopParentScript.TriggerAttack sends the WorkshopEventAttack story event with the
// workbench, and the story manager starts one of the Workshop*Attack quests below, all of them WorkshopAttackScript and
// REScript (read off the game's scripts): stage 10 starts a 24 game-hour timer, whose end begins the attack (stage 20:
// the attackers of its created Attackers collection are enabled, and, with the settlement not loaded and the player's,
// the "under attack" message and objective 10); stage 100 is set when they are dead; stage 1000, the quest's shutdown
// stage, runs CheckResolveAttack, which, with stage 100 not done and the workbench not 3D-loaded, rolls the attack's
// damage on the settlement (WorkshopParentScript.ResolveAttack: food, water, safety, power, population, and the
// workbench's food, water, scrap and caps).
//
// The host decides: their game's run is the attack, and its outcome the only one. A friend's game runs the same quest at
// the same settlement as a mirror (Mirror) that never begins, fights or resolves on its own: its timer is cancelled, its
// attackers stay disabled (the host's are the friend's to fight, shared as spawns while the host's game has them loaded),
// stage 100 is set before it stops, so its 1000 never rolls damage. A friend's own roll at a settlement the host owns, or
// at one the host's attack is at, is stopped the same way (Judge). Pure, so it is tested on its own and with the server
// (tests/AttackBookTests.cpp, tests/ServerAttackTests.cpp); src/game/SettlementAttacks.cpp reads the game for it.
namespace AttackBook
{
	using Clock = std::chrono::steady_clock;
	using Phase = Protocol::AttackPhase;
	using Outcome = Protocol::AttackOutcome;

	// WorkshopAttackScript's stages and aliases on every quest below (their attackStartStage and attackDoneStage, and
	// the Workshop alias the event's reference fills).
	constexpr std::uint16_t START_STAGE = 20;
	constexpr std::uint16_t DONE_STAGE = 100;
	constexpr std::uint16_t OBJECTIVE = 10;          // "Defend <settlement>", shown at stage 20, completed at 100
	constexpr std::uint32_t LOCATION_ALIAS = 0;      // myLocation: the event's location
	constexpr std::uint32_t WORKSHOP_ALIAS = 1;      // Workshop: the settlement's workbench

	struct Attack
	{
		std::uint32_t    quest = 0;  // its form ID in Fallout4.esm (load index 0)
		std::string_view editorId;
	};

	// The random attacks the story manager starts from WorkshopEventAttack, Fallout4.esm's alone for now: Preston's
	// "Defend X" jobs (MinRadiantOwned*), the Castle's defence and the add-ons' attacks are each player's own still (the
	// Brotherhood's in Nuka-World bring a vertibird and its crew; Far Harbor's are Preston's shape).
	inline constexpr std::array<Attack, 13> ATTACKS{ {
		{ 0x00097CC6, "WorkshopSuperMutantAttack01" },
		{ 0x00097CC7, "WorkshopFeralGhoulAttack01" },
		{ 0x000C97A8, "WorkshopGunnerAttack01" },
		{ 0x000CFF72, "WorkshopGunnerAttack02" },
		{ 0x001125A6, "WorkshopRaiderAttack03" },
		{ 0x001125E2, "WorkshopRaiderAttack01" },
		{ 0x001125E3, "WorkshopRaiderAttack02" },
		{ 0x002407D0, "WorkshopMirelurkAttack_SI" },
		{ 0x0024907E, "WorkshopRadscorpionAttack01" },
		{ 0x0024907F, "WorkshopDeathclawAttack01" },
		{ 0x00249080, "WorkshopYaoGuaiAttack01" },
		{ 0x00249081, "WorkshopDeathclawAttack02" },
		{ 0x0024918C, "WorkshopSuperMutantAttack02" },
	} };

	// The attack a Fallout4.esm form ID is (its ID there, the load index left out), or none.
	[[nodiscard]] inline const Attack* Find(std::uint32_t a_esmId)
	{
		const auto it = std::ranges::find(ATTACKS, a_esmId & 0x00FFFFFFu, &Attack::quest);
		return it != ATTACKS.end() ? &*it : nullptr;
	}

	// An attack quest as a game reads it at a look: whether it runs, at which settlement, and its stages.
	struct Seen
	{
		std::uint32_t quest = 0;
		bool          running = false;
		std::uint32_t workshop = 0;  // its Workshop alias (0: empty)
		std::uint32_t location = 0;  // its myLocation alias (0: empty)
		bool          begun = false;  // START_STAGE done
		bool          done = false;   // DONE_STAGE done
	};

	// How a run that ended went, by the stages it reached.
	[[nodiscard]] constexpr Outcome OutcomeOf(bool a_begun, bool a_done)
	{
		return a_done ? Outcome::kDefended : a_begun ? Outcome::kResolved : Outcome::kCalledOff;
	}

	[[nodiscard]] constexpr Phase PhaseOf(bool a_begun, bool a_done)
	{
		return a_done ? Phase::kDefended : a_begun ? Phase::kUnderway : Phase::kRolled;
	}

	// The host's game: its runs, reported as each is rolled, begins, is defended and ends. Every attack quest is looked at
	// each time (running or not); a run that stops, or moves to another settlement between looks, is over.
	class HostRuns
	{
	public:
		[[nodiscard]] std::vector<Protocol::SettlementAttack> Look(const std::vector<Seen>& a_seen)
		{
			std::vector<Protocol::SettlementAttack> reports;
			for (const auto& seen : a_seen) {
				const auto it = runs.find(seen.quest);
				const bool live = seen.running && seen.workshop != 0;
				if (it != runs.end() && (!live || it->second.workshop != seen.workshop)) {
					auto over = it->second;
					over.phase = Phase::kOver;
					// A run that moved was over before this look; one that stopped ended with the stages it has now.
					over.outcome = live ? OutcomeOf(it->second.phase >= Phase::kUnderway, it->second.phase >= Phase::kDefended) : OutcomeOf(seen.begun, seen.done);
					reports.push_back(over);
					runs.erase(it);
				}
				if (!live) {
					continue;
				}
				const auto phase = PhaseOf(seen.begun, seen.done);
				auto [run, fresh] = runs.try_emplace(seen.quest, Protocol::SettlementAttack{ seen.workshop, seen.quest, seen.location, phase, Outcome::kNone });
				if (!fresh && run->second.phase >= phase && (seen.location == 0 || run->second.location == seen.location)) {
					continue;
				}
				run->second.phase = std::max(run->second.phase, phase);
				if (seen.location != 0) {
					run->second.location = seen.location;
				}
				reports.push_back(run->second);
			}
			return reports;
		}

		[[nodiscard]] std::vector<Protocol::SettlementAttack> Live() const
		{
			std::vector<Protocol::SettlementAttack> live;
			for (const auto& [quest, run] : runs) {
				live.push_back(run);
			}
			return live;
		}

		[[nodiscard]] std::size_t Size() const { return runs.size(); }
		void                      Reset() { runs.clear(); }

	private:
		std::unordered_map<std::uint32_t, Protocol::SettlementAttack> runs;  // by quest: a quest runs once at a time
	};

	// What the host's game said of a settlement, as a friend's game has it.
	enum class Word : std::uint8_t
	{
		kUnknown,
		kHost,     // the host owns it
		kNotHost,  // the host doesn't (a friend's own settlement in their save; or the host's game has attacks unshared)
	};

	// The server's word and the host's live attacks, as a friend's game keeps them (kSettlementOwner, kSettlementAttack).
	class View
	{
	public:
		static constexpr std::size_t MAX_OWNERS = 256;
		static constexpr std::size_t MAX_LIVE = 32;
		// A live attack never told over (its host's game crashed mid-run) is forgotten after this.
		static constexpr auto LIVE_FOR = std::chrono::hours(3);

		void OnOwner(const Protocol::SettlementOwner& a_owner)
		{
			if (a_owner.workshop == 0) {
				owners.clear();  // the host left: nothing they said stands
				live.clear();
				return;
			}
			if (owners.size() < MAX_OWNERS || owners.contains(a_owner.workshop)) {
				owners[a_owner.workshop] = a_owner.hostOwned;
			}
		}

		// The host's attack: kept while it is live; the phase it was in before, if it was (none: new here).
		std::optional<Phase> OnAttack(const Protocol::SettlementAttack& a_attack, Clock::time_point a_now)
		{
			const auto it = live.find(a_attack.quest);
			const auto before = it != live.end() ? std::optional{ it->second.attack.phase } : std::nullopt;
			if (a_attack.phase == Phase::kOver) {
				if (it != live.end()) {
					live.erase(it);
				}
				return before;
			}
			// The host owns where their attack is, as far as a friend's game cares: a roll of its own there gives way.
			if (owners.size() < MAX_OWNERS || owners.contains(a_attack.workshop)) {
				owners.try_emplace(a_attack.workshop, true);
			}
			if (it != live.end()) {
				it->second.attack = a_attack;  // a phase on, or the quest moved to another settlement (a new roll)
				return before;
			}
			if (live.size() >= MAX_LIVE) {
				return before;
			}
			live.emplace(a_attack.quest, Live{ a_attack, a_now });
			return before;
		}

		[[nodiscard]] Word WordOn(std::uint32_t a_workshop) const
		{
			const auto it = owners.find(a_workshop);
			return it == owners.end() ? Word::kUnknown : it->second ? Word::kHost : Word::kNotHost;
		}

		// The host's live attack by its quest, or at a settlement.
		[[nodiscard]] const Protocol::SettlementAttack* LiveOf(std::uint32_t a_quest) const
		{
			const auto it = live.find(a_quest);
			return it != live.end() ? &it->second.attack : nullptr;
		}

		[[nodiscard]] bool LiveAt(std::uint32_t a_workshop) const
		{
			return std::ranges::any_of(live, [&](const auto& a_entry) { return a_entry.second.attack.workshop == a_workshop; });
		}

		[[nodiscard]] std::vector<Protocol::SettlementAttack> AllLive() const
		{
			std::vector<Protocol::SettlementAttack> out;
			for (const auto& [quest, entry] : live) {
				out.push_back(entry.attack);
			}
			return out;
		}

		// Live attacks too old to trust are dropped: the quests they were.
		std::vector<std::uint32_t> Expire(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> gone;
			std::erase_if(live, [&](const auto& a_entry) {
				if (a_now - a_entry.second.since < LIVE_FOR) {
					return false;
				}
				gone.push_back(a_entry.first);
				return true;
			});
			return gone;
		}

		[[nodiscard]] std::size_t Owners() const { return owners.size(); }
		[[nodiscard]] std::size_t LiveCount() const { return live.size(); }

		void Reset()
		{
			owners.clear();
			live.clear();
		}

	private:
		struct Live
		{
			Protocol::SettlementAttack attack;
			Clock::time_point          since{};
		};

		std::unordered_map<std::uint32_t, bool> owners;  // workbench -> the host's
		std::unordered_map<std::uint32_t, Live> live;    // by quest
	};

	// A friend's game: what to do with a run of its own (not a mirror) at a settlement.
	struct Facts
	{
		Word word = Word::kUnknown;
		bool hostAttackHere = false;  // the host's attack is live at that settlement
		bool begun = false;           // our run's START_STAGE is done: its attackers are out
		bool done = false;            // our run's DONE_STAGE is done: its 1000 rolls nothing
		bool loadedHere = false;      // the workbench is 3D-loaded in our game: our player is there
		bool askedLately = false;     // we asked the server within ASK_AGAIN
	};

	constexpr auto ASK_AGAIN = std::chrono::seconds(60);

	enum class Verdict : std::uint8_t
	{
		kKeep,     // our own: the host doesn't own the settlement, or no word yet (an attack is never stopped on a guess)
		kAsk,      // ask the server whose settlement it is
		kDefuse,   // the host's: set DONE_STAGE first, so its 1000 rolls no damage (whether or not our player is there)
		kHold,     // the host's, defused, and a fight of its own is on around our player: stopped once they leave
		kStop,     // the host's, defused: stopped, its attackers removed
	};

	[[nodiscard]] constexpr Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.hostAttackHere || a_facts.word == Word::kHost) {
			if (!a_facts.done) {
				return Verdict::kDefuse;
			}
			return a_facts.begun && a_facts.loadedHere ? Verdict::kHold : Verdict::kStop;
		}
		if (a_facts.word == Word::kUnknown && !a_facts.askedLately) {
			return Verdict::kAsk;
		}
		return Verdict::kKeep;
	}

	// A friend's game: its mirror of one of the host's attacks (the same quest at the same settlement). What the next step
	// is, by the host's word and what our run has.
	struct MirrorFacts
	{
		Phase hostPhase = Phase::kRolled;  // kOver: the host's run ended (or the host left)
		bool  running = false;             // our quest runs, at the host's settlement
		bool  elsewhere = false;           // our quest runs at another settlement (our own roll of it): no mirror
		bool  begun = false;               // our START_STAGE is done (we never set it: our own timer beat the cancel)
		bool  done = false;                // our DONE_STAGE is done
		bool  objectiveShown = false;      // we showed OBJECTIVE
		bool  loadedHere = false;          // the workbench is 3D-loaded here: our player is at the settlement
		bool  startTried = false;          // a start was asked and hasn't run (or gave up)
	};

	enum class MirrorStep : std::uint8_t
	{
		kNone,
		kStart,          // start our quest with the host's settlement and location forced
		kDefuse,         // set DONE_STAGE: the host's attack was defended, or ours begun on its own, or it is ending
		kShowObjective,  // the host's attack is on and our player is at the settlement: "Defend X" here too
		kStop,           // the host's run ended: ours stops (defused: its 1000 rolls nothing)
	};

	[[nodiscard]] constexpr MirrorStep Next(const MirrorFacts& a_facts)
	{
		if (!a_facts.running) {
			return a_facts.hostPhase != Phase::kOver && !a_facts.elsewhere && !a_facts.startTried ? MirrorStep::kStart : MirrorStep::kNone;
		}
		const bool ending = a_facts.hostPhase == Phase::kOver;
		if (!a_facts.done && (ending || a_facts.begun || a_facts.hostPhase == Phase::kDefended)) {
			return MirrorStep::kDefuse;
		}
		if (ending) {
			return MirrorStep::kStop;
		}
		if (a_facts.hostPhase == Phase::kUnderway && a_facts.loadedHere && !a_facts.objectiveShown && !a_facts.done) {
			return MirrorStep::kShowObjective;
		}
		return MirrorStep::kNone;
	}

	// The notice a friend's game shows for the host's attack, by its text key (TextEnglish.h), or none.
	[[nodiscard]] constexpr std::string_view NoticeFor(Phase a_phase, Outcome a_outcome, bool a_toldUnderway)
	{
		if (a_phase == Phase::kUnderway) {
			return "attack.under";
		}
		if (!a_toldUnderway) {
			return {};  // one never begun (called off), or one we never heard begin, needs no word on its end
		}
		if (a_phase == Phase::kDefended) {
			return "attack.defended";
		}
		if (a_phase == Phase::kOver && a_outcome == Outcome::kResolved) {
			return "attack.lost";
		}
		return {};  // over after a defence: told when it was defended
	}

	// The server's book: what the host's game said of settlements, and its live attacks (for joiners).
	class Book
	{
	public:
		static constexpr std::size_t MAX_OWNERS = View::MAX_OWNERS;
		static constexpr std::size_t MAX_LIVE = View::MAX_LIVE;
		static constexpr auto        LIVE_FOR = View::LIVE_FOR;
		// A friend's question about one settlement goes to the host's game at most this often.
		static constexpr auto ASK_EVERY = std::chrono::seconds(10);

		// The host's game's word on a settlement. Whether to tell everyone: new or changed (or past the cap: told, not kept).
		bool SetOwner(std::uint32_t a_workshop, bool a_hostOwned)
		{
			if (const auto it = owners.find(a_workshop); it != owners.end()) {
				if (it->second == a_hostOwned) {
					return false;
				}
				it->second = a_hostOwned;
				return true;
			}
			if (owners.size() < MAX_OWNERS) {
				owners.emplace(a_workshop, a_hostOwned);
			}
			return true;
		}

		[[nodiscard]] std::optional<bool> OwnerOf(std::uint32_t a_workshop) const
		{
			const auto it = owners.find(a_workshop);
			return it != owners.end() ? std::optional{ it->second } : std::nullopt;
		}

		// The host's attack, a phase of it. Whether to pass it on: a phase it had already isn't.
		bool OnHostAttack(const Protocol::SettlementAttack& a_attack, Clock::time_point a_now)
		{
			const auto it = live.find(a_attack.quest);
			if (a_attack.phase == Phase::kOver) {
				if (it == live.end()) {
					return true;  // over before the book had it (past the cap): the others may have it
				}
				live.erase(it);
				return true;
			}
			if (it != live.end()) {
				if (it->second.attack.workshop == a_attack.workshop && it->second.attack.phase == a_attack.phase && it->second.attack.location == a_attack.location) {
					return false;
				}
				it->second.attack = a_attack;
				return true;
			}
			if (live.size() < MAX_LIVE) {
				live.emplace(a_attack.quest, Booked{ a_attack, a_now });
			}
			return true;
		}

		// Whether a friend's question about a settlement goes to the host's game now (once per ASK_EVERY).
		bool Ask(std::uint32_t a_workshop, Clock::time_point a_now)
		{
			if (const auto it = asked.find(a_workshop); it != asked.end() && a_now - it->second < ASK_EVERY) {
				return false;
			}
			if (asked.size() >= MAX_OWNERS) {
				std::erase_if(asked, [&](const auto& a_entry) { return a_now - a_entry.second >= ASK_EVERY; });
				if (asked.size() >= MAX_OWNERS) {
					return false;
				}
			}
			asked[a_workshop] = a_now;
			return true;
		}

		[[nodiscard]] std::vector<Protocol::SettlementOwner> Owners() const
		{
			std::vector<Protocol::SettlementOwner> out;
			for (const auto& [workshop, owned] : owners) {
				out.push_back({ workshop, owned });
			}
			return out;
		}

		[[nodiscard]] std::vector<Protocol::SettlementAttack> Live() const
		{
			std::vector<Protocol::SettlementAttack> out;
			for (const auto& [quest, entry] : live) {
				out.push_back(entry.attack);
			}
			return out;
		}

		// Live attacks never told over after LIVE_FOR are dropped (their host's game went without a word).
		std::size_t Expire(Clock::time_point a_now)
		{
			return std::erase_if(live, [&](const auto& a_entry) { return a_now - a_entry.second.since >= LIVE_FOR; });
		}

		[[nodiscard]] std::size_t OwnersKept() const { return owners.size(); }
		[[nodiscard]] std::size_t LiveCount() const { return live.size(); }

		void Clear()
		{
			owners.clear();
			live.clear();
			asked.clear();
		}

	private:
		struct Booked
		{
			Protocol::SettlementAttack attack;
			Clock::time_point          since{};
		};

		std::unordered_map<std::uint32_t, bool>              owners;
		std::unordered_map<std::uint32_t, Booked>            live;  // by quest
		std::unordered_map<std::uint32_t, Clock::time_point> asked;
	};
}
