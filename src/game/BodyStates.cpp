#include "game/BodyStates.h"

#include "game/BodyEffects.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RefTag.h"
#include "game/SessionDeaths.h"
#include "game/WorldSync.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <utility>

namespace BodyStates
{
	namespace
	{
		using namespace std::chrono_literals;
		using BodyEffects::Clock;
		using BodyEffects::State;
		using BodyEffects::Verdict;

		constexpr auto        SCAN_EVERY = 500ms;       // the bodies around us looked at
		constexpr int         TELLS_PER_SCAN = 4;       // a room of bodies is told over a few scans, not in one burst
		constexpr int         MAKES_PER_SCAN = 2;       // each a few console commands
		constexpr std::size_t MAX_PENDING = BodyEffects::MAX;
		constexpr auto        WATCH_FOR = 60s;          // one that just died is looked at by its id this long (the effect plays for seconds)
		constexpr std::size_t MAX_WATCHED = 64;
		constexpr int         LOGGED = 60;

		// ExtraAshPileRef (BSExtraData, then the handle of the reference at the other end: the pile's on the actor, the
		// actor's on the pile). CommonLibF4 has no class for it, so the handle is read at its place after the header and
		// only believed when it names a reference whose base is an activator.
		constexpr std::size_t PILE_HANDLE_AT = sizeof(RE::BSExtraData);
		static_assert(sizeof(RE::ObjectRefHandle) == 4);

		BodyEffects::Book                      book;
		std::vector<std::uint32_t>             pending;  // session ids heard of, something still to make of our body of each
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextScan{};

		// Our game's NPCs that just died (or began to): looked at by their own ids until what they show is told, whatever
		// the engine does with a body it has made unseen (the walk of the process lists is for the rest: bodies that lay
		// here before a friend joined).
		std::mutex                                               inboxLock;
		std::vector<std::uint32_t>                               deathInbox;
		std::vector<std::pair<std::uint32_t, Clock::time_point>> watched;  // (our reference, looked at until)

		// Counters (Describe).
		std::uint32_t told = 0, heard = 0, wanted = 0, made = 0, done = 0, gaveUp = 0, noPile = 0, scans = 0, bodiesSeen = 0;
		int           logged = 0;

		bool Log() { return logged++ < LOGGED; }

		// Dead here, for good (not an essential one down, which gets up: SessionDeaths::GetsUp, as WorldSync's DeadHere).
		bool DeadHere(RE::Actor* a_actor)
		{
			const auto lifeState = static_cast<std::int32_t>(static_cast<RE::ActorState&>(*a_actor).lifeState);
			return a_actor->IsDead(false) && !SessionDeaths::GetsUp(lifeState, WorldSync::IsEssential(a_actor));
		}

		// A body of ours to tell of: an NPC (no player, no friend's stand-in), in the world, dead.
		bool IsCorpse(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsPlayerRef() && !a_actor->IsDeleted() && !Puppets::IsPuppet(a_actor) && a_actor->GetParentCell() && DeadHere(a_actor);
		}

		// ...and one to make something of: loaded too.
		bool IsBody(RE::Actor* a_actor)
		{
			return IsCorpse(a_actor) && !a_actor->IsDisabled() && a_actor->Get3D();
		}

		struct PileRead
		{
			bool          extra = false;  // the actor has the extra
			std::uint32_t handle = 0;     // as read
			std::uint32_t ref = 0;        // the reference it names (0: none, or stale)
			std::uint32_t base = 0;       // its base, when that is an activator
		};

		PileRead ReadPile(RE::Actor* a_actor)
		{
			PileRead out;
			const auto extra = a_actor->extraList ? a_actor->extraList->GetByType(RE::EXTRA_DATA_TYPE::kAshpileRef) : nullptr;
			if (!extra) {
				return out;
			}
			out.extra = true;
			std::memcpy(&out.handle, reinterpret_cast<const std::byte*>(extra) + PILE_HANDLE_AT, sizeof(out.handle));
			if (out.handle == 0) {
				return out;
			}
			RE::ObjectRefHandle handle;
			std::memcpy(static_cast<void*>(&handle), &out.handle, sizeof(out.handle));
			const auto ptr = handle.get();
			const auto                ref = ptr.get();
			if (!ref || ref == a_actor || ref->IsDeleted()) {
				return out;
			}
			out.ref = ref->GetFormID();
			const auto base = ref->GetObjectReference();
			if (base && base->Is(RE::ENUM_FORM_ID::kACTI)) {
				out.base = base->GetFormID();
			}
			return out;
		}

