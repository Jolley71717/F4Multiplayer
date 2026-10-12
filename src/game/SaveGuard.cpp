#include "game/SaveGuard.h"

namespace SaveGuard
{
	namespace
	{
		// The engine's save bookkeeping and its call that forgets a form's changes, by Address Library ID
		// (1.11.240): g_BGSSaveLoadGame (+32A85A8) and BGSSaveLoadGame::RemoveChanges(TESForm*) (+BECB60).
		// CommonLibF4 names 2697789 BGSCreatedObjectManager::Singleton; the game's symbols say otherwise.
		constexpr REL::ID SAVE_LOAD_GAME{ 2697789 };
		constexpr REL::ID REMOVE_CHANGES{ 2227902 };

		std::uint32_t excluded = 0;
		std::uint32_t straysSaved = 0;  // kept out of saves
		std::uint32_t straysLoaded = 0;  // found right after loads
		std::uint32_t lastStray = 0;

		void* SaveLoadGame()
		{
			static REL::Relocation<void**> game{ SAVE_LOAD_GAME };
			return *game;
		}

		// The created actors in no cell that are gone: deleted, or of a base that no longer exists (a stand-in an
		// earlier version saved). The engine writes such a reference's cell into the save without a check, a
		// crash on every save. The game keeps other created actors in no cell (leveled ones whose cell
		// unloaded, disabled ones a quest holds back); those save fine and are left alone.
		std::vector<RE::Actor*> Strays()
		{
			std::vector<RE::Actor*> strays;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSAutoReadLock l{ lock };
			if (!map) {
				return strays;
			}
			for (const auto& [id, form] : *map) {
				if (form && form->IsCreated() && form->Is(RE::ENUM_FORM_ID::kACHR)) {
					const auto actor = static_cast<RE::Actor*>(form);
					if (!actor->GetParentCell() && !actor->IsPlayerRef() && (actor->IsDeleted() || !actor->GetObjectReference())) {
						strays.push_back(actor);
					}
				}
			}
			return strays;
		}

		const char* TypeNameOf(const void* a_object, std::uintptr_t a_base)
		{
			__try {
				const auto vtable = *static_cast<const std::uintptr_t* const*>(a_object);
				// The complete object locator sits just before the vtable; its type descriptor (an offset from
				// the module) names the class as ".?AVName@@".
				const auto locator = reinterpret_cast<const std::uint32_t*>(vtable[-1]);
				const auto type = a_base + locator[3];
				return reinterpret_cast<const char*>(type + 16);
			} __except (1) {
				return nullptr;
			}
		}
	}

	void Exclude(RE::TESForm* a_form)
	{
		static REL::Relocation<void (*)(void*, RE::TESForm*)> removeChanges{ REMOVE_CHANGES };
		const auto game = SaveLoadGame();
		if (!a_form || !game) {
			return;
		}
		removeChanges(game, a_form);
		++excluded;
	}

	std::uint32_t BeforeSave()
	{
		const auto strays = Strays();
		for (const auto actor : strays) {
			lastStray = actor->GetFormID();
			REX::INFO("SaveGuard: {:08X} is in no cell; keeping it out of the save", lastStray);
			Exclude(actor);
		}
		straysSaved += static_cast<std::uint32_t>(strays.size());
		return static_cast<std::uint32_t>(strays.size());
	}

	std::uint32_t AfterLoad()
	{
		const auto strays = Strays();
		for (const auto actor : strays) {
			lastStray = actor->GetFormID();
			REX::INFO("SaveGuard: the save brought back {:08X} in no cell (a friend's stand-in saved by an earlier version?)", lastStray);
		}
		straysLoaded += static_cast<std::uint32_t>(strays.size());
		return static_cast<std::uint32_t>(strays.size());
	}

	std::string TypeName(const void* a_object)
	{
		if (!a_object) {
			return {};
		}
		const auto base = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		const auto name = TypeNameOf(a_object, base);
		return name ? std::string(name) : std::string{};
	}

	std::string Describe()
	{
		return std::format("save: excluded={} straysSaved={} straysLoaded={} lastStray={:08X}", excluded, straysSaved, straysLoaded, lastStray);
	}
}
