#include "game/MenuBadges.h"

#include "Config.h"
#include "MainThread.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/RemotePlayers.h"
#include "game/StandInBadge.h"

namespace MenuBadges
{
	namespace
	{
		using StandInBadge::ModelState;

		// The Pip-Boy (Fallout4.esm), whose world model is the badge.
		constexpr std::uint32_t PIPBOY = 0x00021B3B;
		// The bone it hangs over (Fallout 4's human skeleton), as the HUD icon did.
		constexpr const char* HEAD_BONE = "Head";

		// The model that hangs, asked for the first time a friend is to wear it, and kept until the session ends or a game
		// is loaded (Release), or `menubadges copy` changes. Either the database's own node for the file (one with no collision), of
		// which nothing is changed but where it hangs: the engine clones that node whenever it shows the model (a Pip-Boy's
		// preview, one lying in the world), and the copy takes its transform as it is then. Or, with `menubadges copy on`,
		// our own copy of that node with its collision taken off, which nothing else holds or draws. One at a time: a new
		// one is made only after this one is let go.
		RE::NiPointer<RE::NiNode> model;
		std::string               modelPath;
		bool                      modelAsked = false;
		ModelState                modelState = ModelState::kNotAsked;
		int                       loadError = 0;       // the database's error code, for kLoadFailed
		StandInBadge::Scan        sourceScan;          // the database's node, walked
		std::size_t               collisionsTaken = 0; // off our copy, the last one made
		std::uint32_t             copies = 0;          // copies made this run (one per load of the model: never per hang)
		std::uint32_t             releases = 0;        // Release calls that let a model or our node go
		float                     modelRadius = 0.0f;

		// The dev's switch (menubadges copy on|off), off at every start: set from the dev channel's thread, used on the main
		// thread's next Frame, which takes the badge off and asks for the model again.
		std::atomic<bool> copyWanted{ false };
		bool              copyOn = false;

		// Our own node, made on the first hang after a start or a Release, which the engine never shares: it hangs from the
		// stand-in's root with the turn, height and size on it, and the model hangs under it untouched.
		RE::NiPointer<RE::NiNode> holder;

		// What it hangs from, kept alive by us while it hangs: the stand-in's 3D root, so its skeleton outlives a rebuild
		// of its 3D until the badge is off it, and a new root is never at the address of the one we compare with.
		struct Hung
		{
			RE::NiPointer<RE::NiNode> root;
		};
		std::optional<Hung>   hung;
		// The roots' pins let go of off the main thread (TakeOff from a pre-load step the load's thread ran): kept here
		// until Release lets them go on the main thread after the load, as the last reference to a stand-in's old 3D may
		// be ours, and its skeleton is not to be freed beside a frame. AtExit keeps them with the rest.
		std::vector<RE::NiPointer<RE::NiNode>> parked;
		StandInBadge::Track   track;
		std::uint32_t         notOnRoot = 0;  // taken off a root that no longer held it (moved by something else)
		std::uint32_t         logged = 0;
		// Set by AtExit at the game's shutdown: from then on nothing of the badge runs, and what it held is never freed.
		bool                  exiting = false;
		constexpr std::uint32_t MAX_LOGS = 40;

		void Log(std::string_view a_line)
		{
			if (logged < MAX_LOGS) {
				++logged;
				REX::INFO("MenuBadges: {}", a_line);
			}
		}

		// Every node of a model, StandInBadge::MAX_NODES at most, a_each called on those with a collision object (it may
		// take it off: the node is counted first). Past the cap the scan says so, rather than that it has collision.
		template <class Each>
		StandInBadge::Scan Walk(RE::NiAVObject* a_object, Each&& a_each)
		{
			StandInBadge::Scan           scan;
			std::vector<RE::NiAVObject*> open{ a_object };
			while (!open.empty()) {
				if (scan.nodes >= StandInBadge::MAX_NODES) {
					scan.capped = true;
					break;
				}
				const auto object = open.back();
				open.pop_back();
				if (!object) {
					continue;
				}
				++scan.nodes;
				if (object->collisionObject) {
					++scan.collisions;
					a_each(object);
				}
				if (const auto node = object->IsNode()) {
					for (const auto& child : node->children) {
						open.push_back(child.get());
					}
				}
			}
			return scan;
		}

