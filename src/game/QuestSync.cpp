#include "game/QuestSync.h"
#include "game/Official.h"

#include "AttackBook.h"
#include "Config.h"
#include "RewardBook.h"
#include "game/Achievements.h"
#include "game/BodySave.h"
#include "game/CatchUpMenus.h"
#include "game/Companions.h"
#include "game/EncounterRuns.h"
#include "game/Encounters.h"
#include "game/FadeWatch.h"
#include "game/GameStop.h"
#include "game/HostRules.h"
#include "game/HostileHold.h"
#include "game/Hud.h"
#include "game/MenuInput.h"
#include "game/MenuMessages.h"
#include "game/NpcSync.h"
#include "game/OpenMenus.h"
#include "game/PlacedScenes.h"
#include "game/Papyrus.h"
#include "game/Party.h"
#include "game/Puppets.h"
#include "game/QuestFilter.h"
#include "game/QuestRewards.h"
#include "game/QuestStageLayout.h"
#include "game/QuestStart.h"
#include "game/RadiantRules.h"
#include "game/RadiantSync.h"
#include "game/RemotePlayers.h"
#include "game/RollHook.h"
#include "game/SceneHold.h"
#include "game/ScriptDoors.h"
#include "game/Spawns.h"
#include "game/StageLog.h"
#include "game/StageRules.h"
#include "game/Story.h"
#include "game/WorldSync.h"
#include "Text.h"

#include <deque>
#include <set>
#include <unordered_set>

