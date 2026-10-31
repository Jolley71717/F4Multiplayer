#include "game/RollHook.h"

#include "Config.h"
#include "SessionLevel.h"
#include "game/NpcRoll.h"
#include "game/NpcSync.h"
#include "game/Rolls.h"
#include "game/SharedRoll.h"
#include "game/TextureFlush.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace RollHook
{
	namespace
	{
		// 1.11.240, by Address Library ID (docs/SHARED-ROLLS.md: how each was found, and how sure).
		constexpr REL::ID     RESOLVE{ 2192884 };               // TESActorBaseData's leveled actor resolver (+309B10): (TESActorBaseData* original, TESObjectREFR*, bool skipInit)
		constexpr std::size_t RESOLVED_CALL = 0xCB;             // its call of IsLeveledCreatureResolved(ref): it returns at once on true
		constexpr REL::ID     IS_RESOLVED{ 2202656 };           // TESObjectREFR: an NPC_ base with ExtraLeveledCreature (+5639E0)
		constexpr REL::ID     COPY_FROM_TEMPLATES{ 2207448 };   // TESNPC's TESActorBaseData::CopyFromTemplateForms(TESActorBase**) (+6580A0)
		constexpr std::size_t COPY_FROM_TEMPLATES_SLOT = 7;     // in VTABLE::TESNPC[1] (the TESActorBaseData at +0x68)
		constexpr REL::ID     REF_ENCOUNTER_ZONE{ 2202627 };    // TESObjectREFR::GetEncounterZone (+562950)
		constexpr REL::ID     DEFAULT_ENCOUNTER_ZONE{ 4798032 };  // the zone the engine takes for none (+31ED400): no bounds
		constexpr REL::ID     ZONE_LEVEL{ 2199472 };            // BGSEncounterZone's level for the player (+494EC0)
		constexpr std::size_t ZONE_SCALE_LOAD = 0x4A;           // its mulss xmm1, [rip+disp32]: the scale above the minimum
		constexpr REL::ID     LEV_CREA_MODIFIER{ 2190331 };     // ExtraDataList::GetLevCreaModifier(float* mult, int* allBelowForce) (+284C90)
		constexpr REL::ID     DIFFICULTY{ 2233056 };            // the game's difficulty, 0-6 (+D6D600)
		constexpr REL::ID     LEGENDARY_MULTS{ 2209078 };       // (difficulty, float* chanceMult, float* countMult) (+6CEEA0)
		constexpr std::size_t CALL_STUB = 14;                   // what a write_call<5> takes of the trampoline

		// BGSEncounterZone (no CommonLib layout): its ENCOUNTER_ZONE_DATA at +0x20, so minLevel +0x31, flags +0x32, maxLevel
		// +0x33, as the engine's zone level reads them (+494EC0).
		constexpr std::size_t ZONE_MIN = 0x31, ZONE_FLAGS = 0x32, ZONE_MAX = 0x33;
		constexpr std::uint8_t ZONE_MATCH_BELOW_MIN = 1 << 1;
		constexpr std::uint32_t EACH_SLOT_FLAG = 1u << 9;  // ACTOR_BASE_DATA: each slot rolls its list on its own
		constexpr std::uint8_t  LIST_ALL_LEVELS = 1 << 0;  // TESLeveledList::llFlags: from all levels up to the player's
		constexpr std::uint32_t DELETED_FLAG = 1u << 5;    // TESForm::formFlags

		constexpr std::size_t PREWRITE_PER_FRAME = 8;
		constexpr std::size_t MAX_PREWRITE_QUEUE = Protocol::MAX_SESSION_ROLLS;
		constexpr std::uint32_t LOGGED_BUILDS = 32;  // a session's first builds with a roll of ours, each logged

		using IsResolvedFn = bool (*)(RE::TESObjectREFR*);
		using CopyFn = void (*)(RE::TESActorBaseData*, RE::TESActorBase**);
		using ZoneFn = void* (*)(RE::TESObjectREFR*);
		using ModifierFn = void (*)(RE::ExtraDataList*, float*, std::int32_t*);
		using DifficultyFn = std::int32_t (*)();
		using LegendaryMultsFn = void (*)(std::int32_t, float*, float*);

		IsResolvedFn  originalIsResolved = nullptr;
		CopyFn        originalCopy = nullptr;
		std::uintptr_t zoneScale = 0;  // where the engine's zone scale is, 0 if its code isn't 1.11.240's

		std::atomic<bool> installed{ false };
		std::atomic<bool> enabled{ false };

		// The session (main thread writes, the resolver's thread reads).
		std::atomic<std::uint64_t> sessionId{ 0 };
		std::atomic<std::uint16_t> sessionLevel{ 0 };
		std::atomic<std::uint16_t> sessionLegendary{ 0 };  // the legendary chance multiplier, thousandths; 0 = our difficulty's
		std::atomic<std::uint8_t>  rule{ static_cast<std::uint8_t>(Protocol::NpcLevelRule::kOwn) };

		// The plugins' names (OnDataReady), by load order slot: a form's key whatever the load order.
		std::array<std::uint64_t, 256>  fullKeys{};
		std::array<std::uint64_t, 4096> lightKeys{};
		std::atomic<bool>               keysReady{ false };

		// The session's rolls of plugins' NPCs, by our form ID.
		struct Entry
		{
			Protocol::NpcIdentity identity;
			bool                  session = false;  // the server's (else our own game's report)
		};
		std::mutex                                storeLock;
		std::unordered_map<std::uint32_t, Entry>  store;
		bool                                      storeFullLogged = false;
		std::deque<std::uint32_t>                 prewrites;  // main thread only
		std::unordered_set<std::uint32_t>         prewriteQueued;

		std::mutex           ordinalsLock;
		SharedRoll::Ordinals ordinals;

		enum class Source : std::uint8_t
		{
			kSession,    // the session's roll (the server's, from the first game to run it)
			kOwn,        // our own game's report of it, the session having none
			kPlacement,  // a friend's spawn's roll, as we place our copy of it
			kPick,       // the session's pick (SharedRoll::Pick)
		};

		const char* Name(Source a_source)
		{
			switch (a_source) {
			case Source::kSession:
				return "the session's";
			case Source::kOwn:
				return "our report's";
			case Source::kPlacement:
				return "the spawn's";
			case Source::kPick:
				return "the session's pick";
			}
			return "?";
		}

		// What the resolver is to build: trivially copyable, so it lives in a thread_local without dynamic init.
		struct Forced
		{
			std::uint32_t ref = 0;
			std::uint32_t original = 0;
			std::array<RE::TESActorBase*, Protocol::IDENTITY_TEMPLATES> templates{};
			std::uint16_t mask = 0;
			std::uint16_t level = 0;         // a report's level: the NPC's, where it is its player's times a multiplier
			std::uint16_t sessionLevel = 0;  // a pick's: that level made from the session's
			std::uint16_t listLevel = 0;     // a pick's: what its lists were read at (logged)
			bool          legendary = false;
			Source        source = Source::kPick;
		};

		thread_local Forced tlPending;
		thread_local bool   tlPendingArmed = false;
		thread_local Forced tlPlacement;
		thread_local bool   tlPlacementArmed = false;
		thread_local bool   tlPlacementTaken = false;

		std::atomic<std::uint32_t> forced{ 0 }, placedN{ 0 }, picked{ 0 }, missed{ 0 }, notWhole{ 0 }, unconsumed{ 0 }, faults{ 0 }, levelsSet{ 0 }, logBudget{ 0 };
		std::atomic<std::uint32_t> prewritten{ 0 }, differ{ 0 };
		std::atomic<bool>          faultLogged{ false }, missLogged{ false }, notWholeLogged{ false };

		// ---- Reading the game ---------------------------------------------------------------------------------

		std::optional<std::uint64_t> KeyOf(std::uint32_t a_formId)
		{
			const auto local = SharedRoll::LocalOf(a_formId);
			if (!local || !keysReady.load(std::memory_order_acquire)) {
				return std::nullopt;
			}
			const auto hash = local->light ? (local->index < lightKeys.size() ? lightKeys[local->index] : 0) : fullKeys[local->index & 0xFF];
			if (hash == 0) {
				return std::nullopt;
			}
			return SharedRoll::StableKey(hash, local->id);
		}

		// What the pick needs of a reference, all plain values: read under a fault guard.
		struct Facts
		{
			std::uint32_t refId = 0;
			std::uint32_t baseId = 0;
			std::uint32_t placeId = 0;  // its interior cell, or its worldspace
			float         x = 0, y = 0;
			bool          zone = false;
			std::uint8_t  zoneMin = 0, zoneMax = 0, zoneFlags = 0;
			float         zoneScaleValue = 1.0f;
			float         modifier = 1.0f;
			std::int32_t  allBelow = 0;
			float         legendaryMult = 1.0f;
			bool          legendaryAllowed = true;
		};

		void ReadFactsUnguarded(RE::TESObjectREFR* a_ref, Facts& a_out)
		{
			a_out.refId = a_ref->GetFormID();
			a_out.baseId = a_ref->data.objectReference ? a_ref->data.objectReference->GetFormID() : 0;
			a_out.x = a_ref->data.location.x;
			a_out.y = a_ref->data.location.y;
			if (const auto cell = a_ref->GetParentCell()) {
				a_out.placeId = cell->IsInterior() ? cell->GetFormID() : (cell->worldSpace ? cell->worldSpace->GetFormID() : 0);
			}
			static REL::Relocation<ZoneFn> zoneOf{ REF_ENCOUNTER_ZONE };
			static REL::Relocation<void**> noZone{ DEFAULT_ENCOUNTER_ZONE };
			if (const auto zone = static_cast<const std::uint8_t*>(zoneOf(a_ref)); zone && zone != *noZone) {
				a_out.zone = true;
				a_out.zoneMin = zone[ZONE_MIN];
				a_out.zoneFlags = zone[ZONE_FLAGS];
				a_out.zoneMax = zone[ZONE_MAX];
				a_out.zoneScaleValue = zoneScale ? *reinterpret_cast<const float*>(zoneScale) : 1.0f;
			}
			if (const auto extra = a_ref->extraList.get()) {
				static REL::Relocation<ModifierFn> modifierOf{ LEV_CREA_MODIFIER };
				modifierOf(extra, &a_out.modifier, &a_out.allBelow);
			}
			static REL::Relocation<DifficultyFn>     difficulty{ DIFFICULTY };
			static REL::Relocation<LegendaryMultsFn> legendaryMults{ LEGENDARY_MULTS };
			float                                    count = 1.0f;
			legendaryMults(difficulty(), &a_out.legendaryMult, &count);
			// The session's (sLegendaryDifficulty: the average of the games there, or the host's), where it has one.
			if (const auto session = sessionLegendary.load(std::memory_order_relaxed); session != 0) {
				a_out.legendaryMult = static_cast<float>(session) / 1000.0f;
			}
			a_out.legendaryAllowed = !a_ref->IsDead(false) && (a_ref->GetFormFlags() & DELETED_FLAG) == 0;
		}

		bool ReadFacts(RE::TESObjectREFR* a_ref, Facts* a_out)
		{
			__try {
				ReadFactsUnguarded(a_ref, *a_out);
				return true;
			} __except (1) {
				return false;
			}
		}

		// SharedRoll's view of the game's forms.
		std::optional<SharedRoll::Form> Look(std::uint32_t a_id)
		{
			const auto form = RE::TESForm::GetFormByID(a_id);
			if (!form) {
				return std::nullopt;
			}
			SharedRoll::Form out;
			if (const auto npc = form->As<RE::TESNPC>()) {
				out.kind = SharedRoll::Form::Kind::kNpc;
				out.useFlags = npc->actorData.templateUseFlags.underlying();
				out.baseTemplate = npc->baseTemplateForm ? npc->baseTemplateForm->GetFormID() : 0;
				if (npc->templateForms) {
					for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
						out.slotTemplates[i] = npc->templateForms[i] ? npc->templateForms[i]->GetFormID() : 0;
					}
				}
				out.legendTemplate = npc->legendTemplate ? npc->legendTemplate->GetFormID() : 0;
				out.legendChance = npc->legendChance ? npc->legendChance->GetValue() : 0.0f;
				out.eachSlot = (npc->actorData.actorBaseFlags.underlying() & EACH_SLOT_FLAG) != 0;
				return out;
			}
			if (const auto list = form->As<RE::TESLevCharacter>()) {
				out.kind = SharedRoll::Form::Kind::kList;
				out.listFlags = (list->llFlags & LIST_ALL_LEVELS) ? SharedRoll::ALL_LEVELS : 0;
				for (std::int8_t i = 0; list->leveledLists && i < list->baseListCount; ++i) {
					const auto& entry = list->leveledLists[i];
					out.entries.push_back({ entry.form ? entry.form->GetFormID() : 0, entry.level });
				}
				for (std::int8_t i = 0; list->scriptAddedLists && i < list->scriptListCount; ++i) {
					if (const auto entry = list->scriptAddedLists[i]) {
						out.entries.push_back({ entry->form ? entry->form->GetFormID() : 0, entry->level });
					}
				}
				return out;
			}
			return out;  // kOther
		}

		struct PickArgs
		{
			std::uint32_t        original = 0;
			SharedRoll::Levels   levels;
			std::uint64_t        seed = 0;
			float                legendaryMult = 1.0f;
		};

		void PickUnguarded(const PickArgs& a_args, SharedRoll::Result& a_out)
		{
			a_out = SharedRoll::Pick(a_args.original, a_args.levels, a_args.seed, a_args.legendaryMult, Look);
		}

		bool PickGuarded(const PickArgs* a_args, SharedRoll::Result* a_out)
		{
			__try {
				PickUnguarded(*a_args, *a_out);
				return true;
			} __except (1) {
				return false;
			}
		}

		void Fault(std::string_view a_where)
		{
			++faults;
			enabled.store(false);  // the game's own rolls from here on: something isn't as 1.11.240 has it
			if (!faultLogged.exchange(true)) {
				REX::WARN("RollHook: a fault reading the game's data ({}); leveled NPCs are the game's own roll from now on", a_where);
			}
		}

		// a_templates as the resolver's array, each an NPC here (nothing where one isn't: the roll can't be made whole).
		bool Resolve(const SharedRoll::Templates& a_ids, std::uint16_t a_mask, Forced& a_out)
		{
			for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
				a_out.templates[i] = nullptr;
				if (!(a_mask & (1u << i))) {
					continue;
				}
				const auto form = a_ids[i] ? RE::TESForm::GetFormByID(a_ids[i]) : nullptr;
				const auto npc = form ? form->As<RE::TESNPC>() : nullptr;
				if (!npc) {
					return false;
				}
				a_out.templates[i] = npc;
			}
			a_out.mask = a_mask;
			return a_mask != 0;
		}

		bool FromIdentity(const Protocol::NpcIdentity& a_identity, Source a_source, Forced& a_out)
		{
			a_out = {};
			a_out.ref = a_identity.refId;
			a_out.original = a_identity.originalBase;
			a_out.level = a_identity.level;
			a_out.legendary = (a_identity.flags & Protocol::NpcIdentity::kLegendary) != 0;
			a_out.source = a_source;
			SharedRoll::Templates ids{};
			std::ranges::copy(a_identity.templates, ids.begin());
			return Resolve(ids, a_identity.mask, a_out);
		}

		// What a_ref, about to be rolled, is to be: nothing for the game's own roll.
		bool Decide(RE::TESObjectREFR* a_ref, Forced& a_out)
		{
			const auto base = a_ref->data.objectReference;
			if (!base) {
				return false;
			}
			const auto id = a_ref->GetFormID();
			const auto baseId = base->GetFormID();

			// Our copy of a friend's spawn, placed now on this thread.
			if (tlPlacementArmed && !tlPlacementTaken && tlPlacement.original == baseId) {
				tlPlacementTaken = true;
				a_out = tlPlacement;
				a_out.ref = id;
				++placedN;
				return true;
			}

			// The session's roll of a plugin's NPC.
			if (Protocol::IsShareableRef(id)) {
				std::optional<Entry> entry;
				{
					std::scoped_lock lock{ storeLock };
					if (const auto it = store.find(id); it != store.end()) {
						entry = it->second;
					}
				}
				if (entry && entry->identity.originalBase == baseId) {
					if (FromIdentity(entry->identity, entry->session ? Source::kSession : Source::kOwn, a_out)) {
						++forced;
						return true;
					}
					++notWhole;
					if (!notWholeLogged.exchange(true)) {
						REX::INFO("RollHook: {:08X}'s session roll names an NPC that isn't one here (a mod we lack?); the game rolls it (said once)", id);
					}
					return false;
				}
			}

			// The session's pick.
			const auto session = sessionId.load(std::memory_order_relaxed);
			const auto level = sessionLevel.load(std::memory_order_relaxed);
			if (session == 0 || level == 0 || static_cast<Protocol::NpcLevelRule>(rule.load(std::memory_order_relaxed)) == Protocol::NpcLevelRule::kOwn) {
				return false;
			}
			Facts facts;
			if (!ReadFacts(a_ref, &facts)) {
				Fault("the reference's zone, modifier or difficulty");
				return false;
			}
			std::uint64_t key = 0;
			std::uint32_t ordinal = 0;
			if (Protocol::IsShareableRef(id)) {
				const auto own = KeyOf(id);
				if (!own) {
					return false;
				}
				key = *own;
			} else if ((id >> 24) == 0xFF) {
				const auto original = KeyOf(baseId);
				const auto place = facts.placeId ? KeyOf(facts.placeId) : std::optional<std::uint64_t>{ 0 };
				const auto spot = original && place ? SharedRoll::SpotKey(*original, *place, facts.x, facts.y) : std::nullopt;
				if (!spot) {
					return false;
				}
				key = *spot;
				const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
				std::scoped_lock lock{ ordinalsLock };
				ordinal = ordinals.Next(key, now);
			} else {
				return false;
			}
			std::optional<SharedRoll::Zone> zone;
			if (facts.zone) {
				zone = SharedRoll::Zone{ facts.zoneMin, facts.zoneMax, (facts.zoneFlags & ZONE_MATCH_BELOW_MIN) != 0, facts.zoneScaleValue };
			}
			PickArgs args;
			args.original = baseId;
			args.levels.plain = SharedRoll::ListLevel(level, facts.modifier, zone);
			args.levels.legendary = SharedRoll::ListLevel(level, 1.0f, zone);
			args.levels.allBelow = facts.allBelow < 0 ? SharedRoll::AllBelow::kNever : facts.allBelow == 1 ? SharedRoll::AllBelow::kAlways : SharedRoll::AllBelow::kDefault;
			args.levels.legendaryAllowed = facts.legendaryAllowed;
			args.seed = SharedRoll::SeedOf(session, key, ordinal);
			args.legendaryMult = facts.legendaryMult;
			SharedRoll::Result result;
			if (!PickGuarded(&args, &result)) {
				Fault("a leveled list");
				return false;
			}
			if (!result.picked) {
				++missed;
				if (!missLogged.exchange(true)) {
					REX::INFO("RollHook: no pick for {:08X} (placed as {:08X}): {}; the game rolls it (said once)", id, baseId, SharedRoll::Name(result.miss));
				}
				return false;
			}
			a_out = {};
			a_out.ref = id;
			a_out.original = baseId;
			a_out.sessionLevel = level;
			a_out.listLevel = result.picked->legendary ? args.levels.legendary : args.levels.plain;
			a_out.legendary = result.picked->legendary;
			a_out.source = Source::kPick;
			if (!Resolve(result.picked->templates, result.picked->mask, a_out)) {
				++missed;
				return false;
			}
			++picked;
			return true;
		}

		// ---- The hooks --------------------------------------------------------------------------------------------

		bool IsResolvedHook(RE::TESObjectREFR* a_ref)
		{
			const bool resolved = originalIsResolved(a_ref);
			if (tlPendingArmed) {
				tlPendingArmed = false;  // a decision the resolver never built from (it always builds after this call)
				++unconsumed;
			}
			if (resolved || !a_ref || !enabled.load(std::memory_order_relaxed)) {
				return resolved;
			}
			try {
				Forced decision;
				if (Decide(a_ref, decision)) {
					tlPending = decision;
					tlPendingArmed = true;
				}
			} catch (...) {
				Fault("an exception deciding a roll");
			}
			return resolved;
		}

		void SetLevel(RE::TESActorBaseData* a_data, const Forced& a_roll)
		{
			auto& data = a_data->actorData;
			if (!data.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kPCLevelMult)) {
				return;  // a level of its own: the template's, the same in every game
			}
			const std::uint16_t level = a_roll.level != 0 ? (std::min)(a_roll.level, Protocol::MAX_IDENTITY_LEVEL) :
			                            a_roll.sessionLevel != 0 ? SharedRoll::NpcLevel(a_roll.sessionLevel, data.level, data.calcLevelMin, data.calcLevelMax) :
			                                                       std::uint16_t{ 0 };
			if (level == 0) {
				return;
			}
			// A level of its own, no longer its player's times a multiplier: each game's player is another (as Rolls::Apply).
			data.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kPCLevelMult);
			data.level = level;
			++levelsSet;
		}

		void CopyHook(RE::TESActorBaseData* a_this, RE::TESActorBase** a_templates)
		{
			const bool armed = std::exchange(tlPendingArmed, false);
			if (!armed || !a_templates) {
				originalCopy(a_this, a_templates);
				return;
			}
			const auto roll = tlPending;
			// The resolver's own array: what the NPC is put together from, and then what the engine keeps on the reference.
			std::ranges::copy(roll.templates, a_templates);
			originalCopy(a_this, a_templates);
			SetLevel(a_this, roll);
			if (logBudget.fetch_add(1) < LOGGED_BUILDS) {
				REX::INFO("RollHook: {:08X} built as {} roll of {:08X}{} (slots {:04X}{}, level {})", roll.ref, Name(roll.source), roll.original, roll.legendary ? ", legendary" : "", roll.mask,
					roll.listLevel ? std::format(", lists at {}", roll.listLevel) : std::string{}, a_this->actorData.level);
			}
		}

		std::optional<std::uintptr_t> CallAt(const REL::ID& a_function, std::size_t a_offset, const REL::ID& a_target)
		{
			const auto                  site = a_function.address() + a_offset;
			std::array<std::uint8_t, 5> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
			if (TextureFlush::CallTarget(bytes, site) != a_target.address()) {
				return std::nullopt;
			}
			return site;
		}

		// The zone scale's address, from the engine's own load of it (F3 0F 59 0D disp32: mulss xmm1, [rip+disp32]).
		std::uintptr_t ZoneScaleAddress()
		{
			const auto  site = ZONE_LEVEL.address() + ZONE_SCALE_LOAD;
			std::array<std::uint8_t, 8> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
			if (bytes[0] != 0xF3 || bytes[1] != 0x0F || bytes[2] != 0x59 || bytes[3] != 0x0D) {
				return 0;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, bytes.data() + 4, sizeof(disp));
			return site + bytes.size() + static_cast<std::intptr_t>(disp);
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		if (!Config::Get().sharedRolls) {
			REX::INFO("RollHook: off (bSharedRolls = false): every game rolls its own leveled NPCs");
			return;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("RollHook: no room for the resolver's hook ({} bytes free); every game rolls its own leveled NPCs", trampoline.free_size());
			return;
		}
		const auto slot = RE::VTABLE::TESNPC[1].address() + sizeof(void*) * COPY_FROM_TEMPLATES_SLOT;
		const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
		if (current != COPY_FROM_TEMPLATES.address()) {
			REX::WARN("RollHook: TESNPC's CopyFromTemplateForms is {:X}, not 1.11.240's {:X} (another plugin's hook?); every game rolls its own leveled NPCs", current,
				COPY_FROM_TEMPLATES.address());
			return;
		}
		const auto site = CallAt(RESOLVE, RESOLVED_CALL, IS_RESOLVED);
		if (!site) {
			REX::WARN("RollHook: the leveled actor resolver doesn't ask whether it is resolved where 1.11.240 does (another plugin's patch?); every game rolls its own leveled NPCs");
			return;
		}
		zoneScale = ZoneScaleAddress();
		originalCopy = reinterpret_cast<CopyFn>(current);
		REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&CopyHook));
		originalIsResolved = reinterpret_cast<IsResolvedFn>(trampoline.write_call<5>(*site, &IsResolvedHook));
		installed = true;
		enabled = true;
		REX::INFO("RollHook: leveled NPCs are built with the session's roll (resolver {:X}+{:X}, CopyFromTemplateForms slot {} of {:X}; zone scale {})", RESOLVE.address(), RESOLVED_CALL,
			COPY_FROM_TEMPLATES_SLOT, RE::VTABLE::TESNPC[1].address(), zoneScale ? std::format("at {:X}", zoneScale) : std::string{ "not found: 1 taken" });
	}

	void OnDataReady()
	{
		const auto data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		for (const auto file : data->compiledFileCollection.files) {
			if (file) {
				fullKeys[file->GetCompileIndex()] = SharedRoll::PluginHash(file->GetFilename());
			}
		}
		for (const auto file : data->compiledFileCollection.smallFiles) {
			if (file && file->GetSmallFileCompileIndex() < lightKeys.size()) {
				lightKeys[file->GetSmallFileCompileIndex()] = SharedRoll::PluginHash(file->GetFilename());
			}
		}
		keysReady.store(true, std::memory_order_release);
	}

	void OnWelcome(std::uint64_t a_sessionId, Protocol::NpcLevelRule a_rule)
	{
		if (sessionId.exchange(a_sessionId) != a_sessionId) {
			std::scoped_lock lock{ ordinalsLock };
			ordinals.Clear();
		}
		rule = static_cast<std::uint8_t>(a_rule);
		logBudget = 0;
		REX::INFO("RollHook: session {:016X}, NPC level rule {} ({})", a_sessionId, SessionLevel::Name(a_rule), installed ? (enabled ? "hooked" : "off after a fault") : "not hooked");
	}

	void SetSessionWorld(const Protocol::SessionWorld& a_world)
	{
		if (sessionLevel.exchange(a_world.level) != a_world.level) {
			REX::INFO("RollHook: the session's NPC level is {}", a_world.level);
		}
		if (sessionLegendary.exchange(a_world.legendaryMult) != a_world.legendaryMult) {
			if (a_world.legendaryMult != 0) {
				REX::INFO("RollHook: the session's legendary chance is x{:.3f} (our difficulty's: x{:.3f})", a_world.legendaryMult / 1000.0, OwnDifficulty().legendaryMult / 1000.0);
			} else {
				REX::INFO("RollHook: the legendary chance is our difficulty's");
			}
		}
	}

	namespace
	{
		void ReadDifficultyUnguarded(std::int32_t& a_level, float& a_chance)
		{
			static REL::Relocation<DifficultyFn>     difficulty{ DIFFICULTY };
			static REL::Relocation<LegendaryMultsFn> legendaryMults{ LEGENDARY_MULTS };
			float                                    count = 1.0f;
			a_level = difficulty();
			legendaryMults(a_level, &a_chance, &count);
		}

		bool ReadDifficultyGuarded(std::int32_t* a_level, float* a_chance)
		{
			__try {
				ReadDifficultyUnguarded(*a_level, *a_chance);
				return true;
			} __except (1) {
				return false;
			}
		}

		bool ReadDifficulty(Difficulty* a_out)
		{
			std::int32_t level = -1;
			float        chance = 1.0f;
			if (!ReadDifficultyGuarded(&level, &chance) || level < 0 || level > Protocol::MAX_DIFFICULTY || !std::isfinite(chance) || chance <= 0.0f) {
				return false;
			}
			a_out->level = static_cast<std::uint8_t>(level);
			a_out->legendaryMult = static_cast<std::uint16_t>((std::min)(std::lround(chance * 1000.0f), static_cast<long>(Protocol::MAX_LEGENDARY_MULT)));
			return true;
		}
	}

	Difficulty OwnDifficulty()
	{
		Difficulty out;
		if (!ReadDifficulty(&out)) {
			return {};
		}
		return out;
	}

	void Clear()
	{
		sessionId = 0;
		sessionLevel = 0;
		sessionLegendary = 0;
		rule = static_cast<std::uint8_t>(Protocol::NpcLevelRule::kOwn);
		{
			std::scoped_lock lock{ storeLock };
			store.clear();
			storeFullLogged = false;
		}
		{
			std::scoped_lock lock{ ordinalsLock };
			ordinals.Clear();
		}
		prewrites.clear();
		prewriteQueued.clear();
	}

	void Learn(const Protocol::NpcIdentity& a_identity, bool a_session)
	{
		if (Protocol::IsSpawnRef(a_identity.refId) || a_identity.originalBase == 0 || a_identity.mask == 0) {
			return;
		}
		{
			std::scoped_lock lock{ storeLock };
			const auto       it = store.find(a_identity.refId);
			if (it != store.end() && !a_session && it->second.session) {
				return;  // the session's stands
			}
			if (it == store.end() && store.size() >= Protocol::MAX_SESSION_ROLLS) {
				if (!std::exchange(storeFullLogged, true)) {
					REX::INFO("RollHook: {} session rolls kept, the most; newer ones are left to the game", store.size());
				}
				return;
			}
			store[a_identity.refId] = Entry{ a_identity, a_session };
		}
		if (a_session && prewrites.size() < MAX_PREWRITE_QUEUE && prewriteQueued.insert(a_identity.refId).second) {
			prewrites.push_back(a_identity.refId);
		}
	}

	void Frame()
	{
		for (std::size_t done = 0; done < PREWRITE_PER_FRAME && !prewrites.empty(); ++done) {
			const auto id = prewrites.front();
			prewrites.pop_front();
			prewriteQueued.erase(id);
			std::optional<Protocol::NpcIdentity> theirs;
			{
				std::scoped_lock lock{ storeLock };
				if (const auto it = store.find(id); it != store.end() && it->second.session) {
					theirs = it->second.identity;
				}
			}
			const auto form = theirs ? RE::TESForm::GetFormByID(id) : nullptr;
			const auto actor = form ? form->As<RE::Actor>() : nullptr;
			// Only one with no 3D in a cell that isn't loaded: a loaded one is checked as it is mirrored (NpcSync::CheckRolls).
			if (!actor || actor->IsDeleted() || actor->IsDead(false) || actor->Get3D()) {
				continue;
			}
			if (const auto cell = actor->GetParentCell(); cell && cell->IsAttached()) {
				continue;
			}
			const auto ours = Rolls::ReadOurs(actor, false);
			if (!ours.leveled) {
				continue;  // never rolled here: the resolver makes it the session's when it is (IsResolvedHook)
			}
			const auto plan = NpcRoll::Decide(ours, *theirs, [&](std::uint32_t a_npc) { return Rolls::ReachOf(ours.originalBase).Has(a_npc); });
			if (!plan.templates && !plan.level) {
				continue;
			}
			NpcSync::CountCommands(Rolls::Apply(actor, *theirs, plan));
			++prewritten;
		}
	}

	Placement::Placement(const Protocol::NpcIdentity* a_identity)
	{
		if (!a_identity || !enabled || a_identity->originalBase == 0 || a_identity->mask == 0) {
			return;
		}
		Forced roll;
		if (!FromIdentity(*a_identity, Source::kPlacement, roll)) {
			return;
		}
		tlPlacement = roll;
		tlPlacementArmed = true;
		tlPlacementTaken = false;
		armed = true;
	}

	Placement::~Placement()
	{
		if (armed) {
			tlPlacementArmed = false;
		}
	}

	bool Placement::Consumed() const
	{
		return armed && tlPlacementTaken;
	}

	bool Knows(std::uint32_t a_refId)
	{
		std::scoped_lock lock{ storeLock };
		return store.contains(a_refId);
	}

	std::string Describe()
	{
		std::size_t kept = 0;
		{
			std::scoped_lock lock{ storeLock };
			kept = store.size();
		}
		return std::format("rollHook={} sessionRolls={} rollsForced={} rollsPlaced={} rollsPicked={} rollsMissed={} rollsNotWhole={} rollsUnconsumed={} rollLevels={} rollsPrewritten={} rollsDiffer={} rollFaults={} npcLevel={} npcLevelRule={} legendaryMult={}",
			!installed ? "unhooked" : enabled ? "hooked" : "off", kept, forced.load(), placedN.load(), picked.load(), missed.load(), notWhole.load(), unconsumed.load(), levelsSet.load(),
			prewritten.load(), differ.load(), faults.load(), sessionLevel.load(), SessionLevel::Name(static_cast<Protocol::NpcLevelRule>(rule.load())),
			sessionLegendary.load() ? std::format("{:.3f}", sessionLegendary.load() / 1000.0) : std::string{ "own" });
	}

	std::string DescribePick(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || !a_ref->data.objectReference) {
			return "no reference";
		}
		const auto session = sessionId.load();
		const auto level = sessionLevel.load();
		Facts      facts;
		if (!ReadFacts(a_ref, &facts)) {
			return "a fault reading its zone, modifier or difficulty";
		}
		const auto key = Protocol::IsShareableRef(facts.refId) ? KeyOf(facts.refId) : std::optional<std::uint64_t>{};
		std::optional<SharedRoll::Zone> zone;
		if (facts.zone) {
			zone = SharedRoll::Zone{ facts.zoneMin, facts.zoneMax, (facts.zoneFlags & ZONE_MATCH_BELOW_MIN) != 0, facts.zoneScaleValue };
		}
		PickArgs args;
		args.original = facts.baseId;
		args.levels.plain = SharedRoll::ListLevel(level, facts.modifier, zone);
		args.levels.legendary = SharedRoll::ListLevel(level, 1.0f, zone);
		args.levels.allBelow = facts.allBelow < 0 ? SharedRoll::AllBelow::kNever : facts.allBelow == 1 ? SharedRoll::AllBelow::kAlways : SharedRoll::AllBelow::kDefault;
		args.levels.legendaryAllowed = facts.legendaryAllowed;
		args.seed = SharedRoll::SeedOf(session, key.value_or(0), 0);
		args.legendaryMult = facts.legendaryMult;
		std::string out = std::format("{:08X} placed as {:08X}: session {:016X} level {} rule {} zone={} modifier={:.2f} allBelow={} legendaryMult={:.2f}{}", facts.refId, facts.baseId, session, level,
			SessionLevel::Name(static_cast<Protocol::NpcLevelRule>(rule.load())), zone ? std::format("{}-{}x{:.2f}", zone->min, zone->max, zone->scale) : std::string{ "none" }, facts.modifier,
			facts.allBelow, facts.legendaryMult, key ? "" : " (no plugin key: a runtime reference's pick is keyed by its spot, ordinal 0 here)");
		if (!key && Protocol::IsShareableRef(facts.refId)) {
			return out + "; no key";
		}
		SharedRoll::Result result;
		if (!PickGuarded(&args, &result)) {
			return out + "; a fault reading a leveled list";
		}
		if (!result.picked) {
			return out + std::format("; no pick: {}", SharedRoll::Name(result.miss));
		}
		out += std::format("; pick{} lists at {}:", result.picked->legendary ? " (legendary)" : "", result.picked->legendary ? args.levels.legendary : args.levels.plain);
		for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
			if (result.picked->mask & (1u << i)) {
				out += std::format(" {}={:08X}", i, result.picked->templates[i]);
			}
		}
		{
			std::scoped_lock lock{ storeLock };
			if (const auto it = store.find(facts.refId); it != store.end()) {
				out += std::format("; the session has {} roll of {:08X}{}", it->second.session ? "a" : "our report's", it->second.identity.originalBase,
					(it->second.identity.flags & Protocol::NpcIdentity::kLegendary) ? " (legendary)" : "");
			}
		}
		return out;
	}

	void NoteDiffer()
	{
		++differ;
	}
}
