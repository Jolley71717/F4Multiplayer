#include "game/Rolls.h"

#include <array>
#include <format>
#include <unordered_map>

namespace Rolls
{
	namespace
	{
		using NpcRoll::Templates;

		constexpr std::size_t MAX_REACH_CACHED = 256;
		std::unordered_map<std::uint32_t, NpcRoll::Reach> reaches;

		RE::ExtraLeveledCreature* LeveledOf(RE::Actor* a_actor)
		{
			return a_actor && a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraLeveledCreature>() : nullptr;
		}

		// A leveled NPC's own base is the copy its game made of the roll (a runtime form, this actor's alone): the one
		// base we may change.
		RE::TESNPC* RollCopyOf(RE::Actor* a_actor)
		{
			const auto npc = a_actor ? a_actor->GetNPC() : nullptr;
			return npc && LeveledOf(a_actor) && !Protocol::IsShareableRef(npc->GetFormID()) ? npc : nullptr;
		}

		// The roll's NPCs, slot by slot. One of a runtime form (never a plugin's: nothing another game can name) is 0.
		Templates TemplatesOf(const RE::ExtraLeveledCreature& a_leveled, std::uint16_t& a_mask)
		{
			Templates out{};
			a_mask = 0;
			for (std::size_t i = 0; i < out.size(); ++i) {
				if (const auto form = a_leveled.templates[i]) {
					a_mask = static_cast<std::uint16_t>(a_mask | (1u << i));
					out[i] = Protocol::IsShareableRef(form->GetFormID()) ? form->GetFormID() : 0;
				}
			}
			return out;
		}

		std::vector<NpcRoll::FactionRank> FactionsOf(RE::Actor* a_actor)
		{
			std::vector<NpcRoll::FactionRank> base, changes;
			if (const auto npc = a_actor->GetNPC()) {
				for (const auto& entry : npc->factions) {
					if (entry.faction) {
						base.push_back({ entry.faction->GetFormID(), entry.rank });
					}
				}
			}
			if (a_actor->extraList) {
				if (const auto list = a_actor->extraList->GetByType<RE::ExtraFactionChanges>()) {
					for (const auto& entry : list->factionChanges) {
						if (entry.faction) {
							changes.push_back({ entry.faction->GetFormID(), entry.rank });
						}
					}
				}
			}
			// A runtime faction no other game can name.
			std::erase_if(base, [](const auto& a_entry) { return !Protocol::IsShareableRef(a_entry.faction); });
			std::erase_if(changes, [](const auto& a_entry) { return !Protocol::IsShareableRef(a_entry.faction); });
			return NpcRoll::Effective(base, changes);
		}

		std::uint16_t LevelOf(RE::Actor* a_actor)
		{
			return static_cast<std::uint16_t>(std::clamp<int>(a_actor->GetLevel(), 0, Protocol::MAX_IDENTITY_LEVEL));
		}

		// Walk's view of the game's forms: an NPC leads to its templates (its legendary one marked), a leveled list to
		// its entries.
		std::optional<NpcRoll::Node> Look(std::uint32_t a_id)
		{
			const auto form = RE::TESForm::GetFormByID(a_id);
			if (!form) {
				return std::nullopt;
			}
			NpcRoll::Node node;
			const auto    add = [&](const RE::TESForm* a_form, bool a_legendary) {
				if (a_form) {
					node.next.push_back({ a_form->GetFormID(), a_legendary });
				}
			};
			if (const auto npc = form->As<RE::TESNPC>()) {
				node.npc = true;
				if (npc->UsesTemplate()) {
					add(npc->baseTemplateForm, false);
					if (npc->templateForms) {
						for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
							add(npc->templateForms[i], false);
						}
					}
				}
				add(npc->legendTemplate, true);
				return node;
			}
			if (const auto list = form->As<RE::TESLevCharacter>()) {
				for (std::int8_t i = 0; list->leveledLists && i < list->baseListCount; ++i) {
					add(list->leveledLists[i].form, false);
				}
				for (std::int8_t i = 0; list->scriptAddedLists && i < list->scriptListCount; ++i) {
					if (list->scriptAddedLists[i]) {
						add(list->scriptAddedLists[i]->form, false);
					}
				}
				return node;
			}
			return std::nullopt;
		}

