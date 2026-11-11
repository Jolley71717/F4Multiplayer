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

		// Whether every actor our game made for the quest that stands loaded and alive is shared already (a spawn id the
		// friends' copies go by): only then is the run sent, or a friend's game would make its own for the alias.
		bool ActorsShared(RE::TESQuest* a_quest)
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
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (actor && Protocol::IsSpawnRef(actor->GetFormID()) && !actor->IsDead(false) && !actor->IsDisabled() && !actor->IsDeleted() && actor->Get3D() &&
					Spawns::OwnIdOf(actor->GetFormID()) == 0) {
					return false;
				}
			}
			return true;
		}

		void SendStart(std::uint32_t a_quest, RunKey a_key, std::span<const std::uint16_t> a_done)
		{
			const auto quest = QuestOf(a_quest);
			auto       stages = std::vector<std::uint16_t>(a_done.begin(), a_done.end());
			outgoing.push_back(Protocol::Encode(Protocol::RadiantStart{ 0, a_quest, a_key.serial, RadiantSync::FillsHere(quest), std::move(stages) },
				Protocol::MessageType::kReportRadiantStart));
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
					for (const auto key : book.Over(id)) {
						if (Log()) {
							REX::INFO("EncounterRuns: {:08X} '{}' no longer runs here: {} run of it is over", id, NameOf(id), key.origin == book.Self() ? "our" : std::format("player {}'s", key.origin));
						}
					}
					continue;
				}
				const auto since = runningSince.try_emplace(id, a_now).first->second;
				if (book.Shares(id)) {
					continue;  // ours shared already, or a friend's followed
				}
				if (Spawns::EncounterGivingWay(id) || !Encounters::RunStands(Spawns::EncounterStands(id), a_now - since) || !ActorsShared(quest)) {
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
						outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, id, key.origin, key.serial, stage }, Protocol::MessageType::kReportRadiantStage));
						++stagesSent;
					}
				}
			}
		}

		void SetHere(std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_from)
		{
			const auto quest = QuestOf(a_quest);
			if (quest && !QuestStageLayout::IsStageDone(quest, a_stage)) {
				QuestSync::ApplyRadiant(a_quest, a_stage, a_from, true);
				++stagesSet;
			}
		}

		// A friend's run's fills as our game can take them: the aliases to force (their actors' copies placed here, the same
		// trigger, places and plugin's references), how many copies it names are not placed yet but may be, and whether our
		// player is near (a copy placed, or a reference of it close).
		struct Resolved
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> named;
			std::size_t                                          waiting = 0;
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
				facts.near = resolved.near;
				facts.waiting = resolved.waiting;
				const auto judged = book.Judge(run, facts, a_now);
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
				if (reply.starts_with("error: an earlier radiantstart")) {
					continue;  // the one forced start at a time is busy: next time
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
				const auto stages = book.Following(key);
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
		if (RadiantSync::Enabled() && book.TakeStage(a_stage.quest, { a_stage.origin, a_stage.serial }, a_stage.stage)) {
			SetHere(a_stage.quest, a_stage.stage, a_stage.playerId);
		}
		return true;
	}

	void OnPlayerJoined()
	{
		if (!RadiantSync::Enabled()) {
			return;
		}
		for (const auto& run : book.Runs()) {
			if (run.state == State::kOurs && Running(QuestOf(run.quest))) {
				SendStart(run.quest, run.key, run.done);
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
		if (!a_inSession || book.Self() == 0 || !RadiantSync::Enabled() || !WorldSync::InWorld() || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		if (now - polled < POLL) {
			return;
		}
		polled = now;
		LookAtOurs(now);
		LookAtTheirs(now);
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
		return std::format("encounterRuns: ours={} waiting={} following={} shared={} heard={} started={} followed={} dropped={} startsFailed={} stagesSent={} stagesSet={}", ours,
			waiting, following, announced, heard, started, followed, dropped, startsFailed, stagesSent, stagesSet);
	}
}
