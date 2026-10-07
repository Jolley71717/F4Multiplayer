#include "game/Face.h"

namespace Face
{
	namespace
	{
		namespace Tint = RE::BGSCharacterTint;

		std::uint32_t made = 0;
		std::uint32_t rejected = 0;
		// Which pieces to apply (dev: facemask). Only the body shape by default: the game builds a
		// head at runtime for the player only, so a copy with its own head parts has no head until
		// the engine's head generation can be called (see docs/ROADMAP.md).
		std::uint32_t parts = 4;

		std::uint32_t IdOf(const RE::TESForm* a_form)
		{
			return a_form ? a_form->GetFormID() : 0;
		}

		// The race's tint template entry with this ID (what a tint layer refers to).
		Tint::Template::Entry* FindTemplate(RE::TESRace* a_race, bool a_female, std::uint16_t a_id)
		{
			const auto data = a_race ? a_race->faceRelatedData[a_female ? 1 : 0] : nullptr;
			const auto groups = data && data->tintingTemplate ? &data->tintingTemplate->groups : nullptr;
			if (!groups) {
				return nullptr;
			}
			for (const auto group : *groups) {
				if (!group) {
					continue;
				}
				for (const auto entry : group->entries) {
					if (entry && entry->uniqueID == a_id) {
						return entry;
					}
				}
			}
			return nullptr;
		}

		// A tint layer as the game would make it: the right class (by vtable), pointing at its template.
		Tint::Entry* MakeTint(const Protocol::FaceTint& a_tint, Tint::Template::Entry* a_template)
		{
			const bool palette = a_tint.type == 1;
			const auto size = palette ? sizeof(Tint::PaletteEntry) : sizeof(Tint::Entry);
			const auto memory = RE::calloc(1, size);
			if (!memory) {
				return nullptr;
			}
			const auto& vtable = a_tint.type == 0 ? RE::VTABLE::BGSCharacterTint__MaskEntry :
			                     palette           ? RE::VTABLE::BGSCharacterTint__PaletteEntry :
			                                         RE::VTABLE::BGSCharacterTint__TextureSetEntry;
			*static_cast<std::uintptr_t*>(memory) = vtable[0].address();
			const auto entry = static_cast<Tint::Entry*>(memory);
			entry->templateEntry = reinterpret_cast<Tint::Entry*>(a_template);
			entry->idLink = a_tint.templateId;
			entry->tintingValue = a_tint.value;
			if (palette) {
				const auto p = static_cast<Tint::PaletteEntry*>(entry);
				p->tintingColor = a_tint.color;
				p->swatchID = a_tint.swatch;
			}
			return entry;
		}
	}

	Protocol::Face Read(RE::TESNPC* a_npc)
	{
		Protocol::Face face;
		if (!a_npc) {
			return face;
		}
		face.female = a_npc->IsFemale();
		face.race = IdOf(a_npc->GetFormRace());
		for (const auto part : a_npc->GetHeadParts(true)) {
			if (part && face.headParts.size() < Protocol::MAX_FACE_HEAD_PARTS) {
				face.headParts.push_back(part->GetFormID());
			}
		}
		if (const auto head = a_npc->headRelatedData) {
			face.hairColor = IdOf(head->hairColor);
			face.facialHairColor = IdOf(head->facialHairColor);
		}
		face.weight[0] = a_npc->morphWeight.x;
		face.weight[1] = a_npc->morphWeight.y;
		face.weight[2] = a_npc->morphWeight.z;
		if (const auto sliders = a_npc->morphRegionSliderValues) {
			for (const float v : *sliders) {
				if (face.sliders.size() < Protocol::MAX_FACE_SLIDERS) {
					face.sliders.push_back(v);
				}
			}
		}
		if (const auto bones = a_npc->facialBoneRegionSliderValues) {
			for (const auto& [region, t] : *bones) {
				if (face.bones.size() >= Protocol::MAX_FACE_BONES) {
					break;
				}
				Protocol::FaceBone b;
				b.region = region;
				b.position[0] = t.position.x, b.position[1] = t.position.y, b.position[2] = t.position.z;
				b.rotation[0] = t.rotation.x, b.rotation[1] = t.rotation.y, b.rotation[2] = t.rotation.z;
				b.scale[0] = t.scale.x, b.scale[1] = t.scale.y, b.scale[2] = t.scale.z;
				face.bones.push_back(b);
			}
		}
		if (const auto tints = a_npc->tintingData) {
			for (const auto entry : tints->entriesA) {
				if (!entry || face.tints.size() >= Protocol::MAX_FACE_TINTS) {
					continue;
				}
				Protocol::FaceTint t;
				t.templateId = entry->idLink;
				t.type = static_cast<std::uint8_t>(entry->GetType());
				t.value = entry->tintingValue;
				if (t.type == 1) {
					const auto p = static_cast<Tint::PaletteEntry*>(entry);
					t.color = p->tintingColor;
					t.swatch = p->swatchID;
				}
				if (t.type <= 2) {
					face.tints.push_back(t);
				}
			}
		}
		face.bodyTint[0] = static_cast<std::uint8_t>(a_npc->bodyTintColorR);
		face.bodyTint[1] = static_cast<std::uint8_t>(a_npc->bodyTintColorG);
		face.bodyTint[2] = static_cast<std::uint8_t>(a_npc->bodyTintColorB);
		face.bodyTint[3] = static_cast<std::uint8_t>(a_npc->bodyTintColorA);
		return face;
	}

