#include "game/SaveGuard.h"
#include "game/RemotePlayers.h"
#include "game/SaveStrays.h"

namespace SaveGuard
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The engine's save bookkeeping and two of its calls, by Address Library ID (1.11.240):
		// g_BGSSaveLoadGame (+32A85A8), BGSSaveLoadGame::ClearChanges(form ID) (+BECCC0) and
		// BGSSaveLoadGame::IsFormIDInUse(form ID) (+BEDEB0), which says whether the list of changes holds a
		// record for the id. CommonLibF4 names 2697789 BGSCreatedObjectManager::Singleton; the game's
		// symbols say otherwise.
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
		// Gone already (unloaded with its cell): its record goes now, before the cell could load again and bring it back.
		if (book.Forget(a_id)) {
			DropOrphan(a_id);
		}
	}

	HeldCopy::Lost Refresh(RE::ObjectRefHandle& a_handle, HeldCopy::Ids& a_ids)
	{
		const bool resolves = static_cast<bool>(a_handle.get());
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
		return { a_ref->GetFormID(), base ? base->GetFormID() : 0 };
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
		if (book.ForgottenCount() == 0 || now < forgottenCheckAt) {
			return;
		}
		forgottenCheckAt = now + FORGOTTEN_CHECK_INTERVAL;
		book.KeepForgotten(BaseNow, [](std::uint32_t a_id) { excludedAtDelete += ExcludeId(a_id); }, DropOrphan);
		LetGoOfGone();
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
		const auto strays = book.AfterLoad(Sweep(), StandIn);
		for (const auto& [c, verdict] : strays) {
			lastStray = c.id;
			const bool doomed = SaveStrays::Doomed(c.seen);
			const bool hasChanges = HasChanges(c.id);
			doomedLoaded += doomed;
			if (verdict == SaveStrays::Verdict::kExclude) {
				ExcludeId(c.id);
				REX::WARN("SaveGuard: the save brought back {}, a stand-in or one of its things; its record is dropped", Describe(c, hasChanges));
				// An earlier version's stand-in comes back promoted when the save kept it so: its holder letting go would unload it
				// in no cell (see SaveStrays::Unpromoted), so it is told no from now on and stays as it is, kept out of every save.
				if (const auto actor = c.actor ? RE::TESForm::GetFormByID<RE::Actor>(c.id) : nullptr) {
					KeepUnpromoted(actor);
				}
			} else if (doomed) {
				foreignDoomed += hasChanges;
				REX::WARN("SaveGuard: the save brought back {}, not ours, with neither a cell nor a worldspace; left to the sweep before the next save", Describe(c, hasChanges));
			} else {
				REX::INFO("SaveGuard: the save brought back {:08X} in no cell (a friend's stand-in saved by an earlier version?)", c.id);
			}
		}
		straysLoaded += static_cast<std::uint32_t>(strays.size());
		return static_cast<std::uint32_t>(strays.size());
	}

	void NewGame()
	{
		unpromoted.Clear();
		book.AfterLoad({}, StandIn);  // what was placed before it is gone, as after a load
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
		return std::format("save: excluded={} straysSaved={} straysLoaded={} doomedSaved={} doomedLoaded={} foreignDoomed={} excludedAtDelete={} orphansDropped={} lastStray={:08X} placed={} forgotten={} searches={} unpromoted={} promotionsRefused={}",
			excluded, straysSaved, straysLoaded, doomedSaved, doomedLoaded, foreignDoomed, excludedAtDelete, orphansDropped, lastStray, book.PlacedCount(), book.ForgottenCount(), searches, unpromoted.Size(),
			unpromoted.Refused());
	}
}
