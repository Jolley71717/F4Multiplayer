#include "game/Face.h"

#include "Config.h"
#include "game/EngineRefs.h"

namespace Face
{
	namespace
	{
		namespace Tint = RE::BGSCharacterTint;

		std::uint32_t made = 0;
		std::uint32_t rejected = 0;
		std::uint32_t rebuilds = 0;
		std::uint32_t gaveUpNoHead = 0;
		std::uint32_t gaveUpOurs = 0;
		std::uint32_t repairs = 0;
		// Which pieces to apply, when the dev set them (facemask); else the ini decides (Parts).
		std::optional<std::uint32_t> parts;

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
			face.faceDetails = IdOf(head->faceDetails);
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
		if (const auto morphs = a_npc->morphSliderValues) {
			for (const auto& [id, v] : *morphs) {
				if (face.morphs.size() >= Protocol::MAX_FACE_MORPHS) {
					break;
				}
				face.morphs.push_back({ id, v });
			}
			// In one order whatever the map's: the face is compared each check, and a differing one goes out again.
			std::ranges::sort(face.morphs, {}, &Protocol::FaceMorph::id);
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
			std::ranges::sort(face.bones, {}, &Protocol::FaceBone::region);
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

	std::uint32_t Parts()
	{
		return parts.value_or(Config::Get().friendFaces ? ALL : BODY_ONLY);
	}

	void SetParts(std::uint32_t a_mask)
	{
		parts = a_mask;
	}

	RE::TESNPC* MakeBase(RE::TESNPC* a_base, const Protocol::Face& a_face, std::uint32_t a_parts)
	{
		const auto race = a_base ? a_base->GetFormRace() : nullptr;
		if (!race || race->GetFormID() != a_face.race) {
			++rejected;
			return nullptr;
		}
		const auto npc = static_cast<RE::TESNPC*>(a_base->CreateDuplicateForm(false, nullptr));
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
		std::vector<RE::BGSHeadPart*> headParts;
		for (const auto id : a_face.headParts) {
			if (const auto part = RE::TESForm::GetFormByID<RE::BGSHeadPart>(id)) {
				headParts.push_back(part);
			}
		}
		if (!headParts.empty() && (a_parts & kHeadParts)) {
			const auto array = RE::calloc<RE::BGSHeadPart*>(headParts.size());
			std::ranges::copy(headParts, array);
			npc->headParts = array;
			npc->numHeadParts = static_cast<std::int8_t>(headParts.size());
		}

		// Hair and beard colours, and the complexion (the skin detail texture set the engine lays the tints over).
		const auto head = (a_parts & kHairColors) ? RE::calloc<RE::TESNPC::HeadRelatedData>(1) : nullptr;
		if (head) {
			if (a_base->headRelatedData) {
				*head = *a_base->headRelatedData;
			}
			head->hairColor = RE::TESForm::GetFormByID<RE::BGSColorForm>(a_face.hairColor);
			head->facialHairColor = RE::TESForm::GetFormByID<RE::BGSColorForm>(a_face.facialHairColor);
			if (const auto details = RE::TESForm::GetFormByID<RE::BGSTextureSet>(a_face.faceDetails)) {
				head->faceDetails = details;
			}
			npc->headRelatedData = head;
		}

		if (a_parts & kBody) {
			npc->morphWeight = { a_face.weight[0], a_face.weight[1], a_face.weight[2] };
		}

		if (a_parts & kSliders) {
			const auto sliders = new RE::BSTArray<float>();
			for (const float v : a_face.sliders) {
				sliders->push_back(v);
			}
			npc->morphRegionSliderValues = sliders;
			const auto morphs = EngineRefs::New<RE::BSTHashMap<std::uint32_t, float>>();
			if (!morphs) {
				return nullptr;
			}
			for (const auto& m : a_face.morphs) {
				morphs->insert({ m.id, m.value });
			}
			npc->morphSliderValues = morphs;
		}

		if (a_parts & kBones) {
			const auto bones = EngineRefs::New<RE::BSTHashMap<std::uint32_t, RE::BGSCharacterMorph::Transform>>();
			if (!bones) {
				return nullptr;
			}
			for (const auto& b : a_face.bones) {
				RE::BGSCharacterMorph::Transform t;
				t.position = { b.position[0], b.position[1], b.position[2] };
				t.rotation = { b.rotation[0], b.rotation[1], b.rotation[2] };
				t.scale = { b.scale[0], b.scale[1], b.scale[2] };
				bones->insert({ b.region, t });
			}
			npc->facialBoneRegionSliderValues = bones;
		}

		std::size_t tintCount = 0;
		if (a_parts & kTints) {
			const auto tints = EngineRefs::New<Tint::Entries>();
			if (!tints) {
				return nullptr;
			}
			for (const auto& t : a_face.tints) {
				const auto templ = FindTemplate(race, a_face.female, t.templateId);
				if (!templ) {
					continue;  // a template from a mod the receiver doesn't have
				}
				if (const auto entry = MakeTint(t, templ)) {
					tints->entriesA.push_back(entry);
				}
			}
			tintCount = tints->entriesA.size();
			npc->tintingData = tints;
		}

		if (a_parts & kBodyTint) {
			npc->bodyTintColorR = static_cast<std::int8_t>(a_face.bodyTint[0]);
			npc->bodyTintColorG = static_cast<std::int8_t>(a_face.bodyTint[1]);
			npc->bodyTintColorB = static_cast<std::int8_t>(a_face.bodyTint[2]);
			npc->bodyTintColorA = static_cast<std::int8_t>(a_face.bodyTint[3]);
		}
		// Its own face, not the base's: a settler's look comes from a template (faceNPC and the "use traits" template
		// flag), whose FaceGen files the engine loads as the head; with those left in place the copy's own head parts,
		// sculpting and tints are never looked at. Without them the copy is a runtime form with no files of its own, so
		// the engine builds its head from these at load (or does not: FaceWatch.h watches for that).
		if (a_parts & kHeadParts) {
			npc->faceNPC = nullptr;
			npc->actorData.templateUseFlags.reset(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits);
		}
		++made;
		REX::INFO("Face: made base {:08X} from {:08X} with parts {:02X}: {} head parts, {} sliders, {} morphs, {} bones, {} tints{}", npc->GetFormID(), a_base->GetFormID(), a_parts,
			(a_parts & kHeadParts) ? headParts.size() : 0, (a_parts & kSliders) ? a_face.sliders.size() : 0, (a_parts & kSliders) ? a_face.morphs.size() : 0,
			(a_parts & kBones) ? a_face.bones.size() : 0, tintCount, OwnHead(a_parts) ? " (its own head)" : " (the settler's head)");
		return npc;
	}

	bool HasHead(RE::TESObjectREFR* a_actor)
	{
		return a_actor && a_actor->Get3D() && a_actor->GetFaceNodeSkinned() != nullptr;
	}

	void Rebuild(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		++rebuilds;
		// Everything built again on the engine's next update of it (queued, not from inside our frame), head included:
		// LooksMenu asks the same after editing an NPC's face.
		a_actor->Reset3D(true, 0, true, 0);
	}

	void GaveUp(std::uint32_t a_playerId, bool a_ours)
	{
		if (a_ours) {
			++gaveUpOurs;
			REX::WARN("Face: player {}'s stand-in took our own player's head while loaded; placed again with the settler's face, and their own head not shown again this session", a_playerId);
		} else {
			++gaveUpNoHead;
			REX::WARN("Face: the engine built no head for player {}'s stand-in with their own head parts (not after a rebuild either); placed again with the settler's face, and their own head not shown again this session", a_playerId);
		}
	}

	void RepairOurs()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		++repairs;
		REX::WARN("Face: our own player's head is still gone after the stand-in that took it went; asked the engine for it back");
		constexpr auto head = static_cast<std::uint32_t>(std::to_underlying(RE::RESET_3D_FLAGS::kHead) | std::to_underlying(RE::RESET_3D_FLAGS::kFace));
		player->Reset3D(false, head, true, 0);
	}

	std::string Describe()
	{
		return std::format("faces: parts={:02X} made={} rejected={} rebuilds={} gaveup={}/{} repairs={}", Parts(), made, rejected, rebuilds, gaveUpNoHead, gaveUpOurs, repairs);
	}
}
