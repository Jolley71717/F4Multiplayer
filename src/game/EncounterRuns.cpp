#include "game/EncounterRuns.h"

#include "game/EncounterRunBook.h"
#include "game/Encounters.h"
#include "game/QuestActors.h"
#include "game/QuestStageLayout.h"
#include "game/QuestSync.h"
#include "game/RadiantRules.h"
#include "game/RadiantSync.h"
#include "game/RemoteEdits.h"
#include "game/Spawns.h"
#include "game/Story.h"
#include "game/WorldSync.h"

#include <unordered_map>

namespace EncounterRuns
{
	namespace
	{
		using Clock = Encounters::Clock;
		using Encounters::RunBook;
		using Encounters::RunKey;
		using State = RunBook::State;

		constexpr auto          POLL = std::chrono::seconds(1);
		constexpr std::uint32_t LOGGED = 20;  // lines about single runs (the counters go on)

		// Run numbers start at this game's start (RadiantRules::SerialSeed), as a radiant's do: a game that restarts never
		// numbers a run as one of its last.
		RunBook book{ RadiantRules::SerialSeed(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())) };
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      polled{};

		// The encounters with an objective in our game's files (Encounters::SharesRun), gathered once; and since when each
		// has run here (a run the encounter book doesn't know stands by its time running: Encounters::RunStands).
		std::vector<std::uint32_t>                           objectiveQuests;
		bool                                                 gathered = false;
		std::unordered_map<std::uint32_t, Clock::time_point> runningSince;

		std::uint32_t announced = 0, heard = 0, started = 0, followed = 0, dropped = 0, startsFailed = 0, stagesSent = 0, stagesSet = 0, logged = 0;
		// Runs ended here as their owner's did (RUN_OVER, or the owner left, or reloaded); our hollow runs stopped for a
		// friend's, and those stopped as nobody's (HOLLOW_WAIT); aliases forced again onto copies placed anew (after a save),
		// and asks the VM did not take; stages that came for a quest no longer running (not set).
		std::uint32_t endedHere = 0, hollowStopped = 0, hollowEnded = 0, refilled = 0, refillsFailed = 0, lateStages = 0;

		RE::TESQuest* QuestOf(std::uint32_t a_quest)
		{
			return a_quest != 0 ? RE::TESForm::GetFormByID<RE::TESQuest>(a_quest) : nullptr;
		}

		bool Running(const RE::TESQuest* a_quest)
		{
			return a_quest && (a_quest->data.flags & QuestActors::QUEST_RUNNING) != 0;
		}

		const char* NameOf(std::uint32_t a_quest)
		{
			const auto  quest = QuestOf(a_quest);
			const char* name = quest ? quest->formEditorID.c_str() : nullptr;
			return name ? name : "";
		}

		bool Log()
		{
			return logged++ < LOGGED * 4;
		}

		void Gather()
		{
			if (gathered) {
				return;
			}
			gathered = true;
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			for (const auto quest : handler->GetFormArray<RE::TESQuest>()) {
				const char* name = quest ? quest->formEditorID.c_str() : nullptr;
				if (name && Encounters::IsEncounterName(name) && Encounters::SharesRun(Spawns::IsEncounter(quest), quest->objectives.size())) {
					objectiveQuests.push_back(quest->GetFormID());
				}
			}
			REX::INFO("EncounterRuns: {} random encounters show an objective; their runs are shared as well as their actors", objectiveQuests.size());
		}

