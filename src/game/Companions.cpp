#include "game/Companions.h"

#include "Text.h"
#include "game/CompanionRules.h"
#include "game/Dialogue.h"
#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/NpcSync.h"
#include "game/Papyrus.h"
#include "game/RemotePlayers.h"
#include "game/SessionDeaths.h"
#include "game/Story.h"

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace Companions
{
	namespace
	{
		using Clock = CompanionRules::Clock;
		using Slot = CompanionRules::Slot;

		// The game's Followers quest (Fallout4.esm), whose aliases hold the player's companion and Dogmeat, and whose
		// FollowersScript recruits and dismisses them.
		constexpr std::uint32_t FOLLOWERS_QUEST = 0x000289E4;
		constexpr std::string_view COMPANION_ALIAS = "Companion";
		constexpr std::string_view DOGMEAT_ALIAS = "DogmeatCompanion";
		constexpr std::string_view SCRIPT = "FollowersScript";
		// The script's quiet dismissal: the companion (or Dogmeat), no message, no settlement menu (they go home).
		constexpr std::string_view CLEAR_COMPANION = "ClearCompanion";
		constexpr std::string_view DISMISS_DOGMEAT = "DismissDogmeatCompanion";

		constexpr auto LOOK_INTERVAL = std::chrono::milliseconds(500);

		CompanionRules::Book                   book;
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      lastLook{};
		// The aliases' ids, found by name once (aliasesFound: looked). An id is any value, 0 among them (the Companion
		// alias is 0); an alias not in the quest is nullopt.
		bool                            aliasesFound = false;
		CompanionRules::FollowerAliases aliasIds;
		bool                            questMissingLogged = false;
		std::uint32_t                   dismissCallsRefused = 0;  // calls the VM wouldn't queue
		std::uint32_t                   dismissCallsQueued = 0;   // calls queued; the VM's answers to them below
		std::uint32_t                   unsettledLooks = 0;       // looks skipped: a save or load on, a loading screen, settling after a load, no cell

		// What the VM said of a dismissal it was given, on one of its threads: it ran the function (returned), or cancelled
		// the call. Frame logs each with what the alias holds then, so a dismissal that runs and still leaves the companion
		// in the alias says so in the log, as one that never runs does by its missing answer (queued above the answers).
		struct DismissAnswer
		{
			std::uint32_t id = 0;
			Slot          slot = Slot::kCompanion;
			bool          ran = false;
		};
		std::mutex                  answersLock;
		std::vector<DismissAnswer>  answers;
		std::uint32_t               dismissCallsRan = 0;
		std::uint32_t               dismissCallsCancelled = 0;
		constexpr std::size_t       MAX_ANSWERS = 16;

		class DismissDone final :
			public RE::BSScript::IStackCallbackFunctor
		{
		public:
			DismissDone(std::uint32_t a_id, Slot a_slot) :
				answer{ a_id, a_slot, false }
			{}

			void CallQueued() override {}
			void CallCanceled() override { Give(false); }
			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}
			void operator()(RE::BSScript::Variable) override { Give(true); }

		private:
			void Give(bool a_ran)
			{
				if (given.exchange(true)) {
					return;
				}
				answer.ran = a_ran;
				std::scoped_lock l{ answersLock };
				if (answers.size() < MAX_ANSWERS) {
					answers.push_back(answer);
				}
			}

			DismissAnswer     answer;
			std::atomic<bool> given{ false };
		};

		RE::TESQuest* FollowersQuest()
		{
			return RE::TESForm::GetFormByID<RE::TESQuest>(FOLLOWERS_QUEST);
		}

		bool FindAliases()
		{
			if (aliasesFound) {
				return aliasIds.Any();
			}
			const auto quest = FollowersQuest();
			if (!quest) {
				if (!questMissingLogged) {
					questMissingLogged = true;
					REX::WARN("Companions: the Followers quest ({:08X}) is not in this game; companions are not shared", FOLLOWERS_QUEST);
				}
				return false;
			}
			std::vector<CompanionRules::AliasEntry> entries;
			for (const auto alias : quest->aliases) {
				if (alias) {
					entries.push_back({ alias->aliasName.c_str(), alias->aliasID });
				}
			}
			aliasIds = CompanionRules::FindFollowerAliases(entries, COMPANION_ALIAS, DOGMEAT_ALIAS);
			aliasesFound = true;
			if (!aliasIds.Any()) {
				REX::WARN("Companions: the Followers quest has no {} or {} alias; companions are not shared", COMPANION_ALIAS, DOGMEAT_ALIAS);
				return false;
			}
			REX::INFO("Companions: the Followers quest's aliases: {} = {}, {} = {}", COMPANION_ALIAS, CompanionRules::AliasText(aliasIds.companion), DOGMEAT_ALIAS,
				CompanionRules::AliasText(aliasIds.dogmeat));
			return true;
		}

		// The actor an alias holds, if any (nullopt: no such alias in this game's quest; an id of 0 is a real alias).
		RE::Actor* Aliased(RE::TESQuest* a_quest, const std::optional<std::uint32_t>& a_alias)
		{
			if (!a_alias) {
				return nullptr;
			}
			const auto ref = a_quest->GetAliasedRef(*a_alias).get();
			return ref ? ref->As<RE::Actor>() : nullptr;
		}

		// What the aliases hold now: plugins' NPCs alone (a robot built at a workbench is a runtime reference of this game
		// alone, its builder's by construction).
		std::vector<CompanionRules::Held> HeldNow(RE::TESQuest* a_quest)
		{
			std::vector<CompanionRules::Held> held;
			const auto add = [&](RE::Actor* a_actor, Slot a_slot) {
				if (a_actor && Protocol::IsShareableRef(a_actor->GetFormID()) && !a_actor->IsDeleted()) {
					held.push_back({ a_actor->GetFormID(), a_slot });
				}
			};
			add(Aliased(a_quest, aliasIds.companion), Slot::kCompanion);
			add(Aliased(a_quest, aliasIds.dogmeat), Slot::kDogmeat);
			return held;
		}

		RE::Actor* LoadedActor(std::uint32_t a_id)
		{
			const auto form = RE::TESForm::GetFormByID(a_id);
			return form ? form->As<RE::Actor>() : nullptr;
		}

		std::string NameOf(std::uint32_t a_id)
		{
			const auto actor = LoadedActor(a_id);
			const auto name = actor ? actor->GetDisplayFullName() : nullptr;
			return name && *name ? std::string{ name } : std::string{ "?" };
		}

		std::string PlayerName(std::uint32_t a_playerId)
		{
			const auto name = RemotePlayers::NameOf(a_playerId);
			return name.empty() ? Text::Get("companion.someone") : name;
		}

		// Whether our player is in the NPC's conversation (a dismissal then would leave the menu open on an NPC whose
		// scripts are busy): the recruit's own dialogue, first of all.
		bool Busy(RE::Actor* a_actor)
		{
			return Dialogue::Holds(a_actor) || NpcSync::TalkingTo() == a_actor->GetFormID();
		}

		// The script's quiet dismissal, queued to the script VM (it runs a frame or more later), as the game's own
		// FollowersScript has it (read off the game's scripts, FollowersScript.pex):
		//   DismissDogmeatCompanion(Bool ShowLocationAssignmentListIfAvailable, Bool SuppressDismissMessage): whoever the
		//   DogmeatCompanion alias holds. With no list and the message suppressed it runs only ClearDogmeatCompanion.
		//   ClearCompanion(): whoever the Companion alias holds. It is what DismissCompanion(actor, False, True) runs, and
		//   all it runs, once its first test passes: `Companion.GetActorReference() as CompanionActorScript as Actor ==
		//   CompanionToDismiss`. That test never passed for the actor this plugin packed (the rig, 2026-11-10: three calls
		//   queued, the alias held all along; the same call by console, whose argument the game packs itself, cleared it
		//   at once; Dogmeat's call, with no actor argument, works). Most likely cause: our Variable holds the actor's bound
		//   script object typed with that object's own script (CompanionActorScript, as Variable's operator= types it),
		//   where the game's packing and the script's casts type it Actor, and == on objects typed apart is false. So the
		//   Companion slot calls ClearCompanion, with no actor to pack, as the Dogmeat slot's call has none; the alias is
		//   checked here to hold our copy just before, the test DismissCompanion would have made.
		// The clear turns the alias and the teammate flag off and sends the CompanionChange event; no scene is started (the
		// dismissal scene is reached from dialogue alone). False when it couldn't be queued: no script object, or the alias
		// no longer holds the copy.
		bool Dismiss(RE::TESQuest* a_quest, RE::Actor* a_actor, Slot a_slot)
		{
			const auto self = Papyrus::BoundObject(a_quest, SCRIPT, false);
			if (!self) {
				REX::WARN("Companions: the Followers quest has no {} object bound; can't dismiss {:08X}", SCRIPT, a_actor->GetFormID());
				return false;
			}
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> done{ new DismissDone(a_actor->GetFormID(), a_slot) };
			if (a_slot == Slot::kDogmeat) {
				return Papyrus::CallOn(self, DISMISS_DOGMEAT, [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
					a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
					a_out[0] = false;  // ShowLocationAssignmentListIfAvailable
					a_out[1] = true;   // SuppressDismissMessage
				}, done);
			}
			// ClearCompanion clears whoever the alias holds, so it is only asked while that is our copy (read this frame).
			if (Aliased(a_quest, aliasIds.companion) != a_actor) {
				REX::INFO("Companions: the {} alias no longer holds {:08X}; nothing to dismiss this try", COMPANION_ALIAS, a_actor->GetFormID());
				return false;
			}
			return Papyrus::CallOn(self, CLEAR_COMPANION, [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{};
			}, done);
		}

		std::vector<DismissAnswer> TakeAnswers()
		{
			std::scoped_lock l{ answersLock };
			return std::exchange(answers, {});
		}

		// Whether the copy is alive and on its feet: a dead or downed one's alias is left alone until it is up (or the
		// aliases let it go on their own).
		bool Up(RE::Actor* a_actor)
		{
			const auto lifeState = static_cast<std::int32_t>(static_cast<RE::ActorState&>(*a_actor).lifeState);
			return !a_actor->IsDead(false) && lifeState != SessionDeaths::LIFE_DYING && lifeState != SessionDeaths::LIFE_DEAD && !SessionDeaths::GetsUp(lifeState, false);
		}

		constexpr std::string_view SlotName(Slot a_slot) { return a_slot == Slot::kDogmeat ? DOGMEAT_ALIAS : COMPANION_ALIAS; }
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now - lastLook < LOOK_INTERVAL || Story::InOpening() || !FindAliases()) {
			return;
		}
		lastLook = now;
		const auto quest = FollowersQuest();
		if (!quest) {
			return;
		}
		// Not while a save or load is on, a loading screen is up, or the game is settling after a load (Hud::IsSettled
		// covers the whole hold, from F4SE's pre-load message on), nor while our player is in no cell (the main menu with
		// the session still up): the aliases read then can come back empty, a let-go the owner never made, which the
		// server would act on (the companion freed, a friend's waiting copy granted first, the owner's refused after the
		// load). A dismissal queued then would run against the state before the load.
		const auto player = RE::PlayerCharacter::GetSingleton();
		const bool settled = Hud::IsSettled() && !Hud::LoadingScreenUp() && player && player->GetParentCell();
		if (!settled) {
			++unsettledLooks;
			(void)book.Look({}, now, false);  // nothing is read, reported or changed; the next settled look says it all
			return;
		}
		for (const auto& answer : TakeAnswers()) {
			const auto        held = Aliased(quest, answer.slot == Slot::kDogmeat ? aliasIds.dogmeat : aliasIds.companion);
			const std::string holds = held ? std::format("{:08X}", held->GetFormID()) : std::string{ "nothing" };
			const auto        function = answer.slot == Slot::kDogmeat ? DISMISS_DOGMEAT : CLEAR_COMPANION;
			if (!answer.ran) {
				++dismissCallsCancelled;
				REX::WARN("Companions: the VM cancelled {}.{} for {:08X} ({}); the {} alias holds {}", SCRIPT, function, answer.id, NameOf(answer.id), SlotName(answer.slot), holds);
			} else {
				++dismissCallsRan;
				REX::INFO("Companions: the VM ran {}.{} for {:08X} ({}); the {} alias holds {}", SCRIPT, function, answer.id, NameOf(answer.id), SlotName(answer.slot), holds);
			}
		}
		for (const auto& report : book.Look(HeldNow(quest), now, true)) {
			REX::INFO("Companions: our {} {:08X} ({}); reported", report.recruited ? "aliases hold" : "aliases let go of", report.refId, NameOf(report.refId));
			outgoing.push_back(Protocol::Encode(report));
		}
		for (const auto id : book.GivenUp(now)) {
			REX::WARN("Companions: {:08X} ({}) is still in our aliases after {} dismissals; left as it is", id, NameOf(id), CompanionRules::DISMISS_TRIES);
		}
		if (const auto due = book.DueDismissal(now)) {
			const auto actor = LoadedActor(due->id);
			if (GameStop::Now() || (actor && (Busy(actor) || !Up(actor)))) {
				return;  // after the pause, the conversation, or once the copy is up again
			}
			// An NPC the aliases hold but the game can't find counts as a try: the aliases say otherwise next look, or it is given up on.
			if (actor && Dismiss(quest, actor, due->slot)) {
				++dismissCallsQueued;
				REX::INFO("Companions: dismissing {:08X} ({}) by {}.{} (try {} of {}): player {}'s companion, not ours", due->id, NameOf(due->id), SCRIPT,
					due->slot == Slot::kDogmeat ? DISMISS_DOGMEAT : CLEAR_COMPANION, due->attempt, CompanionRules::DISMISS_TRIES, book.OwnerOf(due->id).value_or(0));
			} else {
				++dismissCallsRefused;
			}
			book.Dismissing(due->id, now);  // a call that couldn't be queued counts as a try: nothing is gained by repeating it at once
		}
	}

	void OnOwner(const Protocol::CompanionOwner& a_owner)
	{
		const auto now = Clock::now();
		const auto word = book.OnOwner(a_owner, NpcSync::LocalPlayer(), now);
		const auto name = NameOf(a_owner.refId);
		switch (word.verdict) {
		case CompanionRules::Verdict::kNone:
			break;
		case CompanionRules::Verdict::kOurs:
			REX::INFO("Companions: {:08X} ({}) is ours", a_owner.refId, name);
			break;
		case CompanionRules::Verdict::kRefused:
			REX::INFO("Companions: {:08X} ({}) is player {}'s{}; our recruit is refused: dismissing our copy", a_owner.refId, name, word.playerId, word.held ? ", held while they are away" : "");
			Hud::Notify(Text::Format("companion.theirs", { { "player", PlayerName(word.playerId) }, { "npc", name } }));
			break;
		case CompanionRules::Verdict::kTheirs:
			REX::INFO("Companions: {:08X} ({}) is player {}'s", a_owner.refId, name, word.playerId);
			break;
		case CompanionRules::Verdict::kHeld:
			REX::INFO("Companions: {:08X} ({}) is held for player {}, who left", a_owner.refId, name, word.playerId);
			break;
		case CompanionRules::Verdict::kFree:
			REX::INFO("Companions: {:08X} ({}) is free", a_owner.refId, name);
			break;
		case CompanionRules::Verdict::kFull:
			REX::WARN("Companions: the server keeps no more companions for us; {:08X} ({}) is left as it is", a_owner.refId, name);
			break;
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnGameLoaded()
	{
		book.Reload();
		lastLook = {};
	}

	void Reset()
	{
		book.Reset();
		outgoing.clear();
		(void)TakeAnswers();
		lastLook = {};
	}

	std::string Describe()
	{
		return book.Describe() + std::format(" callsQueued={} callsRan={} callsCancelled={} callsRefused={} unsettledLooks={}", dismissCallsQueued, dismissCallsRan, dismissCallsCancelled,
			dismissCallsRefused, unsettledLooks);
	}

	std::string List()
	{
		if (!FindAliases()) {
			return "no Followers quest or aliases";
		}
		const auto quest = FollowersQuest();
		if (!quest) {
			return "no Followers quest";
		}
		std::string out;
		for (const auto& held : HeldNow(quest)) {
			const auto owner = book.OwnerOf(held.id);
			out += std::format("{}={:08X} '{}' owner={} ours={} ", SlotName(held.slot), held.id, NameOf(held.id), owner.value_or(0), book.IsOurs(held.id));
		}
		if (out.empty()) {
			out = "aliases empty ";
		}
		const auto due = book.DueDismissal(Clock::now());
		out += std::format("dismissDue={:08X} ", due ? due->id : 0);
		return out + book.Describe();
	}
}
