#include "game/Companion.h"

#include "game/Papyrus.h"

namespace Companion
{
	namespace
	{
		using Args = RE::BSScrapArray<RE::BSScript::Variable>;

		struct State
		{
			bool                 on = false;
			RE::ObjectRefHandle  target;
			std::uint32_t        package = 0;
			Way                  way = Way::kPut;
			std::uint32_t        starts = 0;  // packages handed over since the last On with a new target
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

		RE::TESPackage* CurrentPackage(RE::Actor* a_actor)
		{
			return a_actor && a_actor->currentProcess ? a_actor->currentProcess->currentPackage.package : nullptr;
		}
	}

	std::string On(RE::TESObjectREFR* a_target, RE::TESPackage* a_package, Way a_way, bool a_drive)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->currentProcess || !a_target || !a_package || a_target == player) {
			return "error: no player, target or package";
		}
		const bool same = state.on && state.target.get().get() == a_target && state.package == a_package->GetFormID();
		if (!same) {
			state.starts = 0;
		}
		state.on = true;
		state.target = a_target->GetHandle();
		state.package = a_package->GetFormID();
		state.way = a_way;

		// The follow package's target: the player's own linked ref (no keyword).
		player->SetLinkedRef(a_target, nullptr);
		if (a_drive) {
			// Its own queue: runs on the script VM a frame or so later. The package below waits for it in the
			// process (an AI-driven player runs what it was handed).
			SetDriven(true);
		}
		// Not a created package (the engine would delete a created one when it ends, and this one is the master file's).
		if (CurrentPackage(player) != a_package) {
			if (a_way == Way::kRunOnce) {
				player->currentProcess->SetRunOncePackage(a_package, player);
			} else {
				player->PutCreatedPackage(a_package, true, false, true);
			}
			++state.starts;
		}
		REX::INFO("Companion: following {:08X} with package {:08X} ({}, start {})", a_target->GetFormID(), a_package->GetFormID(), a_way == Way::kRunOnce ? "run once" : "put", state.starts);
		return std::format("on target={:08X} package={:08X} starts={}", a_target->GetFormID(), a_package->GetFormID(), state.starts);
	}

	std::string Off()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		const bool was = state.on;
		state = {};
		player->SetLinkedRef(nullptr, nullptr);
		SetDriven(false);
		Evaluate(player);
		if (was) {
			REX::INFO("Companion: off");
		}
		return was ? "off" : "off (was not on)";
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
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto self = player ? Papyrus::BoundObject(static_cast<RE::Actor*>(player), "Actor", false) : RE::BSTSmartPointer<RE::BSScript::Object>{};
		// Created only for a reference that has none yet (a stand-in made at runtime): one object, not one per frame.
		// Its handle's type must be its real script type: Actor for a stand-in.
		const auto actor = a_target ? a_target->As<RE::Actor>() : nullptr;
		const auto target = actor ? Papyrus::BoundObject(actor, "Actor", true) : a_target ? Papyrus::BoundObject(a_target, "ObjectReference", true) : RE::BSTSmartPointer<RE::BSScript::Object>{};
		if (!self || !target) {
			return "error: no script object for the player or the target";
		}
		const bool queued = Papyrus::CallOn(self, "PathToReference", [&](Args& a_out) {
			a_out = Args{ 2 };
			a_out[0] = target;
			a_out[1] = a_speed;
		});
		return queued ? "queued" : "error: could not queue";
	}

	std::string Describe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		const auto package = CurrentPackage(player);
		const auto process = player->currentProcess;
		const auto packageTarget = process ? process->currentPackage.target.get() : RE::NiPointer<RE::TESObjectREFR>{};
		const auto linked = player->GetLinkedRef(nullptr);
		const auto target = state.target.get();
		const auto combat = player->currentCombatTarget.get();
		const float distance = target ? target->data.location.GetDistance(player->data.location) : -1.0f;
		return std::format("companion={} target={:08X} distance={:.0f} package={:08X} type={} procedure={} packageTarget={:08X} ours={} linked={:08X} aiControlled={} starts={} inCombat={} combatTarget={:08X}",
			state.on, target ? target->GetFormID() : 0, distance, package ? package->GetFormID() : 0, package ? package->data.packType : -1,
			process ? process->currentPackage.currentProcedureIndex : -1, packageTarget ? packageTarget->GetFormID() : 0,
			package && package->GetFormID() == state.package, linked ? linked->GetFormID() : 0, static_cast<bool>(player->aiControlledPackage), state.starts,
			player->IsInCombat(), combat ? combat->GetFormID() : 0);
	}
}
