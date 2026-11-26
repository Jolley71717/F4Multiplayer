#include "game/Rolls.h"
#include "game/LeveledEntries.h"

#include <array>
#include <format>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace Rolls
{
	namespace
	{
		using NpcRoll::Templates;

		constexpr std::size_t MAX_REACH_CACHED = 256;
		std::unordered_map<std::uint32_t, NpcRoll::Reach> reaches;

		// Whether the NPC is put together from templates: a default template or one for a slot. Not
		// ACTOR_BASE_DATA's kUsesTemplate: on the rig a leveled mirelurk (000361C1) read as not using one, and a walk
		// that trusted it found nothing its lists give.
		bool Templated(const RE::TESNPC& a_npc)
		{
			if (a_npc.baseTemplateForm || a_npc.actorData.templateUseFlags.underlying() != 0) {
				return true;
			}
			if (a_npc.templateForms) {
				for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
					if (a_npc.templateForms[i]) {
						return true;
					}
				}
			}
			return false;
		}

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

		// The factions with a merchant container, what a store's vendor is in (the barter menu shows that container's things).
		// Its field may still hold the form ID the engine read from the file, as WorldSync::FindVendorChests notes: either
		// way it is not empty. Read once game data is loaded.
		const std::vector<const RE::TESFaction*>& VendorFactions()
		{
			static std::vector<const RE::TESFaction*> list;
			static bool                               built = false;
			if (!built) {
				if (const auto data = RE::TESDataHandler::GetSingleton()) {
					for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
						if (faction && faction->vendorData.merchantContainer != nullptr && Protocol::IsShareableRef(faction->GetFormID())) {
							list.push_back(faction);
						}
					}
					built = !list.empty();
				}
			}
			return list;
		}

		bool VendorSyncOn = true;  // dev: `vendorsync off` leaves a settler's store to each game's own scripts (the rig's "before")

		// The vendor factions an NPC is in now, by the game's own answer. A store's job puts a settler in its faction, and for a
		// unique NPC (Codsworth) the engine writes that into the NPC's own faction list, not into the reference's changes
		// (`factions` lists none: rig, 2026-11-26), so there is no telling it from what its plugin gave it. Each game says all of
		// them; one a friend's NPC has from its plugin is in ours too, and nothing is done for it.
		std::vector<NpcRoll::FactionRank> VendorFactionsIn(RE::Actor* a_actor)
		{
			std::vector<NpcRoll::FactionRank> out;
			if (!a_actor) {
				return out;
			}
			for (const auto faction : VendorFactions()) {
				if (NpcRoll::Shared(faction->GetFormID()) && a_actor->IsInFaction(faction)) {
					out.push_back({ faction->GetFormID(), 0 });
				}
			}
			std::ranges::sort(out, {}, &NpcRoll::FactionRank::faction);
			return out;
		}

		// What we put each plugin NPC in from a friend's report (actor id -> vendor factions): ours to take out again.
		std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> vendorGiven;

		// The factions the save being loaded says we gave NPCs (a save made inside a session), to be taken off once it has loaded.
		std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> savedVendor;
		constexpr std::size_t                                                MAX_SAVED_VENDORS = 256;
		constexpr std::size_t                                                MAX_SAVED_FACTIONS = 32;

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
				if (Templated(*npc)) {
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
				LeveledEntries::Each(list->leveledLists, list->baseListCount, [&](const auto& entry) {
					add(entry.form, false);
				});
				LeveledEntries::Each(list->scriptAddedLists, list->scriptListCount, [&](const auto entry) {
					if (entry) {
						add(entry->form, false);
					}
				});
				return node;
			}
			return std::nullopt;
		}

		std::string NameOf(RE::Actor* a_actor)
		{
			const auto name = a_actor->GetDisplayFullName();
			return name ? name : "";
		}

		// The NPC's own health value (its stats, before the actor's), "?" without the value's form.
		std::string BaseHealthOf(RE::TESNPC& a_npc)
		{
			const auto values = RE::ActorValue::GetSingleton();
			if (!values || !values->health) {
				return "?";
			}
			return std::format("{:.0f}", static_cast<RE::ActorValueOwner&>(a_npc).GetActorValue(*values->health));
		}

		// Its health now and at most, and its damage resistance: what a hit on it does (a friend's copy's hits go as a
		// fraction of the copy's maximum, WorldSync's kReportDamage).
		std::string ToughnessOf(RE::Actor* a_actor)
		{
			const auto values = RE::ActorValue::GetSingleton();
			if (!values || !values->health) {
				return "health=?";
			}
			auto& owner = static_cast<RE::ActorValueOwner&>(*a_actor);
			auto  out = std::format("health={:.0f}/{:.0f}", owner.GetActorValue(*values->health), owner.GetPermanentActorValue(*values->health));
			if (values->damageResistance) {
				out += std::format(" dr={:.0f}", owner.GetActorValue(*values->damageResistance));
			}
			return out;
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

	std::vector<NpcRoll::FactionRank> OwnVendorFactions(RE::Actor* a_actor)
	{
		auto in = VendorFactionsIn(a_actor);
		if (const auto it = vendorGiven.find(a_actor ? a_actor->GetFormID() : 0u); it != vendorGiven.end()) {
			std::erase_if(in, [&](const auto& a_entry) { return it->second.contains(a_entry.faction); });
		}
		return in;
	}

	void SetVendorSync(bool a_on)
	{
		VendorSyncOn = a_on;
	}

	bool VendorSync()
	{
		return VendorSyncOn;
	}

	void ForgetVendorCache(RE::Actor* a_actor)
	{
		// The engine works out which faction an NPC trades under and keeps it for a while (Actor::vendorFaction, and a timer):
		// a faction put on or taken off is not seen by its barter menu until that runs out (rig, 2026-11-26: a settler taken out
		// of its store's faction still opened that store's chest). Emptied, it is worked out again at the next barter.
		if (a_actor) {
			a_actor->vendorFaction = nullptr;
			a_actor->calculateVendorFactionTimer.timeStamp = 0.0f;
		}
	}

	std::vector<std::uint8_t> SaveVendorRecord()
	{
		// What we put NPCs in from a friend's report, so a save made inside the session can be cleaned when it is loaded: a u16 count,
		// then per NPC its id, a u8 count and that many faction ids (little endian).
		std::vector<std::uint8_t> out;
		const auto put = [&](std::uint32_t a_value, int a_bytes) {
			for (int i = 0; i < a_bytes; ++i) {
				out.push_back(static_cast<std::uint8_t>(a_value >> (8 * i)));
			}
		};
		std::uint16_t count = 0;
		for (const auto& [id, factions] : vendorGiven) {
			count = factions.empty() ? count : static_cast<std::uint16_t>(count + 1);
		}
		if (count == 0) {
			return out;
		}
		put(count, 2);
		std::size_t written = 0;
		for (const auto& [id, factions] : vendorGiven) {
			if (factions.empty() || written >= MAX_SAVED_VENDORS) {
				continue;
			}
			put(id, 4);
			put(static_cast<std::uint32_t>((std::min)(factions.size(), MAX_SAVED_FACTIONS)), 1);
			std::size_t n = 0;
			for (const auto faction : factions) {
				if (n++ < MAX_SAVED_FACTIONS) {
					put(faction, 4);
				}
			}
			++written;
		}
		out[0] = static_cast<std::uint8_t>(written);
		out[1] = static_cast<std::uint8_t>(written >> 8);
		return out;
	}

	void LoadVendorRecord(std::span<const std::uint8_t> a_bytes, const std::function<std::optional<std::uint32_t>(std::uint32_t)>& a_resolve)
	{
		std::size_t at = 0;
		const auto  get = [&](int a_bytesWanted) -> std::optional<std::uint32_t> {
            if (at + a_bytesWanted > a_bytes.size()) {
                return std::nullopt;
            }
            std::uint32_t value = 0;
            for (int i = 0; i < a_bytesWanted; ++i) {
                value |= static_cast<std::uint32_t>(a_bytes[at + i]) << (8 * i);
            }
            at += a_bytesWanted;
            return value;
		};
		const auto count = get(2);
		for (std::uint32_t i = 0; count && i < *count && i < MAX_SAVED_VENDORS; ++i) {
			const auto id = get(4);
			const auto n = get(1);
			if (!id || !n || *n > MAX_SAVED_FACTIONS) {
				return;
			}
			const auto actor = a_resolve(*id);
			for (std::uint32_t j = 0; j < *n; ++j) {
				const auto faction = get(4);
				if (!faction) {
					return;
				}
				if (const auto resolved = a_resolve(*faction); actor && resolved) {
					savedVendor[*actor].insert(*resolved);
				}
			}
		}
	}

	void RevertVendorRecord()
	{
		savedVendor.clear();
		vendorGiven.clear();  // the world the game loads is not the one we gave anything in
	}

	void RemoveSavedVendorFactions()
	{
		for (const auto& [id, factions] : savedVendor) {
			for (const auto faction : factions) {
				RE::Console::ExecuteCommand(std::format("{:08X}.removefromfaction {:08X}", id, faction).c_str());
			}
			if (const auto form = RE::TESForm::GetFormByID(id)) {
				ForgetVendorCache(form->As<RE::Actor>());
			}
			REX::INFO("Rolls: {:08X} is taken out of the {} vendor faction(s) the save it was loaded from kept from a friend's store", id, factions.size());
		}
		savedVendor.clear();
	}

	void UndoVendorFactions()
	{
		for (const auto& [id, factions] : vendorGiven) {
			for (const auto faction : factions) {
				RE::Console::ExecuteCommand(std::format("{:08X}.removefromfaction {:08X}", id, faction).c_str());
			}
			if (const auto form = RE::TESForm::GetFormByID(id)) {
				ForgetVendorCache(form->As<RE::Actor>());
			}
			REX::INFO("Rolls: {:08X} is taken out of the {} vendor faction(s) a friend's game put it in as the session ends", id, factions.size());
		}
		vendorGiven.clear();
	}

	std::string DescribeVendor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "no actor";
		}
		const auto id = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
		std::string out = std::format("{:08X} cachedVendorFaction={:08X} told=", a_actor->GetFormID(), id(a_actor->vendorFaction));
		const auto given = VendorFactionsIn(a_actor);
		for (std::size_t i = 0; i < given.size(); ++i) {
			out += std::format("{}{:08X}:{}", i ? "," : "", given[i].faction, static_cast<int>(given[i].rank));
		}
		out += " in=";
		std::size_t count = 0;
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
				if (!faction || !faction->vendorData.merchantContainer || !a_actor->IsInFaction(faction)) {
					continue;
				}
				const auto raw = reinterpret_cast<std::uintptr_t>(faction->vendorData.merchantContainer);
				const auto chest = raw <= 0xFFFFFFFFull ? static_cast<std::uint32_t>(raw) : faction->vendorData.merchantContainer->GetFormID();
				std::size_t stacks = 0;
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(chest); ref && ref->inventoryList) {
					RE::BSAutoReadLock l{ ref->inventoryList->rwLock };
					stacks = ref->inventoryList->data.size();
				}
				out += std::format("{}{:08X}(chest {:08X} stacks {})", count++ ? "," : "", faction->GetFormID(), chest, stacks);
			}
		}
		out += count ? "" : "none";
		out += std::format(" synced={}", vendorGiven.contains(a_actor->GetFormID()) ? vendorGiven[a_actor->GetFormID()].size() : 0u);
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
		} else {
			ours.vendor = VendorFactionsIn(a_actor);
			if (const auto it = vendorGiven.find(a_actor->GetFormID()); it != vendorGiven.end()) {
				ours.vendorGiven.assign(it->second.begin(), it->second.end());
			}
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
		const auto    steps = NpcRoll::StepsFor(a_plan);
		if (steps.write && npc && leveled) {
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
			const auto before = NameOf(a_actor) + ", " + ToughnessOf(a_actor);
			// The roll is kept on the reference (a save of ours keeps theirs). The engine puts the actor's NPC together
			// from it when the actor loads: on the rig, ghouls written so were the owner's roll (name, damage resistance)
			// once their cell loaded again. The NPC already made is not put together again in place: neither
			// CopyFromTemplateForms, nor a disable and enable, nor Reset3D changed its name, health or damage resistance,
			// and either on an actor that is loaded (it may be in a fight) risks the game; a roll comes only for one with
			// its 3D in (NpcSync::CheckRolls). So only the roll is written, for its next load (a cell left and come back
			// to, a fast travel, a save loaded); our copy of the NPC is put together from it at that load. Until then only
			// the level below is the owner's.
			std::ranges::copy(forms, std::begin(leveled->templates));
			REX::INFO("Rolls: {:08X} ({}) has player {}'s roll of {:08X}{} written: theirs from its next load on (its NPC now {}, {} health)", a_actor->GetFormID(), before,
				a_theirs.playerId, a_theirs.originalBase, (a_theirs.flags & Protocol::NpcIdentity::kLegendary) ? " (legendary)" : "",
				RE::TESFullName::GetFullName(*npc), BaseHealthOf(*npc));
		}
		if (steps.level && npc) {
			// A level of its own, no longer its player's times a multiplier: our player's level isn't theirs. A number in
			// the NPC's data, read as the engine asks for the level: nothing of the 3D is touched.
			npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kPCLevelMult);
			npc->actorData.level = *a_plan.level;
			REX::INFO("Rolls: {:08X} is level {} now (player {}'s: {}), {}", a_actor->GetFormID(), LevelOf(a_actor), a_theirs.playerId, a_theirs.level, ToughnessOf(a_actor));
		}
		for (const auto& entry : a_plan.add) {
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} {}", a_actor->GetFormID(), entry.faction, static_cast<int>(entry.rank)).c_str());
			if (a_plan.vendor) {
				vendorGiven[a_actor->GetFormID()].insert(entry.faction);
				REX::INFO("Rolls: {:08X} is put in vendor faction {:08X} (rank {}): player {}'s game has it in a store", a_actor->GetFormID(), entry.faction, static_cast<int>(entry.rank), a_theirs.playerId);
			}
			++commands;
		}
		for (const auto faction : a_plan.remove) {
			RE::Console::ExecuteCommand(std::format("{:08X}.removefromfaction {:08X}", a_actor->GetFormID(), faction).c_str());
			if (a_plan.vendor) {
				vendorGiven[a_actor->GetFormID()].erase(faction);
				REX::INFO("Rolls: {:08X} is taken out of vendor faction {:08X}: player {}'s game has it out of a store", a_actor->GetFormID(), faction, a_theirs.playerId);
			}
			++commands;
		}
		if (a_plan.vendor && (!a_plan.add.empty() || !a_plan.remove.empty())) {
			ForgetVendorCache(a_actor);  // its barter menu is to see the store at once
		}
		return commands;
	}

	bool IsLeveledNpc(std::uint32_t a_base)
	{
		const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
		return npc && Templated(*npc) && !npc->IsUnique();
	}

	std::string Describe(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "no actor";
		}
		const auto ours = ReadOurs(a_actor, true);
		std::string out = std::format("name=\"{}\" level={} {} leveled={} original={:08X} templates=", NameOf(a_actor), ours.level, ToughnessOf(a_actor), ours.leveled, ours.originalBase);
		for (std::size_t i = 0; i < ours.templates.size(); ++i) {
			out += std::format("{}{:08X}", i ? "," : "", ours.templates[i]);
		}
		if (ours.leveled && ours.originalBase) {
			const auto& reach = ReachOf(ours.originalBase);
			out += std::format(" legendary={} reach={}/{}", NpcRoll::Legendary(ours.templates, NpcRoll::MaskOf(ours.templates), reach), reach.normal.size(), reach.legendary.size());
		}
		// The roll's copy itself: what it is templated on now (the deepest NPCs the roll came to, slot by slot).
		if (const auto npc = a_actor->GetNPC()) {
			const auto idOf = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
			out += std::format(" base={:08X} \"{}\" baseHealth={} flags={:04X} tpl={:08X} slots=", npc->GetFormID(), RE::TESFullName::GetFullName(*npc), BaseHealthOf(*npc), npc->actorData.templateUseFlags.underlying(), idOf(npc->baseTemplateForm));
			for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
				out += std::format("{}{:08X}", i ? "," : "", npc->templateForms ? idOf(npc->templateForms[i]) : 0u);
			}
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
			return "written";
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
