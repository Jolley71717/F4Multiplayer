#include "game/SaveGuard.h"
#include "game/DeferredDelete.h"
#include "game/Equipment.h"
#include "game/FrameLeftovers.h"
#include "game/FrameLoans.h"
#include "game/Hud.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/SaveStrays.h"
#include "game/Spawns.h"
#include "game/WorkshopSync.h"
#include "Text.h"

#include <cstring>

namespace SaveGuard
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The engine's save bookkeeping and two of its calls, by Address Library ID (1.11.240):
		// g_BGSSaveLoadGame (+32A85A8), BGSSaveLoadGame::ClearChanges(form ID) (+BECCC0) and
		// BGSSaveLoadGame::IsFormIDInUse(form ID) (+BEDEB0), which says whether the list of changes holds a
		// record for the id. CommonLibF4 names 2697789 BGSCreatedObjectManager::Singleton; the game's
		// symbols say otherwise. Dropping the record of a form still there never frees its id: the engine's
		// allocator of created ids (TESDataHandler::GetNextID, +2E6C40) passes over an id while a form is under
		// it (TESForm::GetFormByNumericID), before it asks about a record (IsFormIDInUse) or an unloaded form
		// (TESForm::IsUnloadedFormID).
		// A load frees what has a record and nothing else: BGSSaveLoadGame::RevertAllChanges (+BECD20) walks the
		// list of changes (+BEB700) and lets go of each created form with a record, a reference through
		// +BEDC70: TESDataHandler::ReleaseForm, demoted if it persists, out of its cell, then
		// DeferredDeleter::Add (which takes it out of the forms' map and destroys its handle at once). A created
		// form of ours whose record was dropped (they all are, as they go: DespawnAll) stays in memory through the
		// load, and a stand-in so kept points at its base, a face copy with a record, which the load frees: the
		// next load crashed on it (Actor::LoadGame, the process lists, RevertAllChanges). See BeforeLoad.
		constexpr REL::ID RELEASE_CREATED_REFERENCE{ 2227909 };
		// Not RemoveChanges(TESForm*) (+BECB60): it leaves a record alone once the form's cell has unloaded
		// and the record was written out to a buffer (UnloadForm), so a stand-in whose cell had unloaded
		// was saved all the same. ClearChanges takes the record out whether buffered or not, frees the
		// buffer, and also clears it from the second list a load keeps while it runs.
		// A buffered reference also stays listed under its cell (+E0) until that cell loads. That list is
		// only for this run (built again from the records at every load, never saved), and loading the
		// cell skips an id that has no record, so it is left as it is.
		constexpr REL::ID SAVE_LOAD_GAME{ 2697789 };
		constexpr REL::ID CLEAR_CHANGES{ 2227903 };
		constexpr REL::ID HAS_CHANGES{ 2227913 };
		// The engine's own test of what a reference is saved under (+BF82F0, TESObjectREFR* in, TESForm* out): its
		// worldspace, or its cell when it has no worldspace or the cell's flag 0x2000 is set. Null only when it has
		// neither. It takes no lock. The save (SaveInitialData) and the engine unloading a created reference's record read
		// what it returns without a check (see SaveStrays::Doomed).
		constexpr REL::ID SAVE_ANCHOR{ 2228018 };

		std::uint32_t excluded = 0;          // records removed (by anyone)
		std::uint32_t straysSaved = 0;       // strays whose record was kept out of a save
		std::uint32_t straysLoaded = 0;      // strays found right after loads
		std::uint32_t doomedSaved = 0;       // of straysSaved, ones with neither a cell nor a worldspace
		std::uint32_t doomedLoaded = 0;      // of straysLoaded, likewise
		std::uint32_t foreignDoomed = 0;     // doomed ones with a record, not ours, seen by a sweep (each one each time)
		std::uint32_t excludedAtDelete = 0;  // records of ours dropped as they were deleted or lost their cell (ExcludeOurs)
		std::uint32_t orphansDropped = 0;     // records of ours whose form the engine had unloaded with its cell (see SaveStrays::Book::KeepForgotten)
		std::uint32_t lastStray = 0;
		std::uint32_t searches = 0;          // of all forms, for strays

		SaveStrays::Book       book;        // what we placed, and what of it the game took from under us
		SaveStrays::Unpromoted unpromoted;  // the stand-ins the engine is not let keep persistent (see KeepUnpromoted)
		constexpr auto         FORGOTTEN_CHECK_INTERVAL = std::chrono::seconds(1);
		Clock::time_point      forgottenCheckAt{};

		// TESObjectREFR::GetAllowPromoteToPersistent (RE/T/TESObjectREFR.h, slot 0x59): the engine asks it first in
		// BGSDynamicPersistenceManager::PromoteReference and DemoteReference, and does nothing more on a no. An actor answers
		// yes (it is not overridden; read from disk, 1.11.240). Hooked on the vtable of the stand-ins (the Actor class),
		// where every other actor still gets the original answer.
		constexpr std::size_t ALLOW_PROMOTE_INDEX = 0x59;
		using AllowPromoteFn = bool (*)(const RE::TESObjectREFR*);
		std::uintptr_t        promoteVtable = 0;
		AllowPromoteFn        originalAllowPromote = nullptr;
		std::uint32_t         refusalsLogged = 0;
		Clock::time_point     refusalsLoggedAt{};
		constexpr auto        REFUSALS_LOG_INTERVAL = std::chrono::seconds(10);

		// Runs on whichever thread asks (a quest filling an alias, a task, the main thread); reads only the reference's own
		// fields.
		bool AllowPromoteHook(const RE::TESObjectREFR* a_this)
		{
			if (a_this) {
				const auto base = a_this->GetObjectReference();
				if (unpromoted.Refuses(a_this, a_this->GetFormID(), base ? base->GetFormID() : 0)) {
					return false;
				}
			}
			return originalAllowPromote(a_this);
		}

		bool InstallPromoteHook(RE::Actor* a_actor)
		{
			const auto vtable = *reinterpret_cast<std::uintptr_t*>(a_actor);
			if (promoteVtable == vtable) {
				return true;
			}
			if (promoteVtable != 0) {
				REX::WARN("SaveGuard: actor {:08X} uses another vtable; the engine may still keep it persistent", a_actor->GetFormID());
				return false;
			}
			const auto slot = vtable + sizeof(void*) * ALLOW_PROMOTE_INDEX;
			originalAllowPromote = *reinterpret_cast<AllowPromoteFn*>(slot);
			REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&AllowPromoteHook));
			promoteVtable = vtable;
			REX::INFO("SaveGuard: hooked the engine's persistence question (vtable {:X})", vtable);
			return true;
		}

		// What KeepForgotten found gone is no longer ours to refuse.
		void LetGoOfGone()
		{
			for (const auto& [id, base] : book.TakeGone()) {
				unpromoted.Remove(id, base);
			}
		}

		// Now and then, how often the engine was told no (each one a stand-in it would have kept past its cell).
		void LogRefusals(Clock::time_point a_now)
		{
			const auto refused = unpromoted.Refused();
			if (refused == refusalsLogged || a_now < refusalsLoggedAt + REFUSALS_LOG_INTERVAL) {
				return;
			}
			REX::INFO("SaveGuard: the engine asked {} more time(s) to keep a stand-in persistent or let it go; told no ({} in all)", refused - refusalsLogged, refused);
			refusalsLogged = refused;
			refusalsLoggedAt = a_now;
		}

		void* SaveLoadGame()
		{
			static REL::Relocation<void**> game{ SAVE_LOAD_GAME };
			return *game;
		}

		bool HasChanges(std::uint32_t a_id)
		{
			static REL::Relocation<bool (*)(void*, std::uint32_t)> hasChanges{ HAS_CHANGES };
			const auto game = SaveLoadGame();
			return game && hasChanges(game, a_id);
		}

		bool ExcludeId(std::uint32_t a_id)
		{
			static REL::Relocation<void (*)(void*, std::uint32_t)> clearChanges{ CLEAR_CHANGES };
			if (!HasChanges(a_id)) {
				return false;  // nothing would be saved for it
			}
			clearChanges(SaveLoadGame(), a_id);
			const bool removed = !HasChanges(a_id);
			excluded += removed;
			return removed;
		}

		// The base of the reference under the id now (0 when it has none), or nothing when there is no reference.
		std::optional<std::uint32_t> BaseNow(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref) {
				return std::nullopt;
			}
			const auto base = ref->GetObjectReference();
			return base ? base->GetFormID() : 0;
		}

		// Each reference of ours on its way out as SaveGuard noted it just before a load (BeforeLoad): what tells the form under
		// its id after the load from one the loaded game made since under the same id (see SaveStrays::Remains). The address
		// and extra data are only compared, never read through.
		struct Noted
		{
			const void*   ref = nullptr;
			const void*   extra = nullptr;
			const void*   ownership = nullptr;  // its ownership extra data, if it had one (a stand-in's copy: see SaveStrays::Remains)
			std::uint32_t base = 0;
			float         x = 0.0f, y = 0.0f, z = 0.0f;
			bool          on = false;           // neither disabled nor deleted (see SaveStrays::LingerOf)
		};
		std::unordered_map<std::uint32_t, Noted> noted;

		std::uint32_t lingered = 0;         // ours from before a load found still there after it (see SaveStrays::Book::AfterLoad)
		std::uint32_t lingeringDropped = 0; // of those, let go of later: no longer provably ours and nobody's (see SaveStrays::Remains)
		std::uint32_t turnedOff = 0;        // of those, disabled after the load: buried before their 3D came in (SaveStrays::LingerOf)

		// What BeforeLoad did with ours on their way out (SaveStrays::Handover).
		std::uint32_t leftToLoad = 0;     // had a record: the load frees them
		std::uint32_t recordsGiven = 0;   // given their record back, so the load frees them
		std::uint32_t releasedAtLoad = 0; // refused one: let go of just before the load, as the load does
		std::uint32_t basesKept = 0;      // created bases of actors so handed over, kept in memory through the load (see HandToLoad)

		// Strays of ours a save brought back (a friend's stand-in an earlier version wrote), their record dropped after that
		// load (AfterLoad): kept in memory by that, and pointing at a base the next load frees, so each is handed to the next
		// load (BeforeLoad) while it is still the same form. Cleared at each load and new game.
		struct BroughtBack
		{
			std::uint32_t id = 0;
			std::uint32_t base = 0;
			const void*   ref = nullptr;
		};
		std::vector<BroughtBack>  broughtBack;
		constexpr std::size_t     BROUGHT_BACK_MAX = 256;
		std::uint32_t             broughtBackHanded = 0;  // of those, handed to a load
		std::uint32_t             oldSaveNotices = 0;     // loads of a save holding one of those, told on the HUD

		// Somebody here is in this frame, or our player is in it or got out of it last. Every process list: a settler or
		// companion away in their suit is in a low one, and the frame they wear may stand loaded where they got in.
		bool Worn(const RE::TESObjectREFR* a_frame)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (player && (player->lastUsedPowerArmor.get().get() == a_frame || Equipment::PowerArmorFrameOf(player).get() == a_frame)) {
				return true;
			}
			const auto inIt = [&](RE::Actor* a_actor) {
				const auto middle = a_actor && a_actor->currentProcess ? a_actor->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				// After a load the furniture it uses is not set yet (as for our player): the frame the engine holds as worn is.
				return (occupied && occupied.get() == a_frame) || (a_actor && Equipment::PowerArmorFrameOf(a_actor).get() == a_frame);
			};
			if (inIt(player)) {
				return true;
			}
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
				for (const auto& handle : *handles) {
					if (const auto actor = handle.get(); actor && inIt(actor.get())) {
						return true;
					}
				}
			}
			return false;
		}

		// Disabled frames made at runtime that the last load brought, until each is judged (see FrameLeftovers).
		FrameLeftovers::Watch leftovers;
		std::uint32_t         leftoversFound = 0;    // looked at after loads
		std::uint32_t         leftoversCleared = 0;  // of those, nobody's: buried as every copy of ours is
		std::uint32_t         leftoversKept = 0;     // of those, someone's, or no longer the same form: left to the game

		// Where a held copy stands (see HeldCopy::Spot).
		HeldCopy::Spot SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto  cell = a_ref->GetParentCell();
			const auto& at = a_ref->data.location;
			return { cell ? cell->GetFormID() : 0, at.x, at.y, at.z };
		}

		// Its ownership extra data (the owner it was given: SetOwner), compared by address only; none when it has no owner of its own.
		const void* OwnershipOf(const RE::TESObjectREFR* a_ref)
		{
			return a_ref->extraList ? a_ref->extraList->GetByType(RE::EXTRA_DATA_TYPE::kOwnership) : nullptr;
		}

		bool HoldsItems(RE::TESObjectREFR* a_ref)
		{
			const auto list = a_ref->inventoryList;
			if (!list) {
				return false;
			}
			const RE::BSAutoReadLock l{ list->rwLock };
			return !list->data.empty();
		}

		// What is under the id now, against what was noted of our copy there before the load (see SaveStrays::Remains).
		SaveStrays::Remains RemainsOf(std::uint32_t a_id, std::uint32_t a_base)
		{
			SaveStrays::Remains remains;
			const auto          it = noted.find(a_id);
			const auto          ref = it != noted.end() ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) : nullptr;
			const auto          base = ref ? ref->GetObjectReference() : nullptr;
			if (!base) {
				return remains;
			}
			const auto& was = it->second;
			const auto& at = ref->data.location;
			remains.same = was.ref == ref && was.extra == ref->extraList.get() && was.base == a_base && base->GetFormID() == a_base && at.x == was.x && at.y == was.y && at.z == was.z;
			if (!remains.same) {
				return remains;
			}
			remains.frame = Equipment::IsPowerArmorFrame(base);
			const auto actor = ref->As<RE::Actor>();
			remains.actor = actor != nullptr;
			remains.puppet = actor && Puppets::IsRegistered(actor);
			remains.off = ref->IsDeleted() || ref->IsDisabled();
			remains.notedOn = was.on;
			if (remains.frame) {
				remains.worn = Worn(ref);
				remains.newOwner = OwnershipOf(ref) != was.ownership;  // one it had when noted is the stand-in's; a new or another one is not ours
				remains.holdsItems = HoldsItems(ref);
			}
			return remains;
		}

		// The reference's record flag (as in HavokGuard): the engine sets it on a frame it promotes as an actor gets in.
		constexpr std::uint32_t PERSISTENT_FLAG = 1u << 10;

		// Whether a frame never looked into would be filled from its base's container (a loot frame's pieces) the first time
		// its inventory is read: one that has no inventory of its own yet holds what its base holds.
		bool BaseFills(RE::TESBoundObject* a_base)
		{
			const auto furniture = a_base ? a_base->As<RE::TESFurniture>() : nullptr;
			const auto container = furniture ? furniture->GetContainer() : nullptr;
			if (!container) {
				return false;
			}
			bool any = false;
			container->ForEachContainerObject([&](RE::ContainerObject& a_object) {
				any = a_object.obj != nullptr;
				return !any;
			});
			return any;
		}

		// Reads a_size bytes at a_address into a_out, guarded: the address may be anything.
		bool ReadGuarded(std::uintptr_t a_address, void* a_out, std::size_t a_size)
		{
			__try {
				std::memcpy(a_out, reinterpret_cast<const void*>(a_address), a_size);
				return true;
			} __except (1) {
				return false;
			}
		}

		// Where the ownership extra data holds its owner (right after BSExtraData's header; CommonLibF4 has no type for it),
		// and where a form holds its id (TESForm::formID).
		constexpr std::uintptr_t OWNERSHIP_OWNER = 0x18;
		constexpr std::uintptr_t FORM_ID = 0x14;
		static_assert(sizeof(RE::BSExtraData) == OWNERSHIP_OWNER);

		// The form ID of the owner a reference's ownership extra data names, when that is a form in our game now; none when
		// it has none, names none, or cannot be read. Every read is guarded, and what it reads is only taken when the forms'
		// map has that very form under that id: an owner the load freed (a stand-in's face copy), or a wrong guess at the
		// layout, never names a live form, and a frame whose owner is not known is someone's (FrameLeftovers::Owned).
		std::optional<std::uint32_t> OwnerIdOf(const RE::TESObjectREFR* a_ref)
		{
			const auto extra = a_ref->extraList ? a_ref->extraList->GetByType(RE::EXTRA_DATA_TYPE::kOwnership) : nullptr;
			const void* owner = nullptr;
			if (!extra || !ReadGuarded(reinterpret_cast<std::uintptr_t>(extra) + OWNERSHIP_OWNER, &owner, sizeof(owner)) || !owner) {
				return std::nullopt;
			}
			std::uint32_t id = 0;
			if (!ReadGuarded(reinterpret_cast<std::uintptr_t>(owner) + FORM_ID, &id, sizeof(id)) || id == 0) {
				return std::nullopt;
			}
			return static_cast<const void*>(RE::TESForm::GetFormByID(id)) == owner ? std::optional{ id } : std::nullopt;
		}

		// A disabled frame made at runtime, as FrameLeftovers judges it.
		FrameLeftovers::Facts LeftoverFactsOf(RE::TESObjectREFR* a_ref)
		{
			FrameLeftovers::Facts facts;
			const auto            base = a_ref->GetObjectReference();
			facts.frame = Equipment::IsPowerArmorFrame(base);
			facts.created = a_ref->IsCreated();
			facts.disabled = a_ref->IsDisabled();
			facts.deleted = a_ref->IsDeleted();
			facts.persistent = (a_ref->GetFormFlags() & PERSISTENT_FLAG) != 0;
			facts.holdsItems = a_ref->inventoryList ? HoldsItems(a_ref) : BaseFills(base);
			const auto extra = a_ref->extraList.get();
			facts.quest = extra && extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			facts.linked = extra && (extra->HasType(RE::EXTRA_DATA_TYPE::kLinkedRef) || extra->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent));
			facts.worn = Worn(a_ref);
			const auto id = a_ref->GetFormID();
			// One lingering from before the load is provably our old copy, its ownership the one it had then (the stand-in's,
			// whose base the load may have freed: never read through). Any other's owner is read with care (OwnerIdOf).
			facts.lingering = book.IsLingering(id, base ? base->GetFormID() : 0);
			const bool hasOwner = extra && extra->HasType(RE::EXTRA_DATA_TYPE::kOwnership);
			if (hasOwner && !facts.lingering) {
				facts.owner = FrameLeftovers::OwnerOf(hasOwner, OwnerIdOf(a_ref), RemotePlayers::IsStandInBase);
			}
			facts.marked = WorkshopSync::IsCopyMarked(*a_ref);
			// A frame of ours lent to a friend (FrameLoans) is hidden, not disabled, and one lent to us is disabled only while
			// our player wears it (Worn): a backstop.
			facts.lent = FrameLoans::LentOut(id);
			facts.session = Placed(a_ref) || RemotePlayers::OnItsWayOut(a_ref) || Spawns::CopyIdOf(id) != 0;
			return facts;
		}

		// Right after a load: every disabled frame made at runtime is watched, to be judged a moment later (JudgeLeftovers).
		// Picked by id under the forms' lock, as Sweep picks; whether its base is a frame is read once the lock is let go.
		void FindLeftovers()
		{
			leftovers.Clear();
			std::vector<std::uint32_t> off;
			{
				const auto& [map, lock] = RE::TESForm::GetAllForms();
				const RE::BSAutoReadLock l{ lock };
				if (!map) {
					return;
				}
				for (const auto& [id, form] : *map) {
					if (SaveStrays::IsCreatedId(id) && form) {
						if (const auto ref = form->As<RE::TESObjectREFR>(); ref && ref->IsDisabled() && !ref->IsDeleted()) {
							off.push_back(id);
						}
					}
				}
			}
			const auto now = Clock::now();
			for (const auto id : off) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
				const auto base = ref ? ref->GetObjectReference() : nullptr;
				if (base && Equipment::IsPowerArmorFrame(base)) {
					leftovers.Add(id, base->GetFormID(), ref, now);
				}
			}
			if (leftovers.Size() != 0) {
				REX::INFO("SaveGuard: the load brought {} disabled frame(s) made at runtime; each is looked at in {} s", leftovers.Size(), FrameLeftovers::LOOK_AGAIN.count());
			}
		}

		// The disabled frames found after the load, once they have stood so a while: one that is provably nobody's (a copy of
		// ours a save kept switched off) goes as every copy of ours goes, deleted and kept out of every save from then on.
		void JudgeLeftovers(Clock::time_point a_now)
		{
			if (leftovers.Size() == 0) {
				return;
			}
			for (const auto& found : leftovers.TakeDue(a_now, Hud::LoadingScreenUp())) {
				++leftoversFound;
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(found.id);
				const auto base = ref ? ref->GetObjectReference() : nullptr;
				if (!FrameLeftovers::Same(found, ref, base ? base->GetFormID() : 0)) {
					++leftoversKept;
					continue;
				}
				const auto facts = LeftoverFactsOf(ref);
				const auto owner = facts.owner == FrameLeftovers::Owner::kNone ? "none" : facts.owner == FrameLeftovers::Owner::kStandIn ? "a stand-in's base" : "someone's";
				if (!FrameLeftovers::Leftover(facts)) {
					++leftoversKept;
					REX::INFO("SaveGuard: disabled frame {:08X} (base {:08X}) is left as it is ({}; deleted={} persistent={} holds={} quest={} linked={} worn={} owner={} lent={} session={} lingering={} marked={})", found.id,
						found.base, FrameLeftovers::Proven(facts) ? "ours, but not nobody's" : "no proof it is ours", facts.deleted, facts.persistent, facts.holdsItems, facts.quest, facts.linked, facts.worn, owner, facts.lent,
						facts.session, facts.lingering, facts.marked);
					continue;
				}
				++leftoversCleared;
				const auto  cell = ref->GetParentCell();
				const auto& at = ref->data.location;
				REX::INFO("SaveGuard: disabled frame {:08X} (base {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X} is a frame copy a save kept switched off ({}), empty and nobody's; it goes ({} so far)", found.id, found.base,
					at.x, at.y, at.z, cell ? cell->GetFormID() : 0, facts.lingering ? "ours from before the load" : facts.marked ? "a workshop copy's keyword" : "a stand-in's base owns it", leftoversCleared);
				// Ours from now on, as a copy placed this run is, so no save keeps it and a load that comes first frees it.
				// Disabled long since, so no 3D of it is loading: the graveyard deletes it a second from now.
				Track(ref);
				RemotePlayers::Discard(ref->GetHandle(), a_now - DeferredDelete::MAX_WAIT);
			}
		}

		std::string_view KindOf(const SaveStrays::Remains& a_remains)
		{
			return a_remains.frame ? "frame copy" : (a_remains.actor ? "stand-in or NPC copy" : "light or copy");
		}

		// Right after a load: one of ours from before it, still there on its way out (see SaveStrays::Book::AfterLoad). One
		// buried before its 3D came in is disabled now (SaveStrays::LingerOf).
		bool Lingers(std::uint32_t a_id, std::uint32_t a_base)
		{
			auto       remains = RemainsOf(a_id, a_base);
			const auto linger = SaveStrays::LingerOf(remains);
			if (linger == SaveStrays::Linger::kNo) {
				return false;
			}
			if (linger == SaveStrays::Linger::kTurnOff) {
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id)) {
					ref->Disable();
				}
				noted[a_id].on = false;
				++turnedOff;
				REX::INFO("SaveGuard: {:08X}, ours from before the load, was buried before its 3D came in, so never disabled; disabled now", a_id);
			}
			++lingered;
			// An actor's base is a face copy the load may have freed (see RELEASE_CREATED_REFERENCE): worth a line of its own.
			if (remains.actor) {
				REX::WARN("SaveGuard: {:08X} (base {:08X}), our {} from before the load, is still there on its way out; kept out of saves while it stays so", a_id, a_base, KindOf(remains));
			} else {
				REX::INFO("SaveGuard: {:08X} (base {:08X}), our {} from before the load, is still there on its way out; kept out of saves while it stays so", a_id, a_base, KindOf(remains));
			}
			return true;
		}

		// The engine's own letting go of a created reference at a load (see RELEASE_CREATED_REFERENCE).
		void ReleaseNow(RE::TESObjectREFR* a_ref)
		{
			static REL::Relocation<void (*)(void*, RE::TESObjectREFR*)> release{ RELEASE_CREATED_REFERENCE };
			if (const auto game = SaveLoadGame()) {
				release(game, a_ref);
			}
		}

		// Just before a load: a reference of ours on its way out, so the load frees it with the old world (SaveStrays::Handover).
		void HandToLoad(RE::TESObjectREFR* a_ref, std::string_view a_what)
		{
			const auto id = a_ref->GetFormID();
			// The load demotes a persistent one as it lets go of it; a stand-in told no (KeepUnpromoted) stayed listed as
			// persistent once freed, and the load after that crashed (Actor::Revert). It is let go of from here on.
			const auto base = a_ref->GetObjectReference();
			const auto baseId = base ? base->GetFormID() : 0;
			unpromoted.Remove(id, baseId);
			// A stand-in or mirrored NPC handed over is no puppet of ours from here on. Its holder unregisters it only after
			// the load (RemotePlayers::OnGameLoaded, after AfterLoad), and one buried before its 3D came in was never
			// disabled, so never unregistered: kept by the load all the same, it would read as ours in use, not as lingering.
			if (const auto actor = a_ref->As<RE::Actor>()) {
				Puppets::Unregister(actor, id);
			}
			// An actor is freed some frames after the load lets go of it (DeferredDeleter), and reads its base as it goes; a
			// created base with a record (a stand-in's face copy) the load frees at once. Freed so, the load after it crashed
			// in memory the base had held (Actor::Revert on another actor). So the base keeps no record: the load leaves it
			// in memory, nobody's (a face copy is used by its stand-in alone), and no save writes it.
			if (a_ref->As<RE::Actor>() && SaveStrays::IsCreatedId(baseId) && ExcludeId(baseId)) {
				++basesKept;
			}
			const bool had = HasChanges(id);
			if (!had) {
				a_ref->AddChange(1);  // the change of its form flags; the load drops the record, whatever it holds
			}
			switch (SaveStrays::HandoverOf(had, !had && HasChanges(id))) {
			case SaveStrays::Handover::kHasRecord:
				++leftToLoad;
				break;
			case SaveStrays::Handover::kGiveRecord:
				++recordsGiven;
				REX::INFO("SaveGuard: {:08X}, {}, is given its record back, so the load frees it", id, a_what);
				break;
			case SaveStrays::Handover::kRelease:
				++releasedAtLoad;
				REX::INFO("SaveGuard: {:08X}, {} (deleted={}), takes no record; let go of now, as the load would", id, a_what, a_ref->IsDeleted());
				ReleaseNow(a_ref);
				break;
			}
		}

		bool StillLingers(std::uint32_t a_id, std::uint32_t a_base)
		{
			return SaveStrays::StillOurs(RemainsOf(a_id, a_base));
		}

		void LingeringDropped(std::uint32_t a_id)
		{
			++lingeringDropped;
			noted.erase(a_id);
			REX::INFO("SaveGuard: {:08X}, our copy from before the load, is no longer provably ours and nobody's; left to the game from now on", a_id);
		}

		void KeepLingering()
		{
			book.KeepLingering(StillLingers, [](std::uint32_t a_id) { excludedAtDelete += ExcludeId(a_id); }, LingeringDropped);
		}

		// One of ours the engine unloaded with its cell, its form gone and its record kept under the cell, which would bring it
		// back when the cell loads again (see SaveStrays::Book::KeepForgotten): the record is dropped. Only with no form of
		// any kind under the id (BaseNow looks for a reference): a runtime form of another kind there now owns any record.
		void DropOrphan(std::uint32_t a_id)
		{
			if (RE::TESForm::GetFormByID(a_id) || !ExcludeId(a_id)) {
				return;
			}
			++orphansDropped;
			REX::INFO("SaveGuard: {:08X}, ours, was unloaded with its cell; its record is dropped, so the cell never brings it back", a_id);
		}

		// Whether a reference carries the faction change every stand-in gets (see SaveStrays::STAND_IN_FACTION).
		// The changes are the list's, so they are read under its lock.
		bool InStandInFaction(const RE::ExtraDataList& a_list)
		{
			const RE::BSAutoReadLock l{ a_list.extraRWLock };
			const auto changes = static_cast<const RE::ExtraFactionChanges*>(a_list.extraData.GetByType(RE::ExtraFactionChanges::TYPE));
			if (!changes) {
				return false;
			}
			for (const auto& change : changes->factionChanges) {
				if (change.faction && SaveStrays::StandInFaction(change.faction->GetFormID(), change.rank)) {
					return true;
				}
			}
			return false;
		}

		// What an actor was placed from (a leveled one's own base is the game's copy, its leveled data names
		// what was placed), and whether it carries the stand-ins' faction change.
		SaveStrays::Origin OriginOf(std::uint32_t a_id, std::uint32_t a_base)
		{
			SaveStrays::Origin origin{ .base = a_base };
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
			if (!actor || !actor->extraList) {
				return origin;
			}
			if (const auto leveled = actor->extraList->GetByType<RE::ExtraLeveledCreature>()) {
				origin.leveled = true;
				origin.leveledFrom = leveled->originalBase ? leveled->originalBase->GetFormID() : 0;
			}
			origin.standInFaction = InStandInFaction(*actor->extraList);
			return origin;
		}

		// The created references in no cell (only those can be strays: with a cell, that is what the engine saves them
		// under), of every kind, as the mod places frame copies and lights besides stand-ins, and the engine's save
		// crashes on any created reference with a record and neither a cell nor a worldspace, whoever placed it. Which
		// of them are strays, and whose, is SaveStrays::Book's to say.
		std::vector<SaveStrays::Found> Sweep()
		{
			++searches;
			// Picked under the forms' lock and looked at after it is let go: the anchor reads extra data, which has a
			// lock of its own.
			std::vector<std::uint32_t> loose;
			{
				const auto& [map, lock] = RE::TESForm::GetAllForms();
				const RE::BSAutoReadLock l{ lock };
				if (!map) {
					return {};
				}
				// The map's key is the form's id, so the created ones are picked by it, without reading each of the game's
				// other forms (most of them, many cold in memory) under the lock at every save and load.
				for (const auto& [id, form] : *map) {
					if (SaveStrays::IsCreatedId(id) && form) {
						if (const auto ref = form->As<RE::TESObjectREFR>(); ref && !ref->GetParentCell()) {
							loose.push_back(id);
						}
					}
				}
			}
			std::vector<SaveStrays::Found> found;
			for (const auto id : loose) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
				if (!ref || ref->GetParentCell()) {
					continue;
				}
				const auto base = ref->GetObjectReference();
				found.push_back({
					.id = id,
					.base = base ? base->GetFormID() : 0,
					.actor = ref->Is(RE::ENUM_FORM_ID::kACHR),
					.seen = { .inCell = false, .hasBase = base != nullptr, .deleted = ref->IsDeleted(), .anchored = Anchored(ref) },
				});
			}
			return found;
		}

		// Whether an actor not placed this run was placed as stand-ins are (SaveStrays::StandIn).
		bool StandIn(std::uint32_t a_id, std::uint32_t a_base)
		{
			return SaveStrays::StandIn(OriginOf(a_id, a_base), RemotePlayers::IsStandInBase);
		}

		// The form type and flags of the reference under the id, for the log.
		std::pair<std::uint32_t, std::uint32_t> FormInfo(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			return ref ? std::pair{ static_cast<std::uint32_t>(std::to_underlying(ref->GetFormType())), ref->formFlags } : std::pair{ 0u, 0u };
		}
		std::string Describe(const SaveStrays::Found& a_c, bool a_hasChanges)
		{
			const auto [type, flags] = FormInfo(a_c.id);
			return std::format("{:08X} (form type {}, base {:08X}, flags {:08X}, deleted={} anchored={} ours={} record={})", a_c.id, type, a_c.base, flags, a_c.seen.deleted,
				a_c.seen.anchored, a_c.seen.ours, a_hasChanges);
		}

		// Every read is guarded: the address may be anything.
		bool ReadPointer(std::uintptr_t a_address, const void*& a_out)
		{
			__try {
				a_out = *reinterpret_cast<const void* const*>(a_address);
				return true;
			} __except (1) {
				return false;
			}
		}

		// Copies the class name into a_out (at most a_size - 1 characters), every read guarded, the
		// name's own characters too.
		bool CopyTypeName(const void* a_object, std::uintptr_t a_base, char* a_out, std::size_t a_size)
		{
			__try {
				const auto vtable = *static_cast<const std::uintptr_t* const*>(a_object);
				// The complete object locator sits just before the vtable; its type descriptor (an offset from
				// the module) names the class as ".?AVName@@".
				const auto locator = reinterpret_cast<const std::uint32_t*>(vtable[-1]);
				const auto name = reinterpret_cast<const char*>(a_base + locator[3] + 16);
				std::size_t i = 0;
				for (; i + 1 < a_size && name[i]; ++i) {
					a_out[i] = name[i];
				}
				a_out[i] = '\0';
				return true;
			} __except (1) {
				return false;
			}
		}
	}

	bool Exclude(RE::TESForm* a_form)
	{
		return a_form && ExcludeId(a_form->GetFormID());
	}

	bool Anchored(RE::TESObjectREFR* a_ref)
	{
		static REL::Relocation<RE::TESForm* (*)(RE::TESObjectREFR*)> saveAnchor{ SAVE_ANCHOR };
		return a_ref && saveAnchor(a_ref) != nullptr;
	}

	bool ExcludeOurs(RE::TESObjectREFR* a_ref)
	{
		const bool removed = Exclude(a_ref);
		excludedAtDelete += removed;
		return removed;
	}

	void Track(RE::TESObjectREFR* a_ref)
	{
		if (a_ref) {
			const auto base = a_ref->GetObjectReference();
			book.Track(a_ref->GetFormID(), base ? base->GetFormID() : 0);
		}
	}

	void Track(std::uint32_t a_id, std::uint32_t a_base)
	{
		if (a_id != 0) {
			book.Track(a_id, a_base);
		}
	}

	bool Placed(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		const auto base = a_ref->GetObjectReference();
		return book.IsPlaced(a_ref->GetFormID(), base ? base->GetFormID() : 0);
	}

	void Forget(std::uint32_t a_id)
	{
		// Gone already (unloaded with its cell): off the books at once, and its record goes now, before the cell could load
		// again and bring it back (see SaveStrays::Book::ForgetGone).
		if (!RE::TESForm::GetFormByID(a_id)) {
			if (book.ForgetGone(a_id)) {
				DropOrphan(a_id);
			}
			return;
		}
		book.Forget(a_id);
	}

	HeldCopy::Lost Refresh(RE::ObjectRefHandle& a_handle, HeldCopy::Ids& a_ids)
	{
		const auto held = a_handle.get();
		const bool resolves = static_cast<bool>(held);
		if (resolves && held->GetFormID() == a_ids.id) {
			a_ids.at = SpotOf(held.get());  // where it stands now: a copy back from its record will stand there (HeldCopy::Look)
		}
		if (resolves || a_ids.id == 0) {
			return HeldCopy::Lost::kNone;
		}
		HeldCopy::Facts facts;
		const auto      ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ids.id);
		if (ref) {
			const auto base = ref->GetObjectReference();
			facts.baseNow = base ? base->GetFormID() : 0;
			facts.deleted = ref->IsDeleted();
			facts.ours = Placed(ref);
			if (ref->GetParentCell()) {
				facts.atNow = SpotOf(ref);
			}
		} else if (RE::TESForm::GetFormByID(a_ids.id)) {
			facts.baseNow = 0;  // a form of another kind under the id now
		}
		const auto lost = HeldCopy::Look(a_ids, resolves, facts);
		if (lost == HeldCopy::Lost::kBack) {
			a_handle = ref->GetHandle();
		} else if (lost == HeldCopy::Lost::kForget) {
			Forget(a_ids.id);
			a_handle = {};
			a_ids = {};
		}
		return lost;
	}

	HeldCopy::Ids IdsOf(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return {};
		}
		const auto base = a_ref->GetObjectReference();
		return { a_ref->GetFormID(), base ? base->GetFormID() : 0, SpotOf(a_ref) };
	}

	void Release(std::uint32_t a_id)
	{
		book.Release(a_id);
	}

	void KeepUnpromoted(RE::Actor* a_actor)
	{
		if (!a_actor || !InstallPromoteHook(a_actor)) {
			return;
		}
		const auto base = a_actor->GetObjectReference();
		unpromoted.Add(a_actor, a_actor->GetFormID(), base ? base->GetFormID() : 0);
	}

	void Frame()
	{
		const auto now = Clock::now();
		LogRefusals(now);
		JudgeLeftovers(now);
		if ((book.ForgottenCount() == 0 && book.LingeringCount() == 0) || now < forgottenCheckAt) {
			return;
		}
		forgottenCheckAt = now + FORGOTTEN_CHECK_INTERVAL;
		book.KeepForgotten(BaseNow, [](std::uint32_t a_id) { excludedAtDelete += ExcludeId(a_id); }, DropOrphan);
		KeepLingering();
		LetGoOfGone();
	}

	void BeforeLoad()
	{
		noted.clear();
		std::vector<std::pair<RE::TESObjectREFR*, bool>> wayOut;  // and whether it lingers from the last load
		book.ForEachHeld([&](std::uint32_t a_id, std::uint32_t a_base) {
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			const auto base = ref ? ref->GetObjectReference() : nullptr;
			// Only one on its way out can outlive the load as a leak: a copy handed to someone here is released (Release), and one
			// still held keeps its record, so the load frees it (but see below). One lingering from the last load is on its way out
			// too, though no longer in RemotePlayers' lists (cleared at that load): without its note, a second load in a row
			// would leave it to the game, and the next save would write it. Every kind Track is told of: a friend's stand-in
			// leaks through a load as a frame copy does, and one written into a save crashed the second load of it in a run.
			if (base && base->GetFormID() == a_base && !noted.contains(a_id)) {
				const bool lingering = book.IsLingering(a_id, a_base);
				// Or one held with no record: adrift, inside RemotePlayers' ADRIFT_GRACE (SaveStrays::GoesToLoad).
				if (SaveStrays::GoesToLoad(RemotePlayers::OnItsWayOut(ref), lingering, HasChanges(a_id))) {
					const auto& at = ref->data.location;
					noted[a_id] = { ref, ref->extraList.get(), OwnershipOf(ref), a_base, at.x, at.y, at.z, !ref->IsDisabled() && !ref->IsDeleted() };
					wayOut.emplace_back(ref, lingering);
				}
			}
		});
		// Each is handed to the load (see RELEASE_CREATED_REFERENCE), after the walk: letting go of one changes no book here,
		// but the engine's events run as it goes. One that lingers from the last load is handed over too, unless it is an
		// actor: that load may have freed its base already, and the engine reads an actor's base as it lets go of it (the
		// crash in RevertAllChanges on such a stand-in). That one stays noted only.
		for (const auto& [ref, lingering] : wayOut) {
			if (lingering && ref->As<RE::Actor>()) {
				continue;
			}
			HandToLoad(ref, "ours on its way out");
		}
		// A stray of ours the last load brought back, still the same form: its base is the save's, freed by this load.
		for (const auto& stray : std::exchange(broughtBack, {})) {
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(stray.id);
			const auto base = ref ? ref->GetObjectReference() : nullptr;
			if (ref == stray.ref && base && base->GetFormID() == stray.base) {
				++broughtBackHanded;
				HandToLoad(ref, "a stray the last load brought back");
			}
		}
	}

	void HandOver(RE::TESObjectREFR* a_ref, std::string_view a_what)
	{
		if (a_ref) {
			HandToLoad(a_ref, a_what);
		}
	}

	std::uint32_t BeforeSave()
	{
		std::uint32_t kept = 0;
		// Ours the game took from under us: one that still has a cell is not a stray, and would be saved.
		const auto strays = book.BeforeSave(
			BaseNow,
			[&](std::uint32_t a_id) {
				if (ExcludeId(a_id)) {
					++kept;
					REX::INFO("SaveGuard: {:08X}, ours, was taken from us by the game; keeping it out of the save", a_id);
				}
			},
			DropOrphan, Sweep(), StandIn);
		KeepLingering();  // asked again at the save itself: a change since the last second's check would be written
		LetGoOfGone();
		for (const auto& [c, verdict] : strays) {
			const bool doomed = SaveStrays::Doomed(c.seen);
			const bool hasChanges = HasChanges(c.id);
			foreignDoomed += doomed && !c.seen.ours && hasChanges;
			if (verdict == SaveStrays::Verdict::kTell) {
				if (hasChanges) {
					REX::WARN("SaveGuard: {} has neither a cell nor a worldspace; not ours, so its record is left as the game has it (if this save fails, this is the line to report)", Describe(c, hasChanges));
				}
				continue;
			}
			// One already kept out of an earlier save has no record left: nothing to do, nothing to count.
			if (verdict != SaveStrays::Verdict::kExclude || !ExcludeId(c.id)) {
				continue;
			}
			lastStray = c.id;
			++kept;
			if (doomed) {
				++doomedSaved;
				REX::WARN("SaveGuard: {} has neither a cell nor a worldspace; keeping it out of the save, which could not be written with it", Describe(c, hasChanges));
			} else {
				REX::INFO("SaveGuard: {:08X} is in no cell; keeping it out of the save", c.id);
			}
		}
		straysSaved += kept;
		return kept;
	}

	std::uint32_t AfterLoad()
	{
		// What we placed before the load is gone, and its ids name the loaded game's references now (see
		// SaveStrays::Book::AfterLoad).
		unpromoted.Clear();
		const auto strays = book.AfterLoad(Sweep(), StandIn, Lingers);
		KeepLingering();
		// What was forgotten before the load is let go of with it (AfterLoad); this only lets go of the gone list.
		book.KeepForgotten(BaseNow, [](std::uint32_t a_id) { excludedAtDelete += ExcludeId(a_id); }, DropOrphan);
		for (const auto& [c, verdict] : strays) {
			lastStray = c.id;
			const bool doomed = SaveStrays::Doomed(c.seen);
			const bool hasChanges = HasChanges(c.id);
			doomedLoaded += doomed;
			if (verdict == SaveStrays::Verdict::kExclude) {
				ExcludeId(c.id);
				REX::WARN("SaveGuard: the save brought back {}, a stand-in or one of its things; its record is dropped, and the next load frees it", Describe(c, hasChanges));
				// An earlier version's stand-in comes back promoted when the save kept it so: its holder letting go would unload it
				// in no cell (see SaveStrays::Unpromoted), so it is told no from now on and stays as it is, kept out of every save.
				if (const auto actor = c.actor ? RE::TESForm::GetFormByID<RE::Actor>(c.id) : nullptr) {
					KeepUnpromoted(actor);
				}
				// With no record, the next load would keep it in memory as it is (left so, the next load of the same save crashed
				// on the VM, in TESPackage::Revert from the process lists), so it goes to that load (BeforeLoad), as one of ours.
				// That load lets go of it, and the save brings it back again. A save that holds one is still not safe to load
				// again and again in one run (the third load crashed, on a settler's Actor::Revert): a save made after the first
				// load holds none, and one loaded after a restart of the game is.
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(c.id); ref && broughtBack.size() < BROUGHT_BACK_MAX) {
					broughtBack.push_back({ c.id, c.base, ref });
				}
			} else if (doomed) {
				foreignDoomed += hasChanges;
				REX::WARN("SaveGuard: the save brought back {}, not ours, with neither a cell nor a worldspace; left to the sweep before the next save", Describe(c, hasChanges));
			} else {
				REX::INFO("SaveGuard: the save brought back {:08X} in no cell (a friend's stand-in saved by an earlier version?)", c.id);
			}
		}
		// The save holds one of ours an earlier version wrote: loading it again in this run may crash (see DESIGN.md), and a
		// save made now holds none. Told once a load, in plain words.
		if (!broughtBack.empty()) {
			++oldSaveNotices;
			Hud::Notify(Text::Get("save.old_leftover"));
		}
		// After book.AfterLoad has switched off the lingering ones (Lingers): they are among the frames it finds.
		FindLeftovers();
		straysLoaded += static_cast<std::uint32_t>(strays.size());
		return static_cast<std::uint32_t>(strays.size());
	}

	void NewGame()
	{
		unpromoted.Clear();
		broughtBack.clear();
		leftovers.Clear();
		book.AfterLoad({}, StandIn, Lingers);  // what was placed before it is gone, as after a load (or on its way out: Lingers)
	}

	std::string ListStrays()
	{
		const auto  strays = book.List(Sweep(), StandIn);
		std::string out = std::format("strays={} session={}", strays.size(), book.Session());
		for (const auto& [judged, load] : strays) {
			const auto& c = judged.found;
			const auto  save = judged.verdict;
			out += std::format("; {} doomed={} at save: {} after a load: {}", Describe(c, HasChanges(c.id)), SaveStrays::Doomed(c.seen), save == SaveStrays::Verdict::kExclude ? "kept out" : (save == SaveStrays::Verdict::kTell ? "logged" : "left"),
				load == SaveStrays::Verdict::kExclude ? "dropped" : (load == SaveStrays::Verdict::kTell ? "logged" : "left"));
		}
		return out;
	}

	std::string TypeName(const void* a_object)
	{
		if (!a_object) {
			return {};
		}
		const auto base = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		char       name[256];
		return CopyTypeName(a_object, base, name, sizeof(name)) ? std::string(name) : std::string{};
	}

	std::optional<const void*> ReadGlobal(std::uintptr_t a_offset)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		const void* object = nullptr;
		if (!ReadPointer(base + a_offset, object)) {
			return std::nullopt;
		}
		return object;
	}

	std::string Describe()
	{
		return std::format("save: excluded={} straysSaved={} straysLoaded={} doomedSaved={} doomedLoaded={} foreignDoomed={} excludedAtDelete={} orphansDropped={} lingered={} lingeringDropped={} lingering={} turnedOff={} leftToLoad={} recordsGiven={} releasedAtLoad={} basesKept={} broughtBack={} broughtBackHanded={} oldSaveNotices={} lastStray={:08X} placed={} forgotten={} searches={} unpromoted={} promotionsRefused={} frameLeftovers={}/{} leftoversKept={} leftoversWatched={}",
			excluded, straysSaved, straysLoaded, doomedSaved, doomedLoaded, foreignDoomed, excludedAtDelete, orphansDropped, lingered, lingeringDropped, book.LingeringCount(), turnedOff, leftToLoad, recordsGiven, releasedAtLoad, basesKept, broughtBack.size(), broughtBackHanded, oldSaveNotices, lastStray, book.PlacedCount(), book.ForgottenCount(), searches, unpromoted.Size(),
			unpromoted.Refused(), leftoversCleared, leftoversFound, leftoversKept, leftovers.Size());
	}
}