		StandInBadge::Scan Walk(RE::NiAVObject* a_object)
		{
			return Walk(a_object, [](RE::NiAVObject*) {});
		}

		// Our own copy of the database's node, as the engine clones a node (Gamebryo's NiObject::Clone): CreateClone on the
		// source, then ProcessClone on the SOURCE, which finds each copy in the clone map and fixes up its links. (Round 3
		// of the grenade in the hand called ProcessClone on the clone, with a zero-filled cloning process, and crashed in
		// BSGeometry::ProcessClone: docs/GRENADE-IN-HAND.md.) The cloning process is set up as the engine's constructor
		// sets one: empty maps, an exact copy, '$' to append to names (unused by an exact copy), a scale of 1. Both calls
		// are virtuals on the engine's own node. Nothing of the source is written: a clone only reads it.
		RE::NiPointer<RE::NiNode> CopyOf(RE::NiNode* a_source)
		{
			RE::NiCloningProcess cloning{};
			cloning.copyType = RE::NiCloningProcess::CopyType::kCopyExact;
			cloning.appendChar = '$';
			cloning.scale = { 1.0f, 1.0f, 1.0f };
			const auto made = a_source->CreateClone(cloning);
			if (!made) {
				return {};
			}
			const RE::NiPointer<RE::NiObject> held{ made };  // ours from here: let go on any way out
			a_source->ProcessClone(cloning);
			const auto node = made->IsNode();
			return node ? RE::NiPointer<RE::NiNode>{ node } : RE::NiPointer<RE::NiNode>{};
		}

		// The collision taken off our copy (never the database's node: called on the copy alone), each object only when
		// the copy alone holds it and it is that node's own (StandInBadge::MayTakeOff). How many were left on it.
		std::size_t TakeOffCollision(RE::NiNode* a_copy)
		{
			std::size_t kept = 0;
			collisionsTaken = 0;
			Walk(a_copy, [&](RE::NiAVObject* a_object) {
				const auto collision = a_object->collisionObject.get();
				if (!StandInBadge::MayTakeOff(collision->refCount, collision->GetSceneGraphObject() == a_object)) {
					++kept;
					return;
				}
				// Off the node first, then let go: the node never names an object being destroyed.
				auto taken = std::exchange(a_object->collisionObject, RE::NiPointer<RE::NiCollisionObject>{});
				taken.reset();
				++collisionsTaken;
			});
			return kept;
		}

		// The model, loaded as WeaponFire::HandModel loads a throwable's (from the archives if it is not in memory yet: a
		// small file, on this thread), once; then hung as it is, copied without its collision, or refused
		// (StandInBadge::ModelStateFor and CopyStateFor).
		RE::NiNode* Model()
		{
			if (std::exchange(modelAsked, true)) {
				return StandInBadge::Usable(modelState) ? model.get() : nullptr;
			}
			const auto pipboy = RE::TESForm::GetFormByID<RE::TESObjectARMO>(PIPBOY);
			if (!pipboy) {
				modelState = ModelState::kNoForm;
				Log(StandInBadge::Name(modelState));
				return nullptr;
			}
			for (auto& world : pipboy->worldModel) {
				if (const char* path = world.GetModel(); path && *path) {
					modelPath = path;
					break;
				}
			}
			if (modelPath.empty()) {
				modelState = ModelState::kNoWorldModel;
				Log(StandInBadge::Name(modelState));
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
				modelState = ModelState::kLoadFailed;
				loadError = static_cast<int>(std::to_underlying(error));
				Log(std::format("the Pip-Boy's model '{}' did not load (error {})", modelPath, loadError));
				return nullptr;
			}
			sourceScan = Walk(loaded.get());
			modelState = StandInBadge::ModelStateFor(sourceScan, copyOn);
			switch (modelState) {
			case ModelState::kShared:
				model = loaded;
				Log(std::format("loaded the Pip-Boy's model '{}': no collision in its {} nodes, the database's node hangs as it is", modelPath, sourceScan.nodes));
				break;
			case ModelState::kNodeCap:
				Log(std::format("the Pip-Boy's model '{}' is not hung: the node cap ({}) was reached, so whether it has collision is not known ({} found so far)",
					modelPath, StandInBadge::MAX_NODES, sourceScan.collisions));
				break;
			case ModelState::kCollision:
				Log(std::format("the Pip-Boy's model '{}' is not hung: collision found on {} of its {} nodes (menubadges copy on hangs a copy without it)",
					modelPath, sourceScan.collisions, sourceScan.nodes));
				break;
			case ModelState::kCopy:
				{
					const auto  copy = CopyOf(loaded.get());
					std::size_t kept = 0;
					StandInBadge::Scan after;
					if (copy) {
						++copies;
						kept = TakeOffCollision(copy.get());
						after = Walk(copy.get());
					}
					modelState = StandInBadge::CopyStateFor(copy != nullptr, kept, after);
					if (modelState == ModelState::kCopy) {
						model = copy;
						Log(std::format("made our own copy of the Pip-Boy's model '{}' (copy {}): collision taken off {} of its {} nodes, the database's node untouched",
							modelPath, copies, collisionsTaken, after.nodes));
					} else {
						// Let go here (it hangs nowhere), with whatever collision was left on it.
						Log(std::format("the Pip-Boy's model '{}' is not hung: {} ({}; {} collision object(s) taken off, {} left that the copy does not hold alone, {} found after on {} nodes{})",
							modelPath, StandInBadge::Name(modelState), copy ? "copy made" : "no copy made", collisionsTaken, kept, after.collisions, after.nodes,
							after.capped ? ", node cap reached" : ""));
					}
				}
				break;
			default:
				break;
			}
			return StandInBadge::Usable(modelState) ? model.get() : nullptr;
		}

