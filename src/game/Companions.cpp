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

#include <chrono>
#include <format>
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
		constexpr std::string_view DISMISS_COMPANION = "DismissCompanion";
		constexpr std::string_view DISMISS_DOGMEAT = "DismissDogmeatCompanion";

		constexpr auto LOOK_INTERVAL = std::chrono::milliseconds(500);

		CompanionRules::Book                   book;
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      lastLook{};
		// The aliases' ids, found by name once (nullopt: not looked yet; 0: not there).
		std::optional<std::uint32_t> companionAlias;
		std::optional<std::uint32_t> dogmeatAlias;
		bool                         questMissingLogged = false;
		std::uint32_t                dismissCallsRefused = 0;  // calls the VM wouldn't queue
		std::uint32_t                unsettledLooks = 0;       // looks skipped: a save or load on, a loading screen, settling after a load, no cell

		RE::TESQuest* FollowersQuest()
		{
			return RE::TESForm::GetFormByID<RE::TESQuest>(FOLLOWERS_QUEST);
		}

		// The alias of the quest named so (0: none).
		std::uint32_t AliasNamed(RE::TESQuest* a_quest, std::string_view a_name)
		{
			for (const auto alias : a_quest->aliases) {
				const auto name = alias ? alias->aliasName.c_str() : nullptr;
				if (name && _stricmp(name, a_name.data()) == 0) {
					return alias->aliasID;
				}
			}
			return 0;
		}

		bool FindAliases()
		{
			if (companionAlias && dogmeatAlias) {
				return *companionAlias != 0 || *dogmeatAlias != 0;
			}
			const auto quest = FollowersQuest();
			if (!quest) {
				if (!questMissingLogged) {
					questMissingLogged = true;
					REX::WARN("Companions: the Followers quest ({:08X}) is not in this game; companions are not shared", FOLLOWERS_QUEST);
				}
				return false;
			}
			companionAlias = AliasNamed(quest, COMPANION_ALIAS);
			dogmeatAlias = AliasNamed(quest, DOGMEAT_ALIAS);
			if (*companionAlias == 0 && *dogmeatAlias == 0) {
				REX::WARN("Companions: the Followers quest has no {} or {} alias; companions are not shared", COMPANION_ALIAS, DOGMEAT_ALIAS);
				return false;
			}
			REX::INFO("Companions: the Followers quest's aliases: {} = {}, {} = {}", COMPANION_ALIAS, *companionAlias, DOGMEAT_ALIAS, *dogmeatAlias);
			return true;
		}

		// The actor an alias holds, if any.
		RE::Actor* Aliased(RE::TESQuest* a_quest, std::uint32_t a_alias)
		{
			if (a_alias == 0) {
				return nullptr;
			}
			const auto ref = a_quest->GetAliasedRef(a_alias).get();
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
			add(Aliased(a_quest, *companionAlias), Slot::kCompanion);
			add(Aliased(a_quest, *dogmeatAlias), Slot::kDogmeat);
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
		//   DismissCompanion(Actor CompanionToDismiss, Bool ShowLocationAssignmentListIfAvailable, Bool SuppressDismissMessage)
		//   DismissDogmeatCompanion(Bool ShowLocationAssignmentListIfAvailable, Bool SuppressDismissMessage): whoever the
		//   DogmeatCompanion alias holds.
		// With no list and the message suppressed the script skips the settlement menu, the home and the message and runs
		// only its ClearCompanion / ClearDogmeatCompanion: the alias is cleared, the teammate flag turned off, the
		// CompanionChange event sent, and no scene started (the dismissal scene is reached from dialogue alone).
		// DismissCompanion does nothing unless the Companion alias holds the actor passed, so a stale call is safe. False
		// when it couldn't be queued: no script object (one is never made here: creating one in the VM has crashed the
		// game on a mirrored NPC, and a refused game's copy is a mirror of the owner's; a companion the script recruited
		// has its CompanionActorScript bound, which the lookup finds).
		bool Dismiss(RE::TESQuest* a_quest, RE::Actor* a_actor, Slot a_slot)
		{
			const auto self = Papyrus::BoundObject(a_quest, SCRIPT, false);
			if (!self) {
				REX::WARN("Companions: the Followers quest has no {} object bound; can't dismiss {:08X}", SCRIPT, a_actor->GetFormID());
				return false;
			}
			if (a_slot == Slot::kDogmeat) {
				return Papyrus::CallOn(self, DISMISS_DOGMEAT, [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
					a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
					a_out[0] = false;  // ShowLocationAssignmentListIfAvailable
					a_out[1] = true;   // SuppressDismissMessage
				});
			}
			const auto actor = Papyrus::BoundObject(a_actor, "Actor", false);
			if (!actor) {
				REX::WARN("Companions: {:08X} has no script object bound; can't dismiss it this try", a_actor->GetFormID());
				return false;
			}
			return Papyrus::CallOn(self, DISMISS_COMPANION, [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 3 };
				a_out[0] = actor;
				a_out[1] = false;  // ShowLocationAssignmentListIfAvailable
				a_out[2] = true;   // SuppressDismissMessage
			});
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
				REX::INFO("Companions: dismissing {:08X} ({}) by {}.{} (try {} of {}): player {}'s companion, not ours", due->id, NameOf(due->id), SCRIPT,
					due->slot == Slot::kDogmeat ? DISMISS_DOGMEAT : DISMISS_COMPANION, due->attempt, CompanionRules::DISMISS_TRIES, book.OwnerOf(due->id).value_or(0));
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
		lastLook = {};
	}

	std::string Describe()
	{
		return book.Describe() + std::format(" callsRefused={} unsettledLooks={}", dismissCallsRefused, unsettledLooks);
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
