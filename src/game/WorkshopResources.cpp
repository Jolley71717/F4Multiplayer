#include "game/WorkshopResources.h"

#include "game/WorkshopSync.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>

namespace WorkshopResources
{
	namespace
	{
		// The settlement values, as the dev commands name them.
		struct Named
		{
			const char*                               name;
			RE::ActorValueInfo* RE::ActorValue::*     member;
		};
		constexpr std::array NAMED{
			Named{ "food", &RE::ActorValue::resourceFood },
			Named{ "water", &RE::ActorValue::resourceWater },
			Named{ "safety", &RE::ActorValue::resourceSafety },
			Named{ "beds", &RE::ActorValue::resourceBed },
			Named{ "happiness", &RE::ActorValue::resourceHappiness },
			Named{ "power", &RE::ActorValue::powerGenerated },
			Named{ "powerreq", &RE::ActorValue::powerRequired },
			// The build budget (workshop mode's totals, written to the workbench: SharedWorkshop's BudgetScope).
			Named{ "triangles", &RE::ActorValue::workshopCurrentTriangles },
			Named{ "draws", &RE::ActorValue::workshopCurrentDraws },
			Named{ "maxtriangles", &RE::ActorValue::workshopMaxTriangles },
			Named{ "maxdraws", &RE::ActorValue::workshopMaxDraws },
		};

		RE::ActorValueInfo* AvByName(std::string_view a_name)
		{
			const auto values = RE::ActorValue::GetSingleton();
			if (!values) {
				return nullptr;
			}
			for (const auto& named : NAMED) {
				if (a_name == named.name) {
					return values->*named.member;
				}
			}
			std::uint32_t id = 0;
			const auto [ptr, ec] = std::from_chars(a_name.data(), a_name.data() + a_name.size(), id, 16);
			return ec == std::errc{} && ptr == a_name.data() + a_name.size() ? RE::TESForm::GetFormByID<RE::ActorValueInfo>(id) : nullptr;
		}