		// Off the root it hangs from, and the model off our node. One moved elsewhere meanwhile is left where it is. Off
		// the main thread the root's pin is parked, not let go (see parked).
		void TakeOff(std::string_view a_why)
		{
			if (!hung) {
				return;
			}
			const bool onRoot = holder && hung->root && holder->parent == hung->root.get();
			if (onRoot) {
				hung->root->DetachChild(holder.get());
			} else {
				++notOnRoot;
			}
			if (model && holder && model->parent == holder.get()) {
				holder->DetachChild(model.get());
			}
			// The root's count before our pin goes: 1 means ours was the last reference, and letting it go frees its 3D.
			const auto rootRefs = hung->root ? static_cast<std::uint32_t>(hung->root->refCount) : 0u;
			const bool main = MainThread::IsMain();
			if (!main && hung->root) {
				parked.push_back(std::move(hung->root));  // never freed on this thread: Release lets it go on the main one
			}
			hung.reset();
			Log(std::format("took the badge off: {}{} (rootRefs={}{})", a_why, onRoot ? "" : " (it was no longer on the root)", rootRefs,
				main ? "" : "; off the main thread, the root's pin is parked until after the load"));
		}

		// The model let go, to be asked for again (the copy switch changed). Called with nothing hung: our copy, if it was
		// one, is then held by nothing else and goes.
		void ForgetModel()
		{
			model.reset();
			modelPath.clear();
			modelAsked = false;
			modelState = ModelState::kNotAsked;
			loadError = 0;
			sourceScan = {};
			collisionsTaken = 0;
			modelRadius = 0.0f;
		}

		// Whether it is still where it was hung: our node on the root, the model on our node.
		bool StillHung()
		{
			return hung && hung->root && holder && model && holder->parent == hung->root.get() && model->parent == holder.get();
		}

