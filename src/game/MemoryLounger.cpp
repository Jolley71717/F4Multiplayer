#include "game/MemoryLounger.h"

#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/LoungerRescue.h"
#include "game/Papyrus.h"
#include "game/QuestStageLayout.h"

namespace MemoryLounger
{
	namespace
	{
		using LoungerRescue::Clock;
		using LoungerRescue::Door;

		constexpr auto LOOK_INTERVAL = std::chrono::milliseconds(500);

		std::array<LoungerRescue::Guard, LoungerRescue::DOORS.size()> guards;
		Clock::time_point                                             nextLook{};

		RE::TESQuest* QuestOf(const Door& a_door)
		{
			return RE::TESForm::GetFormByID<RE::TESQuest>(a_door.quest);  // Fallout4.esm's: the same form ID in every game
		}

		// The reference an alias of the quest holds, by the alias's name.
		RE::NiPointer<RE::TESObjectREFR> Aliased(RE::TESQuest* a_quest, std::string_view a_name)
		{
			for (const auto alias : a_quest->aliases) {
				const char* name = alias ? alias->aliasName.c_str() : nullptr;
				if (name && a_name == name) {
					return a_quest->GetAliasedRef(alias->aliasID).get();
				}
			}
			return {};
		}

		// The layer the quest's script holds in that property (the sit stage's InputEnableLayer), or none: looked for on
		// every script bound to the quest, whatever its name.
		RE::BSTSmartPointer<RE::BSScript::Object> LayerOf(RE::TESQuest* a_quest, std::string_view a_property)
		{
			RE::BSTSmartPointer<RE::BSScript::Object> layer;
			const auto                                game = RE::GameVM::GetSingleton();
			const auto                                vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				return layer;
			}
			const auto& handles = vm->GetObjectHandlePolicy();
			const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESQuest>(), a_quest);
			if (handle == handles.EmptyHandle()) {
				return layer;
			}
			const RE::BSFixedString                                                                   name{ a_property };
			const RE::BSTThreadScrapFunction<RE::BSContainer::ForEachResult(RE::BSScript::Object*)> visit{ [&](RE::BSScript::Object* a_object) {
				const auto var = a_object ? a_object->GetProperty(name) : nullptr;
				if (var && var->is<RE::BSScript::Object>()) {
					if (auto object = RE::BSScript::get<RE::BSScript::Object>(*var)) {
						layer = std::move(object);
					}
				}
				return Papyrus::BOUND_NEXT;
			} };
			vm->ForEachBoundObject(handle, visit);
			return layer;
		}

		bool InFurniture(RE::PlayerCharacter* a_player)
		{
			const auto& state = static_cast<const RE::ActorState&>(*a_player);
			return static_cast<int>(a_player->DoGetSitSleepState()) != 0 || static_cast<int>(state.interactingState) != 0;
		}

		LoungerRescue::Facts Read(const Door& a_door, RE::TESQuest* a_quest, RE::PlayerCharacter* a_player, bool a_inWorld)
		{
			LoungerRescue::Facts facts;
			facts.inWorld = a_inWorld;
			facts.sitDone = QuestStageLayout::IsStageDone(a_quest, a_door.sitStage);
			if (!facts.sitDone) {
				return facts;  // nearly always: nothing more is looked at
			}
			facts.seatedDone = QuestStageLayout::IsStageDone(a_quest, a_door.seatedStage);
			if (facts.seatedDone) {
				return facts;
			}
			facts.layerHeld = static_cast<bool>(LayerOf(a_quest, a_door.layer));
			facts.inFurniture = InFurniture(a_player);
			return facts;
		}

		RE::BSTSmartPointer<RE::BSScript::Object> RefObject(RE::TESObjectREFR* a_ref)
		{
			return a_ref ? Papyrus::BoundObject(a_ref, "ObjectReference", true) : RE::BSTSmartPointer<RE::BSScript::Object>{};
		}

		// The sit stage's own seat, asked again: lounger.Activate(player, True).
		bool Seat(const Door& a_door, RE::TESQuest* a_quest, RE::PlayerCharacter* a_player)
		{
			const auto lounger = Aliased(a_quest, a_door.lounger);
			const auto self = RefObject(lounger.get());
			const auto activator = Papyrus::BoundObject(static_cast<RE::Actor*>(a_player), "Actor", true);
			return self && activator && Papyrus::CallOn(self, "Activate", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
				a_out[0] = activator;
				a_out[1] = true;  // abDefaultProcessingOnly: the seat itself, past the lounger's blocked activation
			});
		}

		// The press undone: the layer deleted, the sit stage no longer done, the lounger blocked as before it (prompt shown),
		// the ghosts mortal again. What was done, for the log.
		std::string GiveBack(const Door& a_door, RE::TESQuest* a_quest)
		{
			std::string out;
			const auto  layer = LayerOf(a_quest, a_door.layer);
			const bool  deleted = layer && Papyrus::CallOn(layer, "Delete", [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) { a_out = RE::BSScrapArray<RE::BSScript::Variable>{}; });
			out += deleted ? "layer deleted" : "layer NOT deleted";
			const auto before = a_quest->currentStage;
			if (QuestStageLayout::ClearDone(a_quest, a_door.sitStage)) {
				a_quest->currentStage = LoungerRescue::StageBefore(a_quest->currentStage, a_door.sitStage, QuestStageLayout::Done(a_quest));
				a_quest->AddChange(RE::CHANGE_TYPES::kQuestStages);
				out += std::format(", stage {} no longer done (current stage {} -> {})", a_door.sitStage, before, a_quest->currentStage);
			} else {
				out += std::format(", stage {} NOT cleared", a_door.sitStage);
			}
			const auto lounger = Aliased(a_quest, a_door.lounger);
			const auto self = RefObject(lounger.get());
			const bool unblocked = self && Papyrus::CallOn(self, "BlockActivation", [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
				a_out[0] = true;   // still blocked, as the stage before left it: a press goes to the alias
				a_out[1] = false;  // its prompt shown again
			});
			out += std::format(", lounger {:08X} {}", lounger ? lounger->GetFormID() : 0, unblocked ? "prompt shown again" : "NOT asked");
			for (const auto name : a_door.ghosts) {
				const auto ref = name.empty() ? RE::NiPointer<RE::TESObjectREFR>{} : Aliased(a_quest, name);
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				const auto object = actor ? Papyrus::BoundObject(actor, "Actor", true) : RE::BSTSmartPointer<RE::BSScript::Object>{};
				const bool asked = object && Papyrus::CallOn(object, "SetGhost", [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
					a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
					a_out[0] = false;
				});
				if (!name.empty()) {
					out += std::format(", {} {}", name, asked ? "no longer a ghost" : "not asked");
				}
			}
			return out;
		}
	}

	void OnGameStart()
	{
		for (auto& guard : guards) {
			guard.Arm();
		}
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now < nextLook) {
			return;
		}
		nextLook = now + LOOK_INTERVAL;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const bool inWorld = player->GetParentCell() && player->Get3D() && !Hud::LoadingScreenUp() && !GameStop::Now();
		for (std::size_t i = 0; i < LoungerRescue::DOORS.size(); ++i) {
			const auto& door = LoungerRescue::DOORS[i];
			const auto  quest = QuestOf(door);
			if (!quest) {
				continue;
			}
			switch (guards[i].Look(Read(door, quest, player, inWorld), now)) {
			case LoungerRescue::Step::kNothing:
				break;
			case LoungerRescue::Step::kSeat:
				{
					const bool asked = Seat(door, quest, player);
					REX::WARN("MemoryLounger: {:08X} stage {} took our player's controls and never seated them (stage {} not done, in no furniture): its seat asked again ({} of {}){}",
						door.quest, door.sitStage, door.seatedStage, guards[i].Seats(), LoungerRescue::SEATS, asked ? "" : ", could not be asked");
					break;
				}
			case LoungerRescue::Step::kGiveBack:
				REX::WARN("MemoryLounger: {:08X} stage {}: our player was never seated after {} asks; the press undone, the controls back: {}", door.quest, door.sitStage,
					LoungerRescue::SEATS, GiveBack(door, quest));
				break;
			}
		}
	}

	std::string Describe()
	{
		std::string out;
		const auto  player = RE::PlayerCharacter::GetSingleton();
		for (std::size_t i = 0; i < LoungerRescue::DOORS.size(); ++i) {
			const auto& door = LoungerRescue::DOORS[i];
			const auto  quest = QuestOf(door);
			if (!quest || !player) {
				out += std::format("{}{:08X}: not here", out.empty() ? "" : " | ", door.quest);
				continue;
			}
			const auto facts = Read(door, quest, player, true);
			const auto lounger = Aliased(quest, door.lounger);
			out += std::format("{}{:08X}: sit({})={} seated({})={} layer={} inFurniture={} stuck={} lounger={:08X} seats={} gaveBack={}", out.empty() ? "" : " | ", door.quest,
				door.sitStage, facts.sitDone, door.seatedStage, facts.seatedDone, facts.layerHeld, facts.inFurniture, LoungerRescue::Stuck(facts), lounger ? lounger->GetFormID() : 0,
				guards[i].Seats(), guards[i].GaveBack());
		}
		return out;
	}
}