		std::string Values(const RE::TESObjectREFR& a_ref, const RE::ActorValueInfo& a_av)
		{
			const auto& owner = static_cast<const RE::ActorValueOwner&>(a_ref);
			return std::format("{:.2f} (base {:.2f} perm {:.2f} temp {:.2f} dmg {:.2f})", owner.GetActorValue(a_av), owner.GetBaseActorValue(a_av),
				owner.GetModifier(RE::ACTOR_VALUE_MODIFIER::kPermanent, a_av), owner.GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av),
				owner.GetModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, a_av));
		}

		// ---- Pip-Boy -------------------------------------------------------------------------------

		void Dump(RE::PipboyValue* a_value, std::string& a_out, int a_depth)
		{
			if (!a_value || a_out.size() > 12000) {
				return;
			}
			using T = RE::PipboyValue::SERIALIZATION_DATA_TYPE;
			const auto raw = reinterpret_cast<const std::uint8_t*>(a_value) + 0x18;
			switch (a_value->GetType()) {
			case T::kBool:
				a_out += *reinterpret_cast<const bool*>(raw) ? "true" : "false";
				break;
			case T::kInt8:
				a_out += std::format("{}", *reinterpret_cast<const std::int8_t*>(raw));
				break;
			case T::kUint8:
				a_out += std::format("{}", *raw);
				break;
			case T::kInt32:
				a_out += std::format("{}", *reinterpret_cast<const std::int32_t*>(raw));
				break;
			case T::kUint32:
				a_out += std::format("{}", *reinterpret_cast<const std::uint32_t*>(raw));
				break;
			case T::kFloat:
				a_out += std::format("{:.2f}", *reinterpret_cast<const float*>(raw));
				break;
			case T::kString:
				{
					const auto& text = *reinterpret_cast<const RE::BSFixedString*>(raw);
					a_out += std::format("'{}'", text.c_str() ? text.c_str() : "");
					break;
				}
			case T::kArray:
				{
					if (a_depth > 6) {
						a_out += "[...]";
						break;
					}
					const auto array = static_cast<RE::PipboyArray*>(a_value);
					a_out += "[";
					bool first = true;
					for (const auto element : array->elements) {
						a_out += first ? "" : ",";
						first = false;
						Dump(element, a_out, a_depth + 1);
					}
					a_out += "]";
					break;
				}
			case T::kObject:
				{
					if (a_depth > 6) {
						a_out += "{...}";
						break;
					}
					const auto object = static_cast<RE::PipboyObject*>(a_value);
					a_out += "{";
					bool first = true;
					for (const auto& [name, member] : object->memberMap) {
						a_out += std::format("{}{}=", first ? "" : ",", name.c_str() ? name.c_str() : "");
						first = false;
						Dump(member, a_out, a_depth + 1);
					}
					a_out += "}";
					break;
				}
			default:
				a_out += "?";
			}
		}

	}

	std::string DescribeValues(RE::TESObjectREFR* a_ref)
	{
		const auto values = RE::ActorValue::GetSingleton();
		if (!a_ref || !values) {
			return "error: no reference";
		}
		const auto base = a_ref->GetObjectReference();
		std::string out = std::format("{:08X} base={:08X}", a_ref->GetFormID(), base ? base->GetFormID() : 0);
		if (const auto owner = a_ref->GetOwner()) {
			out += std::format(" owner={:08X}", owner->GetFormID());
		}
		for (const auto& named : NAMED) {
			if (const auto av = values->*named.member) {
				out += std::format("; {}[{:08X} flags={:08X} type={} derived={}]={}", named.name, av->GetFormID(), av->flags.underlying(), av->avType.underlying(),
					static_cast<bool>(av->derivationFunction), Values(*a_ref, *av));
			}
		}
		return out;
	}

	std::string ModValue(RE::TESObjectREFR* a_ref, std::string_view a_av, std::string_view a_kind, float a_delta)
	{
		const auto av = AvByName(a_av);
		if (!a_ref || !av) {
			return "error: no such reference or value";
		}
		auto& owner = static_cast<RE::ActorValueOwner&>(*a_ref);
		if (a_kind == "base") {
			owner.ModBaseActorValue(*av, a_delta);
		} else if (a_kind == "perm" || a_kind == "temp" || a_kind == "damage") {
			const auto kind = a_kind == "perm" ? RE::ACTOR_VALUE_MODIFIER::kPermanent : a_kind == "temp" ? RE::ACTOR_VALUE_MODIFIER::kTemporary : RE::ACTOR_VALUE_MODIFIER::kDamage;
			owner.ModActorValue(kind, *av, a_delta);
		} else {
			return "error: kind is base, perm, temp or damage";
		}
		return std::format("{:08X} {} = {}", a_ref->GetFormID(), a_av, Values(*a_ref, *av));
	}

	std::string DescribePipboy(std::string_view a_filter)
	{
		const auto data = RE::PipboyDataManager::GetSingleton();
		if (!data) {
			return "error: no Pip-Boy data";
		}
		auto&       workshop = data->workshopData;
		std::string out = std::format("workshops map={} array={}", workshop.workshopMap.size(), workshop.workshopArray ? workshop.workshopArray->elements.size() : 0);
		if (!workshop.workshopArray) {
			return out;
		}
		for (const auto element : workshop.workshopArray->elements) {
			std::string one;
			Dump(element, one, 0);
			if (a_filter.empty() || one.find(a_filter) != std::string::npos) {
				out += "; " + one;
			}
			if (out.size() > 12000) {
				break;
			}
		}
		return out;
	}

}

namespace WorkshopResources
{
	namespace
	{
		constexpr std::uint32_t WORKSHOP_ITEM_KEYWORD = 0x00054BA6;

		// The ActorValue member of each kind, in ResourceMirror::Kind's order.
		constexpr std::array<RE::ActorValueInfo* RE::ActorValue::*, ResourceMirror::KINDS> KIND_VALUES{ &RE::ActorValue::resourceFood, &RE::ActorValue::resourceWater,
			&RE::ActorValue::resourceSafety, &RE::ActorValue::resourceBed, &RE::ActorValue::powerGenerated, &RE::ActorValue::powerRequired };
	}

	RE::ActorValueInfo* ValueOf(ResourceMirror::Kind a_kind)
	{
		const auto values = RE::ActorValue::GetSingleton();
		return values && a_kind < KIND_VALUES.size() ? values->*KIND_VALUES[a_kind] : nullptr;
	}

	std::optional<bool> CanProduce(RE::TESObjectREFR& a_ref)
	{
		static const auto function = RE::SCRIPT_FUNCTION::LocateScriptCommand("CanProduceForWorkshop");
		if (!function || !function->conditionFunction) {
			return std::nullopt;
		}
		RE::ConditionCheckParams params;
		params.actionRef = &a_ref;
		params.targetRef = &a_ref;
		float result = 0.0f;
		function->conditionFunction(params, nullptr, nullptr, result);
		return result != 0.0f;
	}

	ResourceMirror::Values Counts(RE::TESObjectREFR& a_ref)
	{
		ResourceMirror::Values out{};
		if (!CanProduce(a_ref).value_or(false)) {
			return out;
		}
		const auto& owner = static_cast<const RE::ActorValueOwner&>(a_ref);
		for (std::size_t k = 0; k < ResourceMirror::KINDS; ++k) {
			if (const auto av = ValueOf(static_cast<ResourceMirror::Kind>(k))) {
				out[k] = owner.GetActorValue(*av);
			}
		}
		return ResourceMirror::Clean(out);
	}

