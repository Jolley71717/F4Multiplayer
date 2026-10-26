#include "game/MenuBadges.h"

#include "Config.h"
#include "game/Equipment.h"
#include "game/RemotePlayers.h"
#include "game/StandInBadge.h"

namespace MenuBadges
{
	namespace
	{
		// The Pip-Boy (Fallout4.esm), whose world model is the badge.
		constexpr std::uint32_t PIPBOY = 0x00021B3B;
		// The bone it hangs over (Fallout 4's human skeleton), as the HUD icon did.
		constexpr const char* HEAD_BONE = "Head";
		// Nodes looked at for collision in the model, at most.
		constexpr std::size_t MAX_NODES = 256;

		// The model: the database's own node for the file, asked for once, the first time a friend is to wear it, and
		// kept for the rest of the run. Nothing of it is changed but where it hangs and, while it hangs, its local
		// transform, which is given back as it comes off (the engine clones that node whenever it shows the model).
		RE::NiPointer<RE::NiNode> model;
		std::string               modelPath;
		bool                      modelAsked = false;
		std::string               modelProblem;  // why there is none, or why it is never hung
		float                     modelRadius = 0.0f;

		// What it hangs from, kept alive by us while it hangs: the stand-in's 3D root, so its skeleton outlives a rebuild
		// of its 3D until the model is off it, and a new root is never at the address of the one we compare with.
		struct Hung
		{
			RE::NiPointer<RE::NiNode> root;
			RE::NiTransform           own;  // the model's local transform before it was hung
		};
		std::optional<Hung>   hung;
		StandInBadge::Track   track;
		std::uint32_t         notOnRoot = 0;  // taken off a root that no longer held it (moved by something else)
		std::uint32_t         logged = 0;
		constexpr std::uint32_t MAX_LOGS = 40;

		void Log(std::string_view a_line)
		{
			if (logged < MAX_LOGS) {
				++logged;
				REX::INFO("MenuBadges: {}", a_line);
			}
		}

		// Whether anything in the model has collision (a ground model may): hung under an actor, the engine could add it
		// to the world as it processes the actor's 3D. Such a model is never hung.
		bool HasCollision(RE::NiAVObject* a_object)
		{
			std::vector<RE::NiAVObject*> open{ a_object };
			for (std::size_t seen = 0; !open.empty() && seen < MAX_NODES; ++seen) {
				const auto object = open.back();
				open.pop_back();
				if (!object) {
					continue;
				}
				if (object->collisionObject) {
					return true;
				}
				if (const auto node = object->IsNode()) {
					for (const auto& child : node->children) {
						open.push_back(child.get());
					}
				}
			}
			return !open.empty();  // too many to look through: taken as having it
		}

		// The model, loaded as WeaponFire::HandModel loads a throwable's (from the archives if it is not in memory yet: a
		// small file, on this thread), once.
		RE::NiNode* Model()
		{
			if (std::exchange(modelAsked, true)) {
				return modelProblem.empty() ? model.get() : nullptr;
			}
			const auto pipboy = RE::TESForm::GetFormByID<RE::TESObjectARMO>(PIPBOY);
			if (!pipboy) {
				modelProblem = "no Pip-Boy form";
				return nullptr;
			}
			for (auto& world : pipboy->worldModel) {
				if (const char* path = world.GetModel(); path && *path) {
					modelPath = path;
					break;
				}
			}
			if (modelPath.empty()) {
				modelProblem = "the Pip-Boy has no world model";
				Log(modelProblem);
				return nullptr;
			}
			RE::BSModelDB::DBTraits::ArgsType args{};
			args.lodFadeMult = RE::ENUM_LOD_MULT::kNone;
			args.loadLevel = 0;
			args.prepareAfterLoad = 1;
			args.performProcess = 1;
			args.loadTextures = 1;
			RE::NiPointer<RE::NiNode> loaded;
			const auto error = RE::BSModelDB::Demand(modelPath.c_str(), &loaded, args);
			if (error != RE::BSResource::ErrorCode::kNone || !loaded) {
				modelProblem = std::format("did not load (error {})", std::to_underlying(error));
				Log(std::format("the Pip-Boy's model '{}' {}", modelPath, modelProblem));
				return nullptr;
			}
			if (HasCollision(loaded.get())) {
				modelProblem = "it has collision";
				Log(std::format("the Pip-Boy's model '{}' is not hung: {}", modelPath, modelProblem));
				return nullptr;
			}
			model = loaded;
			Log(std::format("loaded the Pip-Boy's model '{}'", modelPath));
			return model.get();
		}

		// Off the root it hangs from, its own transform given back. One moved elsewhere meanwhile is left where it is.
		void TakeOff(std::string_view a_why)
		{
			if (!hung) {
				return;
			}
			const bool onRoot = model && hung->root && model->parent == hung->root.get();
			if (onRoot) {
				hung->root->DetachChild(model.get());
			} else {
				++notOnRoot;
			}
			if (model && !model->parent) {
				model->local = hung->own;
			}
			hung.reset();
			Log(std::format("took the badge off: {}{}", a_why, onRoot ? "" : " (it was no longer on the root)"));
		}