		std::uint8_t StageOf(RE::Actor* a_actor)
		{
			const auto stage = a_actor->criticalStage.underlying();
			return stage >= 0 && stage <= Protocol::BODY_STAGE_MAX ? static_cast<std::uint8_t>(stage) : std::uint8_t{ 0 };
		}

		State Read(RE::Actor* a_actor)
		{
			return State{ StageOf(a_actor), ReadPile(a_actor).base };
		}

		// The pile base to attach here: the one told when this game has it as an activator, else the game's own for the stage.
		std::uint32_t PileHere(std::uint32_t a_told, std::uint8_t a_stage)
		{
			for (const auto id : { a_told, BodyEffects::DefaultPile(a_stage) }) {
				const auto form = id != 0 ? RE::TESForm::GetFormByID(id) : nullptr;
				if (form && form->Is(RE::ENUM_FORM_ID::kACTI)) {
					return id;
				}
			}
			return 0;
		}

		void Console(const std::string& a_command)
		{
			RE::Console::ExecuteCommand(a_command.c_str());
		}

		// Makes our dead body show a_what (a stage an effect ends at, a pile, or both), with the console's own commands in
		// the order the game's scripts use theirs: the start stage, the pile, the body unseen, the end stage. Returns what
		// was run, for the log.
		std::string Make(RE::Actor* a_actor, const State& a_what)
		{
			const auto  id = a_actor->GetFormID();
			const auto  have = StageOf(a_actor);
			const auto  stage = a_what.stage != 0 ? a_what.stage : have;
			std::string ran;
			if (a_what.stage != 0 && have == 0) {
				Console(std::format("{:08X}.setcriticalstage {}", id, BodyEffects::StartOf(a_what.stage)));
				ran += std::format("stage {}", BodyEffects::StartOf(a_what.stage));
			}
			if (a_what.pile != 0) {
				if (const auto base = PileHere(a_what.pile, stage); base != 0) {
					Console(std::format("{:08X}.attachashpile {:08X}", id, base));
					ran += std::format("{}pile {:08X}", ran.empty() ? "" : ", ", base);
				} else {
					++noPile;
					ran += std::format("{}no pile {:08X} here", ran.empty() ? "" : ", ", a_what.pile);
				}
			}
			if (BodyEffects::LeavesPile(stage) && a_what.Any()) {
				// The pile stands in for the body from here on (the scripts' SetAlpha(0)).
				Console(std::format("{:08X}.setactoralpha 0", id));
				ran += std::format("{}unseen", ran.empty() ? "" : ", ");
			}
			if (a_what.stage != 0) {
				Console(std::format("{:08X}.setcriticalstage {}", id, a_what.stage));
				ran += std::format("{}stage {}", ran.empty() ? "" : ", ", a_what.stage);
			}
			return ran;
		}

		std::string Show(const State& a_state)
		{
			return a_state.Any() ? std::format("stage {} pile {:08X}", a_state.stage, a_state.pile) : std::string{ "whole" };
		}

		RE::Actor* BodyOf(std::uint32_t a_sessionId)
		{
			const auto local = NpcSync::LocalRef(a_sessionId);
			const auto actor = local != 0 ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
			return IsBody(actor) ? actor : nullptr;
		}

		// What friends' games told: made of our bodies that lie dead here now. The rest wait.
		void MakeHeard(Clock::time_point a_now)
		{
			int makes = 0;
			for (auto it = pending.begin(); it != pending.end();) {
				const auto id = *it;
				const auto actor = BodyOf(id);
				if (!actor) {
					++it;
					continue;
				}
				const auto local = Read(actor);
				book.Seen(id, local);
				if (makes >= MAKES_PER_SCAN && BodyEffects::Needed(book.Wanted(id), local).Any()) {
					++it;
					continue;
				}
				const auto step = book.Apply(id, local, a_now);
				switch (step.verdict) {
					case Verdict::kApply: {
						++makes;
						++made;
						const auto ran = Make(actor, step.what);
						if (Log()) {
							REX::INFO("BodyStates: our body {} (session id {:08X}, {}) made {} as a friend's game shows it (try {}): {}", RefTag::Of(actor), id, Show(local),
								Show(step.what), step.attempt, ran);
						}
						++it;
						break;
					}
					case Verdict::kWait:
						++it;
						break;
					case Verdict::kDone:
						++done;
						if (Log()) {
							REX::INFO("BodyStates: our body {} (session id {:08X}) shows {} now, after {} try(s)", RefTag::Of(actor), id, Show(local), step.attempt);
						}
						it = pending.erase(it);
						break;
					case Verdict::kGiveUp:
						++gaveUp;
						REX::WARN("BodyStates: our body {} (session id {:08X}) shows {} and still lacks {} after {} tries: left as it is", RefTag::Of(actor), id, Show(local),
							Show(step.what), step.attempt);
						it = pending.erase(it);
						break;
					case Verdict::kNone:
					default:
						it = pending.erase(it);
						break;
				}
			}
		}

