#include "game/QuestRewards.h"

#include "RewardBook.h"
#include "Text.h"
#include "game/Gifts.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/Party.h"
#include "game/QuestStageLayout.h"
#include "game/QuestSync.h"
#include "game/RemotePlayers.h"
#include "game/RewardLines.h"
#include "game/StageLog.h"
#include "game/StageRules.h"
#include "game/Storage.h"
#include "game/Story.h"
#include "game/WorldSync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace QuestRewards
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		// A pickup is the item the player activated, its gain a moment after: not a script's.
		constexpr auto PICKUP_PAIRING = 2s;
		// Reports kept while not welcomed (reconnecting), oldest dropped past this.
		constexpr std::size_t MAX_OUTBOX = 64;
		// Friends' rewards waiting for our player to be in the world (a load, the opening): never expired, only capped.
		constexpr std::size_t MAX_PENDING = 256;

		struct LineInfo
		{
			std::uint32_t                  quest = 0;
			std::vector<RewardBook::Want> wants;
		};

		// RewardLines' rows by the line's form ID in this game (built once the game's files are loaded).
		std::unordered_map<std::uint32_t, LineInfo> lines;
		// RewardLines::TURN_INS' ends (kCompletes, kStops) by the quest's form ID in this game << 16 | the stage.
		std::unordered_map<std::uint64_t, std::uint8_t> turnIns;
		// COMPANION_QUESTS by their form IDs in this game: never shared as rewards.
		std::unordered_set<std::uint32_t>            companionQuests;
		// OWN_SESSIONS by their form IDs in this game: each player's own sessions, never shared as rewards.
		std::unordered_set<std::uint32_t>            ownSessions;
		bool                                         linesBuilt = false;

		std::uint64_t TurnInKey(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			return (std::uint64_t{ a_quest } << 16) | a_stage;
		}

		std::uint8_t EndsOf(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto found = turnIns.find(TurnInKey(a_quest, a_stage));
			return found != turnIns.end() ? found->second : 0;
		}

		RewardBook::Capture  capture;
		RewardBook::Receiver receiver;

		struct Gained
		{
			std::uint32_t     form = 0;
			std::int32_t      count = 0;
			Clock::time_point at{};
		};
		struct Activated
		{
			std::uint32_t     base = 0;
			Clock::time_point at{};
		};
		std::mutex             inboxLock;
		std::vector<Gained>    gainInbox;
		std::deque<Activated>  activations;  // our player's, the last few seconds
		std::atomic<int>       applying{ 0 };  // giving a friend's reward: those gains are not ours to claim

		struct Pending
		{
			Protocol::QuestReward reward;
			std::optional<Clock::time_point> heldSince;  // first held for a friend's stage of its quest still waiting (RewardBook::Hold)
		};

		std::deque<std::vector<std::uint8_t>>  outbox;
		std::deque<Pending>                    pending;  // friends' rewards to give once our player is in the world
		std::optional<std::uint64_t>           characterBefore;  // the character of the world a load left (BeforeLoad)

		std::uint32_t claimedLines = 0;
		std::uint32_t claimedStages = 0;
		std::uint32_t given = 0;
		std::uint32_t passed = 0;  // judged and not given (the verdict says why)
		std::uint32_t dropped = 0;

		void BuildLines()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (linesBuilt || !handler) {
				return;
			}
			linesBuilt = true;
			for (const auto& local : COMPANION_QUESTS) {
				if (const auto quest = handler->LookupFormID(local.quest, StageRules::FileName(static_cast<StageRules::File>(local.file)))) {
					companionQuests.insert(quest);
				}
			}
			for (const auto& local : OWN_SESSIONS) {
				if (const auto quest = handler->LookupFormID(local.quest, StageRules::FileName(static_cast<StageRules::File>(local.file)))) {
					ownSessions.insert(quest);
				}
			}
			std::size_t missing = 0;
			std::size_t companions = 0;  // rows of a companion's quest: left out (COMPANION_QUESTS)
			for (const auto& row : RewardLines::ROWS) {
				if (IsCompanionQuest(row.questFile, row.quest)) {
					++companions;
					continue;
				}
				const auto info = handler->LookupFormID(row.info, StageRules::FileName(static_cast<StageRules::File>(row.infoFile)));
				const auto quest = handler->LookupFormID(row.quest, StageRules::FileName(static_cast<StageRules::File>(row.questFile)));
				const auto item = handler->LookupFormID(row.item, StageRules::FileName(static_cast<StageRules::File>(row.itemFile)));
				if (info == 0 || quest == 0 || item == 0) {
					++missing;  // an add-on this game doesn't have
					continue;
				}
				auto& line = lines[info];
				line.quest = quest;
				line.wants.push_back({ item, row.count });
			}
			for (const auto& row : RewardLines::TURN_INS) {
				if (IsCompanionQuest(row.file, row.quest)) {
					++companions;
					continue;
				}
				if (const auto quest = handler->LookupFormID(row.quest, StageRules::FileName(static_cast<StageRules::File>(row.file)))) {
					turnIns[TurnInKey(quest, row.stage)] = row.ends;
				}
			}
			REX::INFO("QuestRewards: {} dialogue lines that give a reward ({} rows not in this game's files), {} turn-in stages; {} rows of {} companions' quests left out",
				lines.size(), missing, turnIns.size(), companions, companionQuests.size());
		}

		// Menus whose items come from nowhere and are no quest's: crafting, cooking, building, trading, a container's.
		bool CraftingOrTrading()
		{
			using OpenMenus::Menu;
			return OpenMenus::AnyOpen({ Menu::kExamine, Menu::kCooking, Menu::kWorkshop, Menu::kRobotMod, Menu::kPowerArmorMod, Menu::kBarter, Menu::kContainer });
		}

		class ContainerSink :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Into our player from nowhere: a script's AddItem, or a pickup (told apart by the activation). Not the session's
				// own (shared storage, a gift's copy, a friend's reward given here), nor what a menu makes.
				if (a_event.newContainerFormID != Protocol::PLAYER_REF_ID || a_event.oldContainerFormID != 0 || a_event.itemCount <= 0 ||
					a_event.itemCount > Protocol::MAX_ITEM_COUNT || applying.load() > 0 || Storage::Applying() || Gifts::Applying() || CraftingOrTrading()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				gainInbox.push_back({ a_event.baseObjectFormID, a_event.itemCount, Clock::now() });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class ActivateSink :
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				const auto who = a_event.actionRef.get();
				const auto what = a_event.objectActivated.get();
				const auto base = what ? what->GetObjectReference() : nullptr;
				if (!who || who->GetFormID() != Protocol::PLAYER_REF_ID || !base) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				activations.push_back({ base->GetFormID(), Clock::now() });
				while (activations.size() > 32) {
					activations.pop_front();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ContainerSink containerSink;
		ActivateSink  activateSink;
		bool          installed = false;

		// The gains since the last frame that no activation of the same item explains, into the capture.
		void TakeGains()
		{
			std::vector<Gained> gains;
			std::vector<Activated> recent;
			{
				std::scoped_lock l{ inboxLock };
				gains.swap(gainInbox);
				const auto now = Clock::now();
				while (!activations.empty() && now - activations.front().at > PICKUP_PAIRING * 2) {
					activations.pop_front();
				}
				recent.assign(activations.begin(), activations.end());
			}
			for (const auto& gain : gains) {
				const bool pickup = std::ranges::any_of(recent, [&](const Activated& a_activated) {
					const auto gap = gain.at - a_activated.at;
					return a_activated.base == gain.form && gap >= -PICKUP_PAIRING && gap <= PICKUP_PAIRING;
				});
				if (!pickup) {
					capture.Gain(gain.form, gain.count, gain.at);
				}
			}
		}

		// The line an actor is saying now, if it is one of the table's.
		const LineInfo* Saying(RE::Actor* a_actor, std::uint32_t& a_info)
		{
			if (!a_actor || !a_actor->IsTalking()) {
				return nullptr;
			}
			const auto process = a_actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			const auto item = high ? high->greetTopic : nullptr;
			const auto info = item ? item->info : nullptr;
			if (!info) {
				return nullptr;
			}
			const auto found = lines.find(info->GetFormID());
			if (found == lines.end()) {
				return nullptr;
			}
			a_info = info->GetFormID();
			return &found->second;
		}

		// Our player's conversation: the line they or who they talk to say, if it gives a reward of a quest the session shares.
		void WatchLines(Clock::time_point a_now)
		{
			if (lines.empty()) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto topics = RE::MenuTopicManager::GetSingleton();
			RE::NiPointer<RE::TESObjectREFR> speakerRef;
			if (topics && topics->menuOpen) {
				speakerRef = topics->speaker.get();
			}
			const auto speaker = speakerRef ? speakerRef->As<RE::Actor>() : nullptr;
			for (RE::Actor* actor : { static_cast<RE::Actor*>(player), speaker }) {
				std::uint32_t info = 0;
				const auto    line = Saying(actor, info);
				if (!line) {
					continue;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(line->quest);
				if (!quest || !QuestSync::SharesHere(quest)) {
					continue;
				}
				capture.Line(line->quest, info, line->wants, a_now);
			}
		}

		void Report(const RewardBook::Claimed& a_claimed, Clock::time_point a_now)
		{
			receiver.Reported(a_claimed, a_now);
			if (a_claimed.kind == RewardBook::Kind::kLine) {
				++claimedLines;
			} else {
				++claimedStages;
			}
			std::string items;
			for (const auto& item : a_claimed.items) {
				items += std::format("{}{:08X} x{}", items.empty() ? "" : ", ", item.form, item.count);
			}
			REX::INFO("QuestRewards: our player turned in {:08X} ({} {}): {} and {} XP, for the friends' games", a_claimed.quest,
				a_claimed.kind == RewardBook::Kind::kLine ? "line" : "stage", a_claimed.source, items.empty() ? "no items" : items, a_claimed.xp);
			outbox.push_back(Protocol::Encode(Protocol::QuestReward{ 0, 0, a_claimed.kind, a_claimed.quest, a_claimed.source, a_claimed.xp, a_claimed.items }));
			while (outbox.size() > MAX_OUTBOX) {
				outbox.pop_front();
				++dropped;
			}
		}

		// What a reward says to our player: the items' names and counts, and its XP.
		std::string WhatOf(const Protocol::QuestReward& a_reward)
		{
			std::string out;
			for (const auto& item : a_reward.items) {
				const auto  form = RE::TESForm::GetFormByID(item.form);
				const auto  full = form ? form->As<RE::TESFullName>() : nullptr;
				const char* name = full ? full->GetFullName() : nullptr;
				if (!name || !*name) {
					continue;
				}
				out += (out.empty() ? "" : ", ") + (item.count > 1 ? std::format("{} ({})", name, item.count) : std::string{ name });
			}
			if (a_reward.xp > 0) {
				out += (out.empty() ? "" : ", ") + Text::Format("quest.reward_xp", { { "n", a_reward.xp } });
			}
			return out;
		}

		// A form a player can carry: never a leveled list, nor a bound object that is no item (a static, an activator, an
		// NPC), which our own claims never hold but another game's report might.
		bool InventoryItem(const RE::TESForm* a_form)
		{
			switch (a_form->GetFormType()) {
			case RE::ENUM_FORM_ID::kMISC:
			case RE::ENUM_FORM_ID::kALCH:
			case RE::ENUM_FORM_ID::kAMMO:
			case RE::ENUM_FORM_ID::kBOOK:
			case RE::ENUM_FORM_ID::kWEAP:
			case RE::ENUM_FORM_ID::kARMO:
			case RE::ENUM_FORM_ID::kNOTE:
			case RE::ENUM_FORM_ID::kKEYM:
			case RE::ENUM_FORM_ID::kINGR:
				return true;
			default:
				return false;
			}
		}

		void Give(const Protocol::QuestReward& a_reward)
		{
			const char* kind = a_reward.kind == RewardBook::Kind::kLine ? "line" : "stage";
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_reward.quest);
			const bool stageDone = quest && a_reward.kind == RewardBook::Kind::kStage && QuestStageLayout::IsStageDone(quest, static_cast<std::uint16_t>(a_reward.source));
			const bool finishedOnOwn = quest && (quest->data.flags & StageLog::QUEST_COMPLETED) != 0 && !QuestSync::FriendMoved(a_reward.quest);
			const auto verdict = receiver.Judge(a_reward, stageDone, finishedOnOwn, Clock::now());
			if (verdict != RewardBook::Verdict::kGive) {
				++passed;
				REX::INFO("QuestRewards: player {}'s reward {} ({:08X} {} {}) not given: {}", a_reward.playerId, a_reward.id, a_reward.quest, kind, a_reward.source,
					RewardBook::Why(verdict));
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::uint32_t items = 0;
			++applying;
			for (const auto& item : a_reward.items) {
				const auto object = RE::TESForm::GetFormByID<RE::TESBoundObject>(item.form);
				if (!object || !InventoryItem(object)) {
					REX::WARN("QuestRewards: reward {}'s item {:08X} is no inventory item here{}; left out", a_reward.id, item.form,
						object ? std::format(" (form type {})", std::to_underlying(object->GetFormType())) : std::string{});
					continue;
				}
				player->AddObjectToContainer(object, nullptr, item.count, nullptr, RE::ITEM_REMOVE_REASON::kNone);
				++items;
			}
			--applying;
			// Quest XP, as the game gives it: never a kill's, so never shared on nor topped up (CatchUpXp is for kills).
			if (a_reward.xp > 0) {
				Party::GiveQuestXp(a_reward.xp);
			}
			++given;
			REX::INFO("QuestRewards: player {}'s reward {} ({:08X} {} {}) given here: {} item(s), {} XP", a_reward.playerId, a_reward.id, a_reward.quest, kind,
				a_reward.source, items, a_reward.xp);
			// A turn-in stage's: closed here as the stage would close it, so our player can't turn the same in and get it again.
			// (Not one whose quest allows repeated stages: no game claims those, and marking it done wouldn't close it.)
			if (quest && a_reward.kind == RewardBook::Kind::kStage && (EndsOf(a_reward.quest, static_cast<std::uint16_t>(a_reward.source)) & RewardLines::kRepeats) == 0) {
				const auto stage = static_cast<std::uint16_t>(a_reward.source);
				const auto ends = EndsOf(a_reward.quest, stage);
				QuestSync::CloseTurnIn(quest, stage, (ends & RewardLines::kCompletes) != 0, (ends & RewardLines::kStops) != 0);
			}
			const auto what = WhatOf(a_reward);
			if (!what.empty()) {
				const auto name = RemotePlayers::NameOf(a_reward.playerId);
				Hud::Notify(Text::Format("quest.reward", { { "player", name.empty() ? Text::Get("party.a_friend") : name }, { "what", what } }));
			}
		}

		// A friend's rewards, once our player is in the world and alive (not a load, the opening, a menu that stops the game).
		void GivePending()
		{
			if (pending.empty() || !WorldSync::InWorld() || Story::InOpening()) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || player->IsDead(false)) {
				return;
			}
			// One our player is turning in now too (their own line or stage still settling) waits for it: their report, if
			// any, comes first and makes it a twin (RewardBook::Capture::IsOpen). A turn-in stage's waits for the friends' stages
			// of its quest still waiting here that closing it would drop (RewardBook::Hold::kEarlierStage). The rest go in order.
			const auto now = Clock::now();
			std::deque<Pending> held;
			while (!pending.empty()) {
				auto next = std::move(pending.front());
				pending.pop_front();
				const auto& reward = next.reward;
				std::vector<std::uint16_t> stages;
				std::uint8_t ends = 0;
				if (reward.kind == RewardBook::Kind::kStage) {
					stages = QuestSync::Waiting(reward.quest);
					ends = EndsOf(reward.quest, static_cast<std::uint16_t>(reward.source));
				}
				const auto hold = RewardBook::HoldOf(reward.kind, reward.source, capture.IsOpen(reward.kind, reward.quest, reward.source), stages,
					(ends & (RewardLines::kCompletes | RewardLines::kStops)) != 0, next.heldSince ? now - *next.heldSince : Clock::duration::zero());
				if (hold == RewardBook::Hold::kEarlierStage && !next.heldSince) {
					next.heldSince = now;
					REX::INFO("QuestRewards: player {}'s reward {} ({:08X} stage {}) waits: {} ({} waiting, the first {})", reward.playerId, reward.id, reward.quest,
						reward.source, RewardBook::Why(hold), stages.size(), stages.empty() ? 0 : stages.front());
				}
				if (hold != RewardBook::Hold::kNone) {
					held.push_back(std::move(next));
					continue;
				}
				if (next.heldSince && now - *next.heldSince >= RewardBook::EARLIER_STAGE_HOLD) {
					REX::WARN("QuestRewards: player {}'s reward {} ({:08X} stage {}) held {} min for {} stage(s) of its quest still waiting; given now", reward.playerId,
						reward.id, reward.quest, reward.source, std::chrono::duration_cast<std::chrono::minutes>(now - *next.heldSince).count(), stages.size());
				}
				Give(reward);
			}
			pending = std::move(held);
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		installed = true;
		if (const auto source = RE::TESContainerChangedEvent::GetEventSource()) {
			source->RegisterSink(&containerSink);
		}
		if (const auto source = RE::TESActivateEvent::GetEventSource()) {
			source->RegisterSink(&activateSink);
		}
	}

	void Frame(bool a_inSession)
	{
		const auto now = Clock::now();
		TakeGains();
		if (!a_inSession) {
			capture.Clear();
			return;
		}
		BuildLines();
		WatchLines(now);
		for (const auto& claimed : capture.Poll(now)) {
			Report(claimed, now);
		}
		GivePending();
	}

	void OnOwnStage(std::uint32_t a_quest, std::uint16_t a_stage, std::uint8_t a_ruleFlags)
	{
		if ((a_ruleFlags & StageRules::kPays) == 0) {
			return;
		}
		BuildLines();
		// A companion's quest (MacCready's hire): the pickups around it are our player's alone, and a friend's game must not
		// mark the stage done (COMPANION_QUESTS).
		if (companionQuests.contains(a_quest)) {
			REX::INFO("QuestRewards: our player reached {:08X} stage {}, a companion's quest: not shared", a_quest, a_stage);
			return;
		}
		// Each player's own sessions (the Hubologists' donations, rads and rank: OWN_SESSIONS): what our player got there is
		// theirs alone, and a friend's game must not mark the stage done, or that player's own session would never run.
		if (ownSessions.contains(a_quest)) {
			REX::INFO("QuestRewards: our player reached {:08X} stage {}, each player's own sessions: not shared", a_quest, a_stage);
			return;
		}
		const auto ends = EndsOf(a_quest, a_stage);
		// Its quest allows repeated stages: a friend's game couldn't close it (the engine runs a stage done already again), so
		// its player could turn it in too and get it twice. Not claimed (each player's own, as before rewards were shared).
		if ((ends & RewardLines::kRepeats) != 0) {
			REX::INFO("QuestRewards: our player turned in {:08X} (stage {}), a quest that allows repeated stages: not shared", a_quest, a_stage);
			return;
		}
		capture.Stage(a_quest, a_stage, (ends & RewardLines::kCompletes) != 0, Clock::now());
	}

	void OnOwnXp(float a_xp)
	{
		capture.Xp(a_xp, Clock::now());
	}

	void OnReward(const Protocol::QuestReward& a_reward)
	{
		// A companion's quest (from a build that claimed one): nothing given, and above all its stage not marked done here (it
		// would say this game hired the companion). Passed at once, not held behind its quest's stages.
		BuildLines();
		if (companionQuests.contains(a_reward.quest)) {
			++passed;
			REX::INFO("QuestRewards: player {}'s reward {} ({:08X} {} {}) not given: a companion's quest, never shared", a_reward.playerId, a_reward.id,
				a_reward.quest, a_reward.kind == RewardBook::Kind::kLine ? "line" : "stage", a_reward.source);
			return;
		}
		// The same for each player's own sessions (OWN_SESSIONS).
		if (ownSessions.contains(a_reward.quest)) {
			++passed;
			REX::INFO("QuestRewards: player {}'s reward {} ({:08X} {} {}) not given: each player's own sessions, never shared", a_reward.playerId, a_reward.id,
				a_reward.quest, a_reward.kind == RewardBook::Kind::kLine ? "line" : "stage", a_reward.source);
			return;
		}
		if (pending.size() >= MAX_PENDING) {
			++dropped;
			REX::WARN("QuestRewards: {} friends' rewards waiting already; reward {} dropped", pending.size(), a_reward.id);
			return;
		}
		pending.push_back({ a_reward, std::nullopt });
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending)
	{
		std::vector<std::vector<std::uint8_t>> out;
		if (!a_sending) {
			return out;
		}
		while (!outbox.empty()) {
			out.push_back(std::move(outbox.front()));
			outbox.pop_front();
		}
		return out;
	}

	void OnSessionEnded()
	{
		capture.Clear();
	}

	void OnServerChanged()
	{
		// What waits from the old server goes (rare: a reward waits only through a load). The ids judged stay under their run
		// (Receiver::NewServer): another server's run clears them at its first reward, and those a save's record gave, read
		// before the join (a restart after a crash), are what this world has had of that server's run.
		capture.Clear();
		receiver.NewServer();
		pending.clear();
		outbox.clear();
	}

	void BeforeLoad(std::optional<std::uint64_t> a_character)
	{
		characterBefore = a_character;
	}

	void OnGameLoaded(bool a_newGame, std::uint64_t a_character)
	{
		// A turn-in of the world left behind is not the loaded one's: reported after the load, our player's turn-in of it again
		// past SAME_TURN_IN would give the friends its reward twice.
		capture.Clear();
		{
			std::scoped_lock l{ inboxLock };
			gainInbox.clear();
			activations.clear();
		}
		const auto before = std::exchange(characterBefore, std::nullopt);
		if (!pending.empty() && !RewardBook::KeepsPending(before, a_character, a_newGame)) {
			dropped += static_cast<std::uint32_t>(pending.size());
			REX::INFO("QuestRewards: {} friends' reward(s) not yet given here dropped: {}", pending.size(),
				a_newGame ? "a new game began" : "the loaded save is another character's");
			pending.clear();
		}
	}

	std::vector<std::uint8_t> SaveRecord()
	{
		return receiver.SaveRecord();
	}

	void LoadRecord(std::span<const std::uint8_t> a_bytes)
	{
		if (!receiver.LoadRecord(a_bytes)) {
			REX::WARN("QuestRewards: the save's record of the friends' rewards judged here can't be read ({} bytes); none taken as had", a_bytes.size());
			return;
		}
		REX::INFO("QuestRewards: the save's record names {} friends' reward(s) judged here", receiver.Judged());
	}

	void RevertRecord()
	{
		receiver.LoadRecord({});  // a save without the record: none judged in its world
	}

	std::string Describe()
	{
		const auto heldForStages = std::ranges::count_if(pending, [](const Pending& a_pending) { return a_pending.heldSince.has_value(); });
		return std::format("rewards: lines={} claimedLines={} claimedStages={} given={} passed={} waiting={} heldForStages={} dropped={}", lines.size(), claimedLines,
			claimedStages, given, passed, pending.size(), heldForStages, dropped);
	}
}
