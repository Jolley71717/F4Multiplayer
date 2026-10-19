#include "game/Companion.h"

#include "game/Papyrus.h"

namespace Companion
{
	namespace
	{
		using Args = RE::BSScrapArray<RE::BSScript::Variable>;

		struct State
		{
			bool                on = false;
			RE::ObjectRefHandle target;
			std::uint32_t       targetId = 0;
			std::uint32_t       package = 0;
			std::uint32_t       keyword = 0;
			std::uint32_t       starts = 0;  // packages handed over since the last On with a new target
		};
		State state;

		bool SetDriven(bool a_on)
		{
			return Papyrus::CallStatic("Game", "SetPlayerAIDriven", [&](Args& a_out) {
				a_out = Args{ 1 };
				a_out[0] = a_on;
			});
		}

		void Evaluate(RE::PlayerCharacter* a_player)
		{
			if (const auto self = Papyrus::BoundObject(static_cast<RE::Actor*>(a_player), "Actor", false)) {
				Papyrus::CallOn(self, "EvaluatePackage", [](Args& a_out) {
					a_out = Args{ 1 };
					a_out[0] = false;
				});
			}
		}

		RE::BGSKeyword* Keyword(std::uint32_t a_id)
		{
			return a_id ? RE::TESForm::GetFormByID<RE::BGSKeyword>(a_id) : nullptr;
		}

		std::uint32_t ActorId(const RE::ActorHandle& a_handle)
		{
			const auto actor = a_handle.get();
			return actor ? actor->GetFormID() : 0;
		}

		bool Involves(const RE::DialoguePackage* a_conversation, std::uint32_t a_id)
		{
			return ActorId(a_conversation->talkingActor) == a_id || ActorId(a_conversation->actor) == a_id || ActorId(a_conversation->targetActor) == a_id;
		}

		// The player's AI handles to a_id cleared (follow target, package target, locations, combat target): how many.
		template <class H>
		int Drop(H& a_handle, std::uint32_t a_id)
		{
			const auto ref = a_handle.get();
			if (ref && ref->GetFormID() == a_id) {
				a_handle.reset();
				return 1;
			}
			return 0;
		}

		int Forget(RE::PlayerCharacter* a_player, std::uint32_t a_id)
		{
			int n = Drop(a_player->currentCombatTarget, a_id);
			if (const auto process = a_player->currentProcess) {
				n += Drop(process->followTarget, a_id) + Drop(process->target, a_id) + Drop(process->genericLocation, a_id) +
				     Drop(process->genericSecondLocation, a_id) + Drop(process->patrolLocation, a_id) + Drop(process->idleTarget, a_id) +
				     Drop(process->currentPackage.target, a_id);
			}
			for (auto* slot : { &a_player->closestConversation, &a_player->aiConversationRunning }) {
				if (*slot && Involves(*slot, a_id)) {
					*slot = nullptr;
					++n;
				}
			}
			return n;
		}

		// Why a_target can't be followed now (null: it can). Every crash of companion mode (2026-10-19) came as the friend
		// took a load door: the player's AI went on with a stand-in that had lost its full process (Actor::UpdateInDialogue
		// on it, AIProcess::GetSoundHandle on no middle-high data) or was leaving.
		const char* Unfit(RE::TESObjectREFR* a_target, RE::PlayerCharacter* a_player)
		{
			const auto actor = a_target ? a_target->As<RE::Actor>() : nullptr;
			if (!a_target || a_target->IsDisabled() || a_target->IsDeleted() || !a_target->Get3D()) {
				return "gone";
			}
			if (actor && (!actor->currentProcess || !actor->currentProcess->middleHigh)) {
				return "no full process";
			}
			const auto cell = a_target->GetParentCell();
			const auto mine = a_player->GetParentCell();
			if (!cell || !mine || (cell != mine && (cell->IsInterior() || mine->IsInterior()))) {
				return "in another cell";
			}
			return nullptr;
		}

		std::string Conversation(const RE::DialoguePackage* a_conversation)
		{
			return a_conversation ? std::format("{:08X}>{:08X}", ActorId(a_conversation->talkingActor), ActorId(a_conversation->targetActor)) : "none";
		}
	}