		std::string NameOf(RE::Actor* a_actor)
		{
			const auto name = a_actor->GetDisplayFullName();
			return name ? name : "";
		}
	}

	std::optional<Protocol::NpcIdentity> Read(RE::Actor* a_actor, std::uint32_t a_sessionId, bool a_factions)
	{
		if (!a_actor) {
			return std::nullopt;
		}
		Protocol::NpcIdentity out;
		out.refId = a_sessionId;
		out.level = LevelOf(a_actor);
		const auto leveled = RollCopyOf(a_actor) ? LeveledOf(a_actor) : nullptr;
		const auto original = leveled && leveled->originalBase ? leveled->originalBase->GetFormID() : 0;
		if (leveled && Protocol::IsShareableRef(original)) {
			out.originalBase = original;
			out.templates = TemplatesOf(*leveled, out.mask);
			if (out.mask != 0 && NpcRoll::Legendary(out.templates, out.mask, ReachOf(original))) {
				out.flags |= Protocol::NpcIdentity::kLegendary;
			}
			if (out.mask == 0) {
				out.originalBase = 0;
			}
		}
		if (a_factions) {
			NpcRoll::SetFactions(out, FactionsOf(a_actor));
		}
		if (out.mask == 0 && !a_factions) {
			return std::nullopt;
		}
		return out;
	}

	NpcRoll::Ours ReadOurs(RE::Actor* a_actor, bool a_copy)
	{
		NpcRoll::Ours ours;
		ours.copy = a_copy;
		ours.level = LevelOf(a_actor);
		if (const auto leveled = RollCopyOf(a_actor) ? LeveledOf(a_actor) : nullptr) {
			ours.leveled = true;
			ours.originalBase = leveled->originalBase ? leveled->originalBase->GetFormID() : 0;
			std::uint16_t mask = 0;
			ours.templates = TemplatesOf(*leveled, mask);
		}
		if (a_copy) {
			ours.factions = FactionsOf(a_actor);
		}
		return ours;
	}

	const NpcRoll::Reach& ReachOf(std::uint32_t a_original)
	{
		if (const auto it = reaches.find(a_original); it != reaches.end()) {
			return it->second;
		}
		if (reaches.size() >= MAX_REACH_CACHED) {
			reaches.clear();
		}
		auto reach = NpcRoll::Walk(a_original, Look);
		if (reach.cut) {
			REX::WARN("Rolls: what {:08X} can be rolled as is more than {} forms; the rest is taken for nothing it can give", a_original, NpcRoll::MAX_WALK);
		}
		return reaches.emplace(a_original, std::move(reach)).first->second;
	}

	std::uint32_t Apply(RE::Actor* a_actor, const Protocol::NpcIdentity& a_theirs, const NpcRoll::Plan& a_plan)
	{
		std::uint32_t commands = 0;
		const auto    npc = RollCopyOf(a_actor);
		const auto    leveled = LeveledOf(a_actor);
		bool          rebuild = false;
		if (a_plan.templates && npc && leveled) {
			std::array<RE::TESActorBase*, Protocol::IDENTITY_TEMPLATES> forms{};
			for (std::size_t i = 0; i < forms.size(); ++i) {
				if (!(a_theirs.mask & (1u << i))) {
					continue;
				}
				const auto form = RE::TESForm::GetFormByID(a_theirs.templates[i]);
				forms[i] = form ? form->As<RE::TESActorBase>() : nullptr;
				if (!forms[i]) {
					REX::WARN("Rolls: {:08X}'s roll from player {} names {:08X}, which is no NPC here; ours stays as it is", a_actor->GetFormID(), a_theirs.playerId, a_theirs.templates[i]);
					return commands;
				}
			}
			const auto before = NameOf(a_actor);
			// The roll is kept on the reference (a save of ours keeps theirs), and the roll's copy is put together from
			// it again, as the engine does when it makes one with its templates given.
			std::ranges::copy(forms, std::begin(leveled->templates));
			npc->CopyFromTemplateForms(leveled->templates);
			rebuild = true;
			REX::INFO("Rolls: {:08X} ({}) made player {}'s roll of {:08X}{}: now {}", a_actor->GetFormID(), before, a_theirs.playerId, a_theirs.originalBase,
				(a_theirs.flags & Protocol::NpcIdentity::kLegendary) ? " (legendary)" : "", NameOf(a_actor));
		}
		if (a_plan.level && npc) {
			// A level of its own, no longer its player's times a multiplier: our player's level isn't theirs.
			npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kPCLevelMult);
			npc->actorData.level = *a_plan.level;
			rebuild = true;
		}
		if (rebuild) {
			a_actor->Reset3D(true, 0, true, 0);
		}
		for (const auto& entry : a_plan.add) {
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} {}", a_actor->GetFormID(), entry.faction, static_cast<int>(entry.rank)).c_str());
			++commands;
		}
		for (const auto faction : a_plan.remove) {
			RE::Console::ExecuteCommand(std::format("{:08X}.removefromfaction {:08X}", a_actor->GetFormID(), faction).c_str());
			++commands;
		}
		return commands;
	}

	bool IsLeveledNpc(std::uint32_t a_base)
	{
		const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
		return npc && npc->UsesTemplate() && !npc->IsUnique();
	}

	std::string Describe(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "no actor";
		}
		const auto ours = ReadOurs(a_actor, true);
		std::string out = std::format("name=\"{}\" level={} leveled={} original={:08X} templates=", NameOf(a_actor), ours.level, ours.leveled, ours.originalBase);
		for (std::size_t i = 0; i < ours.templates.size(); ++i) {
			out += std::format("{}{:08X}", i ? "," : "", ours.templates[i]);
		}
		if (ours.leveled && ours.originalBase) {
			const auto& reach = ReachOf(ours.originalBase);
			out += std::format(" legendary={} reach={}/{}", NpcRoll::Legendary(ours.templates, NpcRoll::MaskOf(ours.templates), reach), reach.normal.size(), reach.legendary.size());
		}
		out += " factions=";
		for (std::size_t i = 0; i < ours.factions.size(); ++i) {
			out += std::format("{}{:08X}:{}", i ? "," : "", ours.factions[i].faction, static_cast<int>(ours.factions[i].rank));
		}
		return out;
	}

	const char* Name(NpcRoll::Roll a_roll)
	{
		switch (a_roll) {
		case NpcRoll::Roll::kSame:
			return "same";
		case NpcRoll::Roll::kMake:
			return "made";
		case NpcRoll::Roll::kNotLeveled:
			return "not leveled here";
		case NpcRoll::Roll::kOtherOriginal:
			return "placed as another NPC here";
		case NpcRoll::Roll::kNotWhole:
			return "a form we lack";
		case NpcRoll::Roll::kNotOurs:
			return "not what our lists give";
		}
		return "?";
	}
}
