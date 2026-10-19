#include "game/MuzzleFlash.h"

#include "game/MuzzleFlashBook.h"

#include <unordered_set>

namespace MuzzleFlash
{
	namespace
	{
		using Clock = MuzzleFlashBook::Clock;

		Mode mode = Mode::kRefused;

		MuzzleFlashBook::Book book;
		float                 holdSeconds = 0.0f;  // dev `muzzle hold`: every flash shows this long (0: its own time)

		// The engine's temporary effect showing one shooter's flash, held while it shows. The engine itself ends it at
		// its lifetime or when its target goes; it holds the muzzle node it hangs under, and the shooter only by handle.
		std::unordered_map<std::uint32_t, RE::NiPointer<RE::ModelReferenceEffect>> showing;

		constexpr std::size_t                              MAX_MODELS = 48;  // art objects made (one per flash model)
		std::unordered_map<std::string, RE::BGSArtObject*> arts;             // path -> our art object (never freed: forms live as long as the game)
		std::unordered_set<std::string>                    failedPaths;      // models no art object could be made for: not tried again

		std::uint32_t animated = 0, shown = 0, extended = 0, tooSoon = 0, tooMany = 0, noFlash = 0, noMuzzle = 0, failed = 0, takenDown = 0;
		std::string   lastModel;

		struct Gun
		{
			RE::TESObjectWEAP*                     weapon = nullptr;
			const RE::TESObjectWEAP::InstanceData* instance = nullptr;
			RE::EquippedWeaponData*                data = nullptr;
		};

		// The actor's equipped gun, its instance data and the engine's equipped data for it.
		Gun EquippedGun(RE::Actor* a_actor)
		{
			const auto process = a_actor ? a_actor->currentProcess : nullptr;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return {};
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (auto& equipped : middle->equippedItems) {
				const auto weapon = equipped.item.object ? equipped.item.object->As<RE::TESObjectWEAP>() : nullptr;
				if (weapon && weapon->IsGunWeapon()) {
					Gun gun;
					gun.weapon = weapon;
					gun.instance = static_cast<const RE::TESObjectWEAP::InstanceData*>(equipped.item.instanceData.get());
					gun.data = static_cast<RE::EquippedWeaponData*>(equipped.data.get());
					return gun;
				}
			}
			return {};
		}

		// What the gun fires (its mods' and its ammunition's say included), or null.
		RE::BGSProjectile* ProjectileOf(const Gun& a_gun)
		{
			if (!a_gun.weapon) {
				return nullptr;
			}
			const auto instance = a_gun.instance ? a_gun.instance : &a_gun.weapon->weaponData;
			return RE::TESObjectWEAP::GetProjectile(a_gun.weapon, instance->ammo, instance);
		}

		const char* FlashModelOf(RE::BGSProjectile* a_projectile)
		{
			const char* path = a_projectile ? a_projectile->muzzleFlashModel.GetModel() : nullptr;
			return path && *path ? path : nullptr;
		}

		// The gun's muzzle in the actor's 3D (its loaded 3D only: a stand-in with none shows nothing).
		RE::NiNode* MuzzleOf(RE::NiAVObject* a_root)
		{
			static const RE::BSFixedString name{ MuzzleFlashBook::MUZZLE_NODE };
			const auto found = a_root ? a_root->GetObjectByName(name) : nullptr;
			return found ? found->IsNode() : nullptr;
		}

		// An art object of our own for the model (made once per model, at runtime). Its form ID is 0, so an effect of
		// it caught in a save is saved without one and loads as nothing.
		RE::BGSArtObject* ArtFor(const char* a_path)
		{
			if (const auto it = arts.find(a_path); it != arts.end()) {
				return it->second;
			}
			if (arts.size() >= MAX_MODELS) {
				return nullptr;
			}
			const auto factory = RE::IFormFactory::GetFormFactories()[std::to_underlying(RE::ENUM_FORM_ID::kARTO)];
			const auto form = factory ? factory->DoCreate() : nullptr;
			const auto art = form ? form->As<RE::BGSArtObject>() : nullptr;
			if (!art) {
				failedPaths.emplace(a_path);
				REX::WARN("MuzzleFlash: no art object could be made for '{}'", a_path);
				return nullptr;
			}
			art->SetModel(a_path);
			art->data.artType = RE::BGSArtObject::ArtType::kMagicHitEffect;
			arts.emplace(a_path, art);
			return art;
		}

