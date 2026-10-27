#include "game/ItemCopies.h"

#include "game/StandInGear.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ItemCopies
{
	bool ModHere(std::uint32_t a_mod)
	{
		return RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_mod) != nullptr;
	}

	bool Placeable(const Protocol::DroppedItem& a_item)
	{
		const auto form = RE::TESForm::GetFormByID(a_item.base);
		return form && form->As<RE::TESBoundObject>() && std::ranges::all_of(a_item.mods, [](const Protocol::ItemMod& a_mod) { return ModHere(a_mod.mod); });
	}

	RE::BSTSmartPointer<RE::ExtraDataList> ExtraFor(const Protocol::DroppedItem& a_item, RE::TESBoundObject* a_object)
	{
		RE::BSTSmartPointer<RE::ExtraDataList> extra{ new RE::ExtraDataList() };
		const auto type = a_object->GetFormType();
		const bool gear = type == RE::ENUM_FORM_ID::kWEAP || type == RE::ENUM_FORM_ID::kARMO;
		if (gear && a_item.mods.empty() && !a_object->objectTemplate.items.empty()) {
			// A plain one (its owner's had no mods): its base's default combination, as Equipment::Give does, so the
			// engine doesn't roll one for our level.
			RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*a_object, *extra, nullptr, true);
		} else if (gear && a_item.templateIndex != StandInGear::NO_TEMPLATE) {
			std::unique_ptr<RE::BGSObjectInstanceExtra> instance{ new RE::BGSObjectInstanceExtra() };
			instance->itemIndex = a_item.templateIndex;
			for (const auto& entry : a_item.mods) {
				if (const auto mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.mod)) {
					instance->AddMod(*mod, entry.index, entry.rank, false);
				}
			}
			if (instance->values) {
				RE::BSTSmartPointer<RE::TBO_InstanceData> data;
				instance->CreateBaseInstanceData(*a_object, data);
				extra->AddExtra(instance.release());  // the list owns it from here
				if (data) {
					extra->AddExtra(new RE::ExtraInstanceData(a_object, data));
				}
			}
		}
		if (a_item.count > 1) {
			extra->SetCount(static_cast<std::int16_t>(a_item.count));
		}
		if (a_item.health >= 0.0f) {
			extra->SetHealthPerc(a_item.health);
		}
		if (!a_item.name.empty()) {
			extra->SetOverrideName(a_item.name.c_str());
		}
		return extra;
	}

	std::optional<Protocol::DroppedItem> ItemOf(const RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra)
	{
		if (!a_object) {
			return std::nullopt;
		}
		Protocol::DroppedItem item;
		item.base = a_object->GetFormID();
		item.count = 1;
		item.templateIndex = StandInGear::NO_TEMPLATE;
		if (!a_extra) {
			return item;
		}
		if (const auto instance = a_extra->GetByType<RE::BGSObjectInstanceExtra>(); instance && instance->values) {
			item.templateIndex = instance->itemIndex;
			for (const auto& data : instance->GetIndexData()) {
				if (data.disabled || data.objectID == 0) {
					continue;
				}
				if (item.mods.size() >= Protocol::MAX_ITEM_MODS) {
					return std::nullopt;  // more than the wire carries: not that item
				}
				item.mods.push_back({ data.objectID, data.index, data.rank });
			}
		}
		if (a_extra->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
			if (const auto health = a_extra->GetHealthPerc(); std::isfinite(health)) {
				item.health = std::clamp(health, 0.0f, 1.0f);
			}
		}
		if (const auto text = a_extra->GetByType<RE::ExtraTextDisplayData>();
			text && text->ownerInstance.get() == RE::ExtraTextDisplayData::DisplayDataType::kCustomName && !text->displayName.empty()) {
			item.name = Protocol::Utf8Prefix(text->displayName.c_str(), Protocol::MAX_ITEM_NAME_LENGTH);
		}
		return item;
	}
}