	bool CountsForAnything(RE::TESObjectREFR& a_ref)
	{
		const auto& owner = static_cast<const RE::ActorValueOwner&>(a_ref);
		for (std::size_t k = 0; k < ResourceMirror::KINDS; ++k) {
			if (const auto av = ValueOf(static_cast<ResourceMirror::Kind>(k)); av && owner.GetActorValue(*av) > 0.0f) {
				return true;
			}
		}
		return false;
	}

	bool AddToBench(std::uint32_t a_bench, ResourceMirror::Kind a_kind, float a_delta)
	{
		const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_bench);
		const auto av = ValueOf(a_kind);
		if (!bench || bench->IsDeleted() || !WorkshopSync::IsWorkbenchAnchor(bench) || !av) {
			return false;
		}
		static_cast<RE::ActorValueOwner&>(*bench).ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *av, a_delta);
		return true;
	}

	std::optional<float> TemporaryOf(std::uint32_t a_bench, ResourceMirror::Kind a_kind)
	{
		const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_bench);
		const auto av = ValueOf(a_kind);
		if (!bench || bench->IsDeleted() || !WorkshopSync::IsWorkbenchAnchor(bench) || !av) {
			return std::nullopt;
		}
		return static_cast<const RE::ActorValueOwner&>(*bench).GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, *av);
	}

	std::string DescribeProduce(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "error: no reference";
		}
		const auto can = CanProduce(*a_ref);
		return std::format("canProduce={} {}", can ? (*can ? "true" : "false") : "none", DescribeValues(a_ref));
	}

	std::string SumPieces(RE::TESObjectREFR* a_bench)
	{
		const auto values = RE::ActorValue::GetSingleton();
		const auto children = a_bench && a_bench->extraList ? a_bench->extraList->GetByType<RE::ExtraLinkedRefChildren>() : nullptr;
		if (!values || !children) {
			return "error: no workbench with workshop items";
		}
		std::array<float, NAMED.size()> all{};
		std::array<float, NAMED.size()> producing{};
		std::string                     pieces;
		std::size_t                     items = 0;
		std::size_t                     listed = 0;
		for (const auto& child : children->linkedChildren) {
			const auto ref = child.REFR.get();
			if (!ref || !child.keyword || child.keyword->GetFormID() != WORKSHOP_ITEM_KEYWORD) {
				continue;
			}
			++items;
			const auto& owner = static_cast<const RE::ActorValueOwner&>(*ref);
			const auto  can = CanProduce(*ref).value_or(false);
			std::string counts;
			for (std::size_t k = 0; k < NAMED.size(); ++k) {
				const auto av = values->*NAMED[k].member;
				const auto v = av ? owner.GetActorValue(*av) : 0.0f;
				if (v != 0.0f) {
					all[k] += v;
					producing[k] += can ? v : 0.0f;
					counts += std::format(" {}={:.2f}", NAMED[k].name, v);
				}
			}
			if (!counts.empty() && listed < 40) {
				++listed;
				const auto base = ref->GetObjectReference();
				pieces += std::format("; {:08X} {} can={}{}{}", ref->GetFormID(), base ? base->GetFormTypeString() : "?", can, counts,
					ref->GetOwner() ? std::format(" owner={:08X}", ref->GetOwner()->GetFormID()) : "");
			}
		}
		std::string out = std::format("{:08X} items={}", a_bench->GetFormID(), items);
		for (std::size_t k = 0; k < NAMED.size(); ++k) {
			const auto av = values->*NAMED[k].member;
			out += std::format(" {}: all={:.2f} producing={:.2f} bench={}", NAMED[k].name, all[k], producing[k], av ? Values(*a_bench, *av) : "?");
		}
		return out + pieces;
	}

	std::string FindBuildable(std::string_view a_text)
	{
		const auto lower = [](std::string_view a_str) {
			std::string out{ a_str };
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		};
		const auto  needle = lower(a_text);
		std::string out;
		int         found = 0;
		for (const auto recipe : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSConstructibleObject>()) {
			const auto made = recipe ? recipe->GetCreatedItem() : nullptr;
			if (!made) {
				continue;
			}
			const auto name = RE::TESFullName::GetFullName(*made);
			if (name.empty() || lower(name).find(needle) == std::string::npos) {
				continue;
			}
			out += std::format("{}{:08X} {} '{}' bench={:08X}", found ? "; " : "", made->GetFormID(), made->GetFormTypeString(), name,
				recipe->benchKeyword ? recipe->benchKeyword->GetFormID() : 0);
			if (++found == 20) {
				break;
			}
		}
		return found ? out : "no matches";
	}
}