		// Takes a flash down: its effect ends at once (the engine removes it on its next update). Our reference keeps
		// the effect itself alive until here, even if its shooter is gone.
		void TakeDown(RE::ModelReferenceEffect* a_effect)
		{
			if (a_effect && !a_effect->finished && a_effect->lifetime > a_effect->age) {
				a_effect->lifetime = a_effect->age;
			}
			++takenDown;
		}

		void TakeDown(std::uint32_t a_actorId)
		{
			if (const auto it = showing.find(a_actorId); it != showing.end()) {
				TakeDown(it->second.get());
				showing.erase(it);
			}
		}

		// The effect's clone of the flash model is a fade node of its own, nested in the actor's 3D, where the engine's
		// distance fading never reaches it: it stays at fade 0, drawn fully transparent (seen on the rig). It is drawn
		// at full strength while it shows (its 3D may come a frame late, so this is done every frame).
		void ShowFaded(RE::ModelReferenceEffect* a_effect)
		{
			const auto object = a_effect && !a_effect->finished ? a_effect->artObject3D.get() : nullptr;
			if (const auto fade = object ? object->IsFadeNode() : nullptr) {
				fade->currentFade = 1.0f;
			}
		}

		// The flash model as a temporary art effect hung at the muzzle (the engine clones the model, plays its
		// particles and controllers, and removes it at its lifetime).
		bool Show(RE::Actor* a_actor, RE::NiNode* a_muzzle, const char* a_path, float a_seconds)
		{
			const auto art = ArtFor(a_path);
			const auto effect = art ? a_actor->ApplyArtObject(art, a_seconds, nullptr, false, false, a_muzzle, false) : nullptr;
			if (!effect) {
				++failed;
				return false;
			}
			ShowFaded(effect);
			showing[a_actor->GetFormID()].reset(effect);
			return true;
		}

		// --- Readouts (dev) -----------------------------------------------------------------------------------------

		// A game object's name and kind, every read guarded: a pointer that is not what we think costs a "?".
		bool ReadObject(const RE::NiAVObject* a_object, char* a_name, std::size_t a_nameSize, const char** a_kind)
		{
			__try {
				const char* name = a_object->name.c_str();
				std::size_t i = 0;
				for (; name && i + 1 < a_nameSize && name[i]; ++i) {
					a_name[i] = name[i];
				}
				a_name[i] = '\0';
				const auto rtti = a_object->GetRTTI();
				*a_kind = rtti ? rtti->GetName() : "?";
				return true;
			} __except (1) {
				a_name[0] = '\0';
				*a_kind = "?";
				return false;
			}
		}

		bool detailed = false;  // Label adds each object's scale, world position, flags and a fade node's fade

		std::string Label(const RE::NiAVObject* a_object)
		{
			if (!a_object) {
				return "null";
			}
			char        name[128];
			const char* kind = "?";
			if (!ReadObject(a_object, name, sizeof(name), &kind)) {
				return "?";
			}
			auto out = std::format("{}'{}'{}{}", kind, name, a_object->GetAppCulled() ? "(culled)" : "", a_object->controllers ? "(animated)" : "");
			if (detailed) {
				const auto& w = a_object->world.translate;
				out += std::format("[s={:.2f}/{:.2f} w=({:.0f},{:.0f},{:.0f}) f={:X}", a_object->local.scale, a_object->world.scale, w.x, w.y, w.z, a_object->GetFlags());
				if (std::string_view{ kind } == "BSFadeNode") {
					out += std::format(" fade={:.2f}", static_cast<const RE::BSFadeNode*>(a_object)->currentFade);
				}
				out += "]";
			}
			return out;
		}