		RemoteEdits::Spot SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto  cell = a_ref->GetParentCell();
			const auto& at = a_ref->data.location;
			if (cell && cell->IsInterior()) {
				return { cell->GetFormID(), 0, at.x, at.y, at.z };
			}
			return { 0, cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0, at.x, at.y, at.z };
		}

		bool Near(const RE::TESObjectREFR* a_ref)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !a_ref || !a_ref->GetParentCell() || !player->GetParentCell()) {
				return false;
			}
			return RemoteEdits::Within(RemoteEdits::Distance(SpotOf(player), SpotOf(a_ref)), Encounters::COLLIDE_RANGE);
		}

		// A quest's reference aliases its own start creates (FILL_CREATED: the encounter's actors, a treasure hunt's corpse).
		template <class Fn>
		void ForCreated(RE::TESQuest* a_quest, Fn&& a_fn)
		{
			for (const auto alias : a_quest->aliases) {
				if (!alias || alias->fillType != QuestActors::FILL_CREATED) {
					continue;
				}
				const char* type = alias->QType().c_str();
				if (!type || std::string_view{ type } != RE::BGSRefAlias::TYPE) {
					continue;
				}
				const auto ref = a_quest->GetAliasedRef(alias->aliasID).get();
				a_fn(ref && !ref->IsDeleted() ? ref.get() : nullptr);
			}
		}

		// Whether every actor our game made for the quest that is alive and enabled is shared already (a spawn id the
		// friends' copies go by), loaded or not: only then is the run sent, or a friend's game would hear none of the
		// actors and stand an empty run (an unloaded one shared a while ago keeps its id; after a load none has one).
		bool ActorsShared(RE::TESQuest* a_quest)
		{
			bool shared = true;
			ForCreated(a_quest, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && Protocol::IsSpawnRef(actor->GetFormID()) && !actor->IsDead(false) && !actor->IsDisabled() && Spawns::OwnIdOf(actor->GetFormID()) == 0) {
					shared = false;
				}
			});
			return shared;
		}

		// A run of the quest here with none of the actors its start created standing (Encounters::IsHollow: every such alias
		// empty or holding our own still disabled): a save's of a run we followed (our copies are never saved), never shared
		// as ours; a friend's run of it near us replaces it.
		bool Hollow(RE::TESQuest* a_quest)
		{
			Encounters::CreatedFills fills;
			ForCreated(a_quest, [&](RE::TESObjectREFR* a_ref) {
				++fills.created;
				fills.empty += a_ref == nullptr;
				fills.enabled += a_ref != nullptr && !a_ref->IsDisabled();
			});
			return Encounters::IsHollow(fills);
		}

		void SendStart(std::uint32_t a_quest, RunKey a_key, std::span<const std::uint16_t> a_done)
		{
			const auto quest = QuestOf(a_quest);
			auto       stages = std::vector<std::uint16_t>(a_done.begin(), a_done.end());
			outgoing.push_back(Protocol::Encode(Protocol::RadiantStart{ 0, a_quest, a_key.serial, RadiantSync::FillsHere(quest), std::move(stages) },
				Protocol::MessageType::kReportRadiantStart));
		}

		void SendStage(std::uint32_t a_quest, RunKey a_key, std::uint16_t a_stage)
		{
			outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, a_quest, a_key.origin, a_key.serial, a_stage }, Protocol::MessageType::kReportRadiantStage));
			++stagesSent;
		}

		// Our run of a quest that stopped here: the stages its last moments set (a clean-up or final stage whose fragment
		// stops the quest, within the poll) go to the friends, then RUN_OVER, so a friend's game following it ends it too.
		// A friend's run we follow that stopped here passes nothing on: their game's quest goes on as theirs.
		void EndOurs(std::uint32_t a_id, RE::TESQuest* a_quest)
		{
			std::vector<RunKey> ours;
			for (const auto& run : book.Runs()) {
				if (run.quest == a_id && run.state == State::kOurs) {
					ours.push_back(run.key);
				}
			}
			if (ours.empty()) {
				return;
			}
			for (const auto stage : QuestStageLayout::Done(a_quest)) {  // still read after a stop, until the quest is reset
				for (const auto key : book.Report(a_id, stage)) {
					if (std::ranges::contains(ours, key)) {
						SendStage(a_id, key, stage);
					}
				}
			}
			for (const auto key : ours) {
				SendStage(a_id, key, Encounters::RUN_OVER);
			}
		}

		// Our run of a quest with none of the actors its start created, shared by nobody (not ours in the book, no friend's
		// followed or heard): a save's of a run we followed, loaded with its owner's run gone (a solo load, or theirs over
		// since). One that runs so for HOLLOW_WAIT is stopped: nobody's actors will stand for it, and its objective would
		// stay for good. In a session or out of one (the quest's own stop, as a stopped encounter's).
		void SweepHollow(Clock::time_point a_now)
		{
			Gather();
			for (const auto id : objectiveQuests) {
				const auto quest = QuestOf(id);
				if (!Running(quest)) {
					runningSince.erase(id);
					continue;
				}
				const auto since = runningSince.try_emplace(id, a_now).first->second;
				const bool heard = std::ranges::any_of(book.Runs(), [&](const RunBook::Run& a_run) { return a_run.quest == id; });
				if (book.Shares(id) || Spawns::EncounterGivingWay(id) || !Encounters::EndsHollow(Hollow(quest), heard, a_now - since)) {
					continue;
				}
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", id).c_str());
				runningSince.erase(id);  // asked again on a later poll if it still runs (counted from then)
				if (++hollowEnded <= LOGGED) {
					REX::INFO("EncounterRuns: our {:08X} '{}' has run {} s with none of its actors (a save's of a run we followed) and nobody's run of it: stopped", id, NameOf(id),
						std::chrono::duration_cast<std::chrono::seconds>(a_now - since).count());
				}
			}
		}

		// Our encounters with an objective: one that stands is shared as a run, once; one that stopped running ends our
		// run of it and a friend's we follow.
		void LookAtOurs(Clock::time_point a_now)
		{
			Gather();
			for (const auto id : objectiveQuests) {
				const auto quest = QuestOf(id);
				if (!Running(quest)) {
					runningSince.erase(id);
					if (quest) {
						EndOurs(id, quest);
					}
					for (const auto key : book.Over(id)) {
						if (Log()) {
							REX::INFO("EncounterRuns: {:08X} '{}' no longer runs here: {} run of it is over", id, NameOf(id), key.origin == book.Self() ? "our" : std::format("player {}'s", key.origin));
						}
					}
					continue;
				}
				const auto since = runningSince.try_emplace(id, a_now).first->second;
				// Ours sent again to someone who joined or reloaded, once its actors are shared (one unloaded while ours were let
				// go, after our load, would go as none: their game would stand an empty run).
				for (auto& run : book.Runs()) {
					if (run.quest == id && RunBook::TakeResend(run, ActorsShared(quest))) {
						SendStart(run.quest, run.key, run.done);
					}
				}
				if (book.Shares(id)) {
					continue;  // ours shared already, or a friend's followed
				}
				if (Spawns::EncounterGivingWay(id) || !Encounters::RunStands(Spawns::EncounterStands(id), a_now - since) || !ActorsShared(quest) || Hollow(quest)) {
					continue;
				}
				const auto done = QuestStageLayout::Done(quest);
				if (const auto key = book.Announce(id, done, a_now)) {
					SendStart(id, *key, done);
					++announced;
					if (Log()) {
						REX::INFO("EncounterRuns: our {:08X} '{}' stands: shared as run {} with {} stage(s) done", id, NameOf(id), key->serial, done.size());
					}
				}
			}
		}

		// The stages done here of the quests whose runs are ours or followed: reported under each run, once.
		void ReportStages()
		{
			std::vector<std::uint32_t> quests;
			for (const auto& run : book.Runs()) {
				if ((run.state == State::kOurs || run.state == State::kFollowing) && !std::ranges::contains(quests, run.quest)) {
					quests.push_back(run.quest);
				}
			}
			for (const auto id : quests) {
				const auto quest = QuestOf(id);
				if (!Running(quest)) {
					continue;
				}
				for (const auto stage : QuestStageLayout::Done(quest)) {
					for (const auto key : book.Report(id, stage)) {
						SendStage(id, key, stage);
					}
				}
			}
		}

		// A stage of a run set here: only while its quest runs here. One that comes just after it stopped (before the
		// next poll ends the run) would start it again from that stage, without its trigger or the owner's actors forced
		// in; QuestSync refuses that too, for one that waits in its queue past the stop.
		void SetHere(std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_from)
		{
			const auto quest = QuestOf(a_quest);
			if (!quest || QuestStageLayout::IsStageDone(quest, a_stage)) {
				return;
			}
			if (!Running(quest)) {
				if (lateStages++ < LOGGED) {
					REX::INFO("EncounterRuns: {:08X} '{}' stage {} from player {} not set: the quest no longer runs here", a_quest, NameOf(a_quest), a_stage, a_from);
				}
				return;
			}
			QuestSync::ApplyRadiant(a_quest, a_stage, a_from, true);
			++stagesSet;
		}

		// The runs we follow: their aliases that name the owner's actors hold our copies placed now. A save deletes every
		// copy (no copy is ever saved: Spawns::DiscardCopies) and places them again as new references; so does our player
		// coming back after their copies were let go. Each such alias is forced onto the new copy and read back
		// (RadiantSync::RefillAliases: asked again when it doesn't hold it, up to MAX_FORCE_TRIES asks); our own actor of the
		// quest it still holds (made by our start for it, not seen loaded yet) goes first.
		void Refill(Clock::time_point a_now)
		{
			for (auto& run : book.Runs()) {
				const auto quest = run.state == State::kFollowing ? QuestOf(run.quest) : nullptr;
				if (!Running(quest)) {
					continue;
				}
				const auto count = RadiantSync::RefillAliases(quest, run.fills, run.forced,
					std::format("EncounterRuns: player {}'s {:08X} '{}' (run {})", run.key.origin, run.quest, NameOf(run.quest), run.key.serial), a_now);
				refilled += count.asked;
				refillsFailed += count.failed;
			}
		}

		// Friends' runs whose owner's ended (RUN_OVER, or they left or reloaded) RUN_END_WAIT ago: stopped here if still
		// running (their last stages had their time to end them; one still waiting in the quest queue, held back by a
		// combat hold, is waited for up to RUN_END_LIMIT, so the stop never drops a clean-up stage), then over.
		void EndTheirs(Clock::time_point a_now)
		{
			const auto queued = [](std::uint32_t a_quest) { return !QuestSync::Waiting(a_quest).empty(); };
			for (const auto key : book.ToEnd(a_now, queued)) {
				const auto run = book.Find(key);
				const auto id = run ? run->quest : 0;
				if (Running(QuestOf(id))) {
					RE::Console::ExecuteCommand(std::format("stopquest {:08X}", id).c_str());
					++endedHere;
					if (Log()) {
						REX::INFO("EncounterRuns: player {}'s {:08X} '{}' (run {}) ended in their game: stopped here too", key.origin, id, NameOf(id), key.serial);
					}
				}
				book.Drop(key);
			}
		}

		// A friend's run's fills as our game can take them: the aliases to force (their actors' copies placed here, the same
		// trigger, places and plugin's references), how many copies it names are not placed yet but may be, and whether our
		// player is near (a copy placed, or a reference of it close).
		struct Resolved
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> named;
			std::size_t                                          waiting = 0;
			std::size_t                                          placed = 0;
			bool                                                 near = false;
			bool                                                 namesSpawns = false;
		};

		Resolved Resolve(const RunBook::Run& a_run)
		{
			Resolved out;
			for (const auto& fill : a_run.fills) {
				if (fill.form == 0 || fill.form == Protocol::RADIANT_UNSHARED_REF) {
					continue;  // empty there, or an actor their game doesn't share: our game fills it (and removes the actor it makes)
				}
				if (fill.kind == static_cast<std::uint8_t>(RadiantRules::Kind::kLoc)) {
					if (RE::TESForm::GetFormByID<RE::BGSLocation>(fill.form)) {
						out.named.emplace_back(fill.alias, fill.form);
					}
					continue;
				}
				if (Protocol::IsSpawnRef(fill.form)) {
					out.namesSpawns = true;
					const auto copy = Spawns::CopyFor(fill.form);
					if (copy.local != 0) {
						out.named.emplace_back(fill.alias, copy.local);
						out.near = true;  // a copy is placed only near our player
						++out.placed;
					} else if (!copy.over) {
						++out.waiting;
					}
					continue;
				}
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(fill.form)) {
					out.named.emplace_back(fill.alias, fill.form);
					out.near = out.near || Near(ref);
				}
			}
			return out;
		}

		// Friends' runs heard: started once our player is near, if our game doesn't run the quest as its own; those started
		// followed once they run.
		void LookAtTheirs(Clock::time_point a_now)
		{
			std::vector<RunKey> drop;
			for (auto& run : book.Runs()) {
				const auto quest = QuestOf(run.quest);
				if (run.state == State::kStarting) {
					// One that runs now is followed below, outside the walk.
					if (!Running(quest) && a_now - run.since >= Encounters::RUN_START_WAIT) {
						drop.push_back(run.key);
						++startsFailed;
						REX::WARN("EncounterRuns: player {}'s {:08X} '{}' did not run here {} s after its start; left to them", run.key.origin, run.quest, NameOf(run.quest),
							std::chrono::duration_cast<std::chrono::seconds>(Encounters::RUN_START_WAIT).count());
					}
					continue;
				}
				if (run.state != State::kHeard) {
					continue;
				}
				if (!quest) {
					drop.push_back(run.key);
					continue;
				}
				const auto                resolved = Resolve(run);
				Encounters::HeardFacts    facts;
				facts.ownerLive = !resolved.namesSpawns || Spawns::TheirEncounterLive(run.key.origin, run.quest);
				facts.runningHere = Running(quest);
				facts.oursGivingWay = facts.runningHere && Spawns::EncounterGivingWay(run.quest);
				facts.oursHollow = facts.runningHere && Hollow(quest);
				facts.near = resolved.near;
				facts.namesActors = resolved.namesSpawns;
				facts.placed = resolved.placed;
				facts.waiting = resolved.waiting;
				const auto judged = book.Judge(run, facts, a_now);
				if (judged.verdict == Encounters::Verdict::kStopOurs) {
					// Ours is a save's of a run we followed, with none of its actors: it makes way for theirs (started next time).
					RE::Console::ExecuteCommand(std::format("stopquest {:08X}", run.quest).c_str());
					if (++hollowStopped <= LOGGED) {  // asked again each poll until it has stopped
						REX::INFO("EncounterRuns: our {:08X} '{}' runs with none of its actors (a save's): stopped for player {}'s run {}", run.quest, NameOf(run.quest),
							run.key.origin, run.key.serial);
					}
					continue;
				}
				if (judged.verdict == Encounters::Verdict::kDrop) {
					drop.push_back(run.key);
					++dropped;
					if (Log()) {
						REX::INFO("EncounterRuns: player {}'s {:08X} '{}' (run {}) is not followed here: {}", run.key.origin, run.quest, NameOf(run.quest), run.key.serial, judged.why);
					}
					continue;
				}
				if (judged.verdict != Encounters::Verdict::kStart) {
					continue;
				}
				const auto reply = RadiantSync::StartWithEvent(quest, 0, 0, resolved.named);
				if (reply.starts_with("wait")) {
					continue;  // the game is still shutting the quest's last run here down: next time
				}
				if (reply.starts_with("error")) {
					drop.push_back(run.key);
					++startsFailed;
					REX::WARN("EncounterRuns: player {}'s {:08X} '{}' could not be started here ({}); left to them", run.key.origin, run.quest, NameOf(run.quest), reply);
					continue;
				}
				book.Started(run.key, a_now);
				++started;
				if (Log()) {
					REX::INFO("EncounterRuns: player {}'s {:08X} '{}' (run {}) is started here with {} alias(es) forced to theirs{}: {}", run.key.origin, run.quest, NameOf(run.quest),
						run.key.serial, resolved.named.size(), resolved.waiting > 0 ? std::format(" ({} copies not placed: our game makes its own, removed)", resolved.waiting) : "", reply);
				}
			}
			for (const auto key : drop) {
				book.Drop(key);
			}
			// Started runs that run now are followed: the stages their game had and those that came meanwhile are set here.
			std::vector<RunKey> running;
			for (const auto& run : book.Runs()) {
				if (run.state == State::kStarting && Running(QuestOf(run.quest))) {
					running.push_back(run.key);
				}
			}
			for (const auto key : running) {
				const auto run = book.Find(key);
				const auto quest = run ? run->quest : 0;
				// The stages our quest's own start set are done under the run, never sent back (the owner's game could take
				// another branch of it from one).
				const auto here = QuestOf(quest);
				const auto doneHere = here ? QuestStageLayout::Done(here) : StageLog::Stages{};
				const auto stages = book.Following(key, doneHere);
				++followed;
				if (Log()) {
					REX::INFO("EncounterRuns: player {}'s {:08X} '{}' runs here: followed, {} of their stage(s) to set", key.origin, quest, NameOf(quest), stages.size());
				}
				for (const auto stage : stages) {
					SetHere(quest, stage, key.origin);
				}
			}
		}
	}

	void SetLocalPlayer(std::uint32_t a_id)
	{
		book.SetSelf(a_id);
	}

	bool OnStart(const Protocol::RadiantStart& a_start)
	{
		const auto quest = QuestOf(a_start.quest);
		if (!quest || !Spawns::IsEncounter(quest)) {
			return false;
		}
		if (!RadiantSync::Enabled() || a_start.playerId == book.Self()) {
			return true;
		}
		if (book.Hear(a_start, Clock::now())) {
			++heard;
			if (Log()) {
				REX::INFO("EncounterRuns: player {}'s {:08X} '{}' stands in their game (run {}, {} stage(s) done): followed here once our player is near it", a_start.playerId,
					a_start.quest, NameOf(a_start.quest), a_start.serial, a_start.stages.size());
			}
		}
		return true;
	}

	bool OnStage(const Protocol::RadiantStage& a_stage)
	{
		const auto quest = QuestOf(a_stage.quest);
		if (!quest || !Spawns::IsEncounter(quest)) {
			return false;
		}
		if (!RadiantSync::Enabled()) {
			return true;
		}
		const RunKey key{ a_stage.origin, a_stage.serial };
		if (a_stage.stage == Encounters::RUN_OVER) {
			// The owner's run ended there (after its last stages): ours of it ends RUN_END_WAIT from now if it still runs.
			if (a_stage.origin == a_stage.playerId && book.OwnerEnded(a_stage.quest, key, Clock::now()) && Log()) {
				REX::INFO("EncounterRuns: player {}'s {:08X} '{}' (run {}) ended in their game: ends here in {} s unless it has already", key.origin, a_stage.quest,
					NameOf(a_stage.quest), key.serial, std::chrono::duration_cast<std::chrono::seconds>(Encounters::RUN_END_WAIT).count());
			}
			return true;
		}
		if (book.TakeStage(a_stage.quest, key, a_stage.stage)) {
			SetHere(a_stage.quest, a_stage.stage, a_stage.playerId);
		}
		return true;
	}

	void OnPlayerJoined()
	{
		book.ResendOurs();  // each sent on the next poll its actors are all shared at (LookAtOurs)
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		book.OwnerLeft(a_id, Clock::now());
	}

	void OnPlayerReloaded(std::uint32_t a_id)
	{
		// Their load cleared their book without a RUN_OVER for its runs, and their copies of our actors went with it: the
		// runs of theirs we follow end here as when they leave (what their loaded world runs is announced anew, and
		// followed anew if near). Ours go to them again once their actors are shared (LookAtOurs).
		book.OwnerLeft(a_id, Clock::now());
		book.ResendOurs();
	}

	void EndFollowed()
	{
		// Our copies of the owners' actors went with the session (Spawns::Reset): a quest left running here would hold
		// nothing. Our own actors of it were removed as they came, so it ends.
		for (const auto& run : book.Runs()) {
			if ((run.state == State::kFollowing || run.state == State::kStarting) && Running(QuestOf(run.quest))) {
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", run.quest).c_str());
				++endedHere;
				REX::INFO("EncounterRuns: player {}'s {:08X} '{}' (run {}) followed here: stopped as the session ends", run.key.origin, run.quest, NameOf(run.quest), run.key.serial);
			}
		}
	}

	bool Shares(std::uint32_t a_quest)
	{
		return book.Shares(a_quest);
	}

	bool Follows(std::uint32_t a_quest)
	{
		return book.Follows(a_quest);
	}

	void Frame(bool a_inSession)
	{
		if (!WorldSync::InWorld() || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		if (now - polled < POLL) {
			return;
		}
		polled = now;
		SweepHollow(now);  // a hollow run of ours is nobody's in or out of a session
		if (!a_inSession || book.Self() == 0 || !RadiantSync::Enabled()) {
			return;
		}
		LookAtOurs(now);
		LookAtTheirs(now);
		EndTheirs(now);
		Refill(now);
		ReportStages();
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Clear()
	{
		book.Clear();
		runningSince.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		std::size_t ours = 0, waiting = 0, following = 0;
		for (const auto& run : book.Runs()) {
			ours += run.state == State::kOurs;
			waiting += run.state == State::kHeard;
			following += run.state == State::kFollowing || run.state == State::kStarting;
		}
		return std::format("encounterRuns: ours={} waiting={} following={} shared={} heard={} started={} followed={} dropped={} startsFailed={} stagesSent={} stagesSet={} "
		                   "endedHere={} hollowStopped={} hollowEnded={} refilled={} refillsFailed={} lateStages={}",
			ours, waiting, following, announced, heard, started, followed, dropped, startsFailed, stagesSent, stagesSet, endedHere, hollowStopped, hollowEnded, refilled,
			refillsFailed, lateStages);
	}
}