		// One body of ours: what it shows is told, once it is an ended effect the others don't know. True when they know
		// it now (nothing more to watch it for).
		bool TellOne(RE::Actor* a_actor, Clock::time_point a_now, int& a_tells)
		{
			const auto stage = StageOf(a_actor);
			if (stage == 0 && !(a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kAshpileRef))) {
				return false;  // a whole body, as nearly all are
			}
			const auto id = NpcSync::SessionId(a_actor->GetFormID());
			if (id == 0) {
				return false;
			}
			auto local = State{ stage, ReadPile(a_actor).base };
			if (!Protocol::IsShareableRef(local.pile)) {
				local.pile = 0;  // a pile base made at run time names nothing in another game
			}
			if (!local.Any() || BodyEffects::Running(local.stage)) {
				return false;
			}
			if (!BodyEffects::Adds(local, book.Known(id))) {
				return true;
			}
			if (a_tells >= TELLS_PER_SCAN) {
				return false;
			}
			const auto tell = book.Tell(id, local, a_now);
			if (!tell) {
				return true;
			}
			++a_tells;
			++told;
			outgoing.push_back(Protocol::Encode(Protocol::BodyState{ 0, id, tell->stage, tell->pile }, Protocol::MessageType::kReportBodyState));
			if (Log()) {
				REX::INFO("BodyStates: our body {} (session id {:08X}) shows {}: told the others", RefTag::Of(a_actor), id, Show(*tell));
			}
			return true;
		}

