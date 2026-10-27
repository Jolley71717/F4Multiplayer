#include "game/Destructibles.h"

#include "game/DestructionRules.h"
#include "game/Story.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"

#include <charconv>
#include <format>
#include <mutex>
#include <unordered_map>

namespace Destructibles
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using DestructionRules::Copy;
		using DestructionRules::Known;
		using DestructionRules::Stage;

		constexpr auto  POLL_INTERVAL = std::chrono::milliseconds(100);  // the watched objects: a car's stages come a second apart
		constexpr auto  SCAN_INTERVAL = std::chrono::seconds(1);
		constexpr float SCAN_RADIUS = 4096.0f;  // around our player, for changes no hit of ours made
		constexpr float CLAIM_RADIUS = 1024.0f;  // an object's stage explosion goes off at it (a car's is some 300 across)
		constexpr auto  RELOAD_AFTER = std::chrono::milliseconds(100);  // a model swapped: back on after this
		constexpr std::size_t MAX_KNOWN = 20000;  // as many as the session's table holds (maxWorldEntries)
		constexpr std::uint32_t DESTROYED_FLAG = 1u << 23;  // TESForm::formFlags: the engine's destroyed (TESForm::SetDestroyed)
		constexpr std::uint32_t CHANGE_FORM_FLAGS = 1;           // the save's change flags: the form flags (destroyed)
		constexpr std::uint32_t CHANGE_GAME_ONLY_EXTRA = 0x80000000;  // and the reference's extra data (health, model swap)

		// BGSDestructibleObjectForm::GetDestructibleForm(TESForm*): a form's destruction data by its form type (a
		// static, an activator, a container, ...), none for a type that has none.
		constexpr REL::ID GET_DESTRUCTIBLE{ 2192671 };
		// ExtraDataList::GetHealth: the reference's ExtraObjectHealth (-1 when it has none: never damaged).
		constexpr REL::ID GET_HEALTH{ 2190518 };
		// ExtraDataList::SetHealth(float): sets it (makes it when it has none), as the engine's damage does.
		constexpr REL::ID SET_HEALTH{ 2190519 };
		// ExtraDataList::SetModelSwap(BGSModelMaterialSwap*, TESBoundObject* base): the reference's model in its base's
		// place, as a stage's replacement model does (BGSDestructibleObjectForm's damage, 2192675).
		constexpr REL::ID SET_MODEL_SWAP{ 2190483 };
		// ExtraDataList::RemoveByType: what the engine's damage drops with the old model, its saved physics (kSavedHavokData).
		constexpr REL::ID REMOVE_EXTRA{ 2190971 };
		// BGSDestructibleObjectForm's damage stage on the 3D (destruction data, reference, force): the model's damaged
		// look for the reference's health, as loading its 3D does (TESObjectREFR::Load3D).
		constexpr REL::ID APPLY_DAMAGE_STAGE{ 2192683 };

		RE::BGSDestructibleObjectForm* DestructibleOf(RE::TESForm* a_base)
		{
			using func_t = RE::BGSDestructibleObjectForm* (*)(RE::TESForm*);
			static REL::Relocation<func_t> func{ GET_DESTRUCTIBLE };
			return a_base ? func(a_base) : nullptr;
		}

		float HealthOf(RE::ExtraDataList* a_list)
		{
			using func_t = float (*)(RE::ExtraDataList*);
			static REL::Relocation<func_t> func{ GET_HEALTH };
			return a_list ? func(a_list) : -1.0f;
		}

		void SetHealth(RE::ExtraDataList* a_list, float a_health)
		{
			using func_t = void (*)(RE::ExtraDataList*, float);
			static REL::Relocation<func_t> func{ SET_HEALTH };
			func(a_list, a_health);
		}

		void SetModelSwap(RE::ExtraDataList* a_list, RE::BGSModelMaterialSwap* a_swap, RE::TESBoundObject* a_base)
		{
			using func_t = void (*)(RE::ExtraDataList*, RE::BGSModelMaterialSwap*, RE::TESBoundObject*);
			static REL::Relocation<func_t> func{ SET_MODEL_SWAP };
			func(a_list, a_swap, a_base);
		}

		void RemoveExtra(RE::ExtraDataList* a_list, RE::EXTRA_DATA_TYPE a_type)
		{
			using func_t = void (*)(RE::ExtraDataList*, std::uint8_t);
			static REL::Relocation<func_t> func{ REMOVE_EXTRA };
			func(a_list, static_cast<std::uint8_t>(a_type));
		}

		bool ApplyDamageStage(RE::BGSDestructibleObjectForm* a_form, RE::TESObjectREFR* a_ref)
		{
			using func_t = bool (*)(RE::BGSDestructibleObjectForm*, RE::TESObjectREFR*, bool);
			static REL::Relocation<func_t> func{ APPLY_DAMAGE_STAGE };
			return func(a_form, a_ref, false);
		}

		// A reference's destruction, as the engine has it: none for an actor, a reference made at runtime, or one whose
		// base has no stages.
		struct Object
		{
			RE::BGSDestructibleObjectForm*            form = nullptr;
			std::vector<Stage>                        stages;
			std::vector<RE::DestructibleObjectStage*> engineStages;
			std::uint32_t                             maximum = 0;
			Copy                                      copy;
		};

		// A reference's destruction, or why it has none (a_why, for the log and the dev channel's probe).
		std::optional<Object> Read(RE::TESObjectREFR* a_ref, const char** a_why = nullptr)
		{
			const auto refuse = [&](const char* a_reason) {
				if (a_why) {
					*a_why = a_reason;
				}
				return std::optional<Object>{};
			};
			if (!a_ref) {
				return refuse("no such reference");
			}
			if (!Protocol::IsShareableRef(a_ref->GetFormID())) {
				return refuse("made at runtime, not placed by a plugin");
			}
			if (a_ref->As<RE::Actor>()) {
				return refuse("an actor");
			}
			const auto base = a_ref->GetObjectReference();
			const auto form = DestructibleOf(base);
			const auto data = form ? form->data : nullptr;
			if (!data) {
				return refuse("its base has no destruction data");
			}
			if (data->numStages <= 0 || !data->stagesArray) {
				return refuse("its base has no destruction stages");
			}
			if (data->health == 0) {
				return refuse("its base has 0 health");
			}
			Object out;
			out.form = form;
			out.maximum = data->health;
			for (std::int8_t i = 0; i < data->numStages; ++i) {
				const auto stage = data->stagesArray[i];
				if (!stage) {
					return refuse("a destruction stage is missing");
				}
				out.engineStages.push_back(stage);
				out.stages.push_back({ static_cast<std::uint8_t>(stage->healthPercentage), static_cast<std::uint8_t>(stage->flags),
					stage->explosion ? stage->explosion->GetFormID() : 0, stage->replacementModel != nullptr });
			}
			out.copy.health = HealthOf(a_ref->extraList.get());
			out.copy.destroyed = (a_ref->GetFormFlags() & DESTROYED_FLAG) != 0;
			// A deleted reference is left alone, but for one the engine deleted itself as its stage took it away: a barrel's
			// last stage disables it, then marks it deleted and destroyed (seen on the rig), and a gas canister's may only
			// take it away without marking it destroyed. That is the stage to report.
			if (a_ref->IsDeleted() &&
				!DestructionRules::DeletedByDestruction(out.stages, DestructionRules::Passed(out.stages, out.copy.health, out.maximum), out.copy.destroyed)) {
				return refuse("deleted, not by its destruction");
			}
			return out;
		}

		RE::TESObjectREFR* Lookup(std::uint32_t a_id)
		{
			return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
		}

		// The farthest an explosion of the object's stages reaches (or what one places: a car's nuke places the shove that
		// throws people about), 0 when none sets one off.
		float BlastRadiusOf(const Object& a_object)
		{
			float radius = 0.0f;
			for (const auto stage : a_object.engineStages) {
				const auto explosion = stage ? stage->explosion : nullptr;
				if (!explosion) {
					continue;
				}
				radius = (std::max)(radius, explosion->data.outerRadius);
				if (const auto placed = explosion->data.impactPlacedObject; placed && placed->Is(RE::ENUM_FORM_ID::kEXPL)) {
					if (const auto shove = placed->As<RE::BGSExplosion>()) {
						radius = (std::max)(radius, shove->data.outerRadius);
					}
				}
			}
			return radius;
		}

		struct Counts
		{
			std::uint32_t hits = 0;
			std::uint32_t hitsRefused = 0;  // hits on something not shared (logged, the first twenty)
			std::uint32_t reported = 0;
			std::uint32_t reportedExplosions = 0;
			std::uint32_t applied = 0;
			std::uint32_t already = 0;   // a friend's report our copy was as far gone as
			std::uint32_t notHere = 0;   // waited for its object to load
			std::uint32_t notDestructible = 0;
			std::uint32_t setOff = 0;
			std::uint32_t models = 0;    // replacement models put in
			std::uint32_t disabled = 0;
			std::uint32_t claimed = 0;   // explosions kept from WeaponFire's watch
			std::uint32_t scanned = 0;   // objects the last scan looked at
			long long     scanUs = 0;
			long long     scanUsMax = 0;
		};

		Counts                                       counts;
		DestructionRules::Watch                      watch;
		DestructionRules::Waiting                    waiting;
		std::unordered_map<std::uint32_t, Known>     known;     // what the session has of each object, as far as we know
		std::unordered_map<std::uint32_t, Known>     lastScan;  // the objects around us at the last scan
		std::vector<Protocol::Destruction>           outgoing;
		std::vector<std::pair<RE::ObjectRefHandle, Clock::time_point>> reloading;  // off for a model swap, back on then
		Clock::time_point                            nextPoll{};
		Clock::time_point                            nextScan{};
		std::uint32_t                                logged = 0;
		std::mutex                                   hitLock;
		std::vector<std::uint32_t>                   hitInbox;

		void Log(const std::string& a_line)
		{
			if (logged < 40) {
				++logged;
				REX::INFO("Destructibles: {}", a_line);
			}
		}

		void NoteKnown(std::uint32_t a_ref, const Known& a_now)
		{
			if (known.size() >= MAX_KNOWN && !known.contains(a_ref)) {
				known.clear();  // a flood: start over rather than grow (what is forgotten is reported again, at worst)
			}
			auto& entry = known[a_ref];
			entry.passed = (std::max)(entry.passed, a_now.passed);
			entry.destroyed = entry.destroyed || a_now.destroyed;
		}

		// What the session has of an object: what we reported or took, else what it was at our last scan, else whole.
		Known KnownOf(std::uint32_t a_ref)
		{
			if (const auto it = known.find(a_ref); it != known.end()) {
				return it->second;
			}
			if (const auto it = lastScan.find(a_ref); it != lastScan.end()) {
				return it->second;
			}
			return {};
		}

		// An object's space as the session has it: an interior cell (worldspace 0), a worldspace (cell 0), or none.
		std::pair<std::uint32_t, std::uint32_t> SpaceOf(RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref->GetParentCell();
			return !cell                ? std::pair{ 0u, 0u } :
			       cell->IsInterior()   ? std::pair{ cell->GetFormID(), 0u } :
			                              std::pair{ 0u, cell->worldSpace ? cell->worldSpace->GetFormID() : 0u };
		}

		// Our copy of an object as a report, if it is news for the session.
		void Check(RE::TESObjectREFR* a_ref, const Object& a_object, Clock::time_point a_now)
		{
			const auto id = a_ref->GetFormID();
			auto       report = DestructionRules::Due(id, a_object.stages, a_object.maximum, a_object.copy, KnownOf(id));
			if (!report) {
				return;
			}
			// Where it stands, for the server to hold against where we are.
			const auto [cell, worldspace] = SpaceOf(a_ref);
			report->cell = cell, report->worldspace = worldspace;
			report->x = a_ref->data.location.x, report->y = a_ref->data.location.y, report->z = a_ref->data.location.z;
			NoteKnown(id, { DestructionRules::Passed(a_object.stages, a_object.copy.health, a_object.maximum), a_object.copy.destroyed });
			watch.Changed(id, a_now);
			outgoing.push_back(*report);
			++counts.reported;
			counts.reportedExplosions += report->explode ? 1 : 0;
			// It blew (now, or long after our hit): an explosion its stage sets off beside its own is the others' to see too.
			if (const float radius = report->explode ? BlastRadiusOf(a_object) : 0.0f; radius > 0.0f) {
				WeaponFire::ArmBlastWatch(a_ref, radius);
			}
			Log(std::format("{:08X} ({:08X}) at {:.0f}/{} health{}: reported{}", id, a_ref->GetObjectReference()->GetFormID(), report->health, a_object.maximum,
				report->destroyed ? ", destroyed" : "", report->explode ? ", with its explosion" : ""));
		}

		void Scan(RE::PlayerCharacter* a_player)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			const auto                               started = Clock::now();
			std::unordered_map<std::uint32_t, Known> now;
			tes->ForEachReferenceInRange(a_player->data.location, SCAN_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto object = Read(a_ref);
				if (!object) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto id = a_ref->GetFormID();
				const Known here{ DestructionRules::Passed(object->stages, object->copy.health, object->maximum), object->copy.destroyed };
				now[id] = here;
				// Changed since the last look (by a hit of ours, an explosion, an NPC's shot): reported now, against what it was
				// then (lastScan is still the last look's), and watched closely from now on. One seen for the first time (just
				// loaded, or we just came near) is taken as it is: the damage of a save of our own, from before the session,
				// isn't the session's.
				const auto before = lastScan.find(id);
				if (before != lastScan.end() && (here.passed > before->second.passed || (here.destroyed && !before->second.destroyed))) {
					Check(a_ref, *object, started);
					watch.Add(id, started);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			counts.scanned = static_cast<std::uint32_t>(now.size());
			lastScan = std::move(now);
			counts.scanUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
			counts.scanUsMax = (std::max)(counts.scanUsMax, counts.scanUs);
		}

		void EnableReloaded(Clock::time_point a_now, bool a_all)
		{
			std::erase_if(reloading, [&](const auto& a_entry) {
				if (!a_all && a_now < a_entry.second) {
					return false;
				}
				const auto ref = a_entry.first.get();
				if (ref && !ref->IsDeleted() && ref->IsDisabled()) {
					ref->Enable(false);
				}
				return true;
			});
		}

		// Our copy takes a report: true when done with it (applied, or nothing to do), false when its object isn't in our
		// world yet.
		bool TryApply(const Protocol::Destruction& a_report, std::uint32_t a_playerId)
		{
			const auto ref = Lookup(a_report.refId);
			if (!ref) {
				return false;
			}
			const auto object = Read(ref);
			if (!object) {
				++counts.notDestructible;
				Log(std::format("{:08X} from player {}: not a destructible object here; left as it is", a_report.refId, a_playerId));
				return true;
			}
			const auto plan = DestructionRules::PlanApply(object->stages, object->maximum, object->copy, a_report);
			NoteKnown(a_report.refId, { (std::max)(plan.to, DestructionRules::Passed(object->stages, a_report.health, object->maximum)), a_report.destroyed || plan.destroyed });
			if (!plan.apply) {
				++counts.already;
				return true;
			}
			const auto list = ref->extraList.get();
			if (!list) {
				return true;
			}
			SetHealth(list, plan.health);
			if (plan.destroyed && (ref->GetFormFlags() & DESTROYED_FLAG) == 0) {
				ref->formFlags |= DESTROYED_FLAG;
				ref->AddChange(CHANGE_FORM_FLAGS);
			}
			ref->AddChange(CHANGE_GAME_ONLY_EXTRA);
			// The last replacement model of the stages passed is the one the engine shows (a car's hulk).
			RE::BGSModelMaterialSwap* swap = nullptr;
			if (plan.replaceModel) {
				for (std::size_t i = 0; i < plan.to; ++i) {
					if (object->engineStages[i]->replacementModel) {
						swap = object->engineStages[i]->replacementModel;
					}
				}
			}
			if (swap) {
				SetModelSwap(list, swap, ref->GetObjectReference());
				RemoveExtra(list, RE::EXTRA_DATA_TYPE::kSavedHavokData);
				++counts.models;
			}
			if (plan.disable) {
				if (!ref->IsDisabled()) {
					ref->Disable();
					++counts.disabled;
				}
			} else if (ref->Get3D()) {
				if (swap) {
					// Its 3D loads again with the new model, and the damage stage for its health with it (Load3D).
					if (!ref->IsDisabled()) {
						ref->Disable();
						reloading.emplace_back(ref->GetHandle(), Clock::now() + RELOAD_AFTER);
					}
				} else {
					ApplyDamageStage(object->form, ref);
				}
			}
			// Not in a catch-up: what the friend's game set off goes off here only now, as it happens.
			if (a_playerId != 0) {
				const auto  space = SpaceOf(ref);
				const auto& at = ref->data.location;
				for (const auto explosion : plan.explosions) {
					WeaponFire::ApplyExplosion({ a_playerId, explosion, at.x, at.y, at.z, space.first, space.second });
					++counts.setOff;
				}
			}
			++counts.applied;
			Log(std::format("{:08X} ({:08X}) from player {}: {:.0f}/{} health, stages {} to {}{}{}{}{}", a_report.refId, ref->GetObjectReference()->GetFormID(), a_playerId,
				plan.health, object->maximum, plan.from, plan.to, plan.destroyed ? ", destroyed" : "", swap ? ", new model" : "", plan.disable ? ", gone" : "",
				plan.explosions.empty() ? "" : std::format(", {} explosion(s)", plan.explosions.size())));
			return true;
		}
	}

	void Frame(bool a_welcomed)
	{
		const auto now = Clock::now();
		std::vector<std::uint32_t> hits;
		{
			std::scoped_lock l{ hitLock };
			hits.swap(hitInbox);
		}
		EnableReloaded(now, false);
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_welcomed || !player || !player->GetParentCell()) {
			return;
		}
		for (const auto id : hits) {
			const char* why = nullptr;
			const auto  ref = Lookup(id);
			const auto  object = Read(ref, &why);
			if (!object) {
				// A shot at something that may blow up and is not shared: the reason and the base's type, for the rig.
				if (counts.hitsRefused < 20) {
					++counts.hitsRefused;
					const auto base = ref ? ref->GetObjectReference() : nullptr;
					const auto name = base ? base->As<RE::TESFullName>() : nullptr;
					REX::INFO("Destructibles: hit on {:08X} (base {:08X}, form type {}, \"{}\") is not shared: {}", id, base ? base->GetFormID() : 0,
						base ? static_cast<int>(base->GetFormType()) : -1, name && name->GetFullName() ? name->GetFullName() : "", why ? why : "?");
				}
				continue;
			}
			++counts.hits;
			watch.Add(id, now);
			// Should its stage set off an explosion that isn't the object's own (a mine's), the others see it too: one within
			// its stages' blast radius, not every explosion around our player for the watch's 8 s (no watch for an object
			// none of whose stages sets one off). Its own goes with its wreck, whenever it blows (CopyHealth::OwnersToDeal).
			if (const float radius = BlastRadiusOf(*object); radius > 0.0f) {
				WeaponFire::ArmBlastWatch(Lookup(id), radius);
			}
		}
		if (now >= nextScan) {
			nextScan = now + SCAN_INTERVAL;
			Scan(player);
		}
		if (now >= nextPoll) {
			nextPoll = now + POLL_INTERVAL;
			for (const auto id : watch.Refs()) {
				const auto ref = Lookup(id);
				if (const auto object = Read(ref)) {
					Check(ref, *object, now);
				}
			}
			watch.Prune(now);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		std::vector<std::vector<std::uint8_t>> packets;
		for (std::size_t i = 0; i < outgoing.size(); i += Protocol::MAX_DESTRUCTION_ENTRIES) {
			const auto end = (std::min)(outgoing.size(), i + Protocol::MAX_DESTRUCTION_ENTRIES);
			packets.push_back(Protocol::Encode(Protocol::Destructions{ 0, { outgoing.begin() + i, outgoing.begin() + end } }, Protocol::MessageType::kReportDestruction));
		}
		outgoing.clear();
		return packets;
	}

	void Apply(const Protocol::Destructions& a_message)
	{
		for (const auto& entry : a_message.entries) {
			if (!WorldSync::InWorld() || Story::InOpening() || !TryApply(entry, a_message.playerId)) {
				waiting.Add(entry);
				++counts.notHere;
			} else {
				waiting.Erase(entry.refId);
			}
		}
	}

	void OnHit(std::uint32_t a_ref)
	{
		if (!Protocol::IsShareableRef(a_ref)) {
			return;
		}
		std::scoped_lock l{ hitLock };
		if (hitInbox.size() < 256) {
			hitInbox.push_back(a_ref);
		}
	}

	bool ClaimsExplosion(RE::TESObjectREFR* a_explosion)
	{
		const auto base = a_explosion ? a_explosion->GetObjectReference() : nullptr;
		const auto tes = RE::TES::GetSingleton();
		if (!base || !tes) {
			return false;
		}
		std::uint32_t claimedBy = 0;
		tes->ForEachReferenceInRange(a_explosion->data.location, CLAIM_RADIUS, [&](RE::TESObjectREFR* a_ref) {
			const auto object = Read(a_ref);
			if (!object) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto passed = DestructionRules::Passed(object->stages, object->copy.health, object->maximum);
			for (std::size_t i = 0; i < passed && claimedBy == 0; ++i) {
				const auto explosion = object->engineStages[i]->explosion;
				// Its own, or what that one places (a car's nuke places the shove that throws people about).
				if (explosion && (explosion == base || explosion->data.impactPlacedObject == base)) {
					claimedBy = a_ref->GetFormID();
				}
			}
			return claimedBy != 0 ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
		});
		if (claimedBy == 0) {
			return false;
		}
		++counts.claimed;
		// Its wreck is what sets it off in the others' games, so it must be reported (a car blown by an explosion near
		// it, not by a hit of ours, would wait for the next scan, and one out of the scan's reach never): watched, its
		// next look reports it (Check) unless the session has it already.
		watch.Add(claimedBy, Clock::now());
		return true;
	}

	bool AnyWaiting()
	{
		return !waiting.Empty();
	}

	std::vector<std::uint32_t> WaitingRefs()
	{
		return waiting.Refs();
	}

	bool ApplyWaiting(std::uint32_t a_ref)
	{
		const auto report = waiting.Find(a_ref);
		if (!report) {
			return false;
		}
		if (!TryApply(*report, 0)) {
			return false;
		}
		waiting.Erase(a_ref);
		return true;
	}

	void BeforeSaveOrLoad()
	{
		EnableReloaded(Clock::now(), true);
	}

	void Reset()
	{
		EnableReloaded(Clock::now(), true);
		watch.Clear();
		waiting.Clear();
		known.clear();
		lastScan.clear();
		outgoing.clear();
		std::scoped_lock l{ hitLock };
		hitInbox.clear();
	}

	std::string Describe()
	{
		const auto& c = counts;
		return std::format("destruct: hits={} watched={} reported={} withExplosion={} applied={} already={} waiting={} notDestructible={} setOff={} models={} gone={} claimed={} near={} scanUs={}/{}",
			c.hits, watch.Size(), c.reported, c.reportedExplosions, c.applied, c.already, waiting.Size(), c.notDestructible, c.setOff, c.models, c.disabled, c.claimed,
			c.scanned, c.scanUs, c.scanUsMax);
	}

	std::string Command(std::string_view a_args)
	{
		if (a_args.empty() || a_args == "status") {
			return Describe();
		}
		const auto line = [](RE::TESObjectREFR* a_ref, const Object& a_object) {
			const auto& l = a_ref->data.location;
			std::string stages;
			for (const auto& stage : a_object.stages) {
				stages += std::format("{}{}%{}{}{}", stages.empty() ? "" : ",", stage.healthPercent, stage.explosion ? "x" : "", stage.replacesModel ? "m" : "",
					(stage.flags & DestructionRules::STAGE_DISABLE) ? "d" : "");
			}
			return std::format("{:08X} base={:08X} health={:.0f}/{} passed={} destroyed={} disabled={} 3d={} stages=[{}] at=({:.0f},{:.0f},{:.0f})", a_ref->GetFormID(),
				a_ref->GetObjectReference()->GetFormID(), a_object.copy.health, a_object.maximum,
				DestructionRules::Passed(a_object.stages, a_object.copy.health, a_object.maximum), a_object.copy.destroyed, a_ref->IsDisabled(), a_ref->Get3D() != nullptr,
				stages, l.x, l.y, l.z);
		};
		if (a_args == "near" || a_args.starts_with("near ")) {
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return "error: no player";
			}
			float radius = 3000.0f;
			if (a_args.size() > 5) {
				const auto text = a_args.substr(5);
				std::from_chars(text.data(), text.data() + text.size(), radius);
			}
			std::vector<std::pair<float, std::string>> found;
			tes->ForEachReferenceInRange(player->data.location, radius, [&](RE::TESObjectREFR* a_ref) {
				if (const auto object = Read(a_ref)) {
					found.emplace_back(a_ref->data.location.GetDistance(player->data.location), line(a_ref, *object));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::ranges::sort(found, {}, &std::pair<float, std::string>::first);
			std::string out = std::format("{} destructible objects within {:.0f}", found.size(), radius);
			for (std::size_t i = 0; i < found.size() && i < 25; ++i) {
				out += std::format("\n{:.0f} away: {}", found[i].first, found[i].second);
			}
			return out;
		}
		if (a_args.starts_with("probe ")) {
			const auto    hex = a_args.substr(6);
			std::uint32_t id = 0;
			if (std::from_chars(hex.data(), hex.data() + hex.size(), id, 16).ec != std::errc{}) {
				return "error: usage: destruct probe <refHex>";
			}
			const auto ref = Lookup(id);
			if (!ref) {
				return "error: no such reference";
			}
			const char* why = nullptr;
			const auto  object = Read(ref, &why);
			if (!object) {
				return std::format("{:08X}: not a destructible object: {}", id, why ? why : "?");
			}
			return line(ref, *object) + std::format(" watched={} waiting={}", watch.Contains(id), waiting.Find(id) != nullptr);
		}
		return "error: usage: destruct [status | near [radius] | probe <refHex>]";
	}
}
