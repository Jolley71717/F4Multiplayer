#include "game/PlacedScenes.h"
#include "game/EffectTasks.h"

#include "Config.h"
#include "game/PlacedSceneRules.h"
#include "game/QuestActors.h"
#include "game/QuestStageLayout.h"
#include "game/RemotePlayers.h"
#include "game/StageRules.h"

#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace PlacedScenes
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using PlacedSceneRules::Act;
		using PlacedSceneRules::Event;
		using PlacedSceneRules::Facts;
		using PlacedSceneRules::Role;

		constexpr std::size_t CLAIMS = std::tuple_size_v<decltype(Event::claims)>;
		constexpr std::size_t ACTORS = std::tuple_size_v<decltype(Event::actors)>;
		constexpr std::size_t MAX_PENDING = 256;  // a quest has 17 stages; more waiting is a flood

		// One event's ids in this load order and what our game knows of it.
		struct Run
		{
			const Event*                            event = nullptr;
			std::uint32_t                           quest = 0;
			std::uint32_t                           worldspace = 0;
			std::array<std::uint32_t, ACTORS>       actors{};
			Facts                                   facts{};      // as of the last Frame
			Role                                    role = Role::kNone;
			bool                                    read = false;  // facts read since the last load
			std::set<std::uint16_t>                 sent;          // our stages sent to this server
			bool                                    keepOff = false;     // the run we followed ended: its NPCs stay off
			bool                                    stopDue = false;     // our yielded run is to stop (the runner's ended)
			std::array<std::optional<Clock::time_point>, CLAIMS> friendSince{};  // a friend's stand-in in a claim's area since
			std::set<std::uint16_t>                 claimsSet;     // claims set here for a friend's stand-in (once each)
			std::vector<std::pair<Protocol::QuestStage, bool>> pending;  // friends' stages (and whether live), taken in play (Frame)
		};

		std::vector<Run>                       runs;
		bool                                   resolved = false;
		std::vector<std::vector<std::uint8_t>> outgoing;

		std::uint32_t followed = 0;     // friends' claims followed with our quest not running
		std::uint32_t yielded = 0;      // ... and with our own run going
		std::uint32_t marked = 0;       // friends' stages marked done here
		std::uint32_t endsFollowed = 0;  // runs we followed that ended: their NPCs switched off here
		std::uint32_t stopped = 0;      // our yielded runs stopped
		std::uint32_t sentCount = 0;    // our stages sent
		std::uint32_t claimsForFriends = 0;

		void Resolve()
		{
			if (resolved) {
				return;
			}
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			resolved = true;
			for (const auto& event : PlacedSceneRules::EVENTS) {
				const auto file = StageRules::FileName(static_cast<StageRules::File>(event.file));
				Run run{ .event = &event };
				const auto quest = handler->LookupFormID(event.quest, file);
				if (!quest) {
					continue;  // the add-on isn't loaded
				}
				run.quest = quest;
				run.worldspace = handler->LookupFormID(event.worldspace, file);
				for (std::size_t i = 0; i < ACTORS; ++i) {
					run.actors[i] = handler->LookupFormID(event.actors[i], file);
				}
				runs.push_back(run);
			}
		}

		Run* RunOf(std::uint32_t a_quest)
		{
			Resolve();
			for (auto& run : runs) {
				if (run.quest == a_quest) {
					return &run;
				}
			}
			return nullptr;
		}

		RE::TESQuest* QuestOf(const Run& a_run)
		{
			return RE::TESForm::GetFormByID<RE::TESQuest>(a_run.quest);
		}

		std::vector<std::uint16_t> StagesOf(const RE::TESQuest* a_quest)
		{
			std::vector<std::uint16_t> out;
			for (const auto stage : a_quest->stages) {
				if (stage) {
					out.push_back(QuestStageLayout::Of(stage)->data.index);
				}
			}
			return out;
		}

		Facts Read(const Run& a_run, const RE::TESQuest* a_quest)
		{
			const auto done = QuestStageLayout::Done(a_quest);
			const auto all = StagesOf(a_quest);
			Facts facts;
			facts.running = (a_quest->data.flags & QuestActors::QUEST_RUNNING) != 0;
			facts.claimDone = std::ranges::any_of(a_run.event->claims, [&](const PlacedSceneRules::Claim& a_claim) { return std::ranges::contains(done, a_claim.stage); });
			facts.mirror = PlacedSceneRules::IsMirror(all, done, a_run.event->shutdown);
			facts.released = std::ranges::contains(done, a_run.event->released);
			return facts;
		}

		// Reads the quest again, and says a change of role.
		void Refresh(Run& a_run)
		{
			const auto quest = QuestOf(a_run);
			if (!quest) {
				return;
			}
			a_run.facts = Read(a_run, quest);
			a_run.read = true;
			const auto role = PlacedSceneRules::RoleOf(a_run.facts);
			if (role != a_run.role) {
				REX::INFO("PlacedScenes: {:08X} ({}): {} -> {} (running={} claimDone={} mirror={})", a_run.quest, a_run.event->title, PlacedSceneRules::Name(a_run.role),
					PlacedSceneRules::Name(role), a_run.facts.running, a_run.facts.claimDone, a_run.facts.mirror);
				a_run.role = role;
			}
		}

		// Every stage of the quest but its shutdown marked done, no script run: our triggers never fire again, nor does the
		// story manager start the quest (its 10 done).
		std::uint32_t MarkAll(const Run& a_run, RE::TESQuest* a_quest)
		{
			std::uint32_t count = 0;
			for (const auto stage : StagesOf(a_quest)) {
				if (stage != a_run.event->shutdown && QuestStageLayout::MarkDone(a_quest, stage)) {
					++count;
				}
			}
			return count;
		}

		// The run's NPCs off here, those on (the copies NpcSync switched on for the runner's).
		std::uint32_t SwitchOff(const Run& a_run)
		{
			std::uint32_t count = 0;
			for (const auto id : a_run.actors) {
				const auto actor = id ? RE::TESForm::GetFormByID<RE::Actor>(id) : nullptr;
				if (actor && !actor->IsDisabled() && !actor->IsDeleted()) {
					EffectTasks::Retire(actor);  // its effects go with its AI process (EffectTask.h)
					actor->Disable();
					++count;
				}
			}
			return count;
		}

		// Whether our own player is outdoors in that worldspace.
		bool OurPlayerIn(std::uint32_t a_worldspace)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			return a_worldspace && cell && !cell->IsInterior() && cell->worldSpace && cell->worldSpace->GetFormID() == a_worldspace;
		}

		// A friend's stand-in in the area (our worldspace's): since when, cleared when none is.
		bool FriendIn(Run& a_run, std::size_t a_claim, const PlacedSceneRules::Area& a_area, const std::vector<RemotePlayers::Info>& a_friends, Clock::time_point a_now)
		{
			const bool in = std::ranges::any_of(a_friends, [&](const RemotePlayers::Info& a_friend) {
				return a_friend.state && a_friend.state->cell == 0 && a_friend.state->worldspace == a_run.worldspace &&
				       PlacedSceneRules::Inside(a_area, a_friend.state->x, a_friend.state->y, a_friend.state->z);
			});
			auto& since = a_run.friendSince[a_claim];
			if (!in) {
				since.reset();
				return false;
			}
			if (!since) {
				since = a_now;
			}
			return a_now - *since >= PlacedSceneRules::FRIEND_IN_AREA;
		}

		void Report(Run& a_run, std::uint16_t a_stage)
		{
			if (!a_run.sent.insert(a_stage).second) {
				return;
			}
			outgoing.push_back(Protocol::Encode(Protocol::QuestStage{ a_run.quest, a_stage, 0 }, Protocol::MessageType::kReportQuestStage));
			++sentCount;
			REX::INFO("PlacedScenes: {:08X} ({}): reporting our run's stage {}", a_run.quest, a_run.event->title, a_stage);
		}

		// A friend's stage, as our quest stands.
		void Take(Run* run, const Protocol::QuestStage& a_stage, bool a_live)
		{
			const auto quest = QuestOf(*run);
			if (!quest) {
				return;
			}
			run->facts = Read(*run, quest);
			const auto act = PlacedSceneRules::OnFriendStage(*run->event, run->facts, a_stage.stage, a_live);
			switch (act) {
			case Act::kIgnore:
				break;
			case Act::kMark:
				if (a_stage.stage != run->event->shutdown && QuestStageLayout::MarkDone(quest, a_stage.stage)) {
					++marked;
				}
				break;
			case Act::kFollow:
			case Act::kYield:
				{
					const auto count = MarkAll(*run, quest);
					marked += count;
					++(act == Act::kFollow ? followed : yielded);
					REX::INFO("PlacedScenes: {:08X} ({}): player {}'s run claimed it (stage {}, {}): {} here, {} stages marked done without their scripts", run->quest,
						run->event->title, a_stage.playerId, a_stage.stage, a_live ? "live" : "from the session's state",
						act == Act::kFollow ? "our quest doesn't run, so we follow theirs" : "our own run yields to theirs", count);
					break;
				}
			case Act::kEndFollowed:
				{
					run->keepOff = true;
					const auto off = SwitchOff(*run);
					++endsFollowed;
					REX::INFO("PlacedScenes: {:08X} ({}): the run we follow ended (stage {}): its NPCs stay off here ({} switched off)", run->quest, run->event->title, a_stage.stage, off);
					break;
				}
			case Act::kEndYielded:
				if (!run->stopDue) {
					run->stopDue = true;
					REX::INFO("PlacedScenes: {:08X} ({}): the run we yielded to ended (stage {}): ours stops", run->quest, run->event->title, a_stage.stage);
				}
				break;
			}
			Refresh(*run);
		}
	}

	bool OnFriendStage(const Protocol::QuestStage& a_stage, bool a_live)
	{
		auto run = RunOf(a_stage.quest);
		if (!run) {
			return false;
		}
		// Taken in play, at the next poll: a catch-up that comes during a load would be marked in the world the load
		// replaces.
		if (Config::Get().syncQuests && run->pending.size() < MAX_PENDING) {
			run->pending.emplace_back(a_stage, a_live);
		}
		return true;
	}

	void Frame(bool a_inSession)
	{
		Resolve();
		const auto now = Clock::now();
		std::vector<RemotePlayers::Info> friends;
		if (a_inSession && RemotePlayers::Any()) {
			friends = RemotePlayers::List();
		}
		for (auto& run : runs) {
			const auto quest = QuestOf(run);
			if (!quest) {
				continue;
			}
			for (const auto& [stage, live] : std::exchange(run.pending, {})) {
				Take(&run, stage, live);
			}
			Refresh(run);
			if (run.stopDue) {
				run.stopDue = false;
				if (run.facts.running) {
					RE::Console::ExecuteCommand(std::format("stopquest {:08X}", run.quest).c_str());
					++stopped;
				}
			}
			if (!a_inSession) {
				continue;
			}
			// Our run's stages, once a claim of ours is done (and the shutdown once the quest stopped: its bit may read
			// cleared once it stopped).
			if (PlacedSceneRules::Reports(run.facts)) {
				for (const auto stage : QuestStageLayout::Done(quest)) {
					Report(run, stage);
				}
				if (!run.facts.running) {
					Report(run, run.event->shutdown);
				}
			}
			// A friend's stand-in in a trigger's area: their game, following or not running the quest, sets no claim.
			std::array<bool, CLAIMS> in{};
			for (std::size_t i = 0; i < CLAIMS; ++i) {
				in[i] = FriendIn(run, i, run.event->claims[i].area, friends, now);
			}
			const auto claim = PlacedSceneRules::ClaimFor(
				*run.event, run.facts, OurPlayerIn(run.worldspace), [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(quest, a_stage); }, [&](std::size_t a_index) { return in[a_index]; });
			if (claim && run.claimsSet.insert(*claim).second) {
				++claimsForFriends;
				REX::INFO("PlacedScenes: {:08X} ({}): a friend's stand-in stands in the trigger's area for stage {}: set here, as the trigger would for our player",
					run.quest, run.event->title, *claim);
				RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", run.quest, *claim).c_str());
			}
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	bool Holds(std::uint32_t a_ref)
	{
		for (const auto& run : runs) {
			if (run.read && PlacedSceneRules::Holds(run.facts) && std::ranges::contains(run.actors, a_ref)) {
				return true;
			}
		}
		return false;
	}

	bool SharesActors(std::uint32_t a_quest)
	{
		const auto run = RunOf(a_quest);
		return run && run->read && PlacedSceneRules::Runs(run->facts);
	}

	bool FollowsActors(std::uint32_t a_quest)
	{
		const auto run = RunOf(a_quest);
		return run && run->read && run->role == Role::kYielded;
	}

	bool KeepsOff(std::uint32_t a_ref)
	{
		return std::ranges::any_of(runs, [&](const Run& a_run) { return a_run.keepOff && std::ranges::contains(a_run.actors, a_ref); });
	}

	void OnServerChanged()
	{
		for (auto& run : runs) {
			run.pending.clear();
			run.sent.clear();
			run.claimsSet.clear();
			run.friendSince = {};
		}
	}

	void OnSessionEnded()
	{
		for (auto& run : runs) {
			Refresh(run);
			if (run.role == Role::kYielded && run.facts.running) {
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", run.quest).c_str());
				++stopped;
				REX::INFO("PlacedScenes: {:08X} ({}): the session ended: our run that yielded stops", run.quest, run.event->title);
			} else if (run.role == Role::kFollower) {
				const auto off = SwitchOff(run);
				REX::INFO("PlacedScenes: {:08X} ({}): the session ended: the run we followed is no more here ({} NPCs switched off)", run.quest, run.event->title, off);
			}
			run.pending.clear();
			run.sent.clear();
			run.claimsSet.clear();
			run.friendSince = {};
			run.keepOff = false;
			run.stopDue = false;
		}
	}

	void Rebaseline()
	{
		for (auto& run : runs) {
			run.sent.clear();
			run.claimsSet.clear();
			run.friendSince = {};
			run.keepOff = false;
			run.stopDue = false;
			run.read = false;
			run.facts = {};
			run.role = Role::kNone;
		}
	}

	std::string Describe()
	{
		std::string roles;
		for (const auto& run : runs) {
			roles += std::format("{}{:08X}={}", roles.empty() ? "" : ",", run.quest, PlacedSceneRules::Name(run.role));
		}
		return std::format("placedScenes: roles={} followed={} yielded={} marked={} endsFollowed={} stopped={} sent={} claimsForFriends={}", roles.empty() ? "none" : roles,
			followed, yielded, marked, endsFollowed, stopped, sentCount, claimsForFriends);
	}
}