	std::string On(RE::TESObjectREFR* a_target, RE::TESPackage* a_package, std::uint32_t a_keyword)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->currentProcess || !a_target || !a_package || a_target == player) {
			return "error: no player, target or package";
		}
		if (a_keyword && !Keyword(a_keyword)) {
			return "error: no such keyword";
		}
		if (const auto why = Unfit(a_target, player)) {
			return std::format("error: target {}", why);
		}
		const bool same = state.on && state.target.get().get() == a_target && state.package == a_package->GetFormID();
		if (!same) {
			state.starts = 0;
		}
		if (state.on && state.keyword != a_keyword) {
			player->SetLinkedRef(nullptr, Keyword(state.keyword));
		}
		state.on = true;
		state.target = a_target->GetHandle();
		state.targetId = a_target->GetFormID();
		state.package = a_package->GetFormID();
		state.keyword = a_keyword;

		// The follow package's target: the player's ref linked under the package's keyword.
		player->SetLinkedRef(a_target, Keyword(a_keyword));
		// Its own queue: runs on the script VM a frame or so later. The package below waits for it in the process.
		SetDriven(true);
		// A temporary package, as Papyrus PathToReference hands over; not a created one (the engine would delete a
		// created package when it ends, and this one is the master file's). The player's process does not report the
		// package it runs (currentPackage stays empty for the player, while it moves), so it is handed over each time.
		player->PutCreatedPackage(a_package, true, false, true);
		++state.starts;
		REX::INFO("Companion: following {:08X} with package {:08X} (start {})", a_target->GetFormID(), a_package->GetFormID(), state.starts);
		return std::format("on target={:08X} package={:08X} keyword={:08X} starts={}", a_target->GetFormID(), a_package->GetFormID(), a_keyword, state.starts);
	}

	std::string Off()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		const bool was = state.on;
		if (was) {
			player->SetLinkedRef(nullptr, Keyword(state.keyword));
		}
		state = {};
		SetDriven(false);
		Evaluate(player);
		if (was) {
			REX::INFO("Companion: off");
		}
		return was ? "off" : "off (was not on)";
	}

	void Released(std::uint32_t a_id)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (a_id == 0 || !player) {
			return;
		}
		if (state.on && a_id == state.targetId) {
			REX::INFO("Companion: the stand-in we follow ({:08X}) is going away: off", a_id);
			player->SetAIControlledPackage(false);  // at once: the Papyrus call in Off runs a frame or more later
			Off();
		}
		// What the player's AI still holds of it: the engine would go on with a stand-in that has lost its process.
		if (const int n = Forget(player, a_id)) {
			REX::INFO("Companion: {} of the player's AI handle(s) to {:08X} cleared", n, a_id);
		}
	}

	void Frame()
	{
		if (!state.on) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const auto target = state.target.get();
		if (const auto why = Unfit(target.get(), player)) {
			const auto id = state.targetId;
			REX::INFO("Companion: the stand-in we follow ({:08X}) is {}: off", id, why);
			player->SetAIControlledPackage(false);
			Off();
			if (const int n = Forget(player, id)) {
				REX::INFO("Companion: {} of the player's AI handle(s) to {:08X} cleared", n, id);
			}
		}
	}

	std::string Drive(bool a_on, bool a_native)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		if (a_native) {
			player->SetAIControlledPackage(a_on);
			return std::format("native aiControlled={}", static_cast<bool>(player->aiControlledPackage));
		}
		return SetDriven(a_on) ? "queued" : "error: could not queue";
	}

	std::string PathTo(RE::TESObjectREFR* a_target, float a_speed)
	{
		if (!a_target) {
			return "error: no target";
		}
		SetDriven(true);  // queued first: the VM runs it before the console's call
		// Through the console's CallFunction: the same call made through the VM from here did not move the player
		// (2026-10-19), the console's does.
		const auto command = std::format("player.cf \"Actor.PathToReference\" {:08X} {:.2f}", a_target->GetFormID(), a_speed);
		RE::Console::ExecuteCommand(command.c_str());
		return "asked";
	}

	std::string Describe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		const auto linked = player->GetLinkedRef(Keyword(state.keyword));
		const auto target = state.target.get();
		const auto combat = player->currentCombatTarget.get();
		const float distance = target ? target->data.location.GetDistance(player->data.location) : -1.0f;
		return std::format("companion={} target={:08X} distance={:.0f} package={:08X} keyword={:08X} linked={:08X} aiControlled={} starts={} inCombat={} combatTarget={:08X} conversation={} aiConversation={}",
			state.on, target ? target->GetFormID() : 0, distance, state.package, state.keyword, linked ? linked->GetFormID() : 0,
			static_cast<bool>(player->aiControlledPackage), state.starts, player->IsInCombat(), combat ? combat->GetFormID() : 0,
			Conversation(player->closestConversation), Conversation(player->aiConversationRunning));
	}
}
