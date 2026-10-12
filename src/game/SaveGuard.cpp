#include "game/SaveGuard.h"
#include "game/RemotePlayers.h"
#include "game/SaveStrays.h"

namespace SaveGuard
{
	namespace
	{
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

		std::uint32_t excluded = 0;      // records removed (by anyone)
		std::uint32_t straysSaved = 0;   // strays whose record was kept out of a save
		std::uint32_t straysLoaded = 0;  // strays found right after loads
		std::uint32_t lastStray = 0;
		std::uint32_t searches = 0;      // of all forms, for strays

		SaveStrays::Placed placed;
		bool               loadBroughtStrays = false;

		void* SaveLoadGame()
		{
			static REL::Relocation<void**> game{ SAVE_LOAD_GAME };
			return *game;
		}

		bool ExcludeId(std::uint32_t a_id)
		{
			static REL::Relocation<void (*)(void*, std::uint32_t)> clearChanges{ CLEAR_CHANGES };
			static REL::Relocation<bool (*)(void*, std::uint32_t)> hasChanges{ HAS_CHANGES };
			const auto game = SaveLoadGame();
			if (!game || !hasChanges(game, a_id)) {
				return false;  // nothing would be saved for it
			}
			clearChanges(game, a_id);
			const bool removed = !hasChanges(game, a_id);
			excluded += removed;
			return removed;
		}

		// The created actors in no cell that are gone (see SaveStrays::Stray). The engine writes such a
		// reference's cell into the save without a check, a crash on every save.
		std::vector<std::uint32_t> Strays()
		{
			++searches;
			std::vector<std::uint32_t> strays;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSAutoReadLock l{ lock };
			if (!map) {
				return strays;
			}
			for (const auto& [id, form] : *map) {
				if (form && form->IsCreated() && form->Is(RE::ENUM_FORM_ID::kACHR)) {
					const auto actor = static_cast<RE::Actor*>(form);
					const auto base = actor->GetObjectReference();
					const auto baseId = base ? base->GetFormID() : 0;
					const SaveStrays::Actor seen{
						.inCell = actor->GetParentCell() != nullptr,
						.hasBase = base != nullptr,
						.deleted = actor->IsDeleted(),
						.ours = placed.Ours(id, baseId) || RemotePlayers::IsDefaultBase(baseId),
					};
					if (SaveStrays::Stray(seen)) {
						strays.push_back(id);
					}
				}
			}
			return strays;
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

	void Track(RE::Actor* a_actor)
	{
		if (a_actor) {
			const auto base = a_actor->GetObjectReference();
			placed.Add(a_actor->GetFormID(), base ? base->GetFormID() : 0);
		}
	}

	std::uint32_t BeforeSave()
	{
		if (!SaveStrays::SearchBeforeSave(placed.Any(), loadBroughtStrays)) {
			return 0;
		}
		std::uint32_t kept = 0;
		for (const auto id : Strays()) {
			// One already kept out of an earlier save has no record left: nothing to do, nothing to count.
			if (ExcludeId(id)) {
				lastStray = id;
				++kept;
				REX::INFO("SaveGuard: {:08X} is in no cell; keeping it out of the save", id);
			}
		}
		straysSaved += kept;
		return kept;
	}

	std::uint32_t AfterLoad()
	{
		const auto strays = Strays();
		for (const auto id : strays) {
			lastStray = id;
			REX::INFO("SaveGuard: the save brought back {:08X} in no cell (a friend's stand-in saved by an earlier version?)", id);
		}
		placed.KeepOnly(strays);
		loadBroughtStrays = !strays.empty();
		straysLoaded += static_cast<std::uint32_t>(strays.size());
		return static_cast<std::uint32_t>(strays.size());
	}

	std::uint32_t CountStrays()
	{
		return static_cast<std::uint32_t>(Strays().size());
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
		return std::format("save: excluded={} straysSaved={} straysLoaded={} lastStray={:08X} placed={} searches={}", excluded, straysSaved, straysLoaded, lastStray, placed.Size(), searches);
	}
}