		bool Hang(RE::Actor* a_actor, RE::NiAVObject* a_root)
		{
			const auto root = a_root ? a_root->IsNode() : nullptr;
			const auto badge = model.get();
			// A node has one parent: a model hanging anywhere at all is left where it is.
			if (!root || !badge || badge->parent) {
				Log(std::format("no badge hung on {:08X}: {}", a_actor->GetFormID(), !root ? "its 3D is no node" : badge ? "the model hangs elsewhere" : "no model"));
				return false;
			}
			const auto own = badge->local;
			root->AttachChild(badge, true);
			if (badge->parent != root) {
				badge->local = own;
				Log(std::format("no badge hung on {:08X}: its root did not take the model", a_actor->GetFormID()));
				return false;
			}
			hung = Hung{ RE::NiPointer<RE::NiNode>{ root }, own };
			Log(std::format("hung the badge on {:08X}", a_actor->GetFormID()));
			return true;
		}

		// Over the head and turned to our camera, at its size: the model's local transform under the root.
		void Place(RE::Actor* a_actor)
		{
			const auto root = hung ? hung->root.get() : nullptr;
			if (!root || !model || model->parent != root) {
				return;
			}
			const auto& at = root->world.translate;
			const auto  head = Equipment::ChildNode(root, HEAD_BONE);
			RE::NiPoint3 camera = a_actor->data.location;
			if (const auto playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->cameraRoot) {
				camera = playerCamera->cameraRoot->world.translate;
			}
			StandInBadge::Matrix rootTurn{};
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t column = 0; column < 3; ++column) {
					rootTurn[row][column] = root->world.rotate.entry[row][column];
				}
			}
			const auto turn = StandInBadge::Turn(StandInBadge::FacingYaw(rootTurn, camera.x - at.x, camera.y - at.y));
			auto&      local = model->local;
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t column = 0; column < 3; ++column) {
					local.rotate.entry[row][column] = turn[row][column];
				}
			}
			local.translate = { 0.0f, 0.0f, StandInBadge::Height(head != nullptr, head ? head->world.translate.z : 0.0f, at.z, root->world.scale) };
			modelRadius = StandInBadge::OwnRadius(model->worldBound.fRadius, model->world.scale);
			local.scale = StandInBadge::LocalScale(modelRadius, root->world.scale);
		}
	}

	void Frame()
	{
		const bool on = Config::Get().menuBadges;
		if (!on && !track.hung) {
			return;  // as without the badge: nothing is looked at
		}
		StandInBadge::Facts facts{ .on = on };
		RE::Actor*          wearer = nullptr;
		RE::NiAVObject*     root = nullptr;
		const auto          player = RE::PlayerCharacter::GetSingleton();
		if (on && player && player->GetParentCell()) {
			std::vector<StandInBadge::Candidate> candidates;
			std::vector<RE::Actor*>              actors;
			for (const auto& info : RemotePlayers::List()) {
				if (!info.actor || info.actor->IsDisabled() || !info.actor->Get3D()) {
					continue;
				}
				candidates.push_back({ info.actor->GetFormID(), MenuIconPlace::GlyphOf(info.menu), info.actor->data.location.GetDistance(player->data.location) });
				actors.push_back(info.actor);
			}
			if (const auto pick = StandInBadge::Pick(candidates, track.hung ? track.standIn : 0); pick != StandInBadge::NONE) {
				wearer = actors[pick];
				root = wearer->Get3D();
				facts.standIn = candidates[pick].standIn;
				facts.root = reinterpret_cast<std::uintptr_t>(root);
				facts.model = Model() != nullptr;
			}
		}
		switch (StandInBadge::Frame(track, facts)) {
		case StandInBadge::Do::kTakeOff:
			TakeOff(!on ? "turned off" : !facts.standIn ? "nobody in the Pip-Boy within reach" : "no model");
			return;
		case StandInBadge::Do::kRehang:
			TakeOff("another stand-in, or its 3D rebuilt");
			[[fallthrough]];
		case StandInBadge::Do::kHang:
			if (!Hang(wearer, root)) {
				StandInBadge::Failed(track);
				return;
			}
			break;
		case StandInBadge::Do::kNothing:
			break;
		}
		if (track.hung && wearer) {
			Place(wearer);
		}
	}

	void Drop(std::uint32_t a_actorFormId)
	{
		if (track.hung && track.standIn == a_actorFormId) {
			TakeOff("stand-in deleted");
			StandInBadge::TookOff(track);
		}
	}

	void DropAll()
	{
		if (track.hung) {
			TakeOff("save, load or session end");
			StandInBadge::TookOff(track);
		}
	}

	std::string Describe()
	{
		std::string out = std::format("menuBadges: on={} model='{}' {} radius={:.1f} wearing={:08X} hangs={} takeOffs={} rehangs={} failures={} notOnRoot={} (one at a time: the database's one node)",
			Config::Get().menuBadges, modelPath, !modelAsked ? "not loaded yet" : modelProblem.empty() ? "loaded" : modelProblem, modelRadius, track.hung ? track.standIn : 0,
			track.hangs, track.takeOffs, track.rehangs, track.failures, notOnRoot);
		if (track.hung && model) {
			const auto& at = model->world.translate;
			out += std::format(" at=({:.0f},{:.0f},{:.0f}) scale={:.2f} culled={}", at.x, at.y, at.z, model->local.scale, model->GetAppCulled());
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		for (const auto& info : RemotePlayers::List()) {
			out += std::format("; player {} menu={} glyph={}", info.id, info.menu, std::to_underlying(MenuIconPlace::GlyphOf(info.menu)));
			if (!info.actor || !info.actor->Get3D()) {
				out += " standIn=none";
				continue;
			}
			out += std::format(" standIn={:08X} distance={:.0f}", info.actor->GetFormID(), player ? info.actor->data.location.GetDistance(player->data.location) : -1.0f);
		}
		return out;
	}
}
