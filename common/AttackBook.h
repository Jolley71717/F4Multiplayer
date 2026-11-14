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
// the same settlement as a mirror (Mirror) that never begins, fights or resolves on its own: stage 100 is set as soon as it
// runs, so no save ever holds it able to roll damage (stage 100's own fragment only registers for RECheckForCleanup,
// completes objective 10 and clears WorkshopActorAlert; it never stops the quest), its timer is pushed out, its attackers
// stay disabled (the host's are the friend's to fight, shared as spawns while the host's game has them loaded), and its
// timer is cancelled before it stops. A friend's own roll at a settlement the host owns, or at one the host's attack is
// at, is stopped the same way (Judge). A mirror left behind (a session that ended out of the world, a save loaded later)
// has stage 100 done and stage 20 not: every load stops such a run (Leftover). Pure, so it is tested on its own and with
// the server (tests/AttackBookTests.cpp, tests/ServerAttackTests.cpp); src/game/SettlementAttacks.cpp reads the game for it.
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
		std::string_view file;       // the game file it comes from (Fallout4.esm, or an add-on's master)
		std::uint32_t    quest = 0;  // its form ID in that file (the load index left out: an add-on's is the player's load order's)
		std::string_view editorId;
	};

	inline constexpr std::string_view FALLOUT4 = "Fallout4.esm";
	inline constexpr std::string_view FAR_HARBOR = "DLCCoast.esm";
	inline constexpr std::string_view AUTOMATRON = "DLCRobot.esm";
	inline constexpr std::string_view NUKA_WORLD = "DLCNukaWorld.esm";

	// The random attacks the story manager starts from WorkshopEventAttack: Fallout4.esm's, and the add-ons' (read off
	// their files: every one is WorkshopAttackScript and REScript with the same aliases and stages, none overriding the
	// script's start and done stages; Automatron's merge their robots into the Attackers collection at stage 10; Nuka-World's
	// are on the raider outposts and turn the outpost's turrets on the attackers; the Brotherhood's 02 and 03 there bring
	// a vertibird and its crew, created disabled and enabled at stages 20 and 22, which a mirror never reaches, and the
	// host's reach a friend as any runtime actors the host's game has loaded). Not here: Preston's "Defend X" jobs
	// (MinRadiantOwned*), shared as radiants, the host's run alone (src/game/RadiantRules.h, Entry::attack); the Castle's
	// defence, each player's own; VRWorkshopShared_AttackQuest (Fallout4.esm 0024A38D) is no
	// settlement attack (a Creation Club VR workshop's wave: no WorkshopAttackScript, no event, no Workshop alias).
	inline constexpr std::array<Attack, 24> ATTACKS{ {
		{ FALLOUT4, 0x00097CC6, "WorkshopSuperMutantAttack01" },
		{ FALLOUT4, 0x00097CC7, "WorkshopFeralGhoulAttack01" },
		{ FALLOUT4, 0x000C97A8, "WorkshopGunnerAttack01" },
		{ FALLOUT4, 0x000CFF72, "WorkshopGunnerAttack02" },
		{ FALLOUT4, 0x001125A6, "WorkshopRaiderAttack03" },
		{ FALLOUT4, 0x001125E2, "WorkshopRaiderAttack01" },
		{ FALLOUT4, 0x001125E3, "WorkshopRaiderAttack02" },
		{ FALLOUT4, 0x002407D0, "WorkshopMirelurkAttack_SI" },
		{ FALLOUT4, 0x0024907E, "WorkshopRadscorpionAttack01" },
		{ FALLOUT4, 0x0024907F, "WorkshopDeathclawAttack01" },
		{ FALLOUT4, 0x00249080, "WorkshopYaoGuaiAttack01" },
		{ FALLOUT4, 0x00249081, "WorkshopDeathclawAttack02" },
		{ FALLOUT4, 0x0024918C, "WorkshopSuperMutantAttack02" },
		{ FAR_HARBOR, 0x0004B651, "DLC03WorkshopTrapperAttack01" },
		{ AUTOMATRON, 0x0000FD0D, "DLC01WorkshopRobotAttack01" },
		{ AUTOMATRON, 0x0000FD13, "DLC01WorkshopRobotAttack02" },
		{ AUTOMATRON, 0x0000FD17, "DLC01WorkshopBotRaiderAttack01" },
		{ AUTOMATRON, 0x0000FD1B, "DLC01WorkshopBotRaiderAttack02" },
		{ NUKA_WORLD, 0x00035D8C, "DLC04WorkshopMinutemenAttack01" },
		{ NUKA_WORLD, 0x0003638D, "DLC04WorkshopMinutemenAttack02" },
		{ NUKA_WORLD, 0x0003638C, "DLC04WorkshopMinutemenAttack03" },
		{ NUKA_WORLD, 0x00035F1C, "DLC04WorkshopBOSAttack01" },
		{ NUKA_WORLD, 0x00035F1D, "DLC04WorkshopBOSAttack02" },
		{ NUKA_WORLD, 0x0003638B, "DLC04WorkshopBOSAttack03" },
	} };

	// Two file names are the same file (the game compares them so).
	[[nodiscard]] constexpr bool SameFile(std::string_view a_one, std::string_view a_other)
	{
		return std::ranges::equal(a_one, a_other, [](unsigned char a, unsigned char b) {
			const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c; };
			return lower(a) == lower(b);
		});
	}

	// The attack a form ID of a file is (its ID there, the load index left out), or none.
	[[nodiscard]] inline const Attack* Find(std::string_view a_file, std::uint32_t a_id)
	{
		const auto it = std::ranges::find_if(ATTACKS, [&](const Attack& a_attack) { return a_attack.quest == (a_id & 0x00FFFFFFu) && SameFile(a_attack.file, a_file); });
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

	// A run a session left defused in this game: stage 100 done, stage 20 not. Only a mirror (defused as it runs) or a
	// friend's own roll at the host's settlement (defused before it is stopped) is ever so: the game's own runs reach 100
	// only through their attackers' deaths, which come after 20. Stopped on every load, in a session or not, so its pushed
	// timer never begins an attack in a save of its own.
	[[nodiscard]] constexpr bool Leftover(const Seen& a_seen)
	{
		return a_seen.running && a_seen.done && !a_seen.begun;
	}

	// Closing a run of ours that is the host's to decide (a mirror, or our own roll at the host's settlement) down: stage
	// 100 first (its 1000 then rolls nothing), then its WorkshopAttackScript timer cancelled (a timer that outlived the stop
	// would SetStage(20), which starts a stopped quest again), then a look later the stop.
	struct CloseFacts
	{
		bool running = false;         // the quest runs (at the run's settlement)
		bool done = false;            // DONE_STAGE is done
		bool timerCancelled = false;  // its timer's cancel was asked a look ago or more
	};

	enum class CloseStep : std::uint8_t
	{
		kNone,         // not running: closed
		kDefuse,       // set DONE_STAGE
		kCancelTimer,  // CancelTimerGameTime(0) on its WorkshopAttackScript
		kStop,         // stopquest
	};

	[[nodiscard]] constexpr CloseStep NextClose(const CloseFacts& a_facts)
	{
		if (!a_facts.running) {
			return CloseStep::kNone;
		}
		if (!a_facts.done) {
			return CloseStep::kDefuse;
		}
		return a_facts.timerCancelled ? CloseStep::kStop : CloseStep::kCancelTimer;
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

	// The host's game: the settlements it has spoken of (an answer, or with an attack of its own), read again every
	// CHECK_EVERY. A change of owner (the host took a settlement through the Minutemen, or lost one) is told at once with
	// the same word (kSettlementOwner), so no friend's game keeps a word that is no longer true.
	class HostWords
	{
	public:
		static constexpr std::size_t MAX = 256;
		static constexpr auto        CHECK_EVERY = std::chrono::seconds(2);

		void Said(std::uint32_t a_workshop, bool a_hostOwned)
		{
			if (a_workshop != 0 && (said.size() < MAX || said.contains(a_workshop))) {
				said[a_workshop] = a_hostOwned;
			}
		}

		// Whether it is time to read them again.
		[[nodiscard]] bool Due(Clock::time_point a_now) const { return !said.empty() && a_now - checked >= CHECK_EVERY; }

		// a_read(workshop): whether the host owns it now (none: it can't be read now, as when its workbench isn't
		// loaded as a form here: left as it was). The words that changed, to tell.
		template <class Read>
		[[nodiscard]] std::vector<Protocol::SettlementOwner> Recheck(Clock::time_point a_now, Read&& a_read)
		{
			checked = a_now;
			std::vector<Protocol::SettlementOwner> changed;
			for (auto& [workshop, owned] : said) {
				const std::optional<bool> now = a_read(workshop);
				if (now && *now != owned) {
					owned = *now;
					changed.push_back({ workshop, owned });
				}
			}
			return changed;
		}

		[[nodiscard]] std::size_t Size() const { return said.size(); }

		void Reset()
		{
			said.clear();
			checked = {};
		}

	private:
		std::unordered_map<std::uint32_t, bool> said;  // workbench -> the host's, as last told
		Clock::time_point                       checked{};
	};

	// How long a word on a settlement is trusted without being said again. The host's game tells changes as they come
	// (HostWords), so this is the backstop for a change it missed.
	constexpr auto OWNER_FOR = std::chrono::minutes(5);

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

		void OnOwner(const Protocol::SettlementOwner& a_owner, Clock::time_point a_now)
		{
			if (a_owner.workshop == 0) {
				owners.clear();  // the host left: nothing they said stands
				live.clear();
				return;
			}
			if (owners.size() >= MAX_OWNERS && !owners.contains(a_owner.workshop)) {
				std::erase_if(owners, [&](const auto& a_entry) { return a_now - a_entry.second.since >= OWNER_FOR; });
			}
			if (owners.size() < MAX_OWNERS || owners.contains(a_owner.workshop)) {
				owners[a_owner.workshop] = Owned{ a_owner.hostOwned, a_now };
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
			// The host owns where their attack is, as far as a friend's game cares: a roll of its own there gives way. (The
			// host's game's own word, sent with the roll, stands over this.)
			if (owners.size() < MAX_OWNERS || owners.contains(a_attack.workshop)) {
				const auto [owner, fresh] = owners.try_emplace(a_attack.workshop, Owned{ true, a_now });
				if (!fresh && a_now - owner->second.since >= OWNER_FOR) {
					owner->second = Owned{ true, a_now };
				}
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

		// The word on a settlement; one older than OWNER_FOR is no word.
		[[nodiscard]] Word WordOn(std::uint32_t a_workshop, Clock::time_point a_now) const
		{
			const auto it = owners.find(a_workshop);
			if (it == owners.end() || a_now - it->second.since >= OWNER_FOR) {
				return Word::kUnknown;
			}
			return it->second.host ? Word::kHost : Word::kNotHost;
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
		struct Owned
		{
			bool              host = false;
			Clock::time_point since{};  // when it was said
		};

		std::unordered_map<std::uint32_t, Owned> owners;  // by workbench
		std::unordered_map<std::uint32_t, Live>  live;    // by quest
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
		kAsk,      // ask the server whose settlement it is (again every ASK_AGAIN while the answer is "not the host's")
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
		// "Not the host's" is asked again too: the host may have taken the settlement since (the Minutemen), and a word
		// kept from before would let this roll do damage on a settlement the host owns.
		if (!a_facts.askedLately) {
			return Verdict::kAsk;
		}
		return Verdict::kKeep;
	}

	// A friend's game: its mirror of one of the host's attacks (the same quest at the same settlement). What the next step
	// is, by the host's word and what our run has.
	struct MirrorFacts
	{
		Phase   hostPhase = Phase::kRolled;     // kOver: the host's run ended (or the host left)
		Outcome outcome = Outcome::kNone;       // the host's run's, once it is over
		bool    running = false;                // our quest runs, at the host's settlement
		bool    elsewhere = false;              // our quest runs at another settlement (our own roll of it): no mirror
		bool    begun = false;                  // our START_STAGE is done (we never set it: our own timer beat the push)
		bool    done = false;                   // our DONE_STAGE is done
		bool    hasObjective = false;           // the quest has OBJECTIVE (the creature attacks have none)
		bool    objectiveShown = false;         // we show OBJECTIVE now
		bool    loadedHere = false;             // the workbench is 3D-loaded here: our player is at the settlement
		bool    hostAttackersHere = false;      // copies of friends' NPCs, alive, stand at the settlement here lately
		bool    startTried = false;             // a start was asked and hasn't run (or gave up)
	};

	enum class MirrorStep : std::uint8_t
	{
		kNone,
		kStart,              // start our quest with the host's settlement and location forced
		kDefuse,             // set DONE_STAGE at once: a mirror never holds a damage roll, in this game or in a save
		kShowObjective,      // the host's attack is on, its attackers and our player at the settlement: "Defend X" here too
		kHideObjective,      // shown, and our player or the host's attackers left (or the host's run ended undefended)
		kCompleteObjective,  // shown, and the host's attackers are dead: completed, as the host's
		kStop,               // the host's run ended: ours stops (defused: its 1000 rolls nothing)
	};

	[[nodiscard]] constexpr MirrorStep Next(const MirrorFacts& a_facts)
	{
		if (!a_facts.running) {
			return a_facts.hostPhase != Phase::kOver && !a_facts.elsewhere && !a_facts.startTried ? MirrorStep::kStart : MirrorStep::kNone;
		}
		if (!a_facts.done) {
			return MirrorStep::kDefuse;
		}
		const bool ending = a_facts.hostPhase == Phase::kOver;
		if (a_facts.objectiveShown) {
			// Ended with it shown: completed if the host's attackers died, hidden otherwise (stage 1000 shows "won" for an
			// objective displayed and not completed).
			if (a_facts.hostPhase == Phase::kDefended || (ending && a_facts.outcome == Outcome::kDefended)) {
				return MirrorStep::kCompleteObjective;
			}
			if (ending || a_facts.hostPhase != Phase::kUnderway || !a_facts.loadedHere || !a_facts.hostAttackersHere) {
				return MirrorStep::kHideObjective;
			}
		}
		if (ending) {
			return MirrorStep::kStop;
		}
		if (a_facts.hostPhase == Phase::kUnderway && a_facts.hasObjective && a_facts.loadedHere && a_facts.hostAttackersHere && !a_facts.objectiveShown) {
			return MirrorStep::kShowObjective;
		}
		return MirrorStep::kNone;
	}

	// The notice a friend's game shows for the host's attack, by its text key (TextEnglish.h), or none. An attack that
	// began and ended undefended says only that: the host's game rolls its damage only if the settlement wasn't loaded
	// there at the end, and the roll may go the settlers' way.
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

	// The server's book: what the host's game said of settlements, and its live attacks (for joiners). A friend's question
	// is answered from it only with "the host's", said within OWNER_FOR; "not the host's" goes to the host's game every
	// time (at most once per ASK_EVERY a settlement), whose answer reaches the friends who asked even when it is no news.
	class Book
	{
	public:
		static constexpr std::size_t MAX_OWNERS = View::MAX_OWNERS;
		static constexpr std::size_t MAX_LIVE = View::MAX_LIVE;
		static constexpr auto        LIVE_FOR = View::LIVE_FOR;
		// A friend's question about one settlement goes to the host's game at most this often.
		static constexpr auto ASK_EVERY = std::chrono::seconds(10);
		// The friends waiting for an answer about one settlement, at most.
		static constexpr std::size_t MAX_ASKERS = 16;

		// The host's game's word on a settlement. Whether to tell everyone: new, changed or said again after it had
		// expired (or past the cap: told, not kept).
		bool SetOwner(std::uint32_t a_workshop, bool a_hostOwned, Clock::time_point a_now)
		{
			if (const auto it = owners.find(a_workshop); it != owners.end()) {
				const bool news = it->second.host != a_hostOwned || a_now - it->second.since >= OWNER_FOR;
				it->second = Owned{ a_hostOwned, a_now };
				return news;
			}
			if (owners.size() >= MAX_OWNERS) {
				std::erase_if(owners, [&](const auto& a_entry) { return a_now - a_entry.second.since >= OWNER_FOR; });
			}
			if (owners.size() < MAX_OWNERS) {
				owners.emplace(a_workshop, Owned{ a_hostOwned, a_now });
			}
			return true;
		}

		// The word on a settlement, said within OWNER_FOR (none: never said, or too long ago).
		[[nodiscard]] std::optional<bool> OwnerOf(std::uint32_t a_workshop, Clock::time_point a_now) const
		{
			const auto it = owners.find(a_workshop);
			if (it == owners.end() || a_now - it->second.since >= OWNER_FOR) {
				return std::nullopt;
			}
			return it->second.host;
		}

		// A friend (its player id) waits for the host's game's word on a settlement.
		void Awaits(std::uint32_t a_workshop, std::uint32_t a_player)
		{
			if (!askers.contains(a_workshop) && askers.size() >= MAX_OWNERS) {
				return;
			}
			auto& waiting = askers[a_workshop];
			if (waiting.size() < MAX_ASKERS && !std::ranges::contains(waiting, a_player)) {
				waiting.push_back(a_player);
			}
		}

		// The friends waiting for the word on a settlement, who get it now.
		[[nodiscard]] std::vector<std::uint32_t> TakeAwaiting(std::uint32_t a_workshop)
		{
			const auto it = askers.find(a_workshop);
			if (it == askers.end()) {
				return {};
			}
			auto waiting = std::move(it->second);
			askers.erase(it);
			return waiting;
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

		// Whether a friend's question about a settlement goes to the host's game now (once per ASK_EVERY, or at once after
		// the host's game spoke of it: Answered).
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

		// The host's game spoke of a settlement: the next question about it goes to the host's game at once (one question
		// waits for an answer at a time, so friends' rolls can't flood it).
		void Answered(std::uint32_t a_workshop) { asked.erase(a_workshop); }

		// The words still trusted (for a joiner).
		[[nodiscard]] std::vector<Protocol::SettlementOwner> Owners(Clock::time_point a_now) const
		{
			std::vector<Protocol::SettlementOwner> out;
			for (const auto& [workshop, owned] : owners) {
				if (a_now - owned.since < OWNER_FOR) {
					out.push_back({ workshop, owned.host });
				}
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

		// Live attacks never told over after LIVE_FOR are dropped (their host's game went without a word); words older
		// than OWNER_FOR go too. The live attacks dropped.
		std::size_t Expire(Clock::time_point a_now)
		{
			std::erase_if(owners, [&](const auto& a_entry) { return a_now - a_entry.second.since >= OWNER_FOR; });
			return std::erase_if(live, [&](const auto& a_entry) { return a_now - a_entry.second.since >= LIVE_FOR; });
		}

		[[nodiscard]] std::size_t OwnersKept() const { return owners.size(); }
		[[nodiscard]] std::size_t LiveCount() const { return live.size(); }

		void Clear()
		{
			owners.clear();
			live.clear();
			asked.clear();
			askers.clear();
		}

	private:
		struct Booked
		{
			Protocol::SettlementAttack attack;
			Clock::time_point          since{};
		};
		struct Owned
		{
			bool              host = false;
			Clock::time_point since{};  // when the host's game said it last
		};

		std::unordered_map<std::uint32_t, Owned>                      owners;
		std::unordered_map<std::uint32_t, Booked>                     live;  // by quest
		std::unordered_map<std::uint32_t, Clock::time_point>          asked;
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> askers;  // by workbench: friends' player ids
	};
}