		// Our own: every dead body around us that shows an ended effect is told, once.
		void TellOurs(Clock::time_point a_now)
		{
			int tells = 0;
			// Those that just died, by their ids.
			{
				std::vector<std::uint32_t> died;
				{
					std::scoped_lock l{ inboxLock };
					died.swap(deathInbox);
				}
				for (const auto local : died) {
					const auto seen = std::find_if(watched.begin(), watched.end(), [&](const auto& a_entry) { return a_entry.first == local; });
					if (seen != watched.end()) {
						seen->second = a_now + WATCH_FOR;
						continue;
					}
					if (watched.size() >= MAX_WATCHED) {
						watched.erase(watched.begin());
					}
					watched.emplace_back(local, a_now + WATCH_FOR);
				}
			}
			for (auto it = watched.begin(); it != watched.end();) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(it->first);
				if (!actor || actor->IsDeleted() || a_now >= it->second || (IsCorpse(actor) && TellOne(actor, a_now, tells))) {
					it = watched.erase(it);
				} else {
					++it;
				}
			}
			// The rest: the bodies in the engine's lists.
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
				for (const auto& handle : *handles) {
					const auto ptr = handle.get();
					const auto actor = ptr.get();
					if (!actor || !Protocol::IsActorRef(actor->GetFormID()) || !IsCorpse(actor)) {
						continue;
					}
					++bodiesSeen;
					TellOne(actor, a_now, tells);
				}
			}
		}
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now < nextScan) {
			return;
		}
		nextScan = now + SCAN_EVERY;
		++scans;
		MakeHeard(now);
		TellOurs(now);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnHeard(const Protocol::BodyState& a_state)
	{
		++heard;
		if (!book.Heard(a_state.refId, State{ a_state.stage, a_state.pile }, Clock::now())) {
			return;
		}
		++wanted;
		if (std::find(pending.begin(), pending.end(), a_state.refId) == pending.end()) {
			if (pending.size() >= MAX_PENDING) {
				pending.erase(pending.begin());
			}
			pending.push_back(a_state.refId);
		}
		if (Log()) {
			REX::INFO("BodyStates: player {}'s game shows the body of {:08X} as {}", a_state.playerId, a_state.refId, Show(book.Wanted(a_state.refId)));
		}
	}

	void NoteDeath(std::uint32_t a_local)
	{
		std::scoped_lock l{ inboxLock };
		if (deathInbox.size() < MAX_WATCHED) {
			deathInbox.push_back(a_local);
		}
	}

	void OnPlayerJoined()
	{
		book.ForgetTold();
		nextScan = {};
	}

	void OnGameLoaded()
	{
		// Our bodies are as the save had them (one killed since stands again, until the session's deaths fell it, whole):
		// all that friends told is wanted of them anew, and what ours show is told again.
		pending = book.WantedAgain();
		watched.clear();
		nextScan = {};
		std::scoped_lock l{ inboxLock };
		deathInbox.clear();
	}

	void Reset()
	{
		book.Clear();
		pending.clear();
		watched.clear();
		outgoing.clear();
		nextScan = {};
		logged = 0;  // each session's log says what its bodies did
		std::scoped_lock l{ inboxLock };
		deathInbox.clear();
	}

	std::string Describe()
	{
		return std::format("bodies=known:{} waiting:{} watched:{} told={} heard={} wanted={} made={} done={} gaveUp={} noPile={}", book.Size(), pending.size(), watched.size(), told,
			heard, wanted, made, done, gaveUp, noPile);
	}

	std::string List()
	{
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return "error: not in game";
		}
		std::string out;
		std::size_t count = 0;
		const std::array<std::pair<const char*, const RE::BSTArray<RE::ActorHandle>*>, 4> all{ { { "high", &lists->highActorHandles },
			{ "midHigh", &lists->middleHighActorHandles },
			{ "midLow", &lists->middleLowActorHandles },
			{ "low", &lists->lowActorHandles } } };
		for (const auto& [name, handles] : all) {
			for (const auto& handle : *handles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || !IsCorpse(actor)) {
					continue;
				}
				if (count++ < 24) {
					const auto id = NpcSync::SessionId(actor->GetFormID());
					out += std::format("; {:08X} id={:08X} {} 3d={} {} known=[{}] want=[{}]", actor->GetFormID(), id, name, actor->Get3D() != nullptr, Show(Read(actor)),
						Show(book.Known(id)), Show(book.Wanted(id)));
				}
			}
		}
		return std::format("bodies: {} dead here{} | {} scans={} seen={}", count, out, Describe(), scans, bodiesSeen);
	}

	std::string Why(std::uint32_t a_refId)
	{
		const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
		if (!actor) {
			return std::format("error: {:08X} is no actor here", a_refId);
		}
		const auto pile = ReadPile(actor);
		const auto id = NpcSync::SessionId(a_refId);
		const auto lifeState = static_cast<std::int32_t>(static_cast<RE::ActorState&>(*actor).lifeState);
		return std::format("{} id={:08X} life={} dead={} body={} stage={} pileExtra={} handle={:08X} pileRef={:08X} pileBase={:08X} limbsExtra={} known=[{}] want=[{}] waiting={}",
			RefTag::Of(actor), id, lifeState, actor->IsDead(false), IsBody(actor), actor->criticalStage.underlying(), pile.extra, pile.handle, pile.ref, pile.base,
			actor->extraList && actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kDismemberedLimbs), Show(book.Known(id)), Show(book.Wanted(id)),
			std::find(pending.begin(), pending.end(), id) != pending.end());
	}

	std::string DevApply(std::uint32_t a_refId, std::uint8_t a_stage, std::uint32_t a_pile)
	{
		const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
		if (!IsBody(actor)) {
			return std::format("error: {:08X} is no dead body loaded here", a_refId);
		}
		const auto want = BodyEffects::Whole(State{ a_stage, a_pile });
		if (!want.Any() || BodyEffects::Running(want.stage) || want.stage > Protocol::BODY_STAGE_MAX) {
			return "error: a stage an effect ends at (2 goo, 4 ash, 6 frozen), or 0 with a pile";
		}
		const auto before = Read(actor);
		const auto need = BodyEffects::Needed(want, before);
		if (!need.Any()) {
			return std::format("{:08X} shows {}: nothing to make of it for {}", a_refId, Show(before), Show(want));
		}
		const auto ran = Make(actor, need);
		return std::format("{:08X} was {}, made {} ({}), shows {} now", a_refId, Show(before), Show(need), ran, Show(Read(actor)));
	}
}
