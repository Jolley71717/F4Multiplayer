#include "game/GraphInfo.h"

#include "game/Equipment.h"
#include "game/Puppets.h"

namespace GraphInfo
{
	namespace
	{
		// The locomotion variables Puppets::DriveAnimation sets on a stand-in (and SpeedSampled, which the engine's own
		// movement feeds a player's graph), read back.
		constexpr std::string_view DRIVEN[] = { "Speed", "Direction", "SpeedSampled", "iSyncIdleLocomotion", "iSyncLocomotionSpeed", "iLocomotionSpeedState", "WalkSpeedMult", "RunSpeedMult",
			"bAnimationDriven", "IsPlayer" };

		// Copies a game string into a_out (at most a_size - 1 characters), every read guarded: a field read from a layout
		// that is wrong for this game must cost a "?", not the game.
		bool CopyText(const RE::BSFixedString* a_text, char* a_out, std::size_t a_size)
		{
			__try {
				const char* text = a_text->c_str();
				std::size_t i = 0;
				for (; text && i + 1 < a_size && text[i]; ++i) {
					a_out[i] = text[i];
				}
				a_out[i] = '\0';
				return true;
			} __except (1) {
				a_out[0] = '\0';
				return false;
			}
		}

		std::string Text(const RE::BSFixedString& a_text)
		{
			char buffer[260];
			return CopyText(&a_text, buffer, sizeof(buffer)) ? std::string{ buffer } : std::string{ "?" };
		}

		// The project its race builds, by sex (TESRace::behaviorGraphProjectName and rootBehaviorGraphName).
		std::string Race(RE::Actor* a_actor)
		{
			const auto race = a_actor->race;
			if (!race) {
				return " race=none";
			}
			const auto base = a_actor->GetObjectReference();
			const auto npc = base ? base->As<RE::TESNPC>() : nullptr;
			const int  sex = npc && npc->IsFemale() ? 1 : 0;
			return std::format(" race={:08X}:{} project='{}' root='{}'", race->GetFormID(), Text(race->formEditorID), Text(race->behaviorGraphProjectName[sex]), Text(race->rootBehaviorGraphName[sex]));
		}

		std::string Variable(const RE::IAnimationGraphManagerHolder* a_graph, std::string_view a_name)
		{
			const RE::BSFixedString name{ a_name };
			float                   f = 0.0f;
			std::int32_t            n = 0;
			bool                    b = false;
			// The float getter copies any variable's raw bits: one named like a bool or an int is read as one first.
			if (a_name.starts_with('b') && a_graph->GetGraphVariableImplBool(name, b)) {
				return std::format(" {}={}b", a_name, b);
			}
			if (a_name.starts_with('i') && a_graph->GetGraphVariableImplInt(name, n)) {
				return std::format(" {}={}i", a_name, n);
			}
			if (a_graph->GetGraphVariableImplFloat(name, f)) {
				return std::format(" {}={:.3f}", a_name, f);
			}
			return std::format(" {}=?", a_name);
		}
	}

	std::string Describe(RE::TESObjectREFR* a_ref, std::span<const std::string_view> a_more)
	{
		if (!a_ref) {
			return "no reference";
		}
		const auto base = a_ref->GetObjectReference();
		auto       out = std::format("{:08X} base={:08X} 3d={}", a_ref->GetFormID(), base ? base->GetFormID() : 0, a_ref->Get3D() != nullptr);
		const auto actor = a_ref->As<RE::Actor>();
		if (actor) {
			out += Race(actor);
			out += std::format(" inPowerArmor={} paSkeleton={}", Equipment::InPowerArmor(actor), Equipment::HasPowerArmorSkeleton(actor));
		}

		const auto graph = static_cast<const RE::IAnimationGraphManagerHolder*>(a_ref);
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (graph->GetAnimationGraphManagerImpl(manager) && manager) {
			std::uint32_t loaded = 0;
			for (std::uint32_t i = 0; i < manager->graph.size(); ++i) {
				loaded += manager->graph[i].get() != nullptr;
			}
			out += std::format(" manager graphs={} ({} there) active={} cached={}", manager->graph.size(), loaded, manager->activeGraph, manager->variableCache.variableCache.size());
		} else {
			out += " manager=none";
		}
		out += " vars:";
		for (const auto name : DRIVEN) {
			out += Variable(graph, name);
		}
		for (const auto name : a_more) {
			out += Variable(graph, name);
		}

		if (actor) {
			// What its graph writes each frame it plays: read twice a moment apart (an anim before and after), a dead graph
			// leaves it the same.
			float leg[9];
			out += Equipment::LegRotation(actor, leg) ? std::format(" leg=({:.3f},{:.3f},{:.3f})", leg[0], leg[1], leg[2]) : std::string{ " leg=none" };
			out += std::format(" moveMode={:04X} ", static_cast<const RE::ActorState&>(*actor).moveMode);
			out += Puppets::Describe(actor);
		}
		return out;
	}
}