		// The class name MSVC's RTTI gives an object with a vtable in the game's image (no call made, every read
		// guarded), or nullptr.
		const char* TypeName(std::uintptr_t a_object)
		{
			static const auto base = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
			__try {
				if (a_object < 0x10000 || (a_object & 7) != 0) {
					return nullptr;
				}
				const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_object);
				if (vtable < base || vtable > base + 0x8000000) {
					return nullptr;
				}
				const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - 8);
				if (locator < base || locator > base + 0x8000000 || *reinterpret_cast<const std::uint32_t*>(locator) != 1) {
					return nullptr;
				}
				const auto type = base + *reinterpret_cast<const std::int32_t*>(locator + 12);
				return reinterpret_cast<const char*>(type + 16);
			} __except (1) {
				return nullptr;
			}
		}

		// The first qwords of an object we have no layout for (the engine's MuzzleFlash), guarded.
		bool ReadQwords(const void* a_object, std::uint64_t* a_out, std::size_t a_count)
		{
			__try {
				for (std::size_t i = 0; i < a_count; ++i) {
					a_out[i] = static_cast<const std::uint64_t*>(a_object)[i];
				}
				return true;
			} __except (1) {
				return false;
			}
		}

		// The engine's own flash object on an equipped gun, as read on the rig (no layout in CommonLib): flags (byte 0
		// asked for, byte 1 showing), a timer, then the clone of the flash model, the muzzle, the light, the projectile
		// and the weapon. Every read guarded and type-checked.
		std::string EngineFlash(const void* a_flash)
		{
			const auto    flash = static_cast<const std::uint8_t*>(a_flash);
			std::uint64_t words[5]{};
			if (!flash || !ReadQwords(flash, words, std::size(words))) {
				return "-";
			}
			float timer = 0.0f;
			std::memcpy(&timer, flash + 4, sizeof(timer));
			auto out = std::format("[{:08X} t={:.3f}", static_cast<std::uint32_t>(words[0]), timer);
			if (const auto type = TypeName(words[2]); type && std::string_view{ type } == ".?AVBSFadeNode@@") {
				const auto node = reinterpret_cast<const RE::BSFadeNode*>(words[2]);
				out += std::format(" node{} fade={:.2f}", node->GetAppCulled() ? "(culled)" : "", node->currentFade);
			}
			if (const auto type = TypeName(words[4]); type && std::string_view{ type } == ".?AVNiPointLight@@") {
				const auto light = reinterpret_cast<const RE::NiLight*>(words[4]);
				out += std::format(" light{} dim={:.2f}", light->GetAppCulled() ? "(culled)" : "", light->dimmer);
			}
			return out + "]";
		}

		// Dev `muzzle watch <actor>`: the engine's flash object of the actor's gun, sampled every frame for a while.
		std::uint32_t watchActor = 0;
		int           watchFrames = 0;
		std::string   watched;

		void SampleWatch()
		{
			if (watchFrames <= 0) {
				return;
			}
			--watchFrames;
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(watchActor);
			const auto gun = actor ? EquippedGun(actor) : Gun{};
			watched += EngineFlash(gun.data ? gun.data->muzzleFlash : nullptr) + " ";
		}

		void Tree(const RE::NiAVObject* a_object, int a_depth, std::string& a_out, int& a_budget)
		{
			if (!a_object || a_budget <= 0) {
				return;
			}
			--a_budget;
			a_out += Label(a_object);
			const auto node = const_cast<RE::NiAVObject*>(a_object)->IsNode();
			if (!node || a_depth <= 0) {
				return;
			}
			bool any = false;
			for (const auto& child : node->children) {
				if (child.get()) {
					a_out += any ? ", " : "{";
					any = true;
					Tree(child.get(), a_depth - 1, a_out, a_budget);
				}
			}
			if (any) {
				a_out += "}";
			}
		}
	}

	void OnShot(RE::Actor* a_actor, bool a_animated)
	{
		if (a_animated) {
			++animated;  // the engine draws this one
		}
		if (mode == Mode::kOff || (mode == Mode::kRefused && a_animated) || !a_actor || a_actor->IsDeleted()) {
			return;
		}
		const auto root = a_actor->Get3D();
		if (!root) {
			return;
		}
		const auto id = a_actor->GetFormID();
		const auto now = Clock::now();
		const auto kind = book.Ask(id, now);
		if (kind == MuzzleFlashBook::Kind::kTooSoon) {
			++tooSoon;
			return;
		}
		if (kind == MuzzleFlashBook::Kind::kTooMany) {
			++tooMany;
			return;
		}
		const auto projectile = ProjectileOf(EquippedGun(a_actor));
		const auto path = FlashModelOf(projectile);
		if (!path || failedPaths.contains(path)) {
			++noFlash;
			return;
		}
		const float seconds = holdSeconds > 0.0f ? holdSeconds : MuzzleFlashBook::FlashSeconds(projectile->data.muzzleFlashDuration);
		if (kind == MuzzleFlashBook::Kind::kExtend) {
			// One flash per shooter at a time: an automatic gun's next shot keeps it up. One the engine ended meanwhile
			// (its 3D went) is shown anew.
			const auto it = showing.find(id);
			if (it != showing.end() && it->second && !it->second->finished) {
				it->second->lifetime = it->second->age + seconds;
				book.Extended(id, now, seconds);
				++extended;
				return;
			}
			TakeDown(id);
			book.Forget(id);
		}
		const auto muzzle = MuzzleOf(root);
		if (!muzzle) {
			++noMuzzle;
			return;
		}
		if (Show(a_actor, muzzle, path, seconds)) {
			book.Shown(id, now, seconds);
			++shown;
			lastModel = path;
		}
	}

	void Frame()
	{
		SampleWatch();
		if (showing.empty()) {
			return;
		}
		for (const auto id : book.TakeDue(Clock::now())) {
			TakeDown(id);
		}
		for (const auto& [id, effect] : showing) {
			ShowFaded(effect.get());
		}
	}

	void Drop(std::uint32_t a_actorFormId)
	{
		book.Forget(a_actorFormId);
		TakeDown(a_actorFormId);
	}

	void DropAll()
	{
		(void)book.TakeAll();
		for (const auto& [id, effect] : showing) {
			TakeDown(effect.get());
		}
		showing.clear();
	}

	void SetMode(Mode a_mode)
	{
		DropAll();
		mode = a_mode;
	}

	Mode GetMode()
	{
		return mode;
	}

	void SetHold(float a_seconds)
	{
		holdSeconds = a_seconds > 0.0f && a_seconds <= 10.0f ? a_seconds : 0.0f;
		book.SetHold(MuzzleFlashBook::ToDuration(holdSeconds));
	}

	std::string Watch(std::uint32_t a_actorFormId, int a_frames)
	{
		auto out = std::move(watched);
		watched.clear();
		watchActor = a_actorFormId;
		watchFrames = a_frames;
		return out.empty() ? "none" : out;
	}

	std::string DescribeActor(RE::Actor* a_actor, bool a_tree)
	{
		if (!a_actor) {
			return "error: no such actor";
		}
		const auto gun = EquippedGun(a_actor);
		if (!gun.weapon) {
			return "no gun equipped";
		}
		const auto projectile = ProjectileOf(gun);
		const auto path = FlashModelOf(projectile);
		const auto light = projectile ? projectile->data.muzzleFlashLight : nullptr;
		std::string out = std::format("weapon={:08X} projectile={:08X} flags={:X} model='{}' light={:08X} seconds={:.3f}", gun.weapon->GetFormID(), projectile ? projectile->GetFormID() : 0,
			projectile ? projectile->data.flags : 0, path ? path : "", light ? light->GetFormID() : 0, projectile ? projectile->data.muzzleFlashDuration : 0.0f);
		const auto root = a_actor->Get3D();
		const auto muzzle = MuzzleOf(root);
		out += std::format(" 3d={} muzzle={}", root != nullptr, muzzle != nullptr);
		if (muzzle) {
			const auto& at = muzzle->world.translate;
			out += std::format("@({:.0f},{:.0f},{:.0f})", at.x, at.y, at.z);
		}
		if (gun.data) {
			out += std::format(" fireNode={} engineFlash={}", Label(gun.data->fireNode), EngineFlash(gun.data->muzzleFlash));
		}
		detailed = a_tree;
		if (a_tree) {
			int budget = 120;
			out += " muzzleTree=";
			Tree(muzzle, 6, out, budget);
			if (gun.data && gun.data->fireNode && gun.data->fireNode != muzzle) {
				out += " fireTree=";
				Tree(gun.data->fireNode, 6, out, budget);
			}
		}
		if (const auto it = showing.find(a_actor->GetFormID()); it != showing.end() && it->second) {
			const auto& effect = it->second;
			out += std::format(" showing age={:.3f} life={:.3f} finished={} 3d=", effect->age, effect->lifetime, effect->finished);
			detailed = true;
			int budget = 40;
			Tree(effect->artObject3D.get(), 4, out, budget);
		}
		detailed = false;
		return out;
	}

	std::string Describe()
	{
		const char* name = mode == Mode::kOff ? "off" : (mode == Mode::kRefused ? "refused" : "always");
		return std::format("flash={}:engine={} ours={}/{} flashSkips={}/{}/{}/{}/{} flashing={}", name, animated, shown, extended, tooSoon, tooMany, noFlash, noMuzzle, failed,
			showing.size());
	}
}