namespace QuestSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using StageLog::QUEST_COMPLETED;
		using StageLog::QUEST_RUNNING;

		// BGSScene::parentQuest, read by BGSScene::SetSceneActive (+6376D0: `mov rdx,[rcx+0A8h]`, then the
		// quest's running bit at +F4).
		static_assert(offsetof(RE::BGSScene, parentQuest) == 0xA8);

		constexpr auto POLL_INTERVAL = 500ms;
		// After loading, quest scripts settle their stages for a moment; that isn't progress.
		constexpr auto SETTLE_TIME = 5s;

		// A quest as the sharing filter sees it (QuestFilter): the type the game files it under, whether it
		// has objectives, and the name its designers gave it.
		QuestFilter::Quest Filed(const RE::TESQuest* a_quest)
		{
			const char* editorId = a_quest->formEditorID.c_str();
			return { a_quest->GetFormID(), a_quest->data.questType, !a_quest->objectives.empty(), editorId ? editorId : "" };
		}

		// The factions' last quests, filed under no type (QuestFilter::STORY_FINALES).
		bool IsFinale(const RE::TESQuest* a_quest)
		{
			return QuestFilter::IsFinale(Filed(a_quest));
		}

		// A dialogue quest: a place's people's lines and scenes, under no type and with no objectives, some
		// of whose stages open the way on (Diamond City's entrance scene opens the gate; see QuestFilter).
		// Shared with the rest of the story when the host shares it. With no objective ever shown, such a
		// quest counts as started once it is running or has stages done (SessionStarted).
		bool IsDialogue(const RE::TESQuest* a_quest)
		{
			return QuestFilter::IsDialogue(Filed(a_quest));
		}

		bool IsSafe(const RE::TESQuest* a_quest);
		bool IsSilent(const RE::TESQuest* a_quest);
		bool IsCompanion(const RE::TESQuest* a_quest);

		// A quest whose stages are no steps in order and which counts as started once it runs: a dialogue quest, or an
		// objective-less one the host's sQuests = safe shares (IsSilent). Neither shows an objective.
		bool Unordered(const RE::TESQuest* a_quest)
		{
			return IsDialogue(a_quest) || IsSilent(a_quest);
		}

		// Quests whose progress is watched here: the shared types, the factions' finales and the dialogue quests, the
		// radiants shared by their run whatever their type (Diamond City's wanted posters are misc quests), and the
		// quests the host's sQuests = safe shares (IsSafe, SILENT's too), whatever this session's sQuests: what was
		// done in them is known when a session that shares them begins. And the companions' quests filed under no type
		// (IsCompanion), shared as side quests.
		bool Watched(const RE::TESQuest* a_quest)
		{
			return QuestFilter::Watched(Filed(a_quest)) || RadiantSync::ShareOf(a_quest) == RadiantRules::Share::kShared || IsSafe(a_quest) || IsCompanion(a_quest);
		}

		bool ChainStarted(const RE::TESQuest* a_quest);

		// Whether the player has started the quest here, as reporting, setting friends' stages and the
		// host's baseline read it: an objective shown (PlayerStarted), or for a dialogue or silent quest, which
		// has none (Unordered), running or with stages done; or for a story quest that shows none while its player
		// is in it (StageRules::CHAIN_STARTS: Kellogg's memory), running or with stages done once this player went in by
		// their own lounger (ChainStarted).
		bool SessionStarted(const RE::TESQuest* a_quest)
		{
			if (PlayerStarted(a_quest)) {
				return true;
			}
			if (Unordered(a_quest) && ((a_quest->data.flags & QUEST_RUNNING) || !QuestStageLayout::Done(a_quest).empty())) {
				return true;
			}
			return ChainStarted(a_quest);
		}

		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;
		// Stages a quest's script sets this soon after another player's stage are its reaction to
		// that stage, not this player's progress: every game that applies it gets there by itself.
		constexpr auto SCRIPT_REACTION_WINDOW = 3s;
		// Live stages, the friend nearby and the warning's cover: StageRules (LIVE_FOR, NEAR_FRIEND, WARNING_COVERS).
		using StageRules::LIVE_FOR;
		using StageRules::NEAR_FRIEND;
		using StageRules::WARNING_COVERS;
		// Caught-up stages wait while the player fights and a moment after: a stage that moves them
		// mustn't come mid-fight, and its quest XP would be taken for kill XP (see Party). Not forever:
		// something out of reach can keep the player in combat.
		constexpr auto AFTER_COMBAT = 5s;
		constexpr auto MAX_COMBAT_HOLD = 60s;
		// The quests caught up with are named in one notice, once none came for a moment (or every so often).
		constexpr auto NOTICE_QUIET = 3s;
		constexpr auto NOTICE_EVERY = 15s;
		constexpr std::size_t NOTICE_NAMES = 3;

		// MQ104 "Unlikely Valentine" and MQ105 "Getting a Clue" (Fallout4.esm). MQ104's 210 starts MQ105 at
		// 10 and moves MQ104 on to 1000; nothing else starts MQ105.
		constexpr std::uint32_t MQ104 = 0x0001F25E;
		constexpr std::uint16_t MQ104_STARTS_MQ105 = 210;
		constexpr std::uint32_t MQ105 = 0x000229E6;
		constexpr std::uint16_t MQ105_FIRST = 10;

		// MQ204 "The Glowing Sea" (Fallout4.esm), Virgil's reference (the unique actor its alias holds), and DN053 (Virgil's
		// serum), whose 60 cures him: disables him and enables his cured self (StageRules::FixesVirgil).
		constexpr std::uint32_t MQ204 = 0x0006B500;
		constexpr std::uint32_t VIRGIL_REF = 0x0006B505;
		constexpr std::uint32_t DN053 = 0x000ADCE7;
		constexpr std::uint16_t DN053_CURE = 60;

		// The host's progress reported when its session starts: at most this many stages (a story played to
		// its end has a few thousand), put in the outbox this many at a time (it keeps MAX_UNSENT).
		constexpr std::size_t MAX_BASELINE = 8192;
		constexpr std::size_t BASELINE_BATCH = 256;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last current stage seen per quest
		// The stages seen done per quest, and the digest of the quest's done stages they were read with (QuestStageLayout::
		// DigestOf; none: read them again at the next look, a stage was set here since).
		struct Seen
		{
			StageLog::Stages                        stages;
			std::optional<QuestStageLayout::Digest> digest;
		};
		std::unordered_map<std::uint32_t, Seen>          doneStages;
		std::unordered_map<std::uint32_t, std::uint8_t>  safeQuests;  // SafeKindOf, by form ID, once looked up
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		StageLog::Queue                                  waiting;  // stages from other players, until they are set here
		std::unordered_set<std::uint32_t>                waited;   // quests whose stages waited for the quest to start here
		std::unordered_map<std::uint32_t, StageLog::Wait> waitLogged;  // why each quest's stages wait, as last logged
		StageLog::Unstarted                              unstarted;  // stages done here just before their quest was started
		std::unordered_set<std::uint32_t>                startedHere;  // quests not started here that a friend's stage started for the session
		std::unordered_set<std::uint32_t>                friendMoved;  // quests a friend's stage was set in here this session (QuestRewards: not finished on our own)
		std::unordered_set<std::uint32_t>                catchingUp;   // ... with friends' stages still to set (logged once they are)
		std::unordered_map<std::uint64_t, Clock::duration> startHolds;  // a stage that started its quest: the start-up stage's hold too
		bool                                             inSession = false;  // the last Frame's
		bool                                             hosting = false;    // the last Frame's: our own server runs (a timer's stages are the host's, StageRules::HOST_TIMERS)
		// The host's progress so far, to report (QueueBaseline): a few at a time, so the outbox keeps them all.
		struct Baselined
		{
			std::uint32_t quest;
			std::uint16_t stage;
			bool          story;
			bool          safe;  // of a quest QuestFilter::SAFE holds
		};
		std::deque<Baselined>                            baseline;
		bool                                             baselineQueued = false;
		std::uint32_t                                    baselined = 0;  // reported of it
		struct Live
		{
			Clock::time_point at;
			std::uint32_t     playerId;  // who did it (0: not known)
		};
		std::unordered_map<std::uint64_t, Live>          liveAt;  // friends' stages that came live (quest << 16 | stage)
		std::unordered_set<std::uint64_t>                cameLive;  // ... ever since the last load, for the achievements (liveAt forgets them after LIVE_FOR; a stage that waited longer, or came again after a reconnect, still happened live)
		// A stage set here took the player's controls away (StageRules::LOCKS), and the stage that gives
		// them back is still to come. How long that has taken is the queue's (Queue::Lock).
		struct Locked
		{
			std::uint32_t quest;
			std::uint16_t stage;
			std::uint32_t releaseQuest;
			std::uint16_t releaseStage;
		};
		std::optional<Locked>                            locked;
		std::unordered_set<std::uint64_t>                lockWaitLogged;  // lock stages logged as waiting for their release
		std::unordered_set<std::uint64_t>                openingWaitLogged;  // stages logged as waiting for the opening to end
		std::unordered_set<std::uint64_t>                trapWaitLogged;     // stages logged as waiting for the player to leave a trap's cells (StageRules::TRAPS)
		std::unordered_set<std::uint64_t>                memoryWaitLogged;   // stages logged as waiting for the player to leave a memory (StageRules::MEMORY_PLACES, MEMORY_WAITS)
		std::unordered_set<std::uint64_t>                eachWaitLogged;  // companion quests' kEach stages logged as waiting for their quest to start here
		std::unordered_set<std::uint64_t>                eachSettleLogged;  // ... and as waiting for the game to settle (Companions::AliasesReadable)
		// Stages logged as never done in time, once each while the game runs; gaveUp counts the rest. A stage that was to
		// start its quest, which didn't start (000190BC's 0, Diamond City's entrance scene, at each load away from Diamond
		// City), is logged as that instead (StageLog::Queue::FailedStarts), once per quest.
		std::unordered_set<std::uint64_t>                gaveUpLogged;
		std::unordered_set<std::uint32_t>                startFailedLogged;  // quests a friend's stage didn't start here, logged once each
		// Event-scoped quests a friend's stage starts through the game's own start (StartsFromFriend): how many polls a stop of
		// one still under way has kept its stage waiting (QuestStart::Judge), and those whose start was refused or failed,
		// logged once each.
		std::unordered_map<std::uint32_t, int>           eventStartWaits;
		std::unordered_set<std::uint32_t>                eventStartLogged;
		// Stages, by Key(quest, stage), whose quest such a start started here without running them: set at the next poll, once
		// the start-up stages the start queued have run; for them the quest counts as one that wasn't running (the start rules).
		std::unordered_set<std::uint64_t>                eventStartedFor;
		// The timers' starts a scene sets (StageRules::TIMER_STARTS: FFGoodneighbor08's 25 after its 20), by quest: how long,
		// in play (not while paused or loading), the quest has run with the scene's stage done and the timer's start not.
		struct TimerWait
		{
			Clock::duration   played{};
			Clock::time_point last{};
		};
		std::unordered_map<std::uint32_t, TimerWait>     timerWaits;
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		StageLog::Outbox<std::vector<std::uint8_t>>      outgoing;  // reports, kept or dropped while not connected
		Clock::time_point                                nextPoll{};
		bool                                             busy = false;        // this poll: a conversation or scene
		bool                                             combatHold = false;  // this poll: caught-up stages wait
		std::optional<Clock::time_point>                 combatSince;
		Clock::time_point                                lastCombat{};
		SceneHold::Watch                                 sceneHold;  // whether the scene the player is in holds friends' stages back
		std::uint32_t                                    sessionSceneLogged = 0;  // the session's scene last logged (0: none)
		CatchUpMenus::Guard                              menuGuard;  // menus a friend's stage opened here, closed again
		std::uint64_t                                    lastSetStage = 0;  // the friend's stage last set here (quest << 16 | stage), for the log
		std::atomic<Clock::rep>                          playerActivatedAt{ 0 };  // our player last activated something (any thread)
		Clock::rep                                       playerActivatedSeen = 0;  // ... as last handed to menuGuard
		std::optional<Clock::time_point>                 warnedAt;
		// The follow offers made (StageRules::Verdict::kOffer: a live stage that moves the player, its friend far away),
		// by quest << 16 | stage: when, and the friend to go with. Taken up, run out or set, each is dropped. Its OFFER_FOR is
		// counted in time it was judged: a loading screen (the queue paused), or a teleport to that friend on its way, doesn't eat it.
		struct Offer
		{
			Clock::time_point at;
			std::uint32_t     playerId;
			Clock::time_point seen;      // last judged
			Clock::duration   paused{};  // not counted: gaps between judgements longer than OFFER_GAP, and a teleport's flight
		};
		// Polls are POLL_INTERVAL apart: a longer gap between two judgements of an offer is time the queue was paused.
		constexpr auto OFFER_GAP = 2s;
		std::unordered_map<std::uint64_t, Offer>         offers;
		// Friends' stages that turn a faction on the player (StageRules::HOSTILE), waiting here while any of that faction is near
		// (HostileHold): how long each has waited, in play, and when the log says so again.
		HostileHold::Waits                               hostileWaits;
		std::uint32_t                                    hostileHeld = 0;     // ... that waited and were then set
		std::uint32_t                                    hostileApplied = 0;  // stages that move the player not set, their faction turned on the player here all the same, or their kick-out run (Verdict::kHostileOnly)
		// The companions this player was warned about this session (their ref IDs): a friend's kick-out that dismisses the companion
		// following them (StageRules::DISMISSES) says so on the HUD once, before it lands; not again per stage, nor after a load.
		// A stage that dismisses them only on a condition says nothing and spends nothing (StageRules::DismissalWarnings).
		StageRules::DismissalWarnings                    dismissalsWarned;
		// The hostility calls made here this session, by faction and call (MadeKey), so a stage that comes again (a rejoin's
		// catch-up) or a retried stage doesn't make them twice; and how often a stage's call the VM wouldn't queue was tried
		// (HostileHold::CALL_TRIES). Both forgotten at a load: the loaded save's own state says what holds (AlreadyEnemy).
		std::set<std::uint64_t>                          hostilityMade;
		std::unordered_map<std::uint64_t, unsigned>      hostileCallTries;
		// The player's own faction (Fallout4.esm's PlayerFaction): what the stages' scripts set the faction against (SetEnemy).
		constexpr std::uint32_t PLAYER_FACTION = 0x0001C21C;
		// TESFaction's DATA flag "Player Is Enemy" (bit 3 of the record's flags, what Faction.SetPlayerEnemy sets and
		// Faction.IsPlayerEnemy reads).
		constexpr std::uint32_t FACTION_PLAYER_IS_ENEMY = 1u << 3;
		std::vector<std::string>                         caughtUpNames;
		Clock::time_point                                caughtUpFirst{};
		Clock::time_point                                caughtUpLast{};
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		std::uint32_t                                    caughtUp = 0;  // ... after waiting for the quest to start here
		std::uint32_t                                    gaveUp = 0;    // set, but not done in time
		std::uint32_t                                    refused = 0;   // more than the queue holds
		std::uint32_t                                    unknown = 0;   // stages the quest doesn't have
		std::uint32_t                                    skipped = 0;   // not for this player (StageRules)
		std::uint32_t                                    openingDropped = 0;  // the opening's, never shared (StageRules::IsOpening)
		std::uint32_t                                    encounterStartsRefused = 0;  // an encounter's stage that would have started it (SetHere)
		std::unordered_set<std::uint64_t>                timerDropped;        // a friend's stages of a timer's (StageRules::HOST_TIMERS), logged once each
		std::unordered_set<std::uint64_t>                timerKept;           // our own stages of a timer's not reported (not the host), logged once each
		std::uint32_t                                    debugDropped = 0;    // a developer's quickstart's, never shared (StageRules::IsDebug)
		std::uint32_t                                    debugKept = 0;       // a developer's quickstart done in this game, not reported
		std::uint32_t                                    quickstartSet = 0;   // set by a quickstart done in this game, not reported (StageRules::QUICKSTART_SETS)
		std::uint32_t                                    othersMemory = 0;    // the memory's stages done here, whose player isn't in it: not reported (OthersMemory)
		std::unordered_set<std::uint32_t>                perGameDropped;      // side quests each game fills its own way (PerGame), logged once
		std::uint32_t                                    completed = 0;
		std::uint32_t                                    repaired = 0;
		std::uint32_t                                    released = 0;  // controls given back by the timeout
		std::uint32_t                                    waivedPrices = 0;  // friends' purchases set here with their price at 0 (StageRules::PRICES)
		std::uint32_t                                    eachStages = 0;    // companion quests' stages marked done here by their EACH acts (StageRules kEach)
		std::uint32_t                                    drivenStages = 0;  // ... set with their script here: this game drives the quest (StageRules::DriveOf)
		// The globals that came with friends' stages still waiting (quest << 16 | stage), written here just before each is set
		// (StageRules::CARRIES, PlanCarry): the chooser's values, so the stage's script decides here as it did for them.
		std::unordered_map<std::uint64_t, std::vector<StageRules::GlobalValue>> carriedValues;
		std::unordered_set<std::uint64_t>                carryWaitLogged;  // stages logged as waiting for the stage their row needs
		std::uint32_t                                    carriedSet = 0;      // stages set here with their globals written first
		std::uint32_t                                    carriedSkipped = 0;  // stages not set: they came without a global their script reads
		std::uint32_t                                    carriedDropped = 0;  // values that came with stages that no row here names
		std::uint32_t                                    carriedSent = 0;     // our stages reported with their globals
		std::unordered_map<std::uint32_t, std::uint16_t> miscStages;  // misc quests: stage changes only
		Clock::time_point                                lastStageChange{};
		std::uint32_t                                    lastStageChangeQuest = 0;  // whose stage that was (for Party's log of withheld kill XP)

		void StageChanged(std::uint32_t a_quest, Clock::time_point a_at)
		{
			lastStageChange = a_at;
			lastStageChangeQuest = a_quest;
		}

		std::uint64_t Key(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			return (static_cast<std::uint64_t>(a_quest) << 16) | a_stage;
		}

		std::span<RE::TESQuest*> AllQuests()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return {};
			}
			auto& quests = handler->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}

		// A stage set in the middle of a conversation or scene can break it; wait until it's over. Not a scene
		// a friend's stage started here (Codsworth's search, started by his talk's stage): it is the session's,
		// the friend's later stages end it, and held back by it they would never come (SceneHold). The player's
		// own scene of a quest with stages waiting here only holds them for SceneHold::MAX_HOLD.
		bool PlayerBusy(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto scene = player ? player->GetCurrentScene() : nullptr;
			const auto quest = scene && scene->parentQuest ? scene->parentQuest->GetFormID() : 0u;
			const bool held = sceneHold.Holds({ .talking = OpenMenus::Open(OpenMenus::Menu::kDialogue),
												  .scene = scene ? scene->GetFormID() : 0u,
												  .sceneQuest = quest,
												  .sceneQuestWaiting = quest != 0 && waiting.Contains(quest) },
				a_now);
			if (const auto sessions = sceneHold.SessionScene(); sessions != sessionSceneLogged) {
				sessionSceneLogged = sessions;
				if (sessions) {
					REX::INFO("QuestSync: the player is in scene {:08X} of {:08X}, started by a friend's stage; it holds no stages back, and its people are theirs", sessions, quest);
				}
			}
			return held;
		}

		// Whether caught-up stages wait now (see AFTER_COMBAT).
		bool CombatHold(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (player && player->IsInCombat()) {
				if (!combatSince) {
					combatSince = a_now;
				}
				lastCombat = a_now;
				return a_now - *combatSince < MAX_COMBAT_HOLD;
			}
			combatSince.reset();
			return a_now - lastCombat < AFTER_COMBAT;
		}

		// The game is paused (the Pip-Boy, the pause menu, a container, a terminal): scripts don't run.
		bool Paused()
		{
			return GameStop::Now();
		}

		// Whether this friend (the one who did a stage) is close by. Not any friend: with three players,
		// a stage one of them did across the map mustn't move the two standing together.
		bool NearFriend(std::uint32_t a_id)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto position = a_id ? RemotePlayers::PositionOf(a_id) : std::nullopt;
			if (!player || !position) {
				return false;
			}
			const auto& at = player->data.location;
			const float dx = position->x - at.x;
			const float dy = position->y - at.y;
			const float dz = position->z - at.z;
			return dx * dx + dy * dy + dz * dz <= NEAR_FRIEND * NEAR_FRIEND;
		}

		std::optional<StageRules::File> FileOf(const RE::TESQuest* a_quest)
		{
			const auto file = a_quest ? a_quest->GetFile(0) : nullptr;
			return file ? StageRules::FileOf(file->GetFilename()) : std::nullopt;
		}

		// A quest of StageRules::CHAIN_STARTS counts as started here (SessionStarted): its way in done here (this player went
		// in), and it runs or has stages done. Its id is looked at before its file (every quest goes through SessionStarted at
		// a baseline).
		struct ChainHere
		{
			const StageRules::ChainStart* row = nullptr;
			bool                          wayInDone = false;  // read done here (false where its quest isn't found)
		};
		ChainHere ChainOf(const RE::TESQuest* a_quest)
		{
			const auto local = a_quest ? a_quest->GetFormID() & 0x00FFFFFF : 0u;
			if (!a_quest || std::ranges::none_of(StageRules::CHAIN_STARTS, [&](const StageRules::ChainStart& a_row) { return a_row.quest == local; }) ||
				!Official::IsOfficialForm(a_quest)) {
				return {};
			}
			const auto file = FileOf(a_quest);
			const auto row = file ? StageRules::FindChainStart(*file, local) : nullptr;
			const auto handler = row ? RE::TESDataHandler::GetSingleton() : nullptr;
			const auto wayIn = handler ? handler->LookupForm<RE::TESQuest>(row->wayInQuest, StageRules::FileName(*file)) : nullptr;
			return { row, wayIn && QuestStageLayout::IsStageDone(wayIn, row->wayInStage) };
		}

		bool ChainStarted(const RE::TESQuest* a_quest)
		{
			const auto chain = ChainOf(a_quest);
			return chain.row && StageRules::ChainStarted(chain.row, chain.wayInDone, (a_quest->data.flags & QUEST_RUNNING) != 0, !QuestStageLayout::Done(a_quest).empty());
		}

		// A stage of Kellogg's memory done here by this game's own copy of the memory's scenes, whose player isn't in it
		// (StageRules::OthersMemory): never reported. A way in whose quest isn't found here reads as not done, as ChainStarted
		// reads it.
		bool OthersMemory(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto chain = ChainOf(a_quest);
			if (!StageRules::OthersMemory(chain.row, a_stage, chain.wayInDone)) {
				return false;
			}
			++othersMemory;
			const char* name = a_quest->GetFullName();
			REX::INFO("QuestSync: not reporting {:08X} '{}' stage {}: it is the memory's, whose player isn't this one; this game's own copy of its scenes set it",
				a_quest->GetFormID(), name ? name : "", a_stage);
			return true;
		}

		// The faction a HOSTILE row names, in this game (null if its file isn't loaded).
		RE::TESFaction* FactionOf(const StageRules::Hostile& a_row)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			return handler ? handler->LookupForm<RE::TESFaction>(a_row.faction, StageRules::FileName(static_cast<StageRules::File>(a_row.factionFile))) : nullptr;
		}

		// Whether any of the factions a friend's stage turns on the player (its HOSTILE rows) is near this player now
		// (HostileHold: alive, not disabled, not following a player (our companion, or a friend's by the server's word), not a
		// friend's stand-in, in the game's high process and within RANGE): the faction with the most near, and how many. A fight
		// is not asked here: the capped combat hold is (Situation::held).
		struct FactionNearby
		{
			std::uint32_t faction = 0;  // the form ID here (0: none near); the nearest-listed actor's own when actor is set
			std::size_t   count = 0;
			bool          actor = false;  // ActorNear's: the stage's own actors, not a faction's people
			[[nodiscard]] bool Near() const { return count > 0; }
		};
		FactionNearby FactionNear(std::span<const StageRules::Hostile> a_rows)
		{
			FactionNearby result;
			const auto    player = RE::PlayerCharacter::GetSingleton();
			const auto    lists = RE::ProcessLists::GetSingleton();
			if (!player || !lists) {
				return result;
			}
			const auto& me = player->data.location;
			for (const auto& row : a_rows) {
				const auto faction = FactionOf(row);
				if (!faction) {
					continue;
				}
				std::size_t count = 0;
				for (const auto& handle : lists->highActorHandles) {
					const auto ptr = handle.get();
					const auto actor = ptr.get();
					if (!actor || actor->IsPlayerRef() || Puppets::IsPuppet(actor) || !actor->IsInFaction(faction)) {
						continue;
					}
					if (!HostileHold::Counts(!actor->IsDead(false), actor->IsDisabled(), NpcSync::IsOurCompanion(actor) || Companions::FollowsFriend(actor->GetFormID()))) {
						continue;
					}
					const auto& at = actor->data.location;
					if (HostileHold::Near(at.x - me.x, at.y - me.y, at.z - me.z)) {
						++count;
					}
				}
				if (count > result.count) {
					result.count = count;
					result.faction = faction->GetFormID();
				}
			}
			return result;
		}

		// The NPC_ a leveled actor was placed as (its ExtraLeveledCreature's original), 0 for one that isn't leveled: its own
		// NPC_ (GetNPC) is the copy its game made of the roll, a runtime form (Rolls.cpp, RollCopyOf), so a unique actor
		// built from a leveled list is known by the NPC_ an alias names only through this. Read as Equipment.cpp's
		// OriginalBase and Spawns.cpp's LeveledOriginal read it.
		std::uint32_t PlacedAs(RE::Actor* a_actor)
		{
			const auto leveled = a_actor && a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraLeveledCreature>() : nullptr;
			return leveled && leveled->originalBase ? leveled->originalBase->GetFormID() : 0;
		}

		// Whether an actor a friend's stage turns on the player by itself (its StageRules::HOSTILE_ACTORS rows: StartCombat
		// on the player, its Aggression raised, a PlayerEnemy faction it is put in) is near this player now, as FactionNear
		// asks of a faction's people. The actor is the reference the row's alias holds in this game, or while the alias is
		// empty (that quest isn't running here: Sierra's starts with the stage that turns her) the one the row says the
		// alias is filled from: a placed reference, or a unique actor by its NPC_ (HostileHold::IsWanted), its own or, for
		// one built from a leveled list (Paladin Brandis), the one it was placed as (PlacedAs). It counts when
		// FactionNear would count it: in the game's high process, alive, not disabled, not following a player, not a
		// friend's stand-in, within RANGE. The actors are looked for by form ID among the high process's, so nothing is
		// read through the alias's reference once it is let go. The first near is named (FactionNearby::faction is its
		// form ID, actor set), with how many are.
		FactionNearby ActorNear(std::span<const StageRules::HostileActor> a_rows)
		{
			FactionNearby result;
			const auto    player = RE::PlayerCharacter::GetSingleton();
			const auto    lists = RE::ProcessLists::GetSingleton();
			const auto    handler = RE::TESDataHandler::GetSingleton();
			if (!player || !lists || !handler) {
				return result;
			}
			std::vector<HostileHold::Wanted> wanted;
			for (const auto& row : a_rows) {
				HostileHold::Wanted want;
				if (const auto holder = handler->LookupForm<RE::TESQuest>(row.holder, StageRules::FileName(static_cast<StageRules::File>(row.holderFile)))) {
					if (const auto ref = holder->GetAliasedRef(static_cast<std::uint32_t>(row.alias)).get()) {
						want.aliased = ref->GetFormID();
					}
				}
				if (want.aliased == 0 && row.fill != StageRules::ActorFill::kNone) {
					const auto id = handler->LookupFormID(row.fillId, StageRules::FileName(static_cast<StageRules::File>(row.fillFile)));
					(row.fill == StageRules::ActorFill::kReference ? want.fillRef : want.fillBase) = id;
				}
				if (want.Any()) {
					wanted.push_back(want);
				}
			}
			if (wanted.empty()) {
				return result;
			}
			const auto& me = player->data.location;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor->IsPlayerRef() || Puppets::IsPuppet(actor)) {
					continue;
				}
				const auto npc = actor->GetNPC();
				const auto base = npc ? npc->GetFormID() : 0u;
				const auto placedAs = PlacedAs(actor);
				if (std::ranges::none_of(wanted, [&](const HostileHold::Wanted& a_want) { return HostileHold::IsWanted(a_want, actor->GetFormID(), base, placedAs); })) {
					continue;
				}
				if (!HostileHold::Counts(!actor->IsDead(false), actor->IsDisabled(), NpcSync::IsOurCompanion(actor) || Companions::FollowsFriend(actor->GetFormID()))) {
					continue;
				}
				const auto& at = actor->data.location;
				if (HostileHold::Near(at.x - me.x, at.y - me.y, at.z - me.z)) {
					if (result.count == 0) {
						result.faction = actor->GetFormID();
						result.actor = true;
					}
					++result.count;
				}
			}
			return result;
		}

		// A faction's name by its form ID here; a hold for one actor (ActorNear) is kept by the actor's form ID, and is
		// named by the actor: "Cito".
		std::string FactionName(std::uint32_t a_faction)
		{
			const auto faction = RE::TESForm::GetFormByID<RE::TESFaction>(a_faction);
			if (!faction && a_faction != 0) {
				const auto  actor = RE::TESForm::GetFormByID<RE::Actor>(a_faction);
				const char* shown = actor ? actor->GetDisplayFullName() : nullptr;
				return shown ? std::string{ shown } : std::string{};
			}
			const auto name = faction ? RE::TESFullName::GetFullName(*faction) : std::string_view{};
			return std::string{ name };
		}

		// What a hold that ended waited for, by name: the name it had while it was near (an actor may be gone since), else
		// the faction's or the actor's now.
		std::string HeldName(const HostileHold::Waits::Ended& a_held)
		{
			return a_held.name.empty() ? FactionName(a_held.faction) : a_held.name;
		}

		// Whether a_faction's reactions list a_other as an enemy (TESReactionForm::reactionList, what Faction.SetEnemy writes).
		// The list is walked through a non-const faction: CommonLibF4's const begin() of BSSimpleList returns nothing.
		bool EnemyIn(RE::TESFaction* a_faction, const RE::TESForm* a_other)
		{
			for (const auto reaction : a_faction->reactionList) {
				if (reaction && reaction->form == a_other && reaction->fightReaction == RE::FIGHT_REACTION::kEnemy) {
					return true;
				}
			}
			return false;
		}

		// Whether the hostility a HOSTILE row makes holds here already: a SetEnemy row's faction and PlayerFaction each list the
		// other as an enemy (what Faction.SetEnemy(PlayerFaction, false, false) leaves, from either side), a SetPlayerEnemy row's
		// faction has its player-enemy flag. An AddToFaction row needs the stage's actor: never known. So a stage that comes
		// again (a rejoin, a load of a save past it, a friend's stage behind one that turned the faction already) neither
		// holds nor calls for a faction that is an enemy here already, and no HUD line says it turned.
		bool AlreadyEnemy(const StageRules::Hostile& a_row)
		{
			const auto faction = FactionOf(a_row);
			if (!faction) {
				return false;
			}
			switch (a_row.call) {
			case StageRules::kSetPlayerEnemy:
				return (faction->data.flags & FACTION_PLAYER_IS_ENEMY) != 0;
			case StageRules::kSetEnemy:
				{
					const auto player = RE::TESForm::GetFormByID<RE::TESFaction>(PLAYER_FACTION);
					return player && EnemyIn(faction, player) && EnemyIn(player, faction);
				}
			case StageRules::kAddToFaction:
				return false;
			}
			return false;
		}

		// A hostility call made here this session, for hostilityMade: the faction and the call.
		std::uint64_t MadeKey(const StageRules::Hostile& a_row)
		{
			return (static_cast<std::uint64_t>(a_row.factionFile) << 40) | (static_cast<std::uint64_t>(a_row.faction) << 8) | a_row.call;
		}

		// Turns the faction a HOSTILE row names on the player here the way the stage's script does, without the stage (it
		// would move the player, who didn't go with the friend: Verdict::kHostileOnly): Papyrus Faction.SetEnemy(PlayerFaction,
		// false, false), both sides enemies as the fragment's SetEnemy with neither side neutral, or Faction.SetPlayerEnemy(true),
		// queued on the game's VM as the fragment's own call is (Papyrus.h: it runs a frame or so later). An AddToFaction row
		// needs the actor the stage's alias holds: not made (no stage that moves the player has one; a test holds that). What
		// was called, for the log, and whether it was: never (the faction isn't in this game, or the row is AddToFaction) or
		// not this time (the VM wouldn't queue it: a load on, no handle yet), which is tried again (HostileHold::CALL_TRIES).
		struct Turned
		{
			enum Outcome
			{
				kMade,
				kNever,
				kRetry,
			};
			Outcome     outcome = kNever;
			std::string called;  // kMade: the call, for the log
		};
		Turned TurnOnPlayer(const StageRules::Hostile& a_row)
		{
			const auto faction = FactionOf(a_row);
			if (!faction || a_row.call == StageRules::kAddToFaction) {
				return {};
			}
			switch (a_row.call) {
			case StageRules::kSetPlayerEnemy:
				return Papyrus::CallMethod(faction, "Faction", "SetPlayerEnemy", true, true) ? Turned{ Turned::kMade, "Faction.SetPlayerEnemy(true)" } : Turned{ Turned::kRetry };
			case StageRules::kSetEnemy:
				{
					const auto self = Papyrus::BoundObject(faction, "Faction", true);
					const auto other = Papyrus::BoundObject(RE::TESForm::GetFormByID<RE::TESFaction>(PLAYER_FACTION), "Faction", true);
					if (!self || !other) {
						return { Turned::kRetry };
					}
					const bool queued = Papyrus::CallOn(self, "SetEnemy", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
						a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 3 };
						a_out[0] = other;  // akOther: PlayerFaction
						a_out[1] = false;  // abSelfIsNeutralToOther
						a_out[2] = false;  // abOtherIsNeutralToSelf
					});
					return queued ? Turned{ Turned::kMade, "Faction.SetEnemy(PlayerFaction, false, false)" } : Turned{ Turned::kRetry };
				}
			case StageRules::kAddToFaction:
				break;
			}
			return {};
		}

		// The kick-out quest a KICKOUTS row names, in this game (null if it isn't there).
		RE::TESQuest* KickOutQuest(const StageRules::KickOut& a_kick)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			return handler ? handler->LookupForm<RE::TESQuest>(a_kick.kickQuest, StageRules::FileName(static_cast<StageRules::File>(a_kick.file))) : nullptr;
		}

		// A friend's stage that dismisses a companion (StageRules::DISMISSES: the kick-outs' Danse, Deacon, X6-88), about to land
		// here or to have its kick-out run (a_kickOut): when the companion it dismisses is the one following our player, the HUD
		// says once per companion and session that the friend's choice takes them, as the game's kick-out does for the chooser;
		// the stage goes on all the same. The stage's own row, or its quest's start-up stage's when the stage starts the quest
		// (a_starting: the quest isn't running, as HostileRows merges it) and its own names nobody for sure, or the kick-out's
		// when a mover stands for one. A row that dismisses only on a condition of the game it runs in (most of the
		// Minutemen's) says nothing and leaves the warning for the kick-out that does (StageRules::DismissalWarnings).
		// The faction named is the one whose kick-out drops that companion (StageRules::DismissalFaction), not whichever the
		// stage turns on the player on its way; one without a name here gets the sentence that names none.
		void WarnDismissal(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage, bool a_starting, const StageRules::KickOut* a_kickOut, std::uint32_t a_friendId,
			const char* a_stageName)
		{
			const StageRules::Dismissal* row = a_kickOut ? StageRules::DismissalOf(a_file, a_kickOut->kickQuest, a_kickOut->kickStage) : StageRules::DismissalOf(a_file, a_quest, a_stage);
			if ((!row || row->conditional) && !a_kickOut && a_starting && a_stage != 0) {
				const auto startUp = StageRules::DismissalOf(a_file, a_quest, 0);
				if (startUp && (!row || !startUp->conditional)) {
					row = startUp;
				}
			}
			const auto following = row ? Companions::Following() : nullptr;
			if (!following) {
				return;
			}
			const auto handler = RE::TESDataHandler::GetSingleton();
			const auto companionQuest = handler ? handler->LookupForm<RE::TESQuest>(row->companionQuest, StageRules::FileName(static_cast<StageRules::File>(row->companionFile))) : nullptr;
			const RE::TESObjectREFR* dismissed = nullptr;
			if (companionQuest) {
				const auto ref = companionQuest->GetAliasedRef(row->alias).get();
				dismissed = ref ? &*ref : nullptr;
			}
			if (!dismissed || dismissed != static_cast<RE::TESObjectREFR*>(following) || !dismissalsWarned.Due(*row, following->GetFormID())) {
				return;
			}
			const char* companionName = following->GetDisplayFullName();
			const auto  named = StageRules::DismissalFaction(*row);
			const auto  faction = named && handler ? handler->LookupForm<RE::TESFaction>(named->faction, StageRules::FileName(static_cast<StageRules::File>(named->factionFile))) : nullptr;
			const auto  factionName = faction ? FactionName(faction->GetFormID()) : std::string{};
			const auto  player = a_friendId ? RemotePlayers::NameOf(a_friendId) : Text::Get("quest.a_friend");
			const auto  companion = companionName && *companionName ? std::string{ companionName } : Text::Get("quest.a_companion");
			REX::INFO("QuestSync: '{}' stage {} dismisses the player's companion {:08X} '{}' (a friend's stage, as the game has it for the one who chose; its faction {:08X} '{}'); the HUD says so once",
				a_stageName ? a_stageName : "", a_stage, following->GetFormID(), companionName ? companionName : "", faction ? faction->GetFormID() : 0u, factionName);
			Hud::Notify(factionName.empty() ? Text::Format("quest.hostile_companion_unnamed", { { "player", player }, { "companion", companion } })
											: Text::Format("quest.hostile_companion", { { "player", player }, { "faction", factionName }, { "companion", companion } }));
		}

		// A quest's name for the HUD: not an editor's name in brackets ("[DN070 - Jamaica Plain]", never shown in the Pip-Boy),
		// nor a dialogue or silent quest's: "a quest" then.
		std::string ShownName(const RE::TESQuest* a_quest, const char* a_name)
		{
			return a_name && *a_name && *a_name != '[' && !Unordered(a_quest) ? std::string{ a_name } : Text::Get("quest.a_quest");
		}

		// Whether the host's sQuests = safe shares a quest of the game's own files: 0 no, 1 QuestFilter::SAFE (misc
		// quests and quests under no type with objectives that are the same in every game), 2 QuestFilter::SILENT
		// (those with no objectives). Looked up once per quest.
		std::uint8_t SafeKindOf(const RE::TESQuest* a_quest)
		{
			const auto id = a_quest->GetFormID();
			if (const auto known = safeQuests.find(id); known != safeQuests.end()) {
				return known->second;
			}
			const auto   file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			const auto   local = id & 0x00FFFFFF;
			std::uint8_t kind = 0;
			if (file && QuestFilter::IsSafe(static_cast<std::uint8_t>(*file), local)) {
				kind = 1;
			} else if (file && QuestFilter::IsSilent(static_cast<std::uint8_t>(*file), local)) {
				kind = 2;
			}
			safeQuests.emplace(id, kind);
			return kind;
		}

		// A quest the host's sQuests = safe shares, SAFE or SILENT: every rule for one (shared only with sQuests =
		// safe, only with the story, marked so in the outbox and the baseline) is for both.
		bool IsSafe(const RE::TESQuest* a_quest)
		{
			return SafeKindOf(a_quest) != 0;
		}

		// One of those with no objectives (QuestFilter::SILENT): shared as a dialogue quest is (Unordered).
		bool IsSilent(const RE::TESQuest* a_quest)
		{
			return SafeKindOf(a_quest) == 2;
		}

		// A companion's quest filed under no type that is shared as a side quest (QuestFilter::COMPANION_QUESTS: Curie's,
		// MacCready's): with either sQuests, the story shared or not. Its kEach stages are set here by their EACH acts.
		bool IsCompanion(const RE::TESQuest* a_quest)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file && QuestFilter::IsCompanionQuest(static_cast<std::uint8_t>(*file), a_quest->GetFormID() & 0x00FFFFFF);
		}

		// A stage ScriptDoors shares by itself, with either sQuests (DN142's 50, BoSEnable's 15): never reported as a
		// SILENT quest's, so it goes once, as the door's (a friend's comes to ScriptDoors first anyway).
		bool DoorsStage(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = a_quest->GetFile(0);
			return file && ScriptDoors::Owns(file->GetFilename(), a_quest->GetFormID() & 0x00FFFFFF, a_stage);
		}

		// A stage that is a timer's of its quest's own script (StageRules::HOST_TIMERS: FFGoodneighbor08's 25, 200 and 300,
		// and its 1000, the shutdown stage the stop runs), or null: only the host's game reports one, and no game sets a
		// friend's but the shutdown stage (TakesTimer).
		const StageRules::Timer* TimerOf(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file ? StageRules::FindTimer(*file, a_quest->GetFormID() & 0x00FFFFFF, a_stage) : nullptr;
		}

		// The session shares the quests IsSafe names (the welcome's sQuests = safe); out of a session, no.
		bool SafeShared()
		{
			return HostRules::Get().quests == Protocol::QuestRule::kSafe;
		}

		// The main story and faction quests, their finales and the dialogue quests too, and the quests sQuests = safe shares:
		// those only with the story.
		bool IsStory(const RE::TESQuest* a_quest)
		{
			return IsFinale(a_quest) || IsDialogue(a_quest) || Story::IsStoryType(a_quest->data.questType) || IsSafe(a_quest);
		}

		// A side quest the story manager starts with targets each game picks itself (QuestFilter::PER_GAME: the add-ons'
		// radiants, DN121, DN151): a friend's stage of it would start it here with this game's own.
		bool PerGame(const RE::TESQuest* a_quest)
		{
			const auto file = FileOf(a_quest);
			return file && QuestFilter::IsPerGame(static_cast<std::uint8_t>(*file), a_quest->GetFormID() & 0x00FFFFFF);
		}

		// A stopped quest the story manager starts from an event (FFDiamondCity01, DialogueDiamondCityEntrance,
		// FFGoodneighbor08) that a friend's stage starts here through the engine's own start first, since the console's
		// setstage of a stopped event-scoped quest is refused (QuestStartRules::StartsFromFriend: none of its aliases that
		// must fill comes from the event). Not a radiant, a settlement attack, a random encounter or a PER_GAME quest: each
		// has its own path, or is never started from a friend's stage; nor a run-once handler that acts on this game's world
		// as its event found it (QuestStartRules::NOT_FROM_FRIEND: DN046_ChangeLocation, Inst307ChangeLocation, ...).
		bool StartsFromFriend(const RE::TESQuest* a_quest)
		{
			if (!Encounters::IsStoryEvent(a_quest->eventID) || RadiantSync::IsRadiant(a_quest) || PerGame(a_quest) || Spawns::IsEncounter(a_quest) ||
				EncounterRuns::Shares(a_quest->GetFormID())) {
				return false;
			}
			if (const auto file = FileOf(a_quest); file && QuestStartRules::NotFromFriend(static_cast<std::uint8_t>(*file), a_quest->GetFormID() & 0x00FFFFFF)) {
				return false;
			}
			if (const auto file = a_quest->GetFile(0); file && AttackBook::Find(file->GetFilename(), a_quest->GetFormID())) {
				return false;
			}
			std::vector<QuestStartRules::AliasFill> aliases;
			for (const auto alias : a_quest->aliases) {
				if (alias) {
					aliases.push_back({ alias->fillType == QuestStartRules::FILL_FROM_EVENT, alias->flags.any(RE::BGSBaseAlias::FLAGS::kOptional) });
				}
			}
			return QuestStartRules::StartsFromFriend(QuestStartRules::Facts{ .flags = a_quest->data.flags }, true, aliases);
		}

		// How many polls such a start waits for a stop of the quest still under way (QuestStart::Judge's kStopping or kFilled)
		// before the stage is set as before: the story teller's shutdown takes a frame or two. Counted in polls the queue sets
		// stages in, so in play (none while paused or loading): 20 of POLL_INTERVAL, 10 s.
		constexpr int EVENT_START_POLLS = 20;

		// Quests a session can share: the shared types, the factions' finales and the dialogue quests of the
		// game's own files. Quests from other mods are never shared (see Official.h).
		// The radiants aren't: one is shared by its run, not by its quest (RadiantSync), and nor are the side quests each
		// game fills its own way (PerGame).
		bool Shareable(const RE::TESQuest* a_quest)
		{
			return Official::IsOfficialForm(a_quest) && Watched(a_quest) && !RadiantSync::IsRadiant(a_quest) && !PerGame(a_quest);
		}

		// Quests this session shares now: the story's only if the host chose a shared story, the safe ones only if they chose
		// those too (sQuests = safe). A radiant's while its run here is shared (RadiantSync).
		bool Shared(const RE::TESQuest* a_quest)
		{
			return (Shareable(a_quest) && (Story::Shared() || !IsStory(a_quest)) && (SafeShared() || !IsSafe(a_quest))) || RadiantSync::Shares(a_quest->GetFormID()) ||
			       EncounterRuns::Shares(a_quest->GetFormID());
		}

		const StageRules::Rule* RuleFor(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto file = FileOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest));
			return file ? StageRules::Find(*file, a_quest & 0x00FFFFFF, a_stage) : nullptr;
		}

		// A stage of the opening (Vault 111; StageRules::IsOpening): every player's own, never shared.
		bool IsOpeningStage(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file && StageRules::IsOpening(*file, a_quest->GetFormID() & 0x00FFFFFF, a_stage);
		}

		// A developer's quickstart (StageRules::IsDebug: only a console's setstage sets one): never shared, neither reported
		// nor set from a friend's. It jumps the player through the quest and runs what play would have by then, a faction's
		// kick-out included.
		bool IsDebugStage(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file && StageRules::IsDebug(*file, a_quest->GetFormID() & 0x00FFFFFF, a_stage);
		}

		// Whether a quickstart's stage (its file, quest and stage) is done in this game: StageRules::SetByQuickstart's a_done.
		bool QuickstartDone(StageRules::File a_by, std::uint32_t a_byQuest, std::uint16_t a_byStage)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			const auto by = handler ? handler->LookupForm<RE::TESQuest>(a_byQuest, StageRules::FileName(a_by)) : nullptr;
			return by && QuestStageLayout::IsStageDone(by, a_byStage);
		}

		// A stage that a developer's quickstart done in this game sets (StageRules::QUICKSTART_SETS: its script, the stages it
		// sets, the quests it starts and the shutdown stages of those it stops, as far down as the scan follows them). A console quickstart is the typing player's own,
		// and so is what it did: such a stage is not reported, live or in the host's baseline, while the quickstart is done in
		// this game (the save keeps that, so a later session from it leaves the stage out as well). Whichever of a quickstart's
		// stages the poll sees, the quickstart reads done by then: a stage reads done as it is set, before its script runs
		// and sets the others (StageLog.h). The quickstart's quest is looked up only for a stage that has a row (a binary
		// search of the table first), so the poll's other stages cost no LookupForm.
		bool SetByQuickstart(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file && StageRules::SetByQuickstart(*file, a_quest->GetFormID() & 0x00FFFFFF, a_stage, QuickstartDone);
		}

		// Whether a stage of this quest goes on from a lock stage set here (MQ206's 1000, 1050 and 1200
		// after its 500): without a combat hold, since the player can't fight anyway, and without
		// another warning, since they were warned before the lock.
		bool Unlocking(std::uint32_t a_quest)
		{
			return locked && (a_quest == locked->quest || a_quest == locked->releaseQuest);
		}

		const char* WhyWaiting(StageLog::Wait a_wait)
		{
			switch (a_wait) {
			case StageLog::Wait::kLoading:
				return "the game is loading";
			case StageLog::Wait::kOpening:
				return "the player is still in the opening";
			case StageLog::Wait::kNotRunning:
				return "the quest isn't running here";
			case StageLog::Wait::kNotStarted:
				return "the player hasn't started the quest (no objective shown)";
			case StageLog::Wait::kBusy:
				return "the player is in a conversation or a scene";
			case StageLog::Wait::kStopped:
				return "the quest ran here before and was stopped; starting it again would start it over";
			case StageLog::Wait::kStart:
				return "the quest isn't started here; they start it for the session once the stages that came before them are set";
			default:
				return "";
			}
		}

		// The quest ran here before and was stopped (it isn't running now): stages done, or past its first.
		bool RanBefore(const RE::TESQuest* a_quest)
		{
			return (a_quest->data.flags & QUEST_COMPLETED) || a_quest->currentStage > 0 || !QuestStageLayout::Done(a_quest).empty();
		}

		// Whether stages from other players can be set on this quest now (see StageLog::WaitsFor). A quest
		// this player hasn't started is started for the session by the first of them (Quest::kStart), after
		// the stages that came before it: another quest's stage may start it (MQ104's 210 starts MQ105). A
		// lock's quests go on after the session, when the story is no longer shared (StageLog::Settable).
		// Why a quest's stages wait is logged once for each reason.
		StageLog::Quest StateOf(std::uint32_t a_quest)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || !StageLog::Settable(Shared(quest), Unlocking(a_quest)) || StageLog::Finished(quest->data.flags)) {
				return StageLog::Quest::kGone;
			}
			// A dialogue quest that ran here and stopped is over (its 100 stops Diamond City's entrance scene,
			// the gate open): starting it again would play it over, so what is left of it is dropped, not
			// kept waiting. A silent quest's too (a place's quest that ran its course).
			if (Unordered(quest) && !(quest->data.flags & QUEST_RUNNING) && RanBefore(quest)) {
				if (const auto stages = waiting.Waiting(a_quest); !stages.empty()) {
					REX::INFO("QuestSync: {} stage(s) of {:08X} from friends dropped (the first {}): the scene ran here and is over", stages.size(), a_quest, stages.front());
				}
				return StageLog::Quest::kGone;
			}
			StageLog::QuestHere here;
			// Quest scripts must not run at the main menu or during a loading screen.
			here.inWorld = WorldSync::InWorld();
			here.opening = Story::InOpening();
			here.running = (quest->data.flags & QUEST_RUNNING) != 0;
			here.started = SessionStarted(quest);
			here.busy = busy;
			here.inSession = inSession && Shared(quest);
			here.ranBefore = !here.running && RanBefore(quest);
			here.startedHere = startedHere.contains(a_quest) || RadiantSync::Shares(a_quest) || EncounterRuns::Shares(a_quest);  // a shared run: started here for the session
			const auto wait = StageLog::WaitsFor(here);
			if (wait == StageLog::Wait::kNone) {
				waitLogged.erase(a_quest);
				return StageLog::Quest::kReady;
			}
			if (wait != StageLog::Wait::kBusy || !here.running || !(here.started || here.startedHere)) {
				waited.insert(a_quest);
			}
			if (wait != StageLog::Wait::kLoading && waitLogged[a_quest] != wait) {
				waitLogged[a_quest] = wait;
				const char* name = quest->GetFullName();
				const auto  stages = waiting.Waiting(a_quest);
				REX::INFO("QuestSync: {} stage(s) of {:08X} '{}' from friends wait (the first {}): {}", stages.size(), a_quest, name ? name : "",
					stages.empty() ? 0 : stages.front(), WhyWaiting(wait));
			}
			return wait == StageLog::Wait::kStart ? StageLog::Quest::kStart : StageLog::Quest::kWait;
		}

		// A stage this quest goes on to after it went back in this game (StageRules::GOES_BACK): a stage done here, the
		// Detective Case Files' 99, set a lower one by its script, its 5, and the quest's next steps are numbered from
		// there, the next case's 10 to 60. The quest's current stage says nothing about those. Only the stages that follow
		// (the table's rows): the other case's 65 to 98, done before 99, and a branch to the higher stage this game didn't
		// take (MS09's 1260 in a game that did 1275) are still passed, live or caught up on. So is a lower stage that play
		// sets after a higher one done here (StageRules::SETS_AFTER: MQ204's 5, which Amari's wake-up scene sets after
		// its 10, enabling Virgil), until the row's last stage is done here.
		bool WentBack(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			if (!file) {
				return false;
			}
			const auto id = a_quest->GetFormID() & 0x00FFFFFF;
			const auto done = [&](std::uint16_t a_other) { return QuestStageLayout::IsStageDone(a_quest, a_other); };
			return StageRules::WentBack(*file, id, a_stage, done) || StageRules::SetAfterHigher(*file, id, a_stage, done);
		}

		// Stages only move forward: never undo progress a player already has. A stage this quest is
		// already past here is left alone, done or not (it may be a branch this player didn't take).
		// Except a dialogue quest's: its stages aren't steps in order (a player at Diamond City's gate can
		// have the entrance scene's 400 without the 32, 40 and 100 that open the gate and end it), so only
		// a stage done here is. Nor a silent quest's (Unordered), nor a stage the quest goes on to after it went
		// back here (WentBack): before that rule a friend's first case of the Detective Case Files after the office
		// visit was dropped as passed.
		bool IsSettled(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest) {
				return true;
			}
			if (Unordered(quest) || WentBack(quest, a_stage)) {
				return QuestStageLayout::IsStageDone(quest, a_stage);
			}
			return a_stage <= quest->currentStage || QuestStageLayout::IsStageDone(quest, a_stage);
		}

		void NoteCaughtUp(const char* a_name, Clock::time_point a_now)
		{
			if (!a_name || !*a_name) {
				return;
			}
			if (caughtUpNames.empty()) {
				caughtUpFirst = a_now;
			}
			if (std::ranges::find(caughtUpNames, std::string_view(a_name)) == caughtUpNames.end()) {
				caughtUpNames.emplace_back(a_name);
			}
			caughtUpLast = a_now;
		}

		// One notice for the quests caught up with lately (a late joiner catches up dozens).
		void FlushCaughtUp(Clock::time_point a_now)
		{
			if (caughtUpNames.empty() || (a_now - caughtUpLast < NOTICE_QUIET && a_now - caughtUpFirst < NOTICE_EVERY)) {
				return;
			}
			std::string names;
			const auto  shown = (std::min)(caughtUpNames.size(), NOTICE_NAMES);
			for (std::size_t i = 0; i < shown; ++i) {
				names += (i > 0 ? ", " : "") + caughtUpNames[i];
			}
			if (caughtUpNames.size() > shown) {
				names += " " + Text::Format("quest.and_more", { { "n", caughtUpNames.size() - shown } });
			}
			Hud::Notify(Text::Format("quest.caught_up", { { "names", names } }));
			caughtUpNames.clear();
		}

		// A friend's purchase being set here with its price at 0 (StageRules::Price): the global's own value, put back once
		// the script that sells it has given what was bought, after PRICE_TIMEOUT of play, at a save (for the save), when a
		// load begins and when the main menu opens. The rules are StageRules::PriceWaiver's; this does them to the global.
		StageRules::PriceWaiver waiver;

		std::uint32_t PlayerHas(std::uint32_t a_item)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto item = RE::TESForm::GetFormByID(a_item);
			std::uint32_t n = 0;
			if (player && item) {
				player->GetItemCount(n, item, false);
			}
			return n;
		}

		void SetGlobal(std::uint32_t a_global, float a_value)
		{
			if (const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(a_global)) {
				global->value = a_value;
			}
		}

		// Before a friend's purchase is set here: its price to 0 while the script that takes it runs, so this player gets what
		// was bought without paying for it. False if it can't be (the stage is then not set: a friend's payment is never made
		// here). One already made here is set as it is.
		bool WaivePrice(const StageRules::Price& a_price, std::uint32_t a_quest, std::uint16_t a_stage, Clock::time_point a_now)
		{
			const auto file = a_quest & 0xFF000000;
			const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(file | a_price.global);
			const auto bought = RE::TESForm::GetFormByID<RE::TESQuest>(file | a_price.boughtQuest);
			if (!global || !bought) {
				REX::WARN("QuestSync: {:08X} stage {} is a friend's purchase, but its price {:08X} or its quest {:08X} isn't in this game", a_quest, a_stage,
					file | a_price.global, file | a_price.boughtQuest);
				return false;
			}
			if (QuestStageLayout::IsStageDone(bought, a_price.boughtStage)) {
				REX::INFO("QuestSync: {:08X} stage {} is a friend's purchase this player has made already ({:08X} stage {} done); set as it is", a_quest, a_stage,
					bought->GetFormID(), a_price.boughtStage);
				return true;
			}
			// A global already waived keeps the value from before (the one now may be the 0 of the waiver), and one purchase
			// at a time: another price would be zeroed and never put back.
			const bool again = waiver.On() && waiver.Current()->global == global->GetFormID();
			StageRules::PriceWaiver::Waived waived;
			waived.global = global->GetFormID();
			waived.value = global->value;
			waived.given = file | a_price.given;
			waived.givenBefore = PlayerHas(waived.given);
			waived.quest = a_quest;
			waived.stage = a_stage;
			waived.mainMenus = OpenMenus::MainMenus();
			if (!waiver.Begin(waived, a_now)) {
				REX::WARN("QuestSync: {:08X} stage {} is a friend's purchase, but the price {:08X} of another ({:08X} stage {}) is waived now", a_quest, a_stage,
					waived.global, waiver.Current()->quest, waiver.Current()->stage);
				return false;
			}
			global->value = 0.0f;
			++waivedPrices;
			REX::INFO("QuestSync: {:08X} stage {} is a friend's purchase: its price {:08X} ({}) is 0 here until {:08X} is given (the friend paid){}", a_quest, a_stage,
				waived.global, waiver.Current()->value, waived.given, again ? " (the waiver goes on)" : "");
			return true;
		}

		// The waived price back once what was bought is given, after PRICE_TIMEOUT of play (time paused or loading doesn't
		// count: no script runs then), or when a main menu has opened.
		void CheckWaived(Clock::time_point a_now, bool a_paused)
		{
			if (!waiver.On()) {
				return;
			}
			const auto& current = *waiver.Current();
			const auto  polled = waiver.Poll(a_now, a_paused, PlayerHas(current.given) > current.givenBefore, OpenMenus::MainMenus());
			const auto& waived = polled.waived;
			if (polled.what == StageRules::PriceWaiver::Do::kHold) {
				SetGlobal(waived.global, 0.0f);  // again after a save, which put its own value back (BeforeSaveOrLoad)
				return;
			}
			SetGlobal(waived.global, waived.value);
			switch (polled.why) {
			case StageRules::PriceWaiver::Why::kGiven:
				REX::INFO("QuestSync: {:08X} given for the friend's purchase ({:08X} stage {}); its price {:08X} is {} again", waived.given, waived.quest, waived.stage,
					waived.global, waived.value);
				break;
			case StageRules::PriceWaiver::Why::kMainMenu:
				REX::WARN("QuestSync: the main menu opened with the friend's purchase ({:08X} stage {}) waived; its price {:08X} is {} again", waived.quest, waived.stage,
					waived.global, waived.value);
				break;
			default:
				REX::WARN("QuestSync: {:08X} not given {} s after the friend's purchase ({:08X} stage {}) was set; its price {:08X} is {} again", waived.given,
					std::chrono::duration_cast<std::chrono::seconds>(StageRules::PRICE_TIMEOUT).count(), waived.quest, waived.stage, waived.global, waived.value);
				break;
			}
		}

		// A waiver over because a load began, or a world was loaded or begun: the price back (before anything resets).
		void EndWaived(const char* a_when)
		{
			if (const auto waived = waiver.End()) {
				SetGlobal(waived->global, waived->value);
				REX::INFO("QuestSync: {}, the price {:08X} of the friend's purchase ({:08X} stage {}) is {} again and no longer waived", a_when, waived->global, waived->quest,
					waived->stage, waived->value);
			}
		}

		// The globals a stage of ours carries to the others (StageRules::CARRIES): their values here now, read when the
		// stage is reported (live: as the script that read them saw them; the baseline: as they are when the session starts).
		// One the game lacks leaves the stage bare, and the friends' games then don't set it (StageRules::PlanCarry).
		Protocol::QuestStage WithCarried(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto id = a_quest->GetFormID();
			Protocol::QuestStage msg{ id, a_stage };
			const auto file = FileOf(a_quest);
			const auto row = file ? StageRules::FindCarried(*file, id & 0x00FFFFFF, a_stage) : nullptr;
			if (!row) {
				return msg;
			}
			std::string listed;
			for (const auto local : StageRules::GlobalsOf(*row)) {
				const auto globalId = (id & 0xFF000000) | local;
				const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(globalId);
				if (!global) {
					REX::WARN("QuestSync: {:08X} stage {} carries {:08X}, which isn't in this game; reported without its globals (friends won't set it)", id, a_stage, globalId);
					msg.globals.clear();
					return msg;
				}
				// One that is no finite number would have friends' games refuse the stage itself: it goes without its globals, as
				// for one this game lacks.
				if (!Protocol::Finite(global->value)) {
					REX::WARN("QuestSync: {:08X} stage {} carries {:08X}, whose value is no number; reported without its globals (friends won't set it)", id, a_stage, globalId);
					msg.globals.clear();
					return msg;
				}
				msg.globals.push_back({ globalId, global->value });
				listed += std::format("{}{:08X}={}", listed.empty() ? "" : ", ", globalId, global->value);
			}
			++carriedSent;
			REX::INFO("QuestSync: {:08X} stage {} reported with its globals ({}): the friends' games write them before setting it", id, a_stage, listed);
			return msg;
		}

		// Whether the stage a row needs first (StageRules::Carried::needs*, in the quest's file) is done here.
		static bool NeedDone(const StageRules::Carried& a_row, std::uint32_t a_quest)
		{
			if (a_row.needsQuest == 0) {
				return true;
			}
			const auto needs = RE::TESForm::GetFormByID<RE::TESQuest>((a_quest & 0xFF000000) | a_row.needsQuest);
			return needs && QuestStageLayout::IsStageDone(needs, a_row.needsStage);
		}

		// Before a friend's stage is judged (SetHere): whether its row says it waits for now, the stage its quest needs first
		// (Home Sweet Home's end for Power Play's) not done here. Its quest waits, judged again later (logged once). Asked
		// before any follow offer: offered first, a far friend's 8 was taken up, the player teleported, and the stage then
		// waited with the offer gone, so it was skipped when the parks were finally handed out here.
		bool CarryWaits(std::uint32_t a_quest, std::uint16_t a_stage, std::optional<StageRules::File> a_file, std::uint32_t a_local, const char* a_name)
		{
			const auto row = a_file ? StageRules::FindCarried(*a_file, a_local, a_stage) : nullptr;
			if (!row || !StageRules::CarryWaits(*row, NeedDone(*row, a_quest))) {
				return false;
			}
			if (carryWaitLogged.insert(Key(a_quest, a_stage)).second) {
				REX::INFO("QuestSync: {:08X} '{}' stage {} carries globals; waiting for {:08X} stage {}, which isn't done here (the parks must be handed out here first)",
					a_quest, a_name ? a_name : "", a_stage, (a_quest & 0xFF000000) | row->needsQuest, row->needsStage);
			}
			return true;
		}

		// Before a friend's stage is judged (SetHere): whether its quest's memory runs here (StageRules::MEMORY_WAITS: this
		// player is going through it, from the lounger to getting up again). Its quest waits, judged again later (logged once).
		// A memory's quest not found here (its file missing) holds nothing.
		bool MemoryWaits(std::uint32_t a_quest, std::uint16_t a_stage, std::optional<StageRules::File> a_file, std::uint32_t a_local, const char* a_name)
		{
			const auto row = a_file ? StageRules::FindMemoryWait(*a_file, a_local) : nullptr;
			const auto handler = row ? RE::TESDataHandler::GetSingleton() : nullptr;
			const auto memory = handler ? handler->LookupForm<RE::TESQuest>(row->memory, StageRules::FileName(*a_file)) : nullptr;
			if (!memory || !(memory->data.flags & QUEST_RUNNING)) {
				return false;
			}
			if (memoryWaitLogged.insert(Key(a_quest, a_stage)).second) {
				REX::INFO("QuestSync: {:08X} '{}' stage {} waits: this player is going through its memory ({:08X} runs here); set once they are out", a_quest,
					a_name ? a_name : "", a_stage, memory->GetFormID());
			}
			return true;
		}

		// Before a friend's stage is set here: what its row says to write first (StageRules::PlanCarry), with the values that
		// came with it. kSet: written here and there; kWait: its quest waits (CarryWaits said so before the stage was judged;
		// logged there); kSkip: not set (logged).
		StageRules::Carry PlanCarried(std::uint32_t a_quest, std::uint16_t a_stage, std::optional<StageRules::File> a_file, std::uint32_t a_local, const char* a_name)
		{
			const auto row = a_file ? StageRules::FindCarried(*a_file, a_local, a_stage) : nullptr;
			const auto came = carriedValues.find(Key(a_quest, a_stage));
			const std::span<const StageRules::GlobalValue> values = came != carriedValues.end() ? std::span<const StageRules::GlobalValue>{ came->second } : std::span<const StageRules::GlobalValue>{};
			auto plan = StageRules::PlanCarry(row, values, a_quest, row && NeedDone(*row, a_quest));
			// A global to write that this game lacks: the stage can't be set as the chooser's.
			for (const auto& write : plan.writes) {
				if (!RE::TESForm::GetFormByID<RE::TESGlobal>(write.global)) {
					REX::WARN("QuestSync: {:08X} '{}' stage {} came with {:08X}, which isn't in this game", a_quest, a_name ? a_name : "", a_stage, write.global);
					plan.what = StageRules::Carry::Do::kSkip;
					plan.why = "a global it came with isn't in this game";
					plan.writes.clear();
					break;
				}
			}
			if (plan.dropped) {
				carriedDropped += static_cast<std::uint32_t>(plan.dropped);
				REX::INFO("QuestSync: {:08X} '{}' stage {} came with {} global(s) no row here names; not written", a_quest, a_name ? a_name : "", a_stage, plan.dropped);
			}
			switch (plan.what) {
			case StageRules::Carry::Do::kWait:
				break;
			case StageRules::Carry::Do::kSkip:
				++carriedSkipped;
				REX::WARN("QuestSync: not setting {:08X} '{}' stage {}: {} (set as it came, its script would decide by this game's own globals)", a_quest, a_name ? a_name : "",
					a_stage, plan.why);
				break;
			case StageRules::Carry::Do::kSet:
				break;
			}
			return plan;
		}

		// Sets a stage here as one from another player: through the console (it runs a moment later),
		// and not reported back.
		void NoteSetHere(RE::TESQuest* a_quest, std::uint16_t a_stage);
		void SetStageHere(RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto id = a_quest->GetFormID();
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", id, a_stage).c_str());
			FadeWatch::Arm(std::format("set {:08X} stage {}", id, a_stage));
			NoteSetHere(a_quest, a_stage);
		}

		// What setting a friend's stage here leaves behind, however it was set: known, not reported back (its scripts'
		// reactions neither), its scenes and menus the session's, counted.
		void NoteSetHere(RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto id = a_quest->GetFormID();
			knownStages[id] = std::max(a_quest->currentStage, a_stage);
			if (const auto known = doneStages.find(id); known != doneStages.end()) {
				StageLog::Insert(known->second.stages, a_stage);
				known->second.digest.reset();  // read again at the next look, whether it reads done by then or not
			}
			remoteStageAt[id] = Clock::now();
			StageChanged(id, Clock::now());  // its quest XP is ours, not kill XP
			sceneHold.StageSet(id, Clock::now());  // its script may start a scene with the player in it: the session's
			menuGuard.StageSet(Clock::now());  // ... or open a menu in front of them (CatchUpMenus)
			lastSetStage = Key(id, a_stage);
			++applied;
		}

		// A stage marked done here without its script, known as a friend's: not reported back as new, nor the quest's
		// completion that follows as ours (QuestRewards), its current stage moved on as SetStage leaves it. False if it isn't there.
		bool MarkFriendsStage(RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto id = a_quest->GetFormID();
			if (!QuestStageLayout::MarkDone(a_quest, a_stage)) {
				return false;
			}
			if (!Unordered(a_quest) && a_stage > a_quest->currentStage) {
				a_quest->currentStage = a_stage;
				a_quest->AddChange(RE::CHANGE_TYPES::kQuestStages);
			}
			if (const auto known = doneStages.find(id); known != doneStages.end()) {
				StageLog::Insert(known->second.stages, a_stage);
				known->second.digest.reset();
			}
			knownStages[id] = a_quest->currentStage;
			remoteStageAt[id] = Clock::now();
			friendMoved.insert(id);
			return true;
		}

		// A memory's way out (StageRules::SOLO) skipped in a game whose player isn't in it, where the friend's stages started
		// the memory's quest (StageRules::ClosesOutside): the stage marked done without its script (no lounger, no move, no
		// exit scene), its log entries in the journal, the objectives still shown completed and the quest completed, so it
		// ends here as it ends in the friend's game instead of staying in the Pip-Boy. Known as a friend's stage (not reported
		// back, nor its completion as ours), as a friend's turn-in is (CloseTurnIn). The door (the way in's first stage, the
		// lounger sat in: StageRules::ShutsDoor) is marked done first the same way, so this player's own lounger can't take them
		// into a memory whose way out is done; if it can't be, nothing is closed. Main thread only.
		// a_door: the door's quest when StageRules::ShutsDoor says to mark a_doorStage of it, null otherwise.
		void CloseMemoryHere(RE::TESQuest* a_quest, std::uint16_t a_stage, RE::TESQuest* a_door, std::uint16_t a_doorStage, const char* a_name, bool a_live)
		{
			const auto id = a_quest->GetFormID();
			if (a_door && !MarkFriendsStage(a_door, a_doorStage)) {
				REX::WARN("QuestSync: {:08X} '{}' stage {} (a memory's way out) closes nothing here: the door {:08X} stage {} could not be marked done, and its lounger "
						  "would take the player into a memory with no way out; the quest is left running (setstage {:08X} {} brings a player in it out)",
					id, a_name ? a_name : "", a_stage, a_door->GetFormID(), a_doorStage, id, a_stage);
				return;
			}
			if (!MarkFriendsStage(a_quest, a_stage)) {
				REX::WARN("QuestSync: {:08X} '{}' stage {} (a memory's way out) could not be marked done here; the quest is left running{}", id, a_name ? a_name : "", a_stage,
					a_door ? std::format(" (the door {:08X} stage {} is marked done: the lounger takes nobody in)", a_door->GetFormID(), a_doorStage) : std::string{});
				return;
			}
			const auto journal = QuestStageLayout::LogEntries(a_quest, a_stage);
			std::vector<StageRules::ObjectiveState> states;
			for (const auto objective : a_quest->objectives) {
				if (objective) {
					states.push_back({ objective->index, static_cast<std::int8_t>(objective->state) });
				}
			}
			const auto open = StageRules::OpenObjectives(states);
			for (const auto index : open) {
				RE::Console::ExecuteCommand(std::format("setobjectivecompleted {:08X} {} 1", id, index).c_str());
			}
			const bool completes = !(a_quest->data.flags & QUEST_COMPLETED);
			if (completes) {
				RE::Console::ExecuteCommand(std::format("completequest {:08X}", id).c_str());
			}
			REX::INFO("QuestSync: {:08X} '{}' stage {}{} closes the memory's quest here, whose player isn't in it: marked done without its script ({} of its {} log entries "
					  "in the journal), {} objective(s) completed{}{}; the quest's flags {:04X}",
				id, a_name ? a_name : "", a_stage, a_live ? "" : " (catching up)", journal.logged, journal.entries, open.size(), completes ? ", the quest completed" : "",
				a_door ? std::format(", the door {:08X} stage {} marked done (the lounger takes nobody in)", a_door->GetFormID(), a_doorStage) : std::string{}, a_quest->data.flags);
		}

		// The companion of a companion's quest (StageRules::DRIVERS) in this game, if it is here.
		RE::Actor* CompanionOf(const RE::TESQuest* a_quest, const StageRules::Driver& a_driver)
		{
			return RE::TESForm::GetFormByID<RE::Actor>((a_quest->GetFormID() & 0xFF000000) | a_driver.companion);
		}

		bool InFaction(const RE::Actor* a_actor, std::uint32_t a_faction)
		{
			const auto faction = a_faction ? RE::TESForm::GetFormByID<RE::TESFaction>(a_faction) : nullptr;
			return a_actor && faction && a_actor->IsInFaction(faction);
		}

		// What this save shows of a companion quest's driver's mark (StageRules::Marked): the companion in the driver's
		// faction, or in the alias only the owner's stage's script fills.
		StageRules::MarkFacts MarkFactsOf(RE::TESQuest* a_quest, const StageRules::Driver& a_driver, const RE::Actor* a_companion)
		{
			StageRules::MarkFacts facts;
			if (!a_companion) {
				return facts;
			}
			facts.inFaction = a_driver.faction && InFaction(a_companion, (a_quest->GetFormID() & 0xFF000000) | a_driver.faction);
			if (a_driver.alias != StageRules::NO_ALIAS) {
				const auto held = a_quest->GetAliasedRef(static_cast<std::uint32_t>(a_driver.alias)).get();
				facts.inAlias = held && held.get() == a_companion;
			}
			return facts;
		}

		// Companions' quests one of whose owner's stages (kEach) ran with its script here since the last load: set here with
		// it (from a friend's), or done by this game's own play. Driven here whatever their save shows yet: the stage's
		// script may not have left its mark (Curie's 200 reads done before its DisallowCompanion has run), and the next
		// stage, judged as soon as it reads done, must not take the quest for a mirror (StageRules::DriveFacts::driven).
		std::unordered_set<std::uint32_t> drivenHere;

		// Whether this game drives a companion's quest (StageRules::DriveOf): its owner's stages are set here with their
		// scripts. The facts, for the log. The aliases must be readable (Companions::AliasesReadable).
		StageRules::Drive DriveHere(RE::TESQuest* a_quest, StageRules::File a_file, StageRules::DriveFacts& a_facts)
		{
			const auto driver = StageRules::DriverOf(a_file, a_quest->GetFormID() & 0x00FFFFFF);
			if (!driver) {
				return StageRules::Drive::kMirror;
			}
			const auto done = [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(a_quest, a_stage); };
			const auto companion = CompanionOf(a_quest, *driver);
			a_facts.marked = StageRules::Marked(*driver, done, MarkFactsOf(a_quest, *driver, companion));
			a_facts.driven = drivenHere.contains(a_quest->GetFormID());
			a_facts.mirrored = StageRules::MirroredSoFar(a_file, a_quest->GetFormID() & 0x00FFFFFF, done);
			a_facts.ours = companion && Companions::Ours(companion->GetFormID());
			return StageRules::DriveOf(a_facts);
		}

		// Whether her body changes here (StageRules::BodyOf).
		StageRules::Body BodyHere(RE::TESQuest* a_quest, StageRules::File a_file)
		{
			const auto driver = StageRules::DriverOf(a_file, a_quest->GetFormID() & 0x00FFFFFF);
			const auto companion = driver ? CompanionOf(a_quest, *driver) : nullptr;
			StageRules::BodyFacts facts;
			facts.actor = companion != nullptr;
			if (companion) {
				const auto plugin = a_quest->GetFormID() & 0xFF000000;
				const auto freedBy = driver->metQuest ? RE::TESForm::GetFormByID<RE::TESQuest>(plugin | driver->metQuest) : nullptr;
				facts.met = !driver->metQuest || (freedBy && QuestStageLayout::IsStageDone(freedBy, driver->metStage)) ||
				            InFaction(companion, StageRules::HAS_BEEN_COMPANION_FACTION);
				facts.follows = Companions::Follows(companion->GetFormID());
			}
			return StageRules::BodyOf(facts);
		}

		constexpr const char* BodyText(StageRules::Body a_body)
		{
			switch (a_body) {
			case StageRules::Body::kChange:
				return "changes";
			case StageRules::Body::kUnmet:
				return "left as it is: she was never freed in this world";
			case StageRules::Body::kFollows:
				return "left as it is: she is our player's companion";
			default:
				return "left as it is: no such actor here";
			}
		}

		// A new body under way: the reset (StageRules::NewBodyReset) once the removal of her items has taken (ResetDue), then
		// the second half (NewBodyAfter) BODY_WAIT after the reset, then noted in saves for BodySave::KEPT_AFTER more. Its
		// waits count play (StageRules::PlayedIn): none of its commands lands in a loading screen or under a menu. A load
		// drops it; a save made while it is under way notes it in its co-save (bodyShared), and the game that loads that
		// save carries it on from the step the save was made in (ResumeNewBody).
		struct NewBody
		{
			BodySave::Pending change;               // what is done to whom, and the step it is in
			std::uint32_t     stacksAtRemoval = 0;  // the stacks of items she held as the removal was sent
			std::uint32_t     lastStacks = 0;       // ... at the poll before this one
			Clock::time_point removedAt{};          // for the log
			Clock::time_point last{};               // the last poll's time
			Clock::duration   played{};             // play since the step's commands were sent
			BodySave::Left    leftSaid = BodySave::Left::kNone;  // why no save notes it, as last logged
		};
		std::optional<NewBody>           newBody;
		BodySave::Shared                 bodyShared;  // what a save made now notes of it, and a loaded save's note (their threads' too)
		std::optional<BodySave::Pending> resumeBody;  // a loaded save's note, until the loaded game has settled
		std::uint32_t                    newBodies = 0, bodiesResumed = 0;

		// How many stacks of items a_ref holds (none without an inventory list of its own).
		std::uint32_t StacksOf(RE::TESObjectREFR* a_ref)
		{
			const auto list = a_ref ? a_ref->inventoryList : nullptr;
			if (!list) {
				return 0;
			}
			std::uint32_t            stacks = 0;
			const RE::BSAutoReadLock l{ list->rwLock };
			for (const auto& item : list->data) {
				for (auto stack = item.object ? item.stackData.get() : nullptr; stack; stack = stack->nextStack.get()) {
					++stacks;
				}
			}
			return stacks;
		}

		// A save made from now on notes the new body under way as it is now. One whose record can't be written
		// (BodySave::LeftOut: a form made at runtime) is noted by no save, and a load of such a save finds her as it has
		// her, with nothing to carry on: said once per reason, here, since the save's thread says nothing.
		void NoteBody()
		{
			bodyShared.Set(newBody->change);
			const auto left = BodySave::LeftOut(newBody->change);
			if (left != BodySave::Left::kNone && left != newBody->leftSaid) {
				REX::WARN("QuestSync: a save made now notes nothing of {:08X}'s new body under way (step {}): {} is a form made at runtime{}. A load of that save has her "
						  "as it was made, the change not carried on",
					newBody->change.ref, static_cast<int>(newBody->change.phase), left == BodySave::Left::kCompanion ? "she" : "the body that takes her items",
					left == BodySave::Left::kCompanion ? "" : std::format(" ({:08X}; a save notes it from her reset on)", newBody->change.items));
			}
			newBody->leftSaid = left;
		}

		// The new body's next step begins: its commands were just sent. Play is counted from here, and a save made from now
		// on notes this step.
		void BodyStep(BodySave::Phase a_phase, Clock::time_point a_now)
		{
			newBody->change.phase = a_phase;
			newBody->played = Clock::duration::zero();
			newBody->last = a_now;
			NoteBody();
		}

		// No new body is under way (any more): no save notes one.
		void DropNewBody()
		{
			newBody.reset();
			bodyShared.Set(std::nullopt);
		}

		// The reset, once the removal has taken (StageRules::ResetDue), or at once with a_force (another new body starts). A
		// load does not force it: it drops the change (Rebaseline), and the loaded save's own note decides (ResumeNewBody).
		void ResetNewBody(Clock::time_point a_now, bool a_force)
		{
			if (!newBody || newBody->change.phase != BodySave::Phase::kRemoval) {
				return;
			}
			const auto                   stacks = StacksOf(RE::TESForm::GetFormByID<RE::TESObjectREFR>(newBody->change.ref));
			const StageRules::ResetFacts facts{ .stacks = stacks, .atRemoval = newBody->stacksAtRemoval, .last = newBody->lastStacks, .played = newBody->played };
			newBody->lastStacks = stacks;
			if (!a_force && !StageRules::ResetDue(facts)) {
				return;
			}
			RE::Console::ExecuteCommand(StageRules::NewBodyReset(newBody->change.ref, newBody->change.destination).c_str());
			REX::INFO("QuestSync: {:08X} reset for her new body after {} ms of play ({} ms on the clock) since her items were moved out ({} of {} stacks still read{})",
				newBody->change.ref, std::chrono::duration_cast<std::chrono::milliseconds>(newBody->played).count(),
				std::chrono::duration_cast<std::chrono::milliseconds>(a_now - newBody->removedAt).count(), stacks, newBody->stacksAtRemoval, a_force ? ", forced" : "");
			BodyStep(BodySave::Phase::kReset, a_now);
		}

		// The second half's commands (StageRules::NewBodyAfter).
		void DressNewBody(const BodySave::Pending& a_change)
		{
			const auto lines = StageRules::NewBodyAfter(a_change.ref, a_change.outfit, a_change.affinity, a_change.factions);
			for (const auto& line : lines) {
				RE::Console::ExecuteCommand(line.c_str());
			}
			REX::INFO("QuestSync: {:08X}'s new body dressed: outfit {:08X}, affinity {}, {} faction changes put back", a_change.ref, a_change.outfit,
				a_change.affinity ? std::format("{}", *a_change.affinity) : std::string{ "not read" }, a_change.factions.size());
		}

		// Each in-world poll: the play since the last one counted (none while a_paused), then whatever step is due. With
		// a_force every step left is made at once (another new body starts).
		void FinishNewBody(Clock::time_point a_now, bool a_paused, bool a_force = false)
		{
			if (!newBody) {
				return;
			}
			newBody->played += StageRules::PlayedIn(a_now - newBody->last, a_paused, 2 * POLL_INTERVAL);
			newBody->last = a_now;
			ResetNewBody(a_now, a_force);
			if (newBody->change.phase == BodySave::Phase::kReset && (a_force || newBody->played >= StageRules::BODY_WAIT)) {
				DressNewBody(newBody->change);
				BodyStep(BodySave::Phase::kDressed, a_now);
			}
			if (newBody->change.phase == BodySave::Phase::kDressed && (a_force || newBody->played >= BodySave::KEPT_AFTER)) {
				DropNewBody();
			}
		}

		void RenewRoll(std::uint32_t a_ref);

		// A loaded save's note of a new body under way as it was made (BodySave), once the loaded game has settled: the step
		// the note names is made again (its command may not have landed in the save; each is safe to make twice) and the rest
		// follows as in the game that began it. Dropped if one began here since the load, or her actor is not in this game.
		void ResumeNewBody(Clock::time_point a_now)
		{
			if (!resumeBody || !Companions::AliasesReadable()) {
				return;
			}
			const auto saved = std::move(*resumeBody);
			resumeBody.reset();
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(saved.ref);
			const auto items = saved.items ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(saved.items) : nullptr;
			const auto removal = saved.phase == BodySave::Phase::kRemoval ? StageRules::NewBodyRemove(saved.ref, items ? items->GetFormID() : 0u) : std::string{};
			if (newBody || !actor || actor->IsDeleted() || (saved.phase == BodySave::Phase::kRemoval && removal.empty())) {
				REX::WARN("QuestSync: the loaded save notes a new body under way for {:08X}, not carried on here ({})", saved.ref,
					newBody ? "another began since the load" : !actor || actor->IsDeleted() ? "no such actor in this game" : "the body that takes her items is not in this game");
				if (!newBody) {
					bodyShared.Set(std::nullopt);
				}
				return;
			}
			RenewRoll(saved.ref);
			NewBody body;
			body.change = saved;
			body.removedAt = a_now;
			body.stacksAtRemoval = StacksOf(actor);
			body.lastStacks = body.stacksAtRemoval;
			newBody = std::move(body);
			const char* from = "her dressing";
			if (saved.phase == BodySave::Phase::kRemoval) {
				RE::Console::ExecuteCommand(removal.c_str());
				from = "the removal of her items";
			} else if (saved.phase == BodySave::Phase::kReset) {
				RE::Console::ExecuteCommand(StageRules::NewBodyReset(saved.ref, saved.destination).c_str());
				from = "her reset";
			} else {
				DressNewBody(saved);
			}
			BodyStep(saved.phase, a_now);
			++bodiesResumed;
			REX::INFO("QuestSync: {:08X}'s new body was under way as the loaded save was made: carried on from {}, made again here ({} stacks read on her)", saved.ref, from,
				newBody->stacksAtRemoval);
		}

		// A companion given a new body without dying (Curie's 300 resets her, and the engine builds her again from her leveled
		// list, which her quest's globals now roll as the synth): the rolls kept of her go first, so nothing forces her old body
		// back (RollHook keeps none of her: SharedRoll::RolledByGlobals; NpcSync's report of her old body is forgotten). The
		// game that runs her reports her new body as it changes (NpcSync's SendOwnedRolls).
		void RenewRoll(std::uint32_t a_ref)
		{
			RollHook::Renew(a_ref);
			NpcSync::ForgetRoll(a_ref);
			REX::INFO("QuestSync: {:08X} gets a new body: the rolls kept of her old one are forgotten", a_ref);
		}

		// RenewRoll for the actor of each of a_stage's kNewBody acts, before (or as) the stage's own script gives her the body.
		void RenewBodies(RE::TESQuest* a_quest, StageRules::File a_file, std::uint16_t a_stage)
		{
			for (const auto& each : StageRules::EachOf(a_file, a_quest->GetFormID() & 0x00FFFFFF, a_stage)) {
				if (each.act != StageRules::Act::kNewBody || each.alias < 0) {
					continue;
				}
				if (const auto ref = a_quest->GetAliasedRef(static_cast<std::uint32_t>(each.alias)).get()) {
					RenewRoll(ref->GetFormID());
				}
			}
		}

		// The new body's first half, now: what the reset clears read first (her affinity, her factions), her items into the
		// body she leaves, the rolls kept of her old body forgotten. The reset follows on a later poll, once the removal has
		// taken (ResetNewBody). False if it can't be done (no body to take her items).
		bool StartNewBody(RE::Actor* a_actor, RE::TESObjectREFR* a_items, RE::TESObjectREFR* a_destination, std::uint32_t a_outfit, Clock::time_point a_now)
		{
			const auto removal = StageRules::NewBodyRemove(a_actor->GetFormID(), a_items ? a_items->GetFormID() : 0u);
			if (removal.empty()) {
				return false;
			}
			RenewRoll(a_actor->GetFormID());
			NewBody body;
			body.change.ref = a_actor->GetFormID();
			body.change.items = a_items->GetFormID();
			body.change.destination = a_destination ? a_destination->GetFormID() : 0u;
			body.change.outfit = a_outfit;
			body.change.phase = BodySave::Phase::kRemoval;
			body.stacksAtRemoval = StacksOf(a_actor);  // read before the removal: fewer, later, says it has taken
			body.lastStacks = body.stacksAtRemoval;
			body.removedAt = a_now;
			body.last = a_now;
			if (const auto info = RE::TESForm::GetFormByID<RE::ActorValueInfo>(StageRules::CA_AFFINITY)) {
				body.change.affinity = static_cast<RE::ActorValueOwner&>(*a_actor).GetActorValue(*info);
			}
			if (const auto changes = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraFactionChanges>() : nullptr) {
				for (const auto& change : changes->factionChanges) {
					if (change.faction) {
						body.change.factions.push_back({ change.faction->GetFormID(), change.rank });
					}
				}
			}
			if (newBody) {
				FinishNewBody(a_now, false, true);  // one before it, done now
			}
			RE::Console::ExecuteCommand(removal.c_str());
			newBody = std::move(body);
			NoteBody();
			++newBodies;
			return true;
		}

		// Companions' quests this game's own play has just taken over from a mirror (StageRules::ALIAS_MOVES: the owner
		// gone, a friend carries Curie's quest on at Amari's), whose owner's alias moves are still to be made here. Judged
		// and made at the first poll they can be (the aliases readable, no new body of hers under way), tried again for
		// ADOPT_TRIES polls if the engine takes no force then, dropped at a load (the loaded save's play decides again).
		struct Adoption
		{
			StageRules::File                   file{};
			std::vector<StageRules::AliasMove> moves;
			bool                               judged = false;
			int                                tries = 0;
		};
		constexpr int                               ADOPT_TRIES = 20;  // 10 s of polls
		std::unordered_map<std::uint32_t, Adoption> adoptions;
		std::uint32_t                               adopted = 0;

		void AdoptAliases()
		{
			if (adoptions.empty() || !Companions::AliasesReadable() || newBody || resumeBody) {
				return;
			}
			for (auto it = adoptions.begin(); it != adoptions.end();) {
				const auto id = it->first;
				auto&      adoption = it->second;
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				const auto driver = quest ? StageRules::DriverOf(adoption.file, id & 0x00FFFFFF) : nullptr;
				const auto companion = driver ? CompanionOf(quest, *driver) : nullptr;
				if (!companion || companion->IsDeleted()) {
					it = adoptions.erase(it);  // no companion here to move
					continue;
				}
				const char* fullName = quest->GetFullName();
				const char* name = fullName ? fullName : "";
				const auto  plugin = id & 0xFF000000;
				const auto  done = [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(quest, a_stage); };
				if (!adoption.judged) {
					StageRules::AdoptFacts facts;
					facts.marked = StageRules::Marked(*driver, done, MarkFactsOf(quest, *driver, companion));
					const auto mark = StageRules::NewBodyMarkOf(adoption.file, id & 0x00FFFFFF);
					const auto global = mark ? RE::TESForm::GetFormByID<RE::TESGlobal>(plugin | mark->global) : nullptr;
					facts.body = global && global->value == static_cast<float>(mark->value);
					facts.others = Companions::FollowsFriend(companion->GetFormID());
					if (!StageRules::Adopts(facts)) {
						if (!facts.marked) {
							REX::INFO("QuestSync: {:08X} '{}': this game's own play carries it on from a mirror, and its companion's aliases are left as they are: {}", id, name,
								!facts.body ? "her body did not change in this game" : "another player owns her");
						}
						it = adoptions.erase(it);
						continue;
					}
					adoption.judged = true;
				}
				std::string made, missing;
				for (const auto& move : adoption.moves) {
					if (move.until && done(move.until)) {
						continue;  // done meanwhile: what the move was for is over
					}
					const auto               alias = static_cast<std::uint32_t>(move.alias);
					// What the alias takes: the companion, or what another alias holds (kept alive while it is used).
					const auto               other = move.from == StageRules::NO_ALIAS ? RE::NiPointer<RE::TESObjectREFR>{} : quest->GetAliasedRef(static_cast<std::uint32_t>(move.from)).get();
					RE::TESObjectREFR* const source = move.from == StageRules::NO_ALIAS ? companion : other.get();
					const auto               holds = [&] {
						const auto held = quest->GetAliasedRef(alias).get();
						return held && held.get() == source;
					};
					if (source && !source->IsDeleted() && !holds()) {
						RadiantSync::ForceAliasRefNow(quest, alias, source->GetFormID());  // done as it returns, or not made (its guards)
					}
					if (source && holds()) {
						made += std::format("{}alias {} holds {:08X}", made.empty() ? "" : ", ", move.alias, source->GetFormID());
					} else {
						missing += std::format("{}alias {}", missing.empty() ? "" : ", ", move.alias);
					}
				}
				if (!missing.empty()) {
					if (++adoption.tries < ADOPT_TRIES) {
						++it;
						continue;  // again at the next poll
					}
					REX::WARN("QuestSync: {:08X} '{}': the owner's alias moves could not all be made here in {} polls ({} not moved; {}; quest flags {:04X}); its later stages' "
							  "scripts act on the aliases as they are",
						id, name, ADOPT_TRIES, missing, made.empty() ? std::string{ "none made" } : made, quest->data.flags);
					it = adoptions.erase(it);
					continue;
				}
				// What a stage already done here would have given the actor a move just put in its alias.
				std::size_t keywords = 0;
				for (const auto& row : StageRules::AdoptKeywords(adoption.file, id & 0x00FFFFFF, done)) {
					const auto held = quest->GetAliasedRef(static_cast<std::uint32_t>(row.alias)).get();
					const auto keyword = RE::TESForm::GetFormByID<RE::BGSKeyword>(plugin | row.keyword);
					if (held && !held->IsDeleted() && keyword && !static_cast<const RE::IKeywordFormBase&>(*held).HasKeyword(keyword)) {
						held->AddKeyword(keyword);
						++keywords;
					}
				}
				++adopted;
				REX::INFO("QuestSync: {:08X} '{}' adopted: the owner's alias moves made here as this game's own play carries the quest on from a mirror ({}; {} keyword(s) given); "
						  "this save now carries the driver's mark",
					id, name, made.empty() ? std::string{ "none left to make" } : made, keywords);
				it = adoptions.erase(it);
			}
		}

		// A companion quest's stage whose script is its owner's (StageRules kEach), set here from a friend's in a game that
		// mirrors the quest (DriveHere): the stage marked done without its script running (QuestStageLayout::MarkDone), so
		// the companion's recruit, affinity and perks stay the owner's, and its EACH acts done through the console (the
		// objectives, the world's half). Done once: the stage reads done at once, and StageLog never sets a done stage again.
		// The quest is running (SetHere starts it first). Returns how many acts could not be done.
		std::size_t SetEachHere(RE::TESQuest* a_quest, StageRules::File a_file, std::uint16_t a_stage, Clock::time_point a_now, std::string& a_body)
		{
			const auto id = a_quest->GetFormID();
			const auto plugin = id & 0xFF000000;
			const auto aliased = [&](std::int32_t a_alias) {
				return a_alias >= 0 ? a_quest->GetAliasedRef(static_cast<std::uint32_t>(a_alias)).get() : RE::NiPointer<RE::TESObjectREFR>{};
			};
			const auto acts = StageRules::EachOf(a_file, id & 0x00FFFFFF, a_stage);
			std::optional<StageRules::Body> body;
			if (std::ranges::any_of(acts, [](const StageRules::Each& a_each) { return a_each.body; })) {
				body = BodyHere(a_quest, a_file);
				a_body = std::format(", her body {}", BodyText(*body));
			}
			// The done bit, then the quest's current stage as TESQuest::SetStage leaves it (the companions' quests are ordered;
			// MarkDone leaves it), saved with the stages.
			const bool  fresh = QuestStageLayout::MarkDone(a_quest, a_stage);
			const bool  marked = fresh || QuestStageLayout::IsStageDone(a_quest, a_stage);
			std::size_t missed = marked ? 0 : 1;
			if (marked && a_stage > a_quest->currentStage) {
				a_quest->currentStage = a_stage;
				a_quest->AddChange(RE::CHANGE_TYPES::kQuestStages);
			}
			// Its log entries, which its script's SetStage would have written: in our player's journal as the game puts them
			// there (QuestStageLayout::LogEntries), so the Pip-Boy tells the quest's story here as it does in its owner's game.
			if (fresh) {
				const auto journal = QuestStageLayout::LogEntries(a_quest, a_stage);
				// The quest's flags as the journal leaves them, for the rig: the game's add completes nothing (an entry's Complete
				// Quest flag is TESQuestStageItem::Run's to act on, and Run is not called); the completion is EACH's act, below.
				REX::INFO("QuestSync: {:08X} stage {}: {} of its {} log entries in the journal ({} new); the quest's flags {:04X} after them", id, a_stage, journal.logged, journal.entries,
					journal.fresh, a_quest->data.flags);
			}
			for (const auto& each : acts) {
				using StageRules::Act;
				if (each.body && body != StageRules::Body::kChange) {
					continue;
				}
				if (each.act == Act::kGlobal) {
					if (const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(plugin | each.form)) {
						global->value = static_cast<float>(each.value);
					} else {
						++missed;
					}
					continue;
				}
				// The reference an act is on: its alias's here, or a form that is a reference (kEnable's).
				RE::NiPointer<RE::TESObjectREFR> ref = aliased(each.alias);
				if (!ref && each.alias < 0 && each.form && each.act == Act::kEnable) {
					ref.reset(RE::TESForm::GetFormByID<RE::TESObjectREFR>(plugin | each.form));
				}
				if (each.act == Act::kNewBody) {
					const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
					if (!actor || !StartNewBody(actor, aliased(each.items).get(), aliased(each.value).get(), each.form ? plugin | each.form : 0u, a_now)) {
						++missed;
					}
					continue;
				}
				if (each.act == Act::kKill && ref && ref->IsDisabled()) {
					continue;  // the body never came here (its 300 left it): nothing to kill
				}
				std::uint32_t form = each.form ? plugin | each.form : 0u;
				float         xp = 0.0f;
				if (each.act == Act::kKill || each.act == Act::kTake) {
					// Its base: the robot body's, made not essential first (the script's SetEssential); the item's, taken.
					const auto base = ref ? ref->GetObjectReference() : nullptr;
					form = base ? base->GetFormID() : 0u;
				} else if (each.act == Act::kXp) {
					if (const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(form)) {
						xp = global->value;
						StageChanged(id, Clock::now());  // the quest's XP, not kill XP to share
					}
				}
				const auto lines = StageRules::Commands(each, id, ref ? ref->GetFormID() : 0u, form, xp);
				if (lines.empty()) {
					++missed;
				}
				for (const auto& line : lines) {
					RE::Console::ExecuteCommand(line.c_str());
				}
			}
			NoteSetHere(a_quest, a_stage);
			++eachStages;
			return missed;
		}

		// A mirror's own count of the game-time timers an owner's stage's script starts (StageRules::MIRROR_TIMERS: Curie's
		// day from 200 to 300): the game's days passed at the first poll since the load that saw the quest mirrored at the
		// timer's stage, and which reason the log last gave for a timer that has run and waits (none once it waits for
		// nothing: a reason that comes back is said again). In every game, in a session or not (a friend playing that
		// save alone is as stuck), once the loaded game has settled and the aliases can be read.
		struct MirrorCount
		{
			float                  since = 0.0f;
			StageRules::MirrorHold said = StageRules::MirrorHold::kNone;
		};
		std::unordered_map<std::uint32_t, MirrorCount>       mirrorCounts;
		std::vector<std::pair<std::uint32_t, std::uint16_t>> mirrorSet;  // stages a mirror's timer set at this poll: reported below as this game's own
		std::uint32_t                                        mirrorTimers = 0;

		void MirrorTimers(Clock::time_point a_now)
		{
			const auto calendar = RE::Calendar::GetSingleton();
			if (!calendar || !calendar->gameDaysPassed || !reportFrom || a_now < *reportFrom || !Companions::AliasesReadable()) {
				return;
			}
			const float days = calendar->gameDaysPassed->value;
			const auto  handler = RE::TESDataHandler::GetSingleton();
			const auto  player = RE::PlayerCharacter::GetSingleton();
			for (const auto& row : StageRules::MIRROR_TIMERS) {
				const auto file = static_cast<StageRules::File>(row.file);
				const auto quest = handler ? handler->LookupForm<RE::TESQuest>(row.quest, StageRules::FileName(file)) : nullptr;
				if (!quest) {
					continue;
				}
				const auto id = quest->GetFormID();
				const auto driver = StageRules::DriverOf(file, row.quest);
				const auto companion = driver ? CompanionOf(quest, *driver) : nullptr;
				if (!companion || companion->IsDeleted()) {
					mirrorCounts.erase(id);
					continue;
				}
				StageRules::DriveFacts       drive;
				StageRules::MirrorTimerFacts facts;
				facts.mirror = DriveHere(quest, file, drive) == StageRules::Drive::kMirror && drive.mirrored;
				facts.running = (quest->data.flags & QUEST_RUNNING) != 0;
				facts.after = QuestStageLayout::IsStageDone(quest, row.after);
				facts.sets = QuestStageLayout::IsStageDone(quest, row.sets);
				if (!StageRules::MirrorCounts(facts)) {
					mirrorCounts.erase(id);
					continue;
				}
				auto& count = mirrorCounts.try_emplace(id, MirrorCount{ days }).first->second;
				if (days < count.since) {
					count.since = days;  // the clock was set back: counted again from here
				}
				const float counted = days - count.since;
				facts.others = Companions::FollowsFriend(companion->GetFormID());
				facts.follows = Companions::Follows(companion->GetFormID());
				facts.busy = newBody || resumeBody || Paused() || OpenMenus::Open(OpenMenus::Menu::kDialogue) || (player && player->IsInCombat());
				const char* fullName = quest->GetFullName();
				const char* name = fullName ? fullName : "";
				const auto hold = StageRules::MirrorHolds(row, facts, counted);
				if (hold == StageRules::MirrorHold::kNone) {
					count.said = hold;  // nothing holds it now (the owner left, she was dismissed): the same reason, back later, is said again
				} else if (hold != count.said) {
					count.said = hold;
					REX::INFO("QuestSync: {:08X} '{}' has had stage {} done here for {:.2f} game day(s) and no timer of its script's runs in a mirror; stage {} is not set by the mirror's "
							  "timer yet: {} (logged once each time this reason begins)",
						id, name, row.after, counted, row.sets,
						hold == StageRules::MirrorHold::kOthers ? "its companion is another player's, whose game's timer sets it" :
																  "its companion follows this game's player and would keep her old body; it is set once she is dismissed");
				}
				if (!StageRules::MirrorDue(row, facts, counted)) {
					continue;
				}
				const auto  was = quest->currentStage;
				std::string body;
				const auto  missed = SetEachHere(quest, file, row.sets, a_now, body);
				++mirrorTimers;
				mirrorCounts.erase(id);
				const bool shares = Config::Get().syncQuests;
				if (shares) {
					mirrorSet.emplace_back(id, row.sets);
				}
				REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}) without its script by the mirror's timer: {:.2f} game day(s) since stage {} was seen done here, and no "
						  "other player owns its companion ({} acts{}{}){}",
					id, name, row.sets, was, counted, row.after, StageRules::EachOf(file, row.quest, row.sets).size(), missed ? std::format(", {} could not be done", missed) : std::string{},
					body, shares ? "; reported as this game's own stage" : "");
			}
		}

		// Companions' quests whose startquest is being tried here (a kEach stage waits for it): when first tried and how
		// often, and the ones given up on (StageRules::GiveUpStart), whose kEach stages are left out until a load or the
		// session's end.
		struct StartTry
		{
			Clock::time_point first{};
			int               tries = 0;
		};
		std::unordered_map<std::uint32_t, StartTry> eachStarts;
		std::unordered_set<std::uint32_t>           eachStartsGaveUp;

		// Menus a friend's stage opened here by itself (a dismissed companion's new home, a featured item's inspection) are
		// closed again, so they don't hold the game and the catch-up paused until the player answers them (CatchUpMenus).
		// Every frame, paused or not: the menu is what pauses the game.
		void WatchMenus(Clock::time_point a_now)
		{
			using CatchUpMenus::Menu;
			using OpenMenus::Menu::kBarter;
			using OpenMenus::Menu::kConsole;
			using OpenMenus::Menu::kContainer;
			using OpenMenus::Menu::kCooking;
			using OpenMenus::Menu::kDialogue;
			using OpenMenus::Menu::kPause;
			using OpenMenus::Menu::kPipboy;
			using OpenMenus::Menu::kPowerArmorMod;
			using OpenMenus::Menu::kTerminal;
			using OpenMenus::Menu::kWorkshop;
			// What our player does that opens such a menu themselves: a conversation (dismissing a companion), workshop
			// mode (a supply line), the Pip-Boy (its inspect), a workbench or station, and any activation.
			if (OpenMenus::AnyOpen({ kDialogue, kWorkshop, kPipboy, kContainer, kBarter, kTerminal, kCooking, kPowerArmorMod, kConsole, kPause })) {
				menuGuard.PlayerActed(a_now);
			}
			if (const auto activated = playerActivatedAt.load(); activated != playerActivatedSeen) {
				playerActivatedSeen = activated;
				menuGuard.PlayerActed(Clock::time_point{ Clock::duration{ activated } });
			}
			// Not while the UI is being torn down and built again (a loading screen, a load): the menus are looked at,
			// and closed, once it is settled.
			if (!Hud::IsSettled()) {
				return;
			}
			MenuInput::Frame();  // a cancel's release
			// Each menu with its own state: the guard holds the vignette back while the inspection under it is up.
			const std::array<std::pair<Menu, bool>, 3> menus{ { { Menu::kCaravan, OpenMenus::Open(OpenMenus::Menu::kCaravan) },
				{ Menu::kExamine, OpenMenus::Open(OpenMenus::Menu::kExamine) }, { Menu::kVignette, OpenMenus::Open(OpenMenus::Menu::kVignette) } } };
			for (const auto& [menu, open] : menus) {
				const auto name = CatchUpMenus::NAMES[static_cast<std::size_t>(menu)];
				const auto cancel = CatchUpMenus::HowClosed(menu) == CatchUpMenus::Close::kCancel;
				switch (menuGuard.Watch(menu, open, a_now)) {
				case CatchUpMenus::Action::kClose:
					{
						// Why the close couldn't go (empty: it went). The settlement list is sent its own cancel, a Tab
						// handed to it alone: a hide would leave the script waiting on it suspended for good
						// (CatchUpMenus). Anything else gets a hide message.
						std::string why;
						if (cancel) {
							why = MenuInput::Tap(name, "Cancel", MenuInput::TAB);
						} else if (const auto refusal = MenuMessages::Refusal(name, true)) {
							why = refusal;
						} else if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
							queue->AddMessage(name, RE::UI_MESSAGE_TYPE::kHide);
						} else {
							why = "no UI message queue";
						}
						const auto opened = menuGuard.OpenedAt(menu);
						const auto stage = menuGuard.LastStage();
						const auto player = menuGuard.LastPlayer();
						const auto secs = [](Clock::duration a_gap) { return std::chrono::duration<double>(a_gap).count(); };
						// How long before the menu opened our player last acted (a menu of theirs, an activation): how
						// near the player's own window (CatchUpMenus::PlayerQuiet) the call was.
						const auto playerGap = opened && player ? std::format("{:.1f} s before it opened", secs(*opened - *player)) : std::string{ "never" };
						if (!why.empty()) {
							menuGuard.NotSent(menu);
							REX::WARN("QuestSync: {} looks like a friend's stage's, but its {} wasn't sent ({}); left to the player", name, cancel ? "cancel" : "hide", why);
							break;
						}
						REX::INFO("QuestSync: {} {}, opened {:.1f} s after a friend's stage was set here (the last {:08X} stage {}); our player last acted {}",
							cancel ? "cancelling" : "hiding", name, opened && stage ? secs(*opened - *stage) : 0.0, static_cast<std::uint32_t>(lastSetStage >> 16),
							static_cast<std::uint16_t>(lastSetStage & 0xFFFF), playerGap);
						break;
					}
				case CatchUpMenus::Action::kGaveUp:
					REX::WARN("QuestSync: {} is still up {} s after its {}; left to the player", name,
						std::chrono::duration_cast<std::chrono::seconds>(CatchUpMenus::GIVE_UP).count(), cancel ? "cancel" : "hide");
					break;
				case CatchUpMenus::Action::kNone:
					break;
				}
			}
		}

		// Where a kick-out a mover stands for (StageRules::KICKOUTS) is here: still to run, run here already (its stage done,
		// or its quest, whose start-up stage is the kick-out, started or run here), or not in this game.
		enum class KickOutState
		{
			kPending,
			kDone,
			kMissing,
		};
		KickOutState StateOfKickOut(const StageRules::KickOut& a_kick)
		{
			const auto quest = KickOutQuest(a_kick);
			if (!quest) {
				return KickOutState::kMissing;
			}
			if (a_kick.kickStage == 0) {
				return (quest->data.flags & QUEST_RUNNING) || RanBefore(quest) ? KickOutState::kDone : KickOutState::kPending;
			}
			return IsSettled(quest->GetFormID(), a_kick.kickStage) ? KickOutState::kDone : KickOutState::kPending;
		}

		// Runs the kick-out here (StateOfKickOut said pending): its stage set, as a friend's stage is, or its quest started, so
		// its start-up stage runs for this player (InstKickOut 0 judges where the player is itself). What was done, for the log.
		std::string RunKickOut(const StageRules::KickOut& a_kick)
		{
			const auto quest = KickOutQuest(a_kick);
			if (!quest) {
				return {};
			}
			const auto  id = quest->GetFormID();
			const char* name = quest->GetFullName();
			if (a_kick.kickStage == 0) {
				RE::Console::ExecuteCommand(std::format("startquest {:08X}", id).c_str());
				StageChanged(id, Clock::now());
				return std::format("{:08X} '{}' started, its start-up stage the kick-out", id, name ? name : "");
			}
			SetStageHere(quest, a_kick.kickStage);
			return std::format("{:08X} '{}' stage {} set", id, name ? name : "", a_kick.kickStage);
		}

		// A quest asked for in a skipped mover's place (StartSkippedStarts) is looked at again SKIPPED_START_CHECK of play
		// later: startquest says nothing of how it went, and a quest whose aliases don't fill doesn't start. The log then
		// says which it was.
		constexpr auto SKIPPED_START_CHECK = 5s;
		struct SkippedStartAsked
		{
			std::uint32_t     quest = 0;  // the quest asked for
			std::uint32_t     mover = 0;  // the stage that was not set: its quest
			std::uint16_t     stage = 0;
			Clock::time_point at{};
		};
		std::vector<SkippedStartAsked> skippedStartsAsked;

		void CheckSkippedStarts(Clock::time_point a_now)
		{
			std::erase_if(skippedStartsAsked, [&](const SkippedStartAsked& a_asked) {
				if (a_now - a_asked.at < SKIPPED_START_CHECK) {
					return false;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_asked.quest);
				if (!quest) {
					return true;
				}
				const char* name = quest->GetFullName();
				if ((quest->data.flags & QUEST_RUNNING) || RanBefore(quest)) {
					REX::INFO("QuestSync: {:08X} '{}' runs here now, started in the place of {:08X} stage {}", a_asked.quest, name ? name : "", a_asked.mover, a_asked.stage);
				} else {
					REX::WARN("QuestSync: {:08X} '{}' did not start here (its flags {:04X}), asked for in the place of {:08X} stage {}: the catch-up of the next join asks again",
						a_asked.quest, name ? name : "", quest->data.flags, a_asked.mover, a_asked.stage);
				}
				return true;
			});
		}

		// A friend's mover that was not set here (a_quest's a_stage, skipped for its move): the quests its script starts besides
		// the move (StageRules::SKIPPED_STARTS: Dangerous Minds' lounger starts Nick's talk after the memory) are started here in
		// its place, as the stage would have started them, unless the session shares one (its owner's stages bring it here) or
		// one runs here or ran here before. A friend's, as a stage set here from them: what its start-up does in the next
		// moments isn't reported as this player's own progress. Its scenes are this player's own, though, unlike those of a
		// stage set here (NoteSetHere tells sceneHold and menuGuard; this doesn't): the quest is started here only where the
		// session doesn't share it, so no friend's stage will ever end a scene of it, and friends' stages wait for a talk in it
		// as for any talk of the player's own; a menu it opened would be the player's to answer too.
		void StartSkippedStarts(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage, Clock::time_point a_now)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			for (const auto& row : StageRules::SkippedStartsOf(a_file, a_quest & 0x00FFFFFF, a_stage)) {
				const auto quest = handler ? handler->LookupForm<RE::TESQuest>(row.starts, StageRules::FileName(static_cast<StageRules::File>(row.file))) : nullptr;
				if (!quest) {
					continue;
				}
				const auto  id = quest->GetFormID();
				const char* name = quest->GetFullName();
				const bool  shared = Shared(quest);
				if (!StageRules::SkippedStartDue(shared, (quest->data.flags & QUEST_RUNNING) != 0, RanBefore(quest))) {
					if (shared) {
						REX::INFO("QuestSync: {:08X} '{}', which {:08X} stage {} starts besides its move, is shared in this session: its owner's stages bring it here, so it is not started in the stage's place",
							id, name ? name : "", a_quest, a_stage);
					} else {
						REX::INFO("QuestSync: {:08X} '{}', which {:08X} stage {} starts besides its move, runs or ran here already: left as it is", id, name ? name : "", a_quest,
							a_stage);
					}
					continue;
				}
				RE::Console::ExecuteCommand(std::format("startquest {:08X}", id).c_str());
				remoteStageAt[id] = a_now;
				StageChanged(id, a_now);
				skippedStartsAsked.push_back({ id, a_quest, a_stage, a_now });
				REX::INFO("QuestSync: asked the game to start {:08X} '{}': {:08X} stage {} starts it besides its move, and the stage was not set here", id, name ? name : "", a_quest,
					a_stage);
			}
		}

		// Sets another player's stage here, if it is this player's to have (see StageRules::Judge). That
		// runs the stage's script, as it did in their game (an achievement in it too, where the game
		// awards them), and starts a quest from wherever the player is; it can pull a new character out
		// of the opening.
		StageLog::Set SetHere(std::uint32_t a_quest, std::uint16_t a_stage, Clock::time_point a_now)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest) {
				return StageLog::Set::kSkipped;
			}
			const char* name = quest->GetFullName();
			// A random encounter's stage (its run's: EncounterRuns.h) never starts its quest: started so, it would run without
			// its trigger or the owner's actors forced in. One that waited here past its quest's stop is dropped.
			if (!(quest->data.flags & QUEST_RUNNING) && Spawns::IsEncounter(quest)) {
				if (encounterStartsRefused++ < 20) {
					REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: a random encounter's, and it no longer runs here (a stage never starts one)", a_quest, name ? name : "",
						a_stage);
				}
				return StageLog::Set::kSkipped;
			}
			const auto  file = FileOf(quest);
			const auto  local = a_quest & 0x00FFFFFF;
			// Whether the quest wasn't running before this stage: it isn't, or the game's own start started it for this stage
			// at the last poll (an event-scoped quest, StartsFromFriend), which ran its start-up stages and left this one to set.
			const bool  startedForIt = eventStartedFor.contains(Key(a_quest, a_stage));
			const bool  wasStopped = !(quest->data.flags & QUEST_RUNNING) || startedForIt;
			// Not started here: this stage starts it for the session (StateOf), and if it isn't running, the
			// game runs its start-up stage too.
			const bool  starting = wasStopped || (!SessionStarted(quest) && !startedHere.contains(a_quest) && !RadiantSync::Shares(a_quest) && !EncounterRuns::Shares(a_quest));
			// The quest's start-up stages, as its record flags them, which the game runs first when this stage starts it: none
			// while it runs here already (but for a stage whose quest the game's own start started for it at the last poll).
			const auto  startUps = file && wasStopped ? QuestStageLayout::StartUps(quest) : std::vector<std::uint16_t>{};
			auto        judged = file ? StageRules::Starting(*file, local, a_stage, startUps) : std::nullopt;
			// It turns a faction on the player (StageRules::HOSTILE), itself or through a start-up stage the game runs first when
			// it starts the quest here (InstKickOut 0): kHostile, with whatever else its rule says.
			// A stage whose script picks the faction by a global the stage carries (Power Play's 8: the gang DLC04EnemyGang
			// names) is held for that faction alone, and its landing told by it (StageRules::HostileWith): the value came with
			// the stage, kept in carriedValues until it is set. Without one, every faction the script could pick.
			auto        hostile = file ? StageRules::HostileRows(*file, local, a_stage, startUps) : std::vector<StageRules::Hostile>{};
			if (const auto came = carriedValues.find(Key(a_quest, a_stage)); !hostile.empty() && came != carriedValues.end()) {
				hostile = StageRules::HostileWith(std::move(hostile), came->second, a_quest);
			}
			// Or one actor, with no faction's change (StageRules::HOSTILE_ACTORS: Safari Adventure's 195 starts Cito's fight with
			// the player): kHostile as well, so the stage waits while that actor is near this player (ActorNear, below), but for
			// a mover that is no bridge and turns no faction (StageRules::ActorsHold).
			const auto  hostileActors = file ? StageRules::HostileActorRows(*file, local, a_stage, startUps) : std::vector<StageRules::HostileActor>{};
			const bool  actorsHold = !hostileActors.empty() && (!hostile.empty() || StageRules::ActorsHold(judged ? judged->flags : std::uint8_t{ 0 }));
			if (!hostile.empty() || actorsHold) {
				if (!judged) {
					judged = StageRules::Rule{ static_cast<std::uint8_t>(*file), local, a_stage, 0, 0 };
				}
				judged->flags |= StageRules::kHostile;
			}
			const StageRules::Rule* rule = judged ? &*judged : nullptr;
			const auto  lock = file ? StageRules::FindLock(*file, local, a_stage) : nullptr;
			const auto  releaseId = lock ? (a_quest & 0xFF000000) | lock->releaseQuest : 0u;

			// A stage that carries globals and waits for another first (Power Play's for Home Sweet Home's end here): its
			// quest waits before it is judged, so no offer is made, and no price waived, for a stage that can't be set yet.
			if (CarryWaits(a_quest, a_stage, file, local, name)) {
				return StageLog::Set::kQuestWaits;
			}
			// A quest whose memory this player is going through (StageRules::MEMORY_WAITS: the memory's quest runs here): its
			// friend's stages wait until they are out, before they are judged (a SOLO one is skipped then).
			if (MemoryWaits(a_quest, a_stage, file, local, name)) {
				return StageLog::Set::kQuestWaits;
			}
			// A stopped quest the story manager starts from an event, started here through the game's own start (below): while a
			// stop of it is still under way, its quest waits before it is judged, as above, up to EVENT_START_POLLS; then the start
			// is tried all the same, refused, and the stage set as before. A stage whose start took at an earlier poll, and whose
			// quest has stopped since (a repeatable dialogue quest whose scene ended it: DialogueInstituteSRB2), is dropped: started
			// again, it would wait and start again for good (QuestStartRules::NextStep).
			const bool runsNow = (quest->data.flags & QUEST_RUNNING) != 0;
			const auto step = QuestStartRules::NextStep(!runsNow && !(file && StageRules::IsEach(*file, local, a_stage)) && StartsFromFriend(quest), startedForIt, runsNow);
			if (step == QuestStartRules::Step::kGiveUp) {
				eventStartedFor.erase(Key(a_quest, a_stage));
				eventStartWaits.erase(a_quest);
				++skipped;
				if (eventStartLogged.insert(a_quest).second) {
					REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: the game's own start started the quest for it, and it stopped before the stage could be set "
							  "(its own script ended it); not started again for it (logged once per quest)",
						a_quest, name ? name : "", a_stage);
				}
				return StageLog::Set::kSkipped;
			}
			const bool eventStart = step == QuestStartRules::Step::kStart;
			if (eventStart && QuestStartRules::Waits(QuestStart::Judge(quest)) && ++eventStartWaits[a_quest] <= EVENT_START_POLLS) {
				return StageLog::Set::kQuestWaits;
			}

			StageRules::Situation now;
			now.sharedStory = Story::Shared();
			const auto live = liveAt.find(Key(a_quest, a_stage));
			now.live = live != liveAt.end() && a_now - live->second.at < LIVE_FOR;
			const bool unlocking = Unlocking(a_quest);
			now.held = combatHold && !unlocking;
			if (lock) {
				const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(releaseId);
				now.releaseComing = waiting.Contains(releaseId, lock->releaseStage) || (release && QuestStageLayout::IsStageDone(release, lock->releaseStage));
			}
			if (unlocking) {
				now.warned = StageRules::WARNING;
			} else if (warnedAt && a_now - *warnedAt < WARNING_COVERS) {
				now.warned = a_now - *warnedAt;
			}
			// The follow offer: made once while the stage is live, open for OFFER_FOR, for the friend who did it.
			const auto offer = offers.find(Key(a_quest, a_stage));
			const std::uint32_t friendId = offer != offers.end() ? offer->second.playerId : live != liveAt.end() ? live->second.playerId : 0u;
			// A teleport to that friend still on its way (their cell loading): not beside them until it lands (its cell's load
			// can put the player there a frame before), and the offer's time stands still meanwhile.
			const bool teleporting = friendId != 0 && Party::TeleportingTo(friendId);
			if (offer != offers.end()) {
				auto&      o = offer->second;
				const auto gap = a_now - o.seen;
				if (teleporting || gap > OFFER_GAP) {
					o.paused += gap;
				}
				o.seen = a_now;
				now.offered = a_now - o.at - o.paused;
			}
			now.followable = friendId != 0 && !RemotePlayers::NameOf(friendId).empty();
			if (rule && (rule->flags & (StageRules::kMoves | StageRules::kHostile)) && (now.live || (now.offered && *now.offered < StageRules::OFFER_FOR))) {
				now.nearFriend = !teleporting && NearFriend(friendId);
			}
			// A faction turned on the player: who of it is near (not asked beside the friend who did it: set at once then). Only
			// the rows whose hostility isn't here yet hold or are made (pending: a stage that comes again after a rejoin or a load
			// of a save past it, or behind a stage that turned the faction already, neither waits nor says it turned); for a
			// mover that stands for a kick-out (KICKOUTS), whether the kick-out ran here.
			const auto kickOut = file ? StageRules::KickOutOf(*file, local, a_stage) : nullptr;
			std::vector<StageRules::Hostile> pending;
			if (kickOut) {
				if (StateOfKickOut(*kickOut) == KickOutState::kPending) {
					pending = hostile;
				}
			} else {
				for (const auto& row : hostile) {
					if (!AlreadyEnemy(row) && !hostilityMade.contains(MadeKey(row))) {
						pending.push_back(row);
					}
				}
			}
			FactionNearby nearby;
			const bool    withFriend = now.live && now.nearFriend;
			if (!pending.empty() && !withFriend) {
				nearby = FactionNear(pending);
				now.factionNear = nearby.Near();
			}
			// One actor turned on the player: whether it is near, asked when no faction's people hold the stage already.
			if (actorsHold && !withFriend && !nearby.Near()) {
				if (const auto actorNear = ActorNear(hostileActors); actorNear.Near()) {
					nearby = actorNear;
					now.factionNear = true;
				}
			}
			now.opening = Story::InOpening();
			now.friendInOpening = Party::FriendInOpening();
			now.inMemory = Party::InMemory();
			if (file && StageRules::FindTrap(*file, local, a_stage)) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto cell = player ? player->GetParentCell() : nullptr;
				const auto cellFile = cell ? cell->GetFile(0) : nullptr;
				now.trapped = cellFile && StageRules::Trapped(*file, local, a_stage, StageRules::FileOf(cellFile->GetFilename()), cell->GetFormID() & 0x00FFFFFF);
			}
			// A way out of a memory (StageRules::SOLO): whether this player is in it, its way in done here; and its door (the
			// lounger), for the close of a game whose player isn't (StageRules::ClosesOutside, ShutsDoor).
			const auto solo = file ? StageRules::FindSolo(*file, local, a_stage) : nullptr;
			const auto handler = solo && solo->insideQuest ? RE::TESDataHandler::GetSingleton() : nullptr;
			const auto soloFile = solo ? StageRules::FileName(static_cast<StageRules::File>(solo->file)) : std::string_view{};
			const auto inQuest = handler ? handler->LookupForm<RE::TESQuest>(solo->insideQuest, soloFile) : nullptr;
			const auto door = handler && solo->doorQuest ? handler->LookupForm<RE::TESQuest>(solo->doorQuest, soloFile) : nullptr;
			if (inQuest) {
				now.inside = QuestStageLayout::IsStageDone(inQuest, solo->insideStage);
			}
			// A stage left for this poll by the game's own start (eventStartedFor) was judged at that poll, and kSet then: kept, not judged again.
			const auto verdict = startedForIt ? StageRules::Verdict::kSet : StageRules::Judge(rule, now);
			std::string followed;     // how an offer was taken up, for the log
			std::string hostileNote;  // a faction turned on the player, and the wait for it to be out of sight, for the log
			if (offer != offers.end() && verdict != StageRules::Verdict::kOffer) {
				if (verdict == StageRules::Verdict::kSet) {
					const auto arrived = Party::TeleportedTo(friendId);
					followed = arrived && *arrived >= offer->second.at ? std::format(" after arriving beside player {} by the teleport key", friendId) :
					                                                      std::format(" after arriving beside player {} (came near them)", friendId);
				} else if (!now.followable) {
					REX::INFO("QuestSync: the offer to follow player {} into {:08X} '{}' stage {} ends: they left the session", friendId, a_quest, name ? name : "", a_stage);
				} else {
					REX::INFO("QuestSync: the offer to follow player {} into {:08X} '{}' stage {} was not taken up in {} s", friendId, a_quest, name ? name : "", a_stage,
						std::chrono::duration_cast<std::chrono::seconds>(StageRules::OFFER_FOR).count());
				}
				offers.erase(offer);
			}
			switch (verdict) {
			case StageRules::Verdict::kSkip:
				++skipped;
				hostileWaits.End(Key(a_quest, a_stage));
				REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: {}", a_quest, name ? name : "", a_stage, StageRules::WhySkipped(rule));
				if (file && rule && StageRules::SkippedForMove(rule->flags, now.sharedStory)) {
					StartSkippedStarts(*file, a_quest, a_stage, a_now);
				}
				if (solo && solo->insideQuest) {
					const bool doorDone = door && QuestStageLayout::IsStageDone(door, solo->doorStage);
					const StageRules::MemoryHere here{ .read = inQuest && (door || !solo->doorQuest),
						.inside = now.inside,
						.doorDone = doorDone,
						.running = (quest->data.flags & QUEST_RUNNING) != 0,
						.doneHere = QuestStageLayout::IsStageDone(quest, a_stage) };
					if (StageRules::ClosesOutside(solo, here)) {
						const bool shuts = door && StageRules::ShutsDoor(solo, (door->data.flags & QUEST_RUNNING) != 0, doorDone);
						CloseMemoryHere(quest, a_stage, shuts ? door : nullptr, solo->doorStage, name, now.live);
					} else if (!here.read || (here.doorDone && !here.inside && here.running && !here.doneHere)) {
						// Only what was read: a failed lookup says nothing of whether the quest runs here, and a door done with the
						// way in not is the player on the way in, or (never in the game as shipped) a close that shut the door and
						// then couldn't mark the way out.
						REX::INFO("QuestSync: {:08X} '{}' stage {} closes nothing here: {}", a_quest, name ? name : "", a_stage,
							!here.read ? "its way in or its door could not be looked up, and its player may be in the memory" :
							             "its door is done here and its way in isn't (its player sat in their own lounger and is on the way in)");
					}
				}
				return StageLog::Set::kSkipped;
			case StageRules::Verdict::kHostileWaits:
				{
					// The rest of its quest waits meanwhile; other quests go on. A bridge (one that starts another quest: BoS201's 1)
					// holds the quests not started here yet and the stages that set another quest's with it, so the quest
					// it starts doesn't run here first (StageLog::Set::kStartsWait), and the quests running here go on. Said in the
					// log as the hold begins and every LOG_EVERY of play, and once on the HUD after NOTICE_AFTER of it.
					const bool bridge = rule && (rule->flags & StageRules::kBridge);
					const auto held = hostileWaits.Hold(Key(a_quest, a_stage), nearby.faction, [&](std::uint32_t a_faction) { return FactionName(a_faction); });
					if (held.logDue && nearby.actor) {
						REX::INFO("QuestSync: {:08X} '{}' stage {} turns the actor {:08X} '{}' on the player, and it is near{}; waiting until it is not within {} u{}; {}",
							a_quest, name ? name : "", a_stage, nearby.faction, FactionName(nearby.faction), nearby.count > 1 ? std::format(" (as {} more of its actors are)", nearby.count - 1) : std::string{},
							HostileHold::RANGE, held.begun ? std::string{} : std::format(" ({} s of play so far)", std::chrono::duration_cast<std::chrono::seconds>(held.played).count()),
							bridge ? "its quest waits, and the quests not started here yet wait with it (it starts one); the quests running here go on" : "its quest waits, other quests go on");
					} else if (held.logDue) {
						REX::INFO("QuestSync: {:08X} '{}' stage {} turns {:08X} '{}' on the player, and {} of them {} near; waiting until none of them is within {} u{}; {}",
							a_quest, name ? name : "", a_stage, nearby.faction, FactionName(nearby.faction), nearby.count, nearby.count == 1 ? "is" : "are", HostileHold::RANGE,
							held.begun ? std::string{} : std::format(" ({} s of play so far)", std::chrono::duration_cast<std::chrono::seconds>(held.played).count()),
							bridge ? "its quest waits, and the quests not started here yet wait with it (it starts one); the quests running here go on" : "its quest waits, other quests go on");
					}
					if (held.noticeDue) {
						const auto factionName = FactionName(nearby.faction);
						Hud::Notify(factionName.empty() ? Text::Format("quest.hostile_waiting_quest", { { "quest", ShownName(quest, name) } }) :
						                                  Text::Format("quest.hostile_waiting", { { "quest", ShownName(quest, name) }, { "faction", factionName } }));
					}
					return bridge ? StageLog::Set::kStartsWait : StageLog::Set::kQuestWaits;
				}
			case StageRules::Verdict::kHostileOnly:
				{
					// It moves the player, who didn't go with the friend (the offer run out, nobody to follow, or caught up): the stage
					// is not set, but the faction it turns on the player is turned on them here, as the chooser chose (never skipped):
					// a mover that stands for a kick-out runs the kick-out itself (KICKOUTS: the faction's quests failed, the player
					// out of it, its companion dismissed, as the stage's script would), unless it ran here already; any other makes
					// the rows' calls, the ones not made here yet (pending), and a call the VM wouldn't queue is tried again next poll,
					// the quest waiting, up to CALL_TRIES.
					const auto  key = Key(a_quest, a_stage);
					std::string made;     // what was done, for the log
					std::string already;  // ... or why nothing had to be
					bool        retry = false;
					if (kickOut) {
						switch (StateOfKickOut(*kickOut)) {
						case KickOutState::kPending:
							WarnDismissal(*file, local, a_stage, false, kickOut, friendId, name);
							made = RunKickOut(*kickOut);
							break;
						case KickOutState::kDone:
							already = "its kick-out ran here already";
							break;
						case KickOutState::kMissing:
							REX::WARN("QuestSync: {:08X} '{}' stage {} stands for a kick-out, {:08X}, which isn't in this game", a_quest, name ? name : "", a_stage, kickOut->kickQuest);
							break;
						}
					} else {
						if (pending.empty()) {
							already = "the faction is an enemy of the player here already";
						}
						for (const auto& row : pending) {
							const auto faction = FactionOf(row);
							const auto factionId = faction ? faction->GetFormID() : 0u;
							const auto turned = TurnOnPlayer(row);
							switch (turned.outcome) {
							case Turned::kMade:
								hostilityMade.insert(MadeKey(row));
								made += std::format("{}{:08X} '{}' by {}", made.empty() ? "" : ", ", factionId, FactionName(factionId), turned.called);
								break;
							case Turned::kNever:
								REX::WARN("QuestSync: {:08X} '{}' stage {} turns {:08X} '{}' on the player, which can't be done here without the stage ({})", a_quest, name ? name : "",
									a_stage, factionId, FactionName(factionId), !faction ? "its faction isn't in this game" : "the stage's actor is put in the faction");
								break;
							case Turned::kRetry:
								retry = true;
								break;
							}
						}
					}
					if (retry) {
						const auto tries = ++hostileCallTries[key];
						if (tries <= HostileHold::CALL_TRIES) {
							if (tries == 1) {
								REX::INFO("QuestSync: {:08X} '{}' stage {} turns a faction on the player, and the call couldn't be queued (a load on?); trying again next poll, up to {} times",
									a_quest, name ? name : "", a_stage, HostileHold::CALL_TRIES);
							}
							return StageLog::Set::kQuestWaits;
						}
						REX::WARN("QuestSync: {:08X} '{}' stage {} turns a faction on the player, and the call couldn't be queued in {} tries; given up on", a_quest, name ? name : "", a_stage,
							HostileHold::CALL_TRIES);
					}
					hostileCallTries.erase(key);
					const auto heldFor = hostileWaits.End(key);
					if (!made.empty()) {
						++hostileApplied;
					}
					// The HUD line as for a stage set after its wait: only when it waited (the faction it waited for, or the quest's name).
					if (heldFor && !made.empty()) {
						++hostileHeld;
						const auto factionName = HeldName(*heldFor);
						Hud::Notify(Text::Format("quest.hostile_set", { { "faction", factionName.empty() ? ShownName(quest, name) : factionName },
															{ "player", friendId ? RemotePlayers::NameOf(friendId) : Text::Get("quest.a_friend") } }));
					}
					REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: it moves the player, who isn't with the friend who did it; {}{}{}{}",
						a_quest, name ? name : "", a_stage,
						kickOut ? "the kick-out it stands for is run here all the same" : "the faction it turns on the player is turned on them here all the same",
						made.empty() ? "" : std::format(" ({})", made), already.empty() ? "" : std::format(" ({})", already),
						heldFor ? std::format(" (after {} s of play waiting for {:08X} '{}' to be out of sight)", std::chrono::duration_cast<std::chrono::seconds>(heldFor->played).count(),
						                      heldFor->faction, HeldName(*heldFor)) :
						          std::string{});
					return StageLog::Set::kSkipped;
				}
			case StageRules::Verdict::kOffer:
				// The rest of its quest waits meanwhile; other quests go on, but not past a bridge: what came after it waits
				// as behind its warning (MQ206's 1000 offered, the MQ207 it starts must not run here first).
				if (offer == offers.end()) {
					offers[Key(a_quest, a_stage)] = Offer{ a_now, friendId, a_now };
					const auto blocked = Party::OfferFollow(friendId);
					const bool fight = rule && (rule->flags & StageRules::kHostile);
					// A name in brackets is the editor's, never shown in the Pip-Boy ("[DN070 - Jamaica Plain]"): "a quest" then.
					Hud::Notify(Text::Format(fight ? "quest.follow_fight" : "quest.follow_offer",
						{ { "player", RemotePlayers::NameOf(friendId) },
							{ "quest", name && *name && *name != '[' && !Unordered(quest) ? std::string{ name } : Text::Get("quest.a_quest") },
							{ "key", Party::TeleportKeyName() },
							{ "n", std::chrono::duration_cast<std::chrono::seconds>(StageRules::OFFER_FOR).count() } }));
					if (!blocked.empty()) {
						Hud::Notify(blocked);
					}
					REX::INFO("QuestSync: offer to follow player {} into {:08X} '{}' stage {} ({}) for {} s; it is set once the player is beside them{}", friendId, a_quest,
						name ? name : "", a_stage, fight ? "a faction turns on the player" : "it moves the player", std::chrono::duration_cast<std::chrono::seconds>(StageRules::OFFER_FOR).count(),
						blocked.empty() ? std::string{} : std::format(" (the teleport can't go now: {})", blocked));
				}
				return rule && (rule->flags & StageRules::kBridge) ? StageLog::Set::kLater : StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kWarn:
				warnedAt = a_now;
				Hud::Notify(Text::Format("quest.catching_up", { { "n", std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count() },
					{ "quest", name ? std::string{ name } : Text::Get("quest.a_quest") } }));
				return StageLog::Set::kLater;
			case StageRules::Verdict::kLater:
				return StageLog::Set::kLater;
			case StageRules::Verdict::kQuestWaits:
				if (lockWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} takes the player's controls away; waiting for {:08X} stage {}, which gives them back",
						a_quest, name ? name : "", a_stage, releaseId, lock ? lock->releaseStage : 0);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kOpeningWaits:
				if (openingWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} moves the player; waiting while this game is in the opening", a_quest, name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kTrapWaits:
				if (trapWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} would shut in or kill the player in the cell they stand in; waiting until they are out", a_quest,
						name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kMemoryWaits:
				if (memoryWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} moves the player, who is in a memory; waiting until they are out (StageRules::MEMORY_PLACES)", a_quest,
						name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kSet:
				if (solo) {
					REX::INFO("QuestSync: setting {:08X} '{}' stage {}{}: this player is in the memory ({:08X} stage {} done here), and it brings them out and gives back their controls",
						a_quest, name ? name : "", a_stage, now.live ? "" : " (catching up)", inQuest ? inQuest->GetFormID() : solo->insideQuest, solo->insideStage);
				}
				break;
			}

			// A friend's purchase: what they bought, this player gets too, without paying for it.
			if (rule && (rule->flags & StageRules::kFree) && !startedForIt) {  // waived at the poll that started the quest for it
				const auto price = file ? StageRules::FindPrice(*file, local, a_stage) : nullptr;
				if (!price || !WaivePrice(*price, a_quest, a_stage, a_now)) {
					++skipped;
					REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: a friend's purchase whose price can't be waived here", a_quest, name ? name : "", a_stage);
					return StageLog::Set::kSkipped;
				}
			}

			// A companion quest's stage whose script is its owner's: in the game that drives the quest, set with its script as
			// any stage; in the others only its EACH acts, and marked done. Judged only once the Followers aliases can be read
			// (after a load they can come back empty, and the companion then reads as nobody's).
			StageRules::DriveFacts driveFacts;
			const bool             each = file && StageRules::IsEach(*file, local, a_stage);
			if (each && !Companions::AliasesReadable()) {
				if (eachSettleLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} is a companion quest's: waiting for the game to settle before judging which game drives it", a_quest,
						name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			}
			const auto drive = each ? DriveHere(quest, *file, driveFacts) : StageRules::Drive::kScripts;
			const auto driveText = std::format("marked={} driven={} mirrored={} ours={}", driveFacts.marked, driveFacts.driven, driveFacts.mirrored, driveFacts.ours);
			if (each && drive == StageRules::Drive::kMirror && !(quest->data.flags & QUEST_RUNNING)) {
				// Started first; its aliases are filled as it starts. Given up on if it doesn't (StageRules::GiveUpStart).
				if (eachStartsGaveUp.contains(a_quest)) {
					++skipped;
					return StageLog::Set::kSkipped;
				}
				auto& start = eachStarts.try_emplace(a_quest, StartTry{ a_now, 0 }).first->second;
				if (StageRules::GiveUpStart(start.tries, a_now - start.first)) {
					eachStartsGaveUp.insert(a_quest);
					++skipped;
					REX::WARN("QuestSync: {:08X} '{}' doesn't start here ({} tries over {} s); its companion's owner's stages are left out here (stage {} the first)", a_quest,
						name ? name : "", start.tries, std::chrono::duration_cast<std::chrono::seconds>(a_now - start.first).count(), a_stage);
					return StageLog::Set::kSkipped;
				}
				++start.tries;
				RE::Console::ExecuteCommand(std::format("startquest {:08X}", a_quest).c_str());
				if (eachWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} is its companion's owner's: starting the quest here first", a_quest, name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			}

			// The globals it carries (the chooser's), written just before it: its script decides here as it did for them.
			const auto carry = PlanCarried(a_quest, a_stage, file, local, name);
			switch (carry.what) {
			case StageRules::Carry::Do::kWait:  // not after CarryWaits above; kept whole all the same
				return StageLog::Set::kQuestWaits;
			case StageRules::Carry::Do::kSkip:
				++skipped;
				carriedValues.erase(Key(a_quest, a_stage));
				return StageLog::Set::kSkipped;
			case StageRules::Carry::Do::kSet:
				break;
			}
			// The companion its start-up stage or its own would dismiss: said before anything of it runs here.
			if (file) {
				WarnDismissal(*file, local, a_stage, wasStopped, nullptr, friendId, name);
			}
			// A stopped quest the story manager starts from an event (StartsFromFriend): the console's setstage would be refused,
			// so it is started first through the game's own start, with this game's fills. When that start runs the stage itself
			// (a start-up stage, or one done now) it isn't set again: the globals it carries are written before the start. Any
			// other stage waits for the next poll, after the start-up stages the start queued have run, and is set then
			// (eventStartedFor). Refused or failed, that is said once and the stage is set as before (the game refuses it, and
			// the queue gives the start up: failedStarts).
			bool ranByStart = false;
			if (eventStart) {
				eventStartWaits.erase(a_quest);
				const bool startUp = std::ranges::contains(QuestStageLayout::StartUps(quest), a_stage);  // one the record flags as a start-up stage
				std::string written;  // the carried globals written before a start that runs the stage, for the log
				if (startUp) {
					for (const auto& write : carry.writes) {
						const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(write.global);  // checked in PlanCarried
						written += std::format("{}{:08X} {} -> {}", written.empty() ? "" : ", ", write.global, global ? global->value : 0.0f, write.value);
						SetGlobal(write.global, write.value);
					}
				}
				FadeWatch::Arm(std::format("start {:08X} for stage {}", a_quest, a_stage));  // its start-up stages may fade the screen or move the player
				const auto result = QuestStart::Start(quest);
				if (result.started) {
					if (QuestStartRules::SetAfterStart(startUp, QuestStageLayout::IsStageDone(quest, a_stage))) {
						eventStartedFor.insert(Key(a_quest, a_stage));
						REX::INFO("QuestSync: {:08X} '{}' (the story manager starts it from an event) started here through the game's own start for a friend's stage {}; "
								  "the stage is set at the next poll, after the start's own stages",
							a_quest, name ? name : "", a_stage);
						return StageLog::Set::kQuestWaits;
					}
					ranByStart = true;
					if (!written.empty()) {
						++carriedSet;
						REX::INFO("QuestSync: before {:08X} '{}' stage {}, its globals set to the chooser's: {}", a_quest, name ? name : "", a_stage, written);
					}
					REX::INFO("QuestSync: {:08X} '{}' (the story manager starts it from an event) started here through the game's own start for a friend's stage {}; "
							  "the start runs that stage itself",
						a_quest, name ? name : "", a_stage);
				} else if (eventStartLogged.insert(a_quest).second) {
					REX::INFO("QuestSync: {:08X} '{}' (the story manager starts it from an event) couldn't be started here for a friend's stage {}: {}; the stage is set as before "
							  "(logged once per quest)",
						a_quest, name ? name : "", a_stage, result.why);
				}
			}
			// A faction turned on the player far from the friend who did it: none of it is near now. The log says so, with the
			// wait if it waited; only then the HUD says whose choice it was (the friend's name while the stage is live; a
			// caught-up one has no friend to name): most of these stages set up a fight far away, and a joiner catching up
			// would see one line per stage otherwise.
			if ((!hostile.empty() || actorsHold) && !withFriend) {
				if (const auto heldFor = hostileWaits.End(Key(a_quest, a_stage))) {
					++hostileHeld;
					const auto factionName = HeldName(*heldFor);
					hostileNote = std::format(" (after {} s of play waiting for {:08X} '{}' to be out of sight)", std::chrono::duration_cast<std::chrono::seconds>(heldFor->played).count(),
						heldFor->faction, factionName);
					Hud::Notify(Text::Format("quest.hostile_set", { { "faction", factionName.empty() ? ShownName(quest, name) : factionName },
															{ "player", friendId ? RemotePlayers::NameOf(friendId) : Text::Get("quest.a_friend") } }));
				} else if (hostile.empty()) {
					hostileNote = " (it turns an actor of its own on the player: none of them near)";
				} else {
					const auto faction = FactionOf(hostile.front());
					const auto factionId = faction ? faction->GetFormID() : 0u;
					hostileNote = std::format(" (it turns {:08X} '{}' on the player: {})", factionId, FactionName(factionId),
						pending.empty() ? (kickOut ? "its kick-out ran here already" : "an enemy of the player here already") : "none of them near");
				}
			}
			// Sets the stage with its script: the globals it carries first, then the stage (DoCarry).
			const auto setCarried = [&]() {
				std::string written;
				StageRules::DoCarry(
					carry,
					[&](std::uint32_t a_global, float a_value) {
						const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(a_global);  // checked in PlanCarried
						written += std::format("{}{:08X} {} -> {}", written.empty() ? "" : ", ", a_global, global ? global->value : 0.0f, a_value);
						SetGlobal(a_global, a_value);
					},
					[&]() { SetStageHere(quest, a_stage); });
				if (!carry.writes.empty()) {
					++carriedSet;
					REX::INFO("QuestSync: before {:08X} '{}' stage {}, its globals set to the chooser's: {}", a_quest, name ? name : "", a_stage, written);
				}
			};
			const bool liveHere = cameLive.contains(Key(a_quest, a_stage)) || !followed.empty();  // the achievement gate's view: came live since the load, however long it waited
			if (each && drive == StageRules::Drive::kMirror) {
				eachStarts.erase(a_quest);
				// Its script isn't run here, so nothing decides by the globals: none is written (no carried stage is a companion's).
				const auto  was = quest->currentStage;
				std::string body;
				const auto  missed = SetEachHere(quest, *file, a_stage, a_now, body);
				REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}) without its script, the companion's owner's ({}): {} acts{}{}{}{}{}", a_quest, name ? name : "", a_stage, was,
					driveText, StageRules::EachOf(*file, local, a_stage).size(), missed ? std::format(", {} could not be done", missed) : std::string{}, body,
					liveHere ? "" : " (catching up)", followed, hostileNote);
			} else if (each) {
				RenewBodies(quest, *file, a_stage);  // her script's reset builds her again: no kept roll of her old body forced on it
				setCarried();
				++drivenStages;
				drivenHere.insert(a_quest);  // until a load: its script may not have left its mark yet
				REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}) with its script: this game drives its companion's quest ({}){}{}{}", a_quest, name ? name : "", a_stage,
					quest->currentStage, driveText, liveHere ? "" : " (catching up)", followed, hostileNote);
			} else {
				if (ranByStart) {
					NoteSetHere(quest, a_stage);  // its start ran it: known as set here, not reported back
				} else {
					setCarried();
				}
				REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}){}{}{}{}{}", a_quest, name ? name : "", a_stage, quest->currentStage, liveHere ? "" : " (catching up)",
					starting ? " (starting the quest here for the session)" : "", ranByStart ? " (by the quest's start)" : "", followed, hostileNote);
			}
			carriedValues.erase(Key(a_quest, a_stage));
			carryWaitLogged.erase(Key(a_quest, a_stage));
			friendMoved.insert(a_quest);  // however it was set here: a friend's stage
			eventStartedFor.erase(Key(a_quest, a_stage));
			if (starting) {
				startedHere.insert(a_quest);
				catchingUp.insert(a_quest);
				startHolds[Key(a_quest, a_stage)] = StageRules::Hold(rule);
			}
			if (lock) {
				locked = Locked{ a_quest, a_stage, releaseId, lock->releaseStage };
				waiting.Lock(a_quest, releaseId);
			}
			if (waited.erase(a_quest) > 0) {
				++caughtUp;
				NoteCaughtUp(Unordered(quest) ? nullptr : name, a_now);  // a dialogue or silent quest's name is the editor's, not one the player knows
			}
			Achievements::AfterStageSet(a_quest, a_stage);
			if (file) {
				Achievements::StageSetHere(*file, local, a_stage, liveHere);  // a caught-up stage's achievement is dropped (AchievementGate)
			}
			return StageLog::Set::kSet;
		}

		// The timers' starts a scene sets (StageRules::TIMER_STARTS: FFGoodneighbor08's 20, Sammy's scene, sets 25), in every
		// game, in a session or not, whoever set the scene's stage: while the quest runs with that stage done and the timer's
		// start not, the time is counted in play (not while paused; a gap of a loading screen counts as one poll), and after
		// `within` of it the game sets the start itself: the scene didn't play here (its actors not loaded: the stage came
		// from a friend, or from a session's catch-up, or the save was made in between), and nothing else would ever end the
		// event here. In the host's game that starts the host's timer, which ends the event for the session; in a friend's, its
		// own timer, whose stages are never reported (StageRules::ReportsTimer), so it ends that game's event alone.
		void KickTimers(Clock::time_point a_now, bool a_paused)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			for (const auto& row : StageRules::TIMER_STARTS) {
				const auto quest = handler ? handler->LookupForm<RE::TESQuest>(row.quest, StageRules::FileName(static_cast<StageRules::File>(row.file))) : nullptr;
				if (!quest) {
					continue;
				}
				const auto id = quest->GetFormID();
				const bool running = (quest->data.flags & QUEST_RUNNING) != 0;
				const bool afterDone = running && QuestStageLayout::IsStageDone(quest, row.after);
				const bool setsDone = running && QuestStageLayout::IsStageDone(quest, row.sets);
				const bool endsDone = running && QuestStageLayout::IsStageDone(quest, row.ends);  // the event over here (a joiner caught up the host's 1000)
				if (!running || !afterDone || setsDone || endsDone) {
					timerWaits.erase(id);
					continue;
				}
				const auto [it, fresh] = timerWaits.try_emplace(id, TimerWait{ Clock::duration::zero(), a_now });
				auto&      wait = it->second;
				if (!fresh && !a_paused) {
					wait.played += std::min<Clock::duration>(a_now - wait.last, 2 * POLL_INTERVAL);
				}
				wait.last = a_now;
				if (StageRules::TimerKick(row, running, afterDone, setsDone, endsDone, wait.played)) {
					const char* name = quest->GetFullName();
					REX::INFO("QuestSync: {:08X} '{}' has run {} s of play with stage {} done and {} not (its scene plays only where its actors are loaded); setting {} here, "
							  "the start of {} timer",
						id, name ? name : "", std::chrono::duration_cast<std::chrono::seconds>(wait.played).count(), row.after, row.sets, row.sets,
						hosting ? "the host's" : "this game's own");
					RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", id, row.sets).c_str());
					StageChanged(id, a_now);
					wait.played = Clock::duration::zero();  // not again at once if the console's set takes a poll to show
				}
			}
		}

		// Out of a session only the lock kept its quests' stages (OnSessionEnded); once the player has
		// their controls back, the rest of them are the session's and go too.
		void Unlock(bool a_inSession)
		{
			locked.reset();
			waiting.Unlock();
			if (!a_inSession) {
				waiting.Clear();
			}
		}

		// The player has their controls back once the lock's release stage is done. Looked at before the
		// queue's poll too: out of a session, the poll that finds the release done would go on to set the
		// next of its quest's stages (one of the session's past MQ206's 1200).
		void CheckReleased(bool a_inSession)
		{
			if (!locked) {
				return;
			}
			const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(locked->releaseQuest);
			if (!release || QuestStageLayout::IsStageDone(release, locked->releaseStage)) {
				Unlock(a_inSession);
			}
		}

		// If the lock's quests have done nothing for Queue::LOCK_TIMEOUT of play (a stage given up on, or
		// one that can't be set), the release is set outright rather than leave the player unable to move.
		// Not while the teleport is still to be tried: the time a stage of theirs is being set or its
		// script runs on doesn't count, nor time paused (a menu, alt-tab, a loading screen), when no
		// script runs.
		void CheckLock(bool a_inSession)
		{
			CheckReleased(a_inSession);
			if (!locked || !waiting.LockOverdue()) {
				return;
			}
			const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(locked->releaseQuest);
			REX::WARN("QuestSync: {:08X} stage {} took the player's controls, and nothing has given them back in {} s of play; setting {:08X} stage {}",
				locked->quest, locked->stage, std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::LOCK_TIMEOUT).count(), locked->releaseQuest, locked->releaseStage);
			SetStageHere(release, locked->releaseStage);
			++released;
			Hud::Notify(Text::Get("quest.controls_back"));
			Unlock(a_inSession);
		}

		// The game as the queue sees it.
		struct Game
		{
			Clock::time_point now;

			StageLog::Quest Quest(std::uint32_t a_quest) const { return StateOf(a_quest); }
			bool            Settled(std::uint32_t a_quest, std::uint16_t a_stage) const { return IsSettled(a_quest, a_stage); }
			StageLog::Set   Set(std::uint32_t a_quest, std::uint16_t a_stage) const { return SetHere(a_quest, a_stage, now); }
			Clock::duration Hold(std::uint32_t a_quest, std::uint16_t a_stage) const
			{
				// A stage that started its quest: the start-up stage's script may run on too (SetHere).
				if (const auto start = startHolds.extract(Key(a_quest, a_stage))) {
					return start.mapped();
				}
				return StageRules::Hold(RuleFor(a_quest, a_stage));
			}
			bool SetsAnother(std::uint32_t a_quest, std::uint16_t a_stage) const
			{
				const auto file = FileOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest));
				return file && StageRules::SetsAnother(*file, a_quest & 0x00FFFFFF, a_stage);
			}
		};

		// An older version set only the highest stage of a friend's quest. MQ104 went straight to 1000,
		// its 210 never ran here, and so MQ105 never started: the main story stopped there, and the
		// session file doesn't have 210 to send again. Starts MQ105 for a player left like that, as a
		// stage caught up from the session would be: at its 10, as 210 does, judged by 10's own rule (StageRules).
		// MQ105 has no start-up stage, so setting 10 runs nothing else: its 0, the move to
		// MQ105PlayerStartMarker, is a developer's quickstart (kDebug) the game never runs in play. Not
		// mid-fight, nor in a conversation or a menu; a warning first only if 10 ever gets a row that moves.
		void RepairStory(Clock::time_point a_now)
		{
			const auto mq104 = RE::TESForm::GetFormByID<RE::TESQuest>(MQ104);
			const auto mq105 = RE::TESForm::GetFormByID<RE::TESQuest>(MQ105);
			if (!mq104 || !mq105 || !Official::IsOfficialForm(mq104) || !Official::IsOfficialForm(mq105)) {
				return;
			}
			// In play only 210 moves MQ104 past 210 (to 1000). A quickstart typed here can too (MS07 0 stops MQ104, which runs
			// its shutdown stage 1000): that is this player's own jump, not a stuck story (StageRules::RepairsStory). The cheap
			// test first, every poll; MQ105's stages and the quickstarts are read only for an MQ104 left past 210 without it.
			if (mq104->currentStage <= MQ104_STARTS_MQ105 || QuestStageLayout::IsStageDone(mq104, MQ104_STARTS_MQ105)) {
				return;
			}
			const bool mq105Started = (mq105->data.flags & (QUEST_RUNNING | QUEST_COMPLETED)) || !QuestStageLayout::Done(mq105).empty();
			if (!StageRules::RepairsStory(mq104->currentStage, false, mq105Started, QuickstartDone)) {
				return;
			}
			if (busy || Paused()) {
				return;
			}
			const char* name = mq105->GetFullName();
			const auto  shown = name && *name ? std::string{ name } : Text::Get("quest.next");
			StageRules::Situation now;
			now.sharedStory = true;
			now.held = combatHold;
			now.opening = Story::InOpening();
			now.friendInOpening = Party::FriendInOpening();
			now.inMemory = Party::InMemory();
			if (warnedAt && a_now - *warnedAt < WARNING_COVERS) {
				now.warned = a_now - *warnedAt;
			}
			switch (StageRules::Judge(RuleFor(MQ105, MQ105_FIRST), now)) {
			case StageRules::Verdict::kOpeningWaits:
			case StageRules::Verdict::kMemoryWaits:
				return;
			case StageRules::Verdict::kWarn:
				warnedAt = a_now;
				Hud::Notify(Text::Format("quest.stuck_in", { { "quest", shown }, { "n", std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count() } }));
				return;
			case StageRules::Verdict::kLater:
				return;
			default:
				break;
			}
			REX::WARN("QuestSync: MQ104 is at {} without its stage {}, and MQ105 never started; starting it", mq104->currentStage, MQ104_STARTS_MQ105);
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", MQ105, MQ105_FIRST).c_str());
			FadeWatch::Arm(std::format("set {:08X} stage {}", MQ105, MQ105_FIRST));
			sceneHold.StageSet(MQ105, a_now);
			++repaired;
			Hud::Notify(Text::Format("quest.stuck", { { "quest", shown } }));
		}

		// An objective a stage taken after a higher one showed on a quest that is finished here (StageRules::LEFT_SHOWN:
		// MQ204 5's MQ202 120, after a catch-up's MQ202 320): completed, as the quest's own end left the others. Asked again
		// at most every LEFT_SHOWN_RETRY while it still reads shown (the console runs it a moment later), LEFT_SHOWN_TRIES
		// times in all; if it still reads shown after that, one line says so and it is left alone until the session ends.
		constexpr auto     LEFT_SHOWN_RETRY = 10s;
		constexpr unsigned LEFT_SHOWN_TRIES = 3;
		struct LeftShownAsk
		{
			Clock::time_point at;
			unsigned          tries = 0;
		};
		std::unordered_map<std::uint64_t, LeftShownAsk> leftShownAsked;
		void CompleteLeftShown(Clock::time_point a_now)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			for (const auto& row : StageRules::LEFT_SHOWN) {
				const auto quest = handler->LookupForm<RE::TESQuest>(row.quest, StageRules::FileName(static_cast<StageRules::File>(row.file)));
				if (!quest || !(quest->data.flags & QUEST_COMPLETED)) {
					continue;
				}
				for (const auto objective : quest->objectives) {
					if (!objective || objective->index != row.objective ||
						!StageRules::CompletesLeftShown(true, static_cast<std::int8_t>(objective->state))) {
						continue;
					}
					const auto key = Key(quest->GetFormID(), row.objective);
					auto&      asked = leftShownAsked[key];
					if (asked.tries > LEFT_SHOWN_TRIES || (asked.tries > 0 && a_now - asked.at < LEFT_SHOWN_RETRY)) {
						continue;
					}
					const char* name = quest->GetFullName();
					if (asked.tries == LEFT_SHOWN_TRIES) {
						++asked.tries;  // given up on: no more tries or lines this session
						REX::WARN("QuestSync: {:08X} '{}' objective {} still reads shown after {} tries to complete it; leaving it for this session",
							quest->GetFormID(), name ? name : "", row.objective, LEFT_SHOWN_TRIES);
						continue;
					}
					++asked.tries;
					asked.at = a_now;
					REX::INFO("QuestSync: {:08X} '{}' is completed here, and its objective {} is shown again (a stage taken after a higher one showed it): completing it",
						quest->GetFormID(), name ? name : "", row.objective);
					RE::Console::ExecuteCommand(std::format("setobjectivecompleted {:08X} {} 1", quest->GetFormID(), row.objective).c_str());
					++repaired;
				}
			}
		}

		// Virgil left disabled by an older version in a friend's save past Kellogg's memory (StageRules::FixesVirgil): MQ204's
		// 5 set while the quest runs, past the talk with Amari; Virgil's reference enabled after its end, unless he is dead or
		// cured. Not mid-fight, nor in a conversation or a menu. Asked again at most every VIRGIL_RETRY while it still
		// reads unrepaired (the console runs a moment later), VIRGIL_TRIES times in all, then one line and left alone until
		// the session ends.
		constexpr auto     VIRGIL_RETRY = 10s;
		constexpr unsigned VIRGIL_TRIES = 3;
		Clock::time_point  virgilAskedAt{};
		unsigned           virgilTries = 0;
		void RepairVirgil(Clock::time_point a_now)
		{
			const auto mq204 = RE::TESForm::GetFormByID<RE::TESQuest>(MQ204);
			if (!mq204 || !Official::IsOfficialForm(mq204) || virgilTries > VIRGIL_TRIES) {
				return;
			}
			// The cheap tests first, every poll: 5 done (every game that played the memory through), or MQ204 never reached.
			const bool running = (mq204->data.flags & QUEST_RUNNING) != 0;
			const bool completed = (mq204->data.flags & QUEST_COMPLETED) != 0;
			if (QuestStageLayout::IsStageDone(mq204, StageRules::MQ204_ENABLES_VIRGIL) || (!running && !completed && !QuestStageLayout::IsStageDone(mq204, StageRules::MQ204_END))) {
				return;
			}
			const auto virgil = RE::TESForm::GetFormByID<RE::TESObjectREFR>(VIRGIL_REF);
			const auto actor = virgil ? virgil->As<RE::Actor>() : nullptr;
			const auto dn053 = RE::TESForm::GetFormByID<RE::TESQuest>(DN053);
			StageRules::VirgilState state;
			state.running = running;
			state.completed = completed;
			state.stage = mq204->currentStage;
			state.disabled = actor && actor->IsDisabled();
			state.dead = actor && actor->IsDead(false);
			state.cured = dn053 && QuestStageLayout::IsStageDone(dn053, DN053_CURE);
			const auto fix = StageRules::FixesVirgil(state, [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(mq204, a_stage); });
			if (fix == StageRules::VirgilFix::kNone || busy || Paused() || (virgilTries > 0 && a_now - virgilAskedAt < VIRGIL_RETRY)) {
				return;
			}
			if (virgilTries == VIRGIL_TRIES) {
				++virgilTries;  // given up on: no more tries or lines this session
				REX::WARN("QuestSync: MQ204 at {} without its stage {} still leaves Virgil unrepaired after {} tries; leaving it for this session", mq204->currentStage,
					StageRules::MQ204_ENABLES_VIRGIL, VIRGIL_TRIES);
				return;
			}
			if (fix == StageRules::VirgilFix::kStage5) {
				StageRules::Situation now;
				now.sharedStory = true;
				now.held = combatHold;
				now.opening = Story::InOpening();
				now.friendInOpening = Party::FriendInOpening();
				now.inMemory = Party::InMemory();
				switch (StageRules::Judge(RuleFor(MQ204, StageRules::MQ204_ENABLES_VIRGIL), now)) {
				case StageRules::Verdict::kOpeningWaits:
				case StageRules::Verdict::kMemoryWaits:
				case StageRules::Verdict::kWarn:  // 5 doesn't move the player: never asked to warn first
				case StageRules::Verdict::kLater:
					return;
				default:
					break;
				}
				++virgilTries;
				virgilAskedAt = a_now;
				REX::WARN("QuestSync: MQ204 is at {}, past the talk with Amari, without its stage {} (an older version's catch-up passed it): setting it, which enables Virgil",
					mq204->currentStage, StageRules::MQ204_ENABLES_VIRGIL);
				RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", MQ204, StageRules::MQ204_ENABLES_VIRGIL).c_str());
				FadeWatch::Arm(std::format("set {:08X} stage {}", MQ204, StageRules::MQ204_ENABLES_VIRGIL));
				sceneHold.StageSet(MQ204, a_now);
				++repaired;
				return;
			}
			++virgilTries;
			virgilAskedAt = a_now;
			REX::WARN("QuestSync: MQ204 ended (stage {}) without its stage {}, and Virgil {:08X} is still disabled (alive, not cured): enabling him",
				mq204->currentStage, StageRules::MQ204_ENABLES_VIRGIL, VIRGIL_REF);
			actor->Enable(false);
			++repaired;
		}

		// The session's story starts where the host's is: every stage done in the host's game of the shared
		// quests its player has started or finished (the story's only if it is shared), quest by quest in
		// the game's order and each quest's in ascending order. The server keeps those it doesn't have yet
		// like any of the session's stages, so a friend joining now or later is pushed through them in order
		// (StageLog::Queue: scripts, holds, locks, never backwards), not only through what was done since
		// hosting began. Only the host's game does this: a joiner's progress before joining stays theirs.
		void QueueBaseline()
		{
			baseline.clear();
			std::size_t quests = 0;
			std::size_t left = 0;
			std::size_t byQuickstart = 0;
			for (const auto quest : AllQuests()) {
				if (!quest || !Shareable(quest) || (quest->GetFormID() >> 24) == 0xFF || (IsStory(quest) && !Story::Shared()) || (IsSafe(quest) && !SafeShared())) {
					continue;
				}
				if (!SessionStarted(quest) && !(quest->data.flags & QUEST_COMPLETED)) {
					continue;  // run in the background, not the player's progress
				}
				const auto done = QuestStageLayout::Done(quest);
				if (done.empty()) {
					continue;
				}
				++quests;
				for (const auto stage : done) {
					if (IsOpeningStage(quest, stage)) {
						continue;  // every player's own
					}
					if (IsDebugStage(quest, stage)) {
						continue;  // a quickstart set at the console: this game's only
					}
					if (SetByQuickstart(quest, stage)) {
						++byQuickstart;
						continue;  // ... and so is what its script set: the save has the quickstart done, whenever it was typed
					}
					if (DoorsStage(quest, stage)) {
						continue;  // ScriptDoors'
					}
					if (baseline.size() < MAX_BASELINE) {
						baseline.push_back({ quest->GetFormID(), stage, IsStory(quest), IsSafe(quest) });
					} else {
						++left;
					}
				}
			}
			REX::INFO("QuestSync: the session starts from this game's story: {} stages of {} quests{}{}", baseline.size(), quests,
				left ? std::format(" ({} more left out)", left) : std::string(),
				byQuickstart ? std::format(" ({} not shared: a developer's quickstart set at this game's console set them)", byQuickstart) : std::string());
		}
	}

	bool PlayerStarted(const RE::TESQuest* a_quest)
	{
		return a_quest && std::ranges::any_of(a_quest->objectives, [](const RE::BGSQuestObjective* a_objective) {
			return a_objective && a_objective->state != static_cast<char>(RE::QUEST_OBJECTIVE_STATE::kDormant);
		});
	}

	bool SharesHere(const RE::TESQuest* a_quest)
	{
		return a_quest && Shared(a_quest);
	}

	bool FriendMoved(std::uint32_t a_quest)
	{
		return friendMoved.contains(a_quest);
	}

	void CloseTurnIn(RE::TESQuest* a_quest, std::uint16_t a_stage, bool a_completes, bool a_stops)
	{
		if (!a_quest) {
			return;
		}
		const auto  id = a_quest->GetFormID();
		const auto  flags = a_quest->data.flags;
		const auto  close = RewardBook::CloseTurnIn(a_completes, a_stops, QuestStageLayout::IsStageDone(a_quest, a_stage), (flags & QUEST_COMPLETED) != 0,
			 (flags & QUEST_RUNNING) != 0);
		const auto  now = Clock::now();
		const char* name = a_quest->GetFullName();
		// A friend's, as a stage set here from them: the next look finds it known (not ours to report, nor a turn-in of ours
		// for QuestRewards), and the completion that follows isn't reported as ours.
		remoteStageAt[id] = now;
		friendMoved.insert(id);
		bool marked = false;
		if (close.mark) {
			marked = QuestStageLayout::MarkDone(a_quest, a_stage);
			if (const auto known = doneStages.find(id); known != doneStages.end()) {
				StageLog::Insert(known->second.stages, a_stage);
				known->second.digest.reset();
			}
			// The quest's current stage as SetStage leaves it (a dialogue quest's stages aren't steps in order: left as it is),
			// so a friend's earlier stage still to come is known as passed (IsSettled).
			if (marked && !Unordered(a_quest) && a_stage > a_quest->currentStage) {
				a_quest->currentStage = a_stage;
				a_quest->AddChange(RE::CHANGE_TYPES::kQuestStages);
			}
			knownStages[id] = a_quest->currentStage;
		}
		if (close.complete) {
			a_quest->data.flags |= QUEST_COMPLETED;
			a_quest->AddChange(RE::CHANGE_TYPES::kQuestFlags);
		}
		if (close.stop) {
			RE::Console::ExecuteCommand(std::format("stopquest {:08X}", id).c_str());
		}
		REX::INFO("QuestSync: {:08X} '{}' stage {} is a friend's turn-in whose reward was given here: {}{}{} (its script not run)", id, name ? name : "", a_stage,
			marked ? "marked done" : "done already", close.complete ? ", the quest completed" : "", close.stop ? ", the quest stopped" : "");
	}

	const char* Kind(const RE::TESQuest* a_quest)
	{
		if (a_quest && RadiantSync::ShareOf(a_quest) != RadiantRules::Share::kNotRadiant) {
			if (RadiantSync::ShareOf(a_quest) == RadiantRules::Share::kLeftAlone) {
				return "radiant (each player's own)";
			}
			return RadiantSync::Shares(a_quest->GetFormID()) ? "radiant (this run shared)" : "radiant (by its run)";
		}
		if (a_quest && Official::IsOfficialForm(a_quest) && PerGame(a_quest)) {
			return "radiant (each player's own)";
		}
		if (!a_quest || !Shareable(a_quest)) {
			return "no";
		}
		if (IsFinale(a_quest)) {
			return "finale";
		}
		if (IsDialogue(a_quest)) {
			return "dialogue";
		}
		if (IsSilent(a_quest)) {
			return SafeShared() ? "silent (shared with the story: sQuests = safe)" : "silent (not shared: sQuests = story)";
		}
		if (IsSafe(a_quest)) {
			return SafeShared() ? "safe (shared with the story: sQuests = safe)" : "safe (not shared: sQuests = story)";
		}
		if (IsCompanion(a_quest)) {
			return "companion (a side quest; its owner's stages kEach)";
		}
		return Story::IsStoryType(a_quest->data.questType) ? "story" : "side";
	}

	// Queues a friend's stage to set here (Frame).
	static void Queue(const Protocol::QuestStage& a_stage, bool a_live);

	void Apply(const Protocol::QuestStage& a_stage, bool a_live)
	{
		// A town's scene over placed NPCs is the session's, run by one game (PlacedScenes): its stages are followed there.
		if (PlacedScenes::OnFriendStage(a_stage, a_live)) {
			return;
		}
		// A radiant's stages come by its run (ApplyRadiant): one that comes as the quest's (an older game's, or the
		// session file's from before) is of some run with other targets.
		if (RadiantSync::IsRadiant(a_stage.quest)) {
			return;
		}
		Queue(a_stage, a_live);
	}

	void ApplyRadiant(std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_playerId, bool a_live)
	{
		Queue(Protocol::QuestStage{ a_quest, a_stage, a_playerId }, a_live);
	}

	static void Queue(const Protocol::QuestStage& a_stage, bool a_live)
	{
		if (!Config::Get().syncQuests) {
			return;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		// A side quest whose targets each game picks itself (an older game's report, or a session file's from before).
		if (quest && Official::IsOfficialForm(quest) && PerGame(quest)) {
			if (perGameDropped.insert(a_stage.quest).second) {
				const char* name = quest->GetFullName();
				REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: the story manager picks its targets in each game, so it is each player's own (logged once per quest)",
					a_stage.quest, name ? name : "", a_stage.stage);
			}
			return;
		}
		if (!quest || !Shared(quest) || StageLog::Finished(quest->data.flags) || IsSettled(a_stage.quest, a_stage.stage)) {
			return;
		}
		// The opening is every player's own (Vault 111): a friend's stage of it, from an older version's report or a
		// session file that has one, would pull the player back into the vault's cryopod or start its scenes.
		if (IsOpeningStage(quest, a_stage.stage)) {
			if (openingDropped++ == 0) {
				REX::INFO("QuestSync: not setting {:08X} stage {}: it is the opening's, which every player plays in their own game (and any more like it)",
					a_stage.quest, a_stage.stage);
			}
			return;
		}
		// A developer's quickstart is never shared: a friend's (an older version's report, or a session file that has one)
		// would jump the player through the quest and run a faction's kick-out nobody chose.
		if (IsDebugStage(quest, a_stage.stage)) {
			if (debugDropped++ == 0) {
				REX::INFO("QuestSync: not setting {:08X} stage {}: it is a developer's quickstart, which only the console sets (and any more like it)", a_stage.quest,
					a_stage.stage);
			}
			return;
		}
		// A timer's stage (StageRules::HOST_TIMERS): the host's timer decides for the session, so no game sets a friend's
		// (FFGoodneighbor08's 25 would start this game's own timer, its 200 reads where this game's player is, and its 300
		// would stop the quest here before the host's 1000 came; a friend's game never reports them, an older one might).
		// The host's 1000, the shutdown stage its stop ran (the guards put back), every game takes (TakesTimer).
		if (const auto timer = TimerOf(quest, a_stage.stage); !StageRules::TakesTimer(timer)) {
			if (timerDropped.insert(Key(a_stage.quest, a_stage.stage)).second) {
				const char* name = quest->GetFullName();
				REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: a timer's stage of the quest's own script; the host's timer decides for the session (logged once)",
					a_stage.quest, name ? name : "", a_stage.stage);
			}
			return;
		}
		// A stage this quest doesn't have would only be set, never read done, and hold up the real ones.
		if (!QuestStageLayout::Has(quest, a_stage.stage)) {
			if (unknown++ == 0) {
				REX::WARN("QuestSync: {:08X} has no stage {}; ignoring it (and any more like it)", a_stage.quest, a_stage.stage);
			}
			return;
		}
		// Set by Frame, in the order they came: a session's stages arrive in the order they were reported
		// (all of them for a player who joins late or loads a save).
		if (!waiting.Add(a_stage.quest, a_stage.stage)) {
			if (waiting.Size() >= StageLog::Queue::MAX_WAITING && refused++ == 0) {
				REX::WARN("QuestSync: more than {} stages waiting; dropping some", StageLog::Queue::MAX_WAITING);
			}
			return;
		}
		if (a_live) {
			liveAt[Key(a_stage.quest, a_stage.stage)] = Live{ Clock::now(), a_stage.playerId };
			cameLive.insert(Key(a_stage.quest, a_stage.stage));
		}
		// What came with it, kept until it is set (SetHere writes them first) or skipped.
		if (!a_stage.globals.empty()) {
			auto& values = carriedValues[Key(a_stage.quest, a_stage.stage)];
			values.clear();
			for (const auto& global : a_stage.globals) {
				values.push_back({ global.global, global.value });
			}
		}
	}

	std::vector<std::uint16_t> Waiting(std::uint32_t a_quest)
	{
		return waiting.Waiting(a_quest);
	}

	void ApplyDone(const Protocol::QuestDone& a_done)
	{
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_done.quest);
		const char* name = quest ? quest->GetFullName() : nullptr;
		if (!quest || !Watched(quest) || !Official::IsOfficialForm(quest) || !name || !*name) {
			return;
		}
		REX::INFO("QuestSync: player {} completed {:08X} '{}'", a_done.playerId, a_done.quest, name);
		Hud::Notify(Text::Format("quest.completed", { { "player", RemotePlayers::NameOf(a_done.playerId) }, { "quest", name } }));
	}

	void Frame(bool a_inSession, bool a_hosting)
	{
		const auto now = Clock::now();
		WatchMenus(now);
		if (now < nextPoll) {
			return;
		}
		nextPoll = now + POLL_INTERVAL;
		inSession = a_inSession;
		hosting = a_hosting;

		if (!WorldSync::InWorld()) {
			// A loading screen: no script runs, so no hold runs out.
			Game game{ now };
			waiting.Poll(now, game, true);
			CheckWaived(now, true);
			if (newBody) {
				newBody->last = now;  // and none of a new body's waits: its commands land in play
			}
			return;
		}
		CheckWaived(now, Paused());
		ResumeNewBody(now);
		FinishNewBody(now, Paused());
		// A scene's timer start that never came (TIMER_STARTS): in every game, in a session or not, quest sharing on or off.
		KickTimers(now, Paused());
		// A companion's quest this game's own play took over from a mirror: the owner's alias moves still to be made here.
		AdoptAliases();
		// A mirror's count of the game-time timer an owner's stage's script starts (Curie's day from 200 to 300), which no
		// script runs here: up with the owner gone, the mirror sets the stage itself.
		MirrorTimers(now);
		// Misc quests (radiant ones give XP too) are only watched for stage changes.
		for (const auto quest : AllQuests()) {
			if (quest && quest->data.questType == 6) {
				const auto [it, inserted] = miscStages.try_emplace(quest->GetFormID(), quest->currentStage);
				if (!inserted && it->second != quest->currentStage) {
					it->second = quest->currentStage;
					StageChanged(quest->GetFormID(), now);
				}
			}
		}
		if (!Config::Get().syncQuests) {
			for (const auto quest : AllQuests()) {
				if (quest && Watched(quest)) {
					const auto [it, inserted] = knownStages.try_emplace(quest->GetFormID(), quest->currentStage);
					if (!inserted && it->second != quest->currentStage) {
						it->second = quest->currentStage;
						StageChanged(quest->GetFormID(), now);
					}
				}
			}
			// Nothing is shared, but a settlement attack held back for a session (a save made then, loaded now) is still
			// let go, once the loaded world has settled as for the reports below.
			if (!reportFrom) {
				reportFrom = now + SETTLE_TIME;
			}
			if (now >= *reportFrom) {
				RadiantSync::FrameUnsynced();
			}
			return;
		}

		// Friends' stages are only set during a session (OnSessionEnded drops the rest), except what
		// gives the player their controls back after a lock stage set here.
		if (a_inSession || locked) {
			busy = PlayerBusy(now);
			combatHold = CombatHold(now);
			std::erase_if(liveAt, [&](const auto& a_entry) { return now - a_entry.second.at >= LIVE_FOR; });
			// Offers whose stage was never judged again (its quest went away): long out of date.
			std::erase_if(offers, [&](const auto& a_entry) { return now - a_entry.second.seen >= 5min; });
			Game game{ now };
			const bool paused = Paused();
			CheckReleased(a_inSession);
			hostileWaits.Tick(now, paused);
			// A quest asked for in a skipped mover's place: a pause is not play, so its time starts again after one.
			if (paused) {
				for (auto& asked : skippedStartsAsked) {
					asked.at = now;
				}
			} else {
				CheckSkippedStarts(now);
			}
			const auto gaveUpNow = waiting.Poll(now, game, paused);
			// A held stage whose quest went away (dropped, done by the player, the quest finished): no longer held.
			hostileWaits.Keep([&](std::uint64_t a_key) { return waiting.Contains(static_cast<std::uint32_t>(a_key >> 16), static_cast<std::uint16_t>(a_key & 0xFFFF)); });
			// ... and so for the game's own starts: a stage the queue dropped (given up on, done already, its quest gone) is no longer
			// waited for nor left to set.
			std::erase_if(eventStartedFor, [&](std::uint64_t a_key) { return !waiting.Contains(static_cast<std::uint32_t>(a_key >> 16), static_cast<std::uint16_t>(a_key & 0xFFFF)); });
			std::erase_if(eventStartWaits, [&](const auto& a_entry) { return !waiting.Contains(a_entry.first); });
			// A stage that was to start its quest, and the quest still isn't running here: the game didn't start it (some
			// start only where their place is loaded: Rotten Landfill's fight, Diamond City's entrance scene). The rest of
			// its stages were dropped (StageLog::Queue); the next catch-up (a load, a rejoin) or a friend's live stage of
			// it tries again. It wasn't started here for the session after all.
			const auto& failedStarts = waiting.FailedStarts();
			for (const auto& failed : failedStarts) {
				startedHere.erase(failed.quest);
				catchingUp.erase(failed.quest);
				if (startFailedLogged.insert(failed.quest).second) {
					const auto  quest = RE::TESForm::GetFormByID<RE::TESQuest>(failed.quest);
					const char* name = quest ? quest->GetFullName() : nullptr;
					// A quest the story manager starts from an event (its record's ENAM: DLC01Lair_ChangeLocation's CLOC,
					// DialogueDiamondCityEntrance's LCLD) is refused by setstage on a stopped quest, on purpose, whatever is
					// loaded (QuestStartRules.h): the log names that, not the place. The running game holds the event as its
					// index among the story manager's registered events (Encounters::IsStoryEvent), not as the record's word.
					const auto event = quest ? quest->eventID : Encounters::NO_STORY_EVENT;
					const auto why = Encounters::IsStoryEvent(event) ?
					                     std::format("the story manager starts it from its event (registered event {}), and the game refuses any other start of it", event) :
					                     std::string{ "the game starts some quests only where their place is loaded" };
					REX::INFO("QuestSync: {:08X} '{}' didn't start here when a friend's stage {} was set (still not running {} s later; {}); {} more stage(s) of it not set, "
							  "tried again at the next catch-up (logged once per quest)",
						failed.quest, name ? name : "", failed.stage, std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::TIMEOUT).count(), why, failed.dropped);
				}
			}
			for (const auto& stage : gaveUpNow) {
				++gaveUp;
				if (std::ranges::any_of(failedStarts, [&](const auto& a_failed) { return a_failed.quest == stage.quest && a_failed.stage == stage.stage; })) {
					continue;  // logged above
				}
				if (gaveUpLogged.insert(Key(stage.quest, stage.stage)).second) {
					REX::WARN("QuestSync: {:08X} stage {} still not done {} s after setting it; going on with the next (logged once; gaveUp counts each)", stage.quest, stage.stage,
						std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::TIMEOUT).count());
				}
			}
			// Stages logged as waiting for their quest that a stage set before them did meanwhile (Out of Time's catch-up starts
			// the Museum of Freedom at its 1 and When Freedom Calls at its 20), or the player did: dropped, never set from here.
			// Said, or the log has them waiting for good; and a later wait of the quest is logged again.
			const auto& alreadyDone = waiting.AlreadyDone();
			for (auto it = alreadyDone.begin(); it != alreadyDone.end();) {
				const auto id = it->quest;
				const auto first = it->stage;
				const auto end = std::find_if(it, alreadyDone.end(), [&](const auto& a_stage) { return a_stage.quest != id; });
				const auto dropped = static_cast<std::size_t>(std::distance(it, end));
				it = end;
				if (!waited.contains(id)) {
					continue;
				}
				const bool more = waiting.Contains(id);
				if (!more) {
					waitLogged.erase(id);
				}
				const auto  quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				const char* name = quest ? quest->GetFullName() : nullptr;
				REX::INFO("QuestSync: {} stage(s) of {:08X} '{}' from friends no longer wait (the first {}): already done here, by a stage set before them or by the player{}",
					dropped, id, name ? name : "", first, more ? "; the rest still wait" : "");
			}
			std::erase_if(waited, [](std::uint32_t a_quest) { return !waiting.Contains(a_quest); });
			std::erase_if(catchingUp, [](std::uint32_t a_quest) {
				if (waiting.Contains(a_quest)) {
					return false;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
				const char* name = quest ? quest->GetFullName() : nullptr;
				REX::INFO("QuestSync: caught up on {:08X} '{}' to stage {} (started here for the session)", a_quest, name ? name : "", quest ? quest->currentStage : 0);
				return true;
			});
			FlushCaughtUp(now);
			if (!paused) {
				CheckLock(a_inSession);
			}
		}

		if (!reportFrom) {
			reportFrom = now + SETTLE_TIME;
		}
		const bool report = now >= *reportFrom;

		// The session's story starts from the host's: once its own server has welcomed it (and again after
		// a drop or a load), the host's game reports its progress so far (QueueBaseline).
		if (a_hosting && a_inSession && report && !Story::InOpening() && !baselineQueued) {
			baselineQueued = true;
			QueueBaseline();
		}
		while (!baseline.empty() && outgoing.Size() < BASELINE_BATCH) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(baseline.front().quest);
			const auto stage = quest ? WithCarried(quest, baseline.front().stage) : Protocol::QuestStage{ baseline.front().quest, baseline.front().stage };
			outgoing.Add(Protocol::Encode(stage, Protocol::MessageType::kReportQuestStage), baseline.front().story, baseline.front().safe);
			baseline.pop_front();
			++baselined;
		}

		// A stage a mirror's timer set at this poll (MirrorTimers) is this game's own progress: reported as a stage its play
		// did would be (set here, it is never seen fresh below).
		for (const auto& [id, stage] : std::exchange(mirrorSet, {})) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
			if (!quest || !report || (id >> 24) == 0xFF || !Shareable(quest) || Story::InOpening() || (!SessionStarted(quest) && !startedHere.contains(id)) ||
				OthersMemory(quest, stage)) {
				continue;
			}
			const bool story = IsStory(quest);
			const bool safe = IsSafe(quest);
			if ((story && a_inSession && !Story::Shared()) || (safe && a_inSession && !SafeShared())) {
				continue;
			}
			outgoing.Add(Protocol::Encode(WithCarried(quest, stage), Protocol::MessageType::kReportQuestStage), story, safe);
			++reported;
		}

		// Every poll, not once: the catch-up itself can leave MQ104 past 210 without it (a session
		// file from before 210 was reported), after this check first ran.
		if (a_inSession && report && Story::Shared() && !Story::InOpening()) {
			RepairStory(now);
			RepairVirgil(now);
			CompleteLeftShown(now);
		}

		// Radiants' runs: ours begun, friends' started here, ended ones closed (before the stages done here are looked at,
		// so a run begun now takes the stages done so far with its start).
		if (report) {
			RadiantSync::Frame(a_inSession, a_hosting);
			EncounterRuns::Frame(a_inSession);  // encounters shown by an objective, shared by their run as a radiant is
			PlacedScenes::Frame(a_inSession);   // a town's scene over placed NPCs (Diamond City's accusation), run by one game
		}

		unstarted.Expire(now);
		for (const auto quest : AllQuests()) {
			if (!quest || !Watched(quest)) {
				continue;
			}
			const auto id = quest->GetFormID();

			// Completed here, not because of another player's stage: tell the others who did it.
			const bool done = (quest->data.flags & QUEST_COMPLETED) != 0;
			const auto [doneIt, firstLook] = knownDone.try_emplace(id, done);
			if (!firstLook && doneIt->second != done) {
				doneIt->second = done;
				const auto remote = remoteStageAt.find(id);
				const bool byOther = remote != remoteStageAt.end() && now - remote->second < REMOTE_COMPLETION_WINDOW;
				const char* name = quest->GetFullName();
				// Not one that a console quickstart finished here (its current stage is one the quickstart's script set): that is
				// this game's only, as the stage is, and the friends get no "completed" line for it.
				if (done && report && !byOther && name && *name && (id >> 24) != 0xFF && !Unordered(quest) && !SetByQuickstart(quest, quest->currentStage)) {
					outgoing.Add(Protocol::Encode(Protocol::QuestDone{ 0, id }, Protocol::MessageType::kReportQuestDone), false, IsSafe(quest));
					++completed;
				}
			}

			// Any stage change gives quest XP, not kill XP (see Party).
			const auto [it, inserted] = knownStages.try_emplace(id, quest->currentStage);
			if (!inserted && it->second != quest->currentStage) {
				it->second = quest->currentStage;
				StageChanged(id, now);
			}

			// Every stage done since the last look is progress, not just the current stage: a stage whose
			// script moves the quest on at once (MQ104's 210 stops it) is never seen as the current one,
			// and the others then never ran it, nor what it does (starting MQ105, an achievement).
			// They are read again only when their digest changed (or a stage was set here since), not at each poll of every
			// quest watched.
			const auto digest = QuestStageLayout::DigestOf(quest);
			const auto [known, firstLookStages] = doneStages.try_emplace(id);
			if (firstLookStages) {
				known->second = { QuestStageLayout::Done(quest), digest };
				continue;
			}
			StageLog::Stages fresh;
			if (known->second.digest != digest) {
				fresh = StageLog::NewlyDone(known->second.stages, QuestStageLayout::Done(quest));
				known->second.digest = digest;
			}
			if (!fresh.empty()) {
				StageChanged(id, now);
				// A companion quest's owner's stage (kEach) done by this game's own play: its script ran here, so this game
				// drives the quest (drivenHere). One set here from a friend's is never fresh: NoteSetHere knew it first. Any
				// quest with a driver (StageRules::DRIVERS): the companions' own, and the others' with kEach stages (Cait's).
				const auto questFile = FileOf(quest);
				const auto companionFile = questFile && (IsCompanion(quest) || StageRules::DriverOf(*questFile, id & 0x00FFFFFF)) ? questFile : std::nullopt;
				if (companionFile && std::ranges::any_of(fresh, [&](std::uint16_t a_stage) { return StageRules::IsEach(*companionFile, id & 0x00FFFFFF, a_stage); }) &&
					drivenHere.insert(id).second) {
					REX::INFO("QuestSync: {:08X} '{}': one of its companion's owner's stages ran here by this game's own play; this game drives it until a load", id,
						quest->GetFullName() ? quest->GetFullName() : "");
					// If it mirrored the quest until now, the alias moves of the owner's stages done before this poll are
					// still to be made here (AdoptAliases judges that, and makes them).
					auto moves = StageRules::AdoptMoves(
						*companionFile, id & 0x00FFFFFF, [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(quest, a_stage); },
						[&](std::uint16_t a_stage) { return std::ranges::find(fresh, a_stage) != fresh.end(); });
					if (!moves.empty()) {
						adoptions.insert_or_assign(id, Adoption{ *companionFile, std::move(moves) });
					}
				}
				if (companionFile) {
					for (const auto stage : fresh) {
						RenewBodies(quest, *companionFile, stage);  // her own play's new body: what we kept of her old one goes
					}
				}
			} else if (!unstarted.Has(id)) {
				continue;
			}
			// Only of quests this player has started: the game moves some quests along in the background,
			// and a new game sets up many while the player is still in the opening. What it reached just
			// before it was started goes once it is (StageLog::Unstarted).
			const auto remote = remoteStageAt.find(id);
			const bool reaction = remote != remoteStageAt.end() && now - remote->second < SCRIPT_REACTION_WINDOW;
			// The story's stages whatever the story is now while the connection is being remade: it is the
			// player's own then (Story::SetShared(false)), and these stages are seen done only now. The outbox
			// sends them only if the welcome says the session shares the story (StageLog::Outbox).
			const bool story = IsStory(quest);
			const bool safe = IsSafe(quest);
			// A radiant's stages go by its run, if it is shared (RadiantSync); a stage of it set here from a friend is
			// never seen fresh, and one its script sets in reply goes too (each game takes each stage of a run once).
			if (RadiantSync::IsRadiant(quest)) {
				if (report && !Story::InOpening()) {
					RadiantSync::OnDone(quest, fresh);
				}
				continue;
			}
			if (!report || reaction || (id >> 24) == 0xFF || !Shareable(quest) || (story && a_inSession && !Story::Shared()) || (safe && a_inSession && !SafeShared()) ||
				Story::InOpening()) {
				continue;
			}
			if (!SessionStarted(quest) && !startedHere.contains(id)) {
				unstarted.Add(id, fresh, now);
				continue;
			}
			fresh = unstarted.Take(id, std::move(fresh), now);
			for (const auto stage : fresh) {
				if (IsOpeningStage(quest, stage)) {
					continue;  // the opening is every player's own: a stage of it done here (the vault's 110 outside) isn't reported
				}
				if (IsDebugStage(quest, stage)) {
					// A quickstart set at the console is this game's only: the friends aren't jumped through the quest with it.
					if (debugKept++ == 0) {
						REX::INFO("QuestSync: not reporting {:08X} stage {}: it is a developer's quickstart, set at this game's console (and any more like it)", id, stage);
					}
					continue;
				}
				if (SetByQuickstart(quest, stage)) {
					// ... nor is what its script set there their progress: a kick-out nobody played for stays in this game.
					if (quickstartSet++ == 0) {
						REX::INFO("QuestSync: not reporting {:08X} stage {}: a developer's quickstart set at this game's console set it (and any more like it)", id, stage);
					}
					continue;
				}
				if (DoorsStage(quest, stage)) {
					continue;  // ScriptDoors reports it
				}
				if (OthersMemory(quest, stage)) {
					continue;  // Kellogg's memory's, whose player isn't this one: their game sets it on their walk
				}
				// A timer's stage (StageRules::HOST_TIMERS): only the host's game reports one. A friend's own timer (its own
				// play started the quest's) runs to its own end in its own game and stays there, the shutdown stage its own
				// stop ran included (FFGoodneighbor08's 1000: reported, it would delete Sammy and the guards in the host's
				// game while the host's event still runs).
				if (!StageRules::ReportsTimer(hosting, TimerOf(quest, stage))) {
					if (timerKept.insert(Key(id, stage)).second) {
						const char* name = quest->GetFullName();
						REX::INFO("QuestSync: not reporting {:08X} '{}' stage {}: a timer's stage of the quest's own script, which only the host's game reports (logged once)",
							id, name ? name : "", stage);
					}
					continue;
				}
				outgoing.Add(Protocol::Encode(WithCarried(quest, stage), Protocol::MessageType::kReportQuestStage), story, safe);
				// A payment's stage (a delivery turned in) is never set in the friends' games: what it gave goes to them as a
				// reward (QuestRewards).
				if (const auto rule = RuleFor(id, stage); rule && a_inSession) {
					QuestRewards::OnOwnStage(id, stage, rule->flags);
				}
				++reported;
			}
		}
		// A radiant's run is the story's: a session that doesn't share the story doesn't get it (StageLog::Outbox).
		for (auto& packet : RadiantSync::TakeOutgoing()) {
			outgoing.Add(std::move(packet), true);
		}
		for (auto& packet : EncounterRuns::TakeOutgoing()) {
			outgoing.Add(std::move(packet), true);
		}
		for (auto& packet : PlacedScenes::TakeOutgoing()) {
			outgoing.Add(std::move(packet), true);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending, bool a_inSession)
	{
		// Welcomed, Story::Shared is the welcome's.
		return outgoing.Take(Clock::now(), { a_sending, a_inSession, Story::Shared(), SafeShared() });
	}

	void OnServerChanged()
	{
		outgoing.NewSession();
		cameLive.clear();  // another session: its stages are its own catch-up
		RadiantSync::OnServerChanged();
		EncounterRuns::Clear();
		PlacedScenes::OnServerChanged();
	}

	void OnSessionEnded()
	{
		// Nothing of the session reaches a solo game: no stage still waiting is set, and what other players
		// moved lately belongs to the session. A quest started later in solo play must not be caught up to
		// the session's stage. Except a lock: a stage set here has the player's controls, and its quest goes
		// on until they are given back.
		if (locked) {
			waiting.Keep({ locked->quest, locked->releaseQuest });
		} else {
			waiting.Clear();
		}
		waited.clear();
		liveAt.clear();
		offers.clear();
		hostileWaits.Clear();
		hostilityMade.clear();
		hostileCallTries.clear();
		dismissalsWarned.Clear();
		caughtUpNames.clear();
		warnedAt.reset();
		sceneHold.Clear();  // what was set is forgotten; a scene of the session's the player is still in stays the session's (a reconnect)
		sessionSceneLogged = 0;
		lockWaitLogged.clear();
		openingWaitLogged.clear();
		trapWaitLogged.clear();
		memoryWaitLogged.clear();
		eachWaitLogged.clear();
		eachSettleLogged.clear();
		eventStartWaits.clear();
		eventStartedFor.clear();
		timerWaits.clear();
		eachStarts.clear();
		eachStartsGaveUp.clear();
		skippedStartsAsked.clear();
		leftShownAsked.clear();  // a later session tries again
		virgilTries = 0;
		waitLogged.clear();
		remoteStageAt.clear();
		startedHere.clear();
		friendMoved.clear();
		catchingUp.clear();
		startHolds.clear();
		carriedValues.clear();  // with the waiting stages they came with (a lock's quests carry none)
		carryWaitLogged.clear();
		RadiantSync::OnSessionEnded();  // the radiants go on, each player's own
		EncounterRuns::EndFollowed();   // and the encounters: ours go on, a friend's we follow ends with its actors' copies
		PlacedScenes::OnSessionEnded();  // a town's scene we yielded stops; one we followed leaves its NPCs off
		EncounterRuns::Clear();
		// A host's game reports its progress again once its server welcomes it back (QueueBaseline).
		baseline.clear();
		baselineQueued = false;
		// Reporting goes on without a new settle (not reportFrom.reset): a stage done while the connection is
		// remade is reported once it is back (outgoing), and playing alone the reports are dropped.
	}

	void BeforeSaveOrLoad()
	{
		if (const auto waived = waiver.BeforeSave()) {
			SetGlobal(waived->global, waived->value);
			REX::INFO("QuestSync: before the save or load, the price {:08X} of the friend's purchase ({:08X} stage {}) is {} again", waived->global, waived->quest,
				waived->stage, waived->value);
		}
	}

	void BeforeLoad()
	{
		EndWaived("a load begins");
	}

	void Rebaseline()
	{
		EndWaived("a world is loaded");  // first: nothing is reset with a price still at 0
		knownStages.clear();
		doneStages.clear();
		miscStages.clear();
		knownDone.clear();
		remoteStageAt.clear();
		reportFrom.reset();
		waiting.Clear();
		waited.clear();
		liveAt.clear();
		cameLive.clear();
		offers.clear();
		caughtUpNames.clear();
		warnedAt.reset();
		sceneHold.Clear();
		sessionSceneLogged = 0;
		lockWaitLogged.clear();
		openingWaitLogged.clear();
		trapWaitLogged.clear();
		memoryWaitLogged.clear();
		eachWaitLogged.clear();
		eachSettleLogged.clear();
		eventStartWaits.clear();
		eventStartedFor.clear();
		timerWaits.clear();
		eachStarts.clear();
		eachStartsGaveUp.clear();
		skippedStartsAsked.clear();
		// A new body under way here is dropped: the loaded save's actor is as that save has it. If that save was made while
		// one was under way, its co-save says so and the change is carried on from there (ResumeNewBody).
		DropNewBody();
		resumeBody = bodyShared.Take();
		bodyShared.Set(resumeBody);  // a save made before it is carried on notes it as the loaded one did
		drivenHere.clear();  // the loaded save's marks decide again
		adoptions.clear();
		mirrorCounts.clear();  // counted again from the loaded game's clock
		mirrorSet.clear();
		waitLogged.clear();
		unstarted.Clear();
		hostileWaits.Clear();  // a held stage comes again with the loaded save's catch-up, and holds again if the faction is still near
		hostilityMade.clear();  // the loaded save's factions say what holds (AlreadyEnemy); a call made after it was saved is made again
		hostileCallTries.clear();
		locked.reset();  // the save says whether the player has their controls (waiting.Clear drops the queue's)
		outgoing.Clear();  // progress of the game before the load
		startedHere.clear();
		friendMoved.clear();  // a friend's stage set in the world before the load isn't the loaded save's (what the catch-up sets marks it again)
		catchingUp.clear();
		startHolds.clear();
		carriedValues.clear();
		carryWaitLogged.clear();
		baseline.clear();  // a host's game reports the loaded save's progress instead
		baselineQueued = false;
		RadiantSync::Rebaseline();
		EncounterRuns::Clear();
		PlacedScenes::Rebaseline();
	}

	std::string DriveText(RE::TESQuest* a_quest)
	{
		const auto file = a_quest && Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
		if (!file || !StageRules::DriverOf(*file, a_quest->GetFormID() & 0x00FFFFFF)) {
			return {};
		}
		if (!Companions::AliasesReadable()) {
			return "drive=unsettled";
		}
		StageRules::DriveFacts facts;
		const auto             drive = DriveHere(a_quest, *file, facts);
		return std::format("drive={} marked={} driven={} mirrored={} ours={}", drive == StageRules::Drive::kScripts ? "scripts" : "mirror", facts.marked, facts.driven,
			facts.mirrored, facts.ours);
	}

	std::vector<std::uint32_t> HeldByQuests()
	{
		std::vector<std::uint32_t> out;
		for (const auto& driver : StageRules::DRIVERS) {
			if (!driver.faction || driver.file != static_cast<std::uint8_t>(StageRules::File::kFallout4)) {
				continue;  // Fallout4.esm's form IDs are its own here
			}
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(driver.quest);
			const auto companion = quest ? CompanionOf(quest, driver) : nullptr;
			const auto done = [&](std::uint16_t a_stage) { return QuestStageLayout::IsStageDone(quest, a_stage); };
			if (companion && StageRules::HeldByQuest(driver, done, MarkFactsOf(quest, driver, companion))) {
				out.push_back(companion->GetFormID());
			}
		}
		return out;
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::uint32_t LastStageChangeQuest()
	{
		return lastStageChangeQuest;
	}

	void OnPlayerActivated()
	{
		playerActivatedAt = Clock::now().time_since_epoch().count();
	}

	std::vector<std::uint8_t> SaveBodyRecord()
	{
		return bodyShared.Save();
	}

	void LoadBodyRecord(std::span<const std::uint8_t> a_bytes, const std::function<std::optional<std::uint32_t>(std::uint32_t)>& a_resolve)
	{
		const auto read = BodySave::Decode(a_bytes);
		auto       pending = read ? BodySave::Resolved(*read, a_resolve) : std::nullopt;
		if (pending) {
			REX::INFO("QuestSync: the loaded save notes a new body under way for {:08X} (step {})", pending->ref, static_cast<int>(pending->phase));
		} else {
			REX::WARN("QuestSync: the loaded save's note of a new body under way is left ({} bytes: {})", a_bytes.size(),
				read ? "its companion, or the body for her items, is of a plugin no longer loaded" : "not readable");
		}
		bodyShared.Load(std::move(pending));
	}

	void RevertBodyRecord()
	{
		bodyShared.Revert();
	}

	std::uint32_t SessionScene()
	{
		return sceneHold.SessionScene();
	}

	std::string Describe()
	{
		return std::format("quests: reported={} baseline={} applied={} caughtUp={} startedHere={} waiting={} setting={} gaveUp={} skipped={} openingStages={} debugStages={} debugKept={} quickstartSet={} othersMemory={} unknown={} refused={} completed={} repaired={} released={} waivedPrices={}{} eachStages={} drivenStages={} newBodies={} bodiesResumed={} adopted={} mirrorTimers={} hostileHeld={} hostileWaiting={} hostileApplied={} unsent={} unstarted={}{}",
			reported, baselined, applied, caughtUp, startedHere.size(), waiting.Size(), waiting.Setting(), gaveUp, skipped, openingDropped, debugDropped, debugKept, quickstartSet, othersMemory, unknown, refused, completed, repaired, released,
			waivedPrices, waiver.On() ? " (one waived now)" : "", eachStages, drivenStages, newBodies, bodiesResumed, adopted, mirrorTimers, hostileHeld, hostileWaits.Size(), hostileApplied, outgoing.Size(), unstarted.Size(), locked ? " locked" : "") +
		       std::format(" carriedSent={} carriedSet={} carriedSkipped={} carriedDropped={} carriedWaiting={}", carriedSent, carriedSet, carriedSkipped, carriedDropped, carriedValues.size()) +
		       std::format(" menusSent={} menusCancelled={} menusHidden={} menusGaveUp={} menusLeft={}", menuGuard.sent, menuGuard.cancelled, menuGuard.hidden,
			           menuGuard.gaveUp, menuGuard.left) +
		       (sceneHold.SessionScene() ? std::format(" sessionScene={:08X}", sceneHold.SessionScene()) : std::string()) + " " + RadiantSync::Describe() + " " + EncounterRuns::Describe() + " " + PlacedScenes::Describe();
	}
}