	RE::TESNPC* MakeBase(RE::TESNPC* a_base, const Protocol::Face& a_face)
	{
		const auto race = a_base ? a_base->GetFormRace() : nullptr;
		if (!race || race->GetFormID() != a_face.race) {
			++rejected;
			return nullptr;
		}
		const auto npc = a_base->CreateDuplicateForm(false, nullptr) ? static_cast<RE::TESNPC*>(a_base->CreateDuplicateForm(false, nullptr)) : nullptr;
		if (!npc) {
			++rejected;
			return nullptr;
		}
		if (a_face.female) {
			npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
		} else {
			npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
		}

		// Head parts: only ones that exist here (same load order, so all should).
		std::vector<RE::BGSHeadPart*> parts;
		for (const auto id : a_face.headParts) {
			if (const auto part = RE::TESForm::GetFormByID<RE::BGSHeadPart>(id)) {
				parts.push_back(part);
			}
		}
		if (!parts.empty() && (Face::parts & 1)) {
			const auto array = RE::calloc<RE::BGSHeadPart*>(parts.size());
			std::ranges::copy(parts, array);
			npc->headParts = array;
			npc->numHeadParts = static_cast<std::int8_t>(parts.size());
		}

		const auto head = (Face::parts & 2) ? RE::calloc<RE::TESNPC::HeadRelatedData>(1) : nullptr;
		if (head) {
			if (a_base->headRelatedData) {
				*head = *a_base->headRelatedData;
			}
			head->hairColor = RE::TESForm::GetFormByID<RE::BGSColorForm>(a_face.hairColor);
			head->facialHairColor = RE::TESForm::GetFormByID<RE::BGSColorForm>(a_face.facialHairColor);
			npc->headRelatedData = head;
		}

		if (Face::parts & 4) {
			npc->morphWeight = { a_face.weight[0], a_face.weight[1], a_face.weight[2] };
		}

		const auto sliders = new RE::BSTArray<float>();
		for (const float v : a_face.sliders) {
			sliders->push_back(v);
		}
		if (Face::parts & 8) {
			npc->morphRegionSliderValues = sliders;
		}

		const auto bones = new RE::BSTHashMap<std::uint32_t, RE::BGSCharacterMorph::Transform>();
		for (const auto& b : a_face.bones) {
			RE::BGSCharacterMorph::Transform t;
			t.position = { b.position[0], b.position[1], b.position[2] };
			t.rotation = { b.rotation[0], b.rotation[1], b.rotation[2] };
			t.scale = { b.scale[0], b.scale[1], b.scale[2] };
			bones->insert({ b.region, t });
		}
		if (Face::parts & 16) {
			npc->facialBoneRegionSliderValues = bones;
		}

		const auto tints = new Tint::Entries();
		for (const auto& t : a_face.tints) {
			const auto templ = FindTemplate(race, a_face.female, t.templateId);
			if (!templ) {
				continue;  // a template from a mod the receiver doesn't have
			}
			if (const auto entry = MakeTint(t, templ)) {
				tints->entriesA.push_back(entry);
			}
		}
		if (Face::parts & 32) {
			npc->tintingData = tints;
		}

		if (Face::parts & 64) {
		npc->bodyTintColorR = static_cast<std::int8_t>(a_face.bodyTint[0]);
		npc->bodyTintColorG = static_cast<std::int8_t>(a_face.bodyTint[1]);
		npc->bodyTintColorB = static_cast<std::int8_t>(a_face.bodyTint[2]);
		npc->bodyTintColorA = static_cast<std::int8_t>(a_face.bodyTint[3]);
		}
		// Its own face, not the base's: a settler's look comes from a template (faceNPC and the
		// "use traits" template flag); with those left in place the copy's own head parts are ignored.
		if (Face::parts & 1) {
			npc->faceNPC = nullptr;
			npc->actorData.templateUseFlags.reset(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits);
		}
		++made;
		REX::INFO("Face: made base {:08X} from {:08X}: {} head parts, {} sliders, {} bones, {} tints", npc->GetFormID(), a_base->GetFormID(), parts.size(), a_face.sliders.size(), a_face.bones.size(), tints->entriesA.size());
		return npc;
	}

	void SetParts(std::uint32_t a_mask)
	{
		parts = a_mask;
	}

	std::string Describe()
	{
		return std::format("faces: made={} rejected={}", made, rejected);
	}
}
