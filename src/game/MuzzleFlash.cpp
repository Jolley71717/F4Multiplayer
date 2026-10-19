#include "game/MuzzleFlash.h"

#include "game/MuzzleFlashBook.h"

#include <unordered_set>

namespace MuzzleFlash
{
	namespace
	{
		using Clock = MuzzleFlashBook::Clock;

		Mode mode = Mode::kArt;

		MuzzleFlashBook::Book book;

		// What shows for one shooter, held by us while it shows. For a hung model: the model (the database's own node,
		// never a copy), the muzzle it hangs under and the actor's 3D root, so the skeleton the muzzle is in stays alive
		// if the engine lets go of the actor's 3D meanwhile, and the model always comes off a live node. For an art
		// effect: the engine's effect, which the engine itself ends at its lifetime or when its target goes.
		struct Showing
		{
			Mode                                    how = Mode::kOff;
			RE::NiPointer<RE::NiNode>               model;
			RE::NiPointer<RE::NiNode>               muzzle;
			RE::NiPointer<RE::NiAVObject>           root;
			RE::NiPointer<RE::ModelReferenceEffect> effect;
		};
		std::unordered_map<std::uint32_t, Showing> showing;

		constexpr std::size_t                                      MAX_MODELS = 48;  // flash models kept (one per kind of gun)
		std::unordered_map<std::string, RE::NiPointer<RE::NiNode>> models;           // path -> the database's node
		std::unordered_map<std::string, RE::BGSArtObject*>         arts;             // path -> our art object (never freed: forms live as long as the game)
		std::unordered_set<std::string>                            failedPaths;      // models that did not load: not tried again

		std::uint32_t shown = 0, extended = 0, tooSoon = 0, tooMany = 0, noFlash = 0, noMuzzle = 0, inUse = 0, failed = 0, takenDown = 0;
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

		// The model as the engine's model database keeps it (loaded from the archives the first time: a small file, on
		// this thread), and kept for the next shot.
		RE::NiNode* Model(const char* a_path)
		{
			if (const auto it = models.find(a_path); it != models.end()) {
				return it->second.get();
			}
			if (models.size() >= MAX_MODELS || failedPaths.contains(a_path)) {
				return nullptr;
			}
			RE::BSModelDB::DBTraits::ArgsType args{};
			args.lodFadeMult = RE::ENUM_LOD_MULT::kNone;
			args.loadLevel = 0;
			args.prepareAfterLoad = 1;
			args.performProcess = 1;
			args.loadTextures = 1;
			RE::NiPointer<RE::NiNode> loaded;
			const auto                error = RE::BSModelDB::Demand(a_path, &loaded, args);
			if (error != RE::BSResource::ErrorCode::kNone || !loaded) {
				failedPaths.emplace(a_path);
				REX::WARN("MuzzleFlash: the model '{}' did not load (error {})", a_path, std::to_underlying(error));
				return nullptr;
			}
			models.emplace(a_path, loaded);
			return loaded.get();
		}

		// An art object of our own for the model (made once per model, at runtime).
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

		// Takes a flash down: a hung model off the muzzle it was hung under (one moved elsewhere meanwhile is left where
		// it is), an art effect ended at once.
		void TakeDown(Showing& a_flash)
		{
			if (a_flash.how == Mode::kDatabase && a_flash.model && a_flash.muzzle && a_flash.model->parent == a_flash.muzzle.get()) {
				a_flash.muzzle->DetachChild(a_flash.model.get());
			}
			if (a_flash.how == Mode::kArt && a_flash.effect && !a_flash.effect->finished && a_flash.effect->lifetime > a_flash.effect->age) {
				a_flash.effect->lifetime = a_flash.effect->age;
			}
			++takenDown;
		}

		void TakeDown(std::uint32_t a_actorId)
		{
			if (const auto it = showing.find(a_actorId); it != showing.end()) {
				TakeDown(it->second);
				showing.erase(it);
			}
		}