		// Over the head and turned to our camera, at its size: our node's local transform under a_root.
		void Place(RE::Actor* a_actor, RE::NiNode* a_root)
		{
			const auto& at = a_root->world.translate;
			const auto  head = Equipment::ChildNode(a_root, HEAD_BONE);
			RE::NiPoint3 camera = a_actor->data.location;
			if (const auto playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->cameraRoot) {
				camera = playerCamera->cameraRoot->world.translate;
			}
			StandInBadge::Matrix rootTurn{};
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t column = 0; column < 3; ++column) {
					rootTurn[row][column] = a_root->world.rotate.entry[row][column];
				}
			}
			const auto turn = StandInBadge::Turn(StandInBadge::FacingYaw(rootTurn, camera.x - at.x, camera.y - at.y));
			auto&      local = holder->local;
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t column = 0; column < 3; ++column) {
					local.rotate.entry[row][column] = turn[row][column];
				}
			}
			local.translate = { 0.0f, 0.0f, StandInBadge::Height(head != nullptr, head ? head->world.translate.z : 0.0f, at.z, a_root->world.scale) };
			// The model's radius in our node's units (its own local scale in it), from what the engine last worked out.
			modelRadius = StandInBadge::OwnRadius(model->worldBound.fRadius, holder->world.scale);
			local.scale = StandInBadge::LocalScale(modelRadius, a_root->world.scale);
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
			if (!holder) {
				holder = RE::NiPointer<RE::NiNode>{ new RE::NiNode(1) };
			}
			if (const auto parent = holder->parent) {
				parent->DetachChild(holder.get());  // left on a root by something else: ours to move
			}
			holder->AttachChild(badge, true);
			// Where it is to be before it goes on, so the first frame does not draw it where it last was. A fresh model has no
			// world bound until its first update, so this Place sizes it by its own scale alone; Frame's Place after Hang,
			// once holder->Update below has run, sets its size. Keep that one after Hang, or the first frame pops in size.
			Place(a_actor, root);
			root->AttachChild(holder.get(), true);
			if (holder->parent != root || badge->parent != holder.get()) {
				// Nothing is left half hung: not our node on a root we no longer hold, nor the model on our node.
				if (holder->parent == root) {
					root->DetachChild(holder.get());
				}
				if (badge->parent == holder.get()) {
					holder->DetachChild(badge);
				}
				Log(std::format("no badge hung on {:08X}: its root did not take it", a_actor->GetFormID()));
				return false;
			}
			RE::NiUpdateData update{};
			holder->Update(update);  // our node and the model under it, in the world now
			hung = Hung{ RE::NiPointer<RE::NiNode>{ root } };
			Log(std::format("hung the badge ({}) on {:08X}", modelState == ModelState::kCopy ? "our copy" : "the database's node", a_actor->GetFormID()));
			return true;
		}
	}

	void Frame()
	{
		if (exiting) {
			return;
		}
		const bool on = Config::Get().menuBadges;
		const bool idle = !on && !track.hung;  // as without the badge: the flag read, then at most the copy switch below
		// A loading screen: the world is going or coming, and the stand-ins' 3D with it. Nothing of it is touched until the
		// screen is down (a save or load took the badge off before it: DropAll). The frame's own gate (main.cpp's RunFrame)
		// skips the frame then too; this one holds wherever Frame is called from.
		if (!idle && Hud::LoadingScreenUp()) {
			return;
		}
		// The dev's copy switch changed: off whoever wears it, and the model asked for again with the new setting. (When
		// idle nothing hangs, so no scene graph is touched: the model is in none.)
		if (const bool want = copyWanted.load(std::memory_order_relaxed); want != copyOn) {
			if (track.hung) {
				TakeOff("menubadges copy changed");
				StandInBadge::TookOff(track);
			}
			copyOn = want;
			ForgetModel();
		}
		if (idle) {
			return;  // nothing is looked at
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
				// No worldspace check on the distance: a stand-in in another one has no 3D, and was skipped above.
				candidates.push_back({ info.actor->GetFormID(), MenuIconPlace::GlyphOf(info.menu), info.actor->data.location.GetDistance(player->data.location) });
				actors.push_back(info.actor);
			}
			if (const auto pick = StandInBadge::Pick(candidates, track.hung ? track.standIn : 0); pick != StandInBadge::NONE) {
				wearer = actors[pick];
				root = wearer->Get3D();
				facts.standIn = candidates[pick].standIn;
				facts.root = reinterpret_cast<std::uintptr_t>(root);
				facts.model = Model() != nullptr;
				facts.lost = track.hung && !StillHung();  // dressed again on the same root, say: on again
			}
		}
		switch (StandInBadge::Frame(track, facts)) {
		case StandInBadge::Do::kTakeOff:
			TakeOff(!on ? "turned off" : !facts.standIn ? "nobody in the Pip-Boy within reach" : !facts.root ? "its 3D is gone" : "no model");
			return;
		case StandInBadge::Do::kRehang:
			TakeOff(facts.lost ? "taken off by something else" : "another stand-in, or its 3D rebuilt");
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
		if (track.hung && wearer && StillHung()) {
			Place(wearer, hung->root.get());
		}
	}

	void Drop(std::uint32_t a_actorFormId)
	{
		if (exiting) {
			return;
		}
		if (track.hung && track.standIn == a_actorFormId) {
			TakeOff("stand-in deleted");
			StandInBadge::TookOff(track);
		}
	}

	// Before a save or a load this may run on the load's thread (main.cpp's RunBeforeSaveOrLoad, when the main thread is
	// late), as the grenade's DropHandThrowables does, normally once no frame is working (it waits for that), among the
	// stand-ins' own deletes there: it takes our node off the root there and frees nothing at all. Our node and the model stay ours; the root's pin is parked (TakeOff), not let go, so the
	// stand-in's old 3D is never freed on that thread even when ours was its last reference. Release lets it go after
	// the load, on the main thread.
	void DropAll()
	{
		if (exiting) {
			return;
		}
		if (track.hung) {
			TakeOff("save, load or session end");
			StandInBadge::TookOff(track);
		}
	}

	// On the main thread only (the session's end, OnGameLoaded): our node and our copy are freed here.
	void Release()
	{
		if (exiting) {
			return;
		}
		DropAll();
		// The model off our node (TakeOff has normally done it), then both let go. Either one moved elsewhere by something
		// else is held by its new parent, not freed from under it.
		if (holder && model && model->parent == holder.get()) {
			holder->DetachChild(model.get());
		}
		const bool held = model || holder || !parked.empty();
		ForgetModel();
		holder.reset();
		parked.clear();  // the roots' pins TakeOff parked off the main thread, let go here on it
		if (held) {
			++releases;
			Log("let the model and our node go (session end or load)");
		}
	}

	void AtExit()
	{
		if (std::exchange(exiting, true)) {
			return;
		}
		// Let go of without being freed, on purpose, as other engine objects of ours are at exit (Achievements' hooked
		// functions), and not taken off the root either: the world is being torn down. Our last references to the root,
		// our node and the model would otherwise go in a static's destructor at the DLL's unload, after the renderer and
		// Havok are gone, and could crash the quit.
		struct Kept
		{
			RE::NiPointer<RE::NiNode>              root;
			RE::NiPointer<RE::NiNode>              holder;
			RE::NiPointer<RE::NiNode>              model;
			std::vector<RE::NiPointer<RE::NiNode>> parked;
		};
		const bool held = hung || holder || model || !parked.empty();
		if (held) {
			new Kept{ hung ? std::move(hung->root) : RE::NiPointer<RE::NiNode>{}, std::move(holder), std::move(model), std::move(parked) };  // never deleted
		}
		hung.reset();
		if (held) {
			REX::INFO("MenuBadges: the game is quitting: the badge's model, our node and the root it hung from are kept, not freed");
		}
	}

	void SetCopy(bool a_on)
	{
		copyWanted.store(a_on, std::memory_order_relaxed);
	}

	bool CopyWanted()
	{
		return copyWanted.load(std::memory_order_relaxed);
	}

	std::string Describe()
	{
		std::string state{ StandInBadge::Name(modelState) };
		if (modelState == ModelState::kLoadFailed) {
			state += std::format(" (error {})", loadError);
		}
		std::string out = std::format("menuBadges: on={} model='{}' {} nodes={} collisions={}{} copy={}{} copies={} collisionsTaken={} releases={}",
			Config::Get().menuBadges, modelPath, state, sourceScan.nodes, sourceScan.collisions, sourceScan.capped ? " (node cap reached)" : "",
			copyOn ? "on" : "off", CopyWanted() != copyOn ? " (changing next frame)" : "", copies, collisionsTaken, releases);
		if (model && modelState == ModelState::kCopy) {
			out += std::format(" copyRefs={}", model->refCount);  // ours, plus our node's while it hangs: 1 or 2
		}
		out += std::format(" radius={:.1f} wearing={:08X} hangs={} takeOffs={} rehangs={} lost={} failures={} notOnRoot={} (one at a time)",
			modelRadius, track.hung ? track.standIn : 0, track.hangs, track.takeOffs, track.rehangs, track.lost, track.failures, notOnRoot);
		if (track.hung && model && holder) {
			const auto& at = model->world.translate;
			out += std::format(" at=({:.0f},{:.0f},{:.0f}) scale={:.2f} culled={}", at.x, at.y, at.z, holder->local.scale, model->GetAppCulled());
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
