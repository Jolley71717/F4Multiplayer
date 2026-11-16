#include "game/LinkedTargets.h"

#include "game/LinkedRefs.h"

#include <optional>

namespace LinkedTargets
{
	namespace
	{
		// The form ID a form's memory says it has, every read guarded: a stale pointer must cost a lookup, not the game.
		bool ReadFormId(const RE::TESForm* a_form, std::uint32_t& a_out)
		{
			__try {
				a_out = a_form->formID;
				return true;
			} __except (1) {
				return false;
			}
		}

		std::optional<std::uint32_t> GuardedId(const RE::TESObjectREFR* a_ref)
		{
			std::uint32_t id = 0;
			return ReadFormId(a_ref, id) ? std::optional{ id } : std::nullopt;
		}

		RE::TESObjectREFR* Lookup(std::uint32_t a_id)
		{
			return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
		}

		std::uintptr_t Raw(const RE::ExtraLinkedRef::LinkedRef& a_link)
		{
			return reinterpret_cast<std::uintptr_t>(a_link.REFR);
		}

		// The entry under the keyword. The keyword field is always a real pointer (or null): the engine resolves it as
		// it reads the entry and never writes an ID there.
		const RE::ExtraLinkedRef::LinkedRef* EntryUnder(const RE::TESObjectREFR& a_ref, std::uint32_t a_keyword)
		{
			const auto linked = a_ref.extraList ? a_ref.extraList->GetByType<RE::ExtraLinkedRef>() : nullptr;
			if (!linked) {
				return nullptr;
			}
			for (const auto& link : linked->linkedRefs) {
				if (link.keyword && link.keyword->GetFormID() == a_keyword && link.REFR) {
					return &link;
				}
			}
			return nullptr;
		}
	}

	RE::TESObjectREFR* Target(const RE::ExtraLinkedRef::LinkedRef& a_link)
	{
		return LinkedRefs::Resolve<RE::TESObjectREFR>(Raw(a_link), GuardedId, Lookup);
	}

	std::uint32_t TargetId(const RE::ExtraLinkedRef::LinkedRef& a_link)
	{
		return LinkedRefs::TargetId<RE::TESObjectREFR>(Raw(a_link), GuardedId, Lookup);
	}

	RE::TESObjectREFR* Under(const RE::TESObjectREFR& a_ref, std::uint32_t a_keyword)
	{
		const auto link = EntryUnder(a_ref, a_keyword);
		return link ? Target(*link) : nullptr;
	}

	std::uint32_t IdUnder(const RE::TESObjectREFR& a_ref, std::uint32_t a_keyword)
	{
		const auto link = EntryUnder(a_ref, a_keyword);
		return link ? TargetId(*link) : 0;
	}
}