		bool Show(RE::Actor* a_actor, RE::NiAVObject* a_root, RE::NiNode* a_muzzle, const char* a_path, float a_seconds)
		{
			Showing flash;
			flash.how = mode;
			if (mode == Mode::kDatabase) {
				const auto model = Model(a_path);
				if (!model) {
					++failed;
					return false;
				}
				// A node has one parent: a model hanging anywhere (another shooter's flash of the same gun) is left there.
				if (model->parent) {
					++inUse;
					return false;
				}
				a_muzzle->AttachChild(model, true);
				if (model->parent != a_muzzle) {
					++failed;
					return false;
				}
				flash.model.reset(model);
				flash.muzzle.reset(a_muzzle);
				flash.root.reset(a_root);
			} else {
				const auto art = ArtFor(a_path);
				const auto effect = art ? a_actor->ApplyArtObject(art, a_seconds, nullptr, false, false, a_muzzle, false) : nullptr;
				if (!effect) {
					++failed;
					return false;
				}
				flash.effect.reset(effect);
			}
			showing[a_actor->GetFormID()] = std::move(flash);
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
			return std::format("{}'{}'{}", kind, name, a_object->GetAppCulled() ? "(culled)" : "");
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
	}

	void OnShot(RE::Actor* a_actor)
	{
		if (mode == Mode::kOff || !a_actor || a_actor->IsDeleted()) {
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
		const float seconds = MuzzleFlashBook::FlashSeconds(projectile->data.muzzleFlashDuration);
		if (kind == MuzzleFlashBook::Kind::kExtend) {
			// One flash per shooter at a time: an automatic gun's next shot keeps it up. A shooter whose flash is no
			// longer where it was put (a new 3D, the gun changed) gets a new one.
			const auto it = showing.find(id);
			const bool intact = it != showing.end() &&
			                    (it->second.how == Mode::kDatabase ? it->second.model && it->second.muzzle && it->second.model->parent == it->second.muzzle.get() && it->second.root.get() == root :
			                                                         it->second.effect && !it->second.effect->finished);
			if (intact) {
				if (it->second.effect) {
					it->second.effect->lifetime = it->second.effect->age + seconds;
				}
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
		if (Show(a_actor, root, muzzle, path, seconds)) {
			book.Shown(id, now, seconds);
			++shown;
			lastModel = path;
		}
	}

	void Frame()
	{
		if (showing.empty()) {
			return;
		}
		for (const auto id : book.TakeDue(Clock::now())) {
			TakeDown(id);
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
		for (auto& [id, flash] : showing) {
			TakeDown(flash);
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
			out += std::format(" fireNode={} engineFlash={:X}", Label(gun.data->fireNode), reinterpret_cast<std::uintptr_t>(gun.data->muzzleFlash));
			if (gun.data->muzzleFlash) {
				std::uint64_t words[16]{};
				if (ReadQwords(gun.data->muzzleFlash, words, std::size(words))) {
					out += " flashWords=";
					for (const auto word : words) {
						out += std::format("{:X} ", word);
					}
				}
			}
		}
		if (a_tree) {
			int budget = 120;
			out += " muzzleTree=";
			Tree(muzzle, 6, out, budget);
			if (gun.data && gun.data->fireNode && gun.data->fireNode != muzzle) {
				out += " fireTree=";
				Tree(gun.data->fireNode, 6, out, budget);
			}
		}
		if (const auto it = showing.find(a_actor->GetFormID()); it != showing.end()) {
			out += std::format(" showing({})", it->second.how == Mode::kDatabase ? "db" : "art");
		}
		return out;
	}

	std::string Describe()
	{
		const char* name = mode == Mode::kOff ? "off" : (mode == Mode::kArt ? "art" : "db");
		return std::format("flash={}:{}/{} flashSkips={}/{}/{}/{}/{}/{} flashing={}", name, shown, extended, tooSoon, tooMany, noFlash, noMuzzle, inUse, failed, showing.size());
	}
}
