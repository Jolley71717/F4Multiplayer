#include "game/RemotePlayers.h"
#include "game/ArmorFrames.h"
#include "game/DeferredDelete.h"
#include "game/DownRules.h"
#include "game/Dialogue.h"
#include "game/EngineRefs.h"
#include "game/FriendLight.h"
#include "game/SaveGuard.h"
#include "game/SaveStrays.h"
#include "game/StandInGear.h"
#include "game/StandInHold.h"
#include "game/StandInRange.h"
#include "game/StateBuffer.h"

#include "game/Face.h"
#include "Config.h"
#include "game/Equipment.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/WeaponFire.h"

namespace RemotePlayers
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// A place as a friend's state names it.
		struct Spot
		{
			std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
			std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
			RE::NiPoint3  at;
		};

		struct RemotePlayer
		{
			std::uint32_t        id = 0;  // their player ID: what the log calls them (never by name)
			std::string          name;
			std::uint32_t        appearance = 0;
			bool                 appearanceWarned = false;  // told the log once that their look is not one we place
			std::optional<Protocol::Face> face;
			std::uint32_t        faceBaseId = 0;  // our copy of the base NPC wearing their face (a runtime form; resolved at use, it may not survive a load)
			bool                 faceTried = false;
			StandInGear::Gear    gear;  // what they wear and hold, and what the stand-in was dressed in
			StateBuffer::Buffer  states;  // what they sent lately, and how far in the past it is drawn
			RE::ObjectRefHandle  actor;
			// The pointer registered with Puppets. Kept separately so it can be unregistered even
			// if the game deletes the actor behind our back (its handle then no longer resolves).
			// Never dereferenced; registeredId is its form ID.
			RE::Actor*           registered = nullptr;
			std::uint32_t        registeredId = 0;
			std::uint8_t         health = 100;
			bool                 downed = false;
			std::string          shownName;  // the name tag currently on the stand-in
			Clock::time_point    knownAt{};    // when we first heard of them
			bool                 lightOn = false;  // their Pip-Boy light
			RE::ObjectRefHandle  light;            // the light that stands in for it, following the stand-in
			FriendLight::Track   lightTrack;       // when it was placed, and whether its cell was drawn since (see FriendLight::Frame)
			StandInRange::Track  range;  // the current stand-in: when it was placed and whether it was drawn (see StandInRange::Frame)
			bool                 powerArmorOn = false;   // they are in power armor
			std::uint32_t        powerArmorFrame = 0;    // the base form of their frame
			std::uint32_t        powerArmorFrameRef = 0; // their frame itself when every game has it (a pre-placed one), else 0
			bool                 inPowerArmor = false;   // they are in a frame here: the stand-in goes into its copy as soon as it can (see ArmorFrames)
			RE::ObjectRefHandle  frame;                  // the frame we placed for it: the stand-in's to get into, the engine's while it is in, or where they got out
			Clock::time_point    framePlacedAt{};
			Clock::time_point    frameActionAt{};        // when they last got out (the copy stands where they did a while)
			const RE::NiAVObject* frameCollisionOff = nullptr;  // the frame's 3D whose collision we turned off; only compared
			int                  switchTries = 0;        // switch-ins in this wear that did not take, or whose skeleton never came (see ArmorFrames::ChooseSwitch)
			Clock::time_point    switchTriedAt{};
			Clock::time_point    switchedAt{};           // when the switch-in took for this stand-in
			const RE::NiAVObject* armorSkeleton = nullptr;  // the stand-in's 3D found built on the power armor skeleton; only compared
			bool                 switchGaveUp = false;   // logged
			std::optional<RE::NiPoint3> lastTarget;      // where the stand-in was put last frame
			StandInHold::Check   hold;                   // frames it stood still somewhere else than put (net status)
			Clock::time_point    holdLoggedAt{};
			RE::ObjectRefHandle  original;               // our copy of their pre-placed frame, hidden while they wear theirs
			const RE::NiAVObject* originalHidden = nullptr;  // its 3D we stopped drawing; only compared
			const RE::NiAVObject* originalCollisionOff = nullptr;  // its 3D whose collision we turned off; only compared
			Clock::time_point    originalCheckAt{};
			int                  originalChoice = -1;    // the last ArmorFrames::Original logged
			bool                 originalCounted = false;  // a hide counted for this wear (one hidden again after a load is not a new one)
			ArmorFrames::UnknownFrame frameUnknown;      // a wear the stand-in goes through with no frame copy, for the log (see Update)
			std::optional<Spot>  wornAt;                 // where they were as they got into the pre-placed frame they wear
			bool                 wornAtWanted = false;   // that spot is taken from their next state
			bool                 originalMoved = false;  // ours was not where they got in (or moved, or was used) in this wear: left alone
		};

		std::map<std::uint32_t, RemotePlayer> players;
		// The looks friends asked for this run (base NPCs; see Spawn), kept after they leave: a stand-in an
		// earlier version saved may wear one (see IsStandInBase).
		std::set<std::uint32_t> looks;

		// Where a friend is now, as where they got into a pre-placed frame (see SetPowerArmor).
		void TakeWornAt(RemotePlayer& a_player, const Protocol::PlayerState& a_state)
		{
			a_player.wornAt = Spot{ a_state.cell, a_state.worldspace, { a_state.x, a_state.y, a_state.z } };
			a_player.wornAtWanted = false;
		}

		// Stand-ins to delete once their 3D is in (see DeferredDelete).
		// A stand-in (or its light or frame) goes in two steps: disabled now, so the engine takes its 3D and
		// collision out at its own safe point, and marked for deletion a moment later. Deleting in one
		// step crashed a physics job that was still handling a collision with the actor
		// (CharacterCollisionHandler -> Actor::GetDesiredProcessLevel -> Get3D on a freed actor).
		constexpr auto DELETE_AFTER_DISABLE = 1s;
		struct Grave
		{
			RE::ObjectRefHandle              actor;
			Clock::time_point                spawnedAt;
			std::optional<Clock::time_point> disabledAt;
			Clock::time_point                buriedAt{};
			bool                             held = false;  // has waited for the player's conversation
			// A stand-in stays one of our puppets until it is disabled: its AI off, kept healthy, and not to be
			// talked to. One kept for the player's conversation used to be let go first, so its AI ran and the
			// player could press E on it, start another conversation, and have it deleted in the middle of
			// that one. Never dereferenced (see RemotePlayer::registered); null for a light or frame.
			const RE::Actor*                 puppet = nullptr;
			std::uint32_t                    puppetId = 0;
			std::uint32_t                    id = 0;  // the reference's form ID, for when its handle stops resolving (SaveGuard::Forget)
			// A frame whose stand-in is kept for the player's conversation while in it: it waits for the stand-in (hidden
			// with it), instead of being disabled from under the actor held in it.
			RE::ObjectRefHandle              wearer;
		};
		std::vector<Grave> graveyard;
		std::uint32_t      flushedBeforeSave = 0;
		// Marked for deletion, and still there until the engine lets go of them: a save in that moment
		// would write them too.
		struct Deleting
		{
			RE::ObjectRefHandle ref;
			std::uint32_t       id = 0;
		};
		std::vector<Deleting> deleting;
		std::uint32_t      heldForConversation = 0;

		// The stand-in is out of the world (or gone): no longer one of our puppets.
		void Release(Grave& a_grave)
		{
			if (a_grave.puppet) {
				Puppets::Unregister(a_grave.puppet, a_grave.puppetId);
				a_grave.puppet = nullptr;
				a_grave.puppetId = 0;
			}
		}

		void Disable(Grave& a_grave, RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			Release(a_grave);
			a_ref->Disable();
			a_grave.disabledAt = a_now;
		}

		// What is kept for the player's conversation is not seen: its friend has left, or is shown by a new
		// stand-in already, and the two stood side by side for up to a minute. Hidden rather than disabled
		// or moved, which would take the conversation's speaker away; again each frame, since a 3D that is
		// loaded again comes back shown.
		void Hide(RE::TESObjectREFR* a_ref)
		{
			if (const auto root = a_ref->Get3D()) {
				root->SetAppCulled(true);
			}
		}

		// A stand-in in the player's conversation stays (not even disabled) until the conversation ends
		// (see DeferredDelete::Held). Deleting the one speaking to the player left the conversation open
		// for good: no weapons, Pip-Boy or workshop for the host near Codsworth.
		bool HeldForConversation(Grave& a_grave, RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			if (!DeferredDelete::Held(Dialogue::Holds(a_ref), a_now - a_grave.buriedAt)) {
				return false;
			}
			Hide(a_ref);
			if (!a_grave.held) {
				a_grave.held = true;
				++heldForConversation;
				REX::INFO("RemotePlayers: {:08X} is in the player's conversation; it goes when that ends", a_ref->GetFormID());
			}
			return true;
		}

		// A frame waits while the stand-in kept in it is still there (see Grave::wearer), hidden with it.
		bool WaitsForWearer(const Grave& a_grave, RE::TESObjectREFR* a_ref)
		{
			const auto wearer = a_grave.wearer.get();
			if (!wearer || wearer->IsDisabled() || wearer->IsDeleted()) {
				return false;
			}
			Hide(a_ref);
			return true;
		}

		// Disables the reference now if it is safe to, and leaves it to the graveyard to delete. One that has lost its
		// cell already (the cells were cleared under it: a door, a coc, a fast travel) has its record dropped at once
		// (see SaveGuard::ExcludeOurs), and again each frame while it waits (see Update).
		void Bury(Grave a_grave)
		{
			const auto ref = a_grave.actor.get();
			if (!ref) {
				Release(a_grave);
				return;
			}
			const auto now = Clock::now();
			a_grave.buriedAt = now;
			a_grave.id = ref->GetFormID();
			if (!ref->GetParentCell()) {
				SaveGuard::ExcludeOurs(ref.get());
			}
			if (DeferredDelete::Safe(ref->Get3D() != nullptr, now - a_grave.spawnedAt) && !HeldForConversation(a_grave, ref.get(), now) &&
				!WaitsForWearer(a_grave, ref.get())) {
				Disable(a_grave, ref.get(), now);
			}
			graveyard.push_back(a_grave);
		}

		// A reference of ours with neither a cell nor a worldspace (see SaveGuard::Anchored): its record is dropped, and it
		// is one to let go of once it has had a moment to attach (one just placed may not be in its cell yet).
		constexpr auto ADRIFT_GRACE = 2s;
		std::uint32_t  adrift = 0;  // lights and frame copies let go of so

		bool Adrift(RE::TESObjectREFR* a_ref, Clock::time_point a_placedAt, Clock::time_point a_now)
		{
			if (!a_ref || a_ref->GetParentCell() || SaveGuard::Anchored(a_ref)) {
				return false;
			}
			SaveGuard::ExcludeOurs(a_ref);
			return a_now - a_placedAt >= ADRIFT_GRACE;
		}

		// The first stand-in waits this long for the player's face, instead of being placed and then
		// replaced a moment later when the face arrives.
		constexpr auto FACE_GRACE = 1s;

		// The light form placed at a friend's stand-in while their Pip-Boy light is on (dev: friendlight).
		std::uint32_t friendLightForm = 0x0001ED2D;  // a plain white light, radius 800, in Fallout4.esm
		constexpr float LIGHT_HEIGHT = 110.0f;  // above the feet: about the Pip-Boy
		std::uint32_t   lightsPlaced = 0;
		std::uint32_t   framesPlaced = 0;
		std::uint32_t   framesFollowed = 0;  // frames moved back under their stand-in
		std::uint32_t   leftBehind = 0;      // stand-ins replaced because they were left in another cell
		std::uint32_t   downedStandIns = 0;  // stand-ins replaced because something here put them down (DownRules::StandInBroken)
		std::uint32_t   spawns = 0;          // stand-ins placed
		std::uint32_t   respawnWaits = 0;    // of those left behind, never drawn where they were placed: none placed for a moment
		std::uint32_t   originalsHidden = 0; // our copies of pre-placed frames hidden while a friend wore theirs (once a wear)
		std::uint32_t                         equipmentFixesTotal = 0;
		std::uint32_t                         gearChanges = 0;  // reported changes put on over what a stand-in wore
		std::uint32_t                         redraws = 0;      // stand-ins that put their gun away and drew it again for their gear
		// Frames drawn while a friend was moving, and how many of those ran out of states to blend
		// (the next one was late) and held them still: the interpolation delay is too short if
		// this is more than a few percent.
		std::uint32_t                         movingFrames = 0;
		std::uint32_t                         heldFrames = 0;

		// The items we can put on a stand-in here (forms another game has but ours doesn't are skipped).
		// A grenade or mine the owner holds: given to the stand-in (its replayed throw needs one in hand,
		// Equipment::HoldThrowable puts it there) but never counted when checking what the stand-in wears,
		// since an NPC's inventory does not report a throwable as equipped. Counting it made the gear check
		// fail for ever: the stand-in was stripped and dressed again every few seconds, which also threw it
		// out of its power armor frame each time.
		bool Throwable(std::uint32_t a_item)
		{
			const auto weapon = RE::TESForm::GetFormByID<RE::TESObjectWEAP>(a_item);
			if (!weapon) {
				return false;
			}
			const auto type = weapon->weaponData.type.get();
			return type == RE::WEAPON_TYPE::kGrenade || type == RE::WEAPON_TYPE::kMine;
		}

		// What an item of theirs is, for what the stand-in wears (see ArmorFrames::Wears).
		ArmorFrames::Item KindOf(std::uint32_t a_item)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			if (!form) {
				return ArmorFrames::Item::kOther;
			}
			if (form->Is(RE::ENUM_FORM_ID::kWEAP)) {
				return ArmorFrames::Item::kWeapon;
			}
			if (Equipment::IsFrameItem(form)) {
				return ArmorFrames::Item::kFrame;
			}
			return Equipment::IsPowerArmorPiece(form) ? ArmorFrames::Item::kPiece : ArmorFrames::Item::kOther;
		}

		bool PowerArmorPiece(std::uint32_t a_item)
		{
			return KindOf(a_item) == ArmorFrames::Item::kPiece;
		}

		// The items a stand-in dressed so wears (see ArmorFrames::Wears), in their order.
		std::vector<std::uint32_t> Kept(const std::vector<std::uint32_t>& a_items, ArmorFrames::Dress a_dress)
		{
			std::vector<std::uint32_t> out;
			for (const auto item : a_items) {
				if (ArmorFrames::Wears(a_dress, KindOf(item))) {
					out.push_back(item);
				}
			}
			return out;
		}

		// What a stand-in should wear now, dressed as the engine has it (ArmorFrames::ChooseDress): out of a frame, all the
		// owner has on but the power armor pieces and the frame (their gear list names those while they are in theirs, and
		// the stand-in is not in its own until both are loaded, or ever, should the switch not take); in its frame, their
		// pieces and weapons.
		std::vector<std::uint32_t> Outfit(const RemotePlayer& a_player, ArmorFrames::Dress a_dress)
		{
			return Kept(a_player.gear.items, a_dress);
		}

		std::vector<std::uint32_t> Wearable(const std::vector<std::uint32_t>& a_items)
		{
			std::vector<std::uint32_t> out;
			for (const auto item : a_items) {
				const auto form = RE::TESForm::GetFormByID(item);
				if (form && form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP) && !Throwable(item)) {
					out.push_back(item);
				}
			}
			return out;
		}

		// A weapon held in the hand: not a grenade or mine (see Throwable), nor one this game does not have.
		bool HandWeapon(std::uint32_t a_item)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			return form && form->Is(RE::ENUM_FORM_ID::kWEAP) && !Throwable(a_item);
		}

		std::string NameTag(const RemotePlayer& a_player)
		{
			if (a_player.downed) {
				return a_player.name + " (down - help them up)";
			}
			if (a_player.health == 0) {
				return a_player.name + " (dead)";
			}
			// Rounded to tens so the tag doesn't change with every scratch.
			const int shown = (a_player.health + 9) / 10 * 10;
			return shown >= 100 ? a_player.name : std::format("{} ({}%)", a_player.name, shown);
		}

		RE::Actor* GetActor(const RemotePlayer& a_player)
		{
			const auto ref = a_player.actor.get();
			if (!ref || ref->IsDeleted()) {
				return nullptr;
			}
			return ref->As<RE::Actor>();
		}

		std::int32_t LifeState(RE::Actor* a_actor)
		{
			return static_cast<std::int32_t>(static_cast<RE::ActorState&>(*a_actor).lifeState);
		}

		// A dead puppet (killed despite protection, e.g. by a script), or one put down by a hit bigger than its whole
		// health, is replaced by a fresh one.
		bool IsBroken(RE::Actor* a_actor)
		{
			return DownRules::StandInBroken(LifeState(a_actor), a_actor->IsDead(false), a_actor->IsDisabled());
		}

		// Removes the stand-in's light, once its 3D is in (the same rule as for the stand-in itself).
		void DeleteLight(RemotePlayer& a_player)
		{
			if (const auto light = a_player.light.get()) {
				Bury({ .actor = a_player.light, .spawnedAt = a_player.lightTrack.placedAt });
			}
			a_player.light = {};
		}

		// Where a reference is, as FriendLight::SamePlace compares places.
		FriendLight::Place PlaceOf(RE::TESObjectREFR* a_ref)
		{
			FriendLight::Place place;
			if (const auto cell = a_ref ? a_ref->GetParentCell() : nullptr) {
				place.cell = cell->GetFormID();
				place.interior = cell->IsInterior();
				place.worldspace = !place.interior && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
			}
			return place;
		}

		// Two references in one place: the same cell, or both outside in the same worldspace (FriendLight::SamePlace).
		std::optional<bool> SamePlace(RE::TESObjectREFR* a_one, RE::TESObjectREFR* a_other)
		{
			return FriendLight::SamePlace(PlaceOf(a_one), PlaceOf(a_other));
		}

		// Keeps a light at the stand-in while their Pip-Boy light is on, and removes it when it goes off. It is remade only
		// when the stand-in went into another place, or its own cell is leaving the loaded area (FriendLight::Frame).
		void UpdateLight(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now)
		{
			auto light = a_player.light.get();
			FriendLight::Facts facts;
			facts.wanted = a_player.lightOn && a_actor && a_actor->Get3D();
			facts.lit = static_cast<bool>(light);
			if (light) {
				facts.light = PlaceOf(light.get());
				if (const auto lightCell = light->GetParentCell()) {
					facts.lightCell = static_cast<StandInRange::CellState>(std::to_underlying(lightCell->cellState));
				}
			}
			if (a_actor) {
				facts.standIn = PlaceOf(a_actor);
			}
			const auto action = FriendLight::Frame(a_player.lightTrack, facts, a_now);
			if (action == FriendLight::Action::kNone) {
				return;
			}
			if (action == FriendLight::Action::kRemove || action == FriendLight::Action::kReplace) {
				DeleteLight(a_player);
				light = nullptr;
			}
			if (action == FriendLight::Action::kRemove) {
				return;
			}
			RE::NiPoint3 at = a_actor->data.location;
			at.z += LIGHT_HEIGHT;
			if (!light) {
				const auto form = RE::TESForm::GetFormByID<RE::TESObjectLIGH>(friendLightForm);
				if (!form) {
					return;
				}
				RE::NEW_REFR_DATA data;
				data.location = at;
				data.object = form;
				data.interior = a_cell->IsInterior() ? a_cell : nullptr;
				data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
				data.clearStillLoadingFlag = true;
				const auto handle = EngineRefs::CreateReferenceAtLocation(data);
				light = handle.get().get();
				if (!light) {
					return;
				}
				a_player.light = handle;
				FriendLight::Placed(a_player.lightTrack, a_now);
				++lightsPlaced;
				SaveGuard::Track(light.get());
			}
			light->SetLocationOnReference(at);
			light->Update3DPosition(true);  // the reference's place alone left the light shining where it was made
		}

		// A frame stands clear of another reference (its stand-in, or a frame copy) further than this, or when the two
		// are in different places altogether. A frame given its collision back while the stand-in was still in it
		// shoved the stand-in aside.
		constexpr float FRAME_CLEAR_DISTANCE = 70.0f;

		bool Clear(RE::TESObjectREFR* a_frame, RE::TESObjectREFR* a_other)
		{
			if (!a_other || a_other == a_frame) {
				return true;
			}
			const auto samePlace = SamePlace(a_frame, a_other);
			return !samePlace || !*samePlace || a_frame->data.location.GetDistance(a_other->data.location) > FRAME_CLEAR_DISTANCE;
		}

		// Turns a frame's collision off, or back on. a_off is the 3D whose collision we turned off (only compared): a
		// 3D loaded since comes with its own collision, and is left alone. True when it was turned off just now.
		bool SetCollision(const RE::NiAVObject*& a_off, RE::TESObjectREFR* a_frame, bool a_on)
		{
			const auto root = a_frame ? a_frame->Get3D() : nullptr;
			if (!root) {
				a_off = nullptr;
				return false;
			}
			if (a_on) {
				if (a_off == root) {
					RE::bhkWorld::EnableCollision(root, true, true, true);
				}
				a_off = nullptr;
				return false;
			}
			if (a_off == root) {
				return false;
			}
			RE::bhkWorld::EnableCollision(root, false, true, true);
			a_off = root;
			return true;
		}

		// A frame drawn or not (while it has 3D).
		void SetShown(RE::TESObjectREFR* a_frame, bool a_shown)
		{
			if (const auto root = a_frame ? a_frame->Get3D() : nullptr; root && root->GetAppCulled() == a_shown) {
				root->SetAppCulled(!a_shown);
			}
		}

		// Our own player is in this frame, or left it last: their own suit, wherever it stands.
		bool OurFrame(RE::TESObjectREFR* a_frame)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
			const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
			return (occupied && occupied.get() == a_frame) || player->lastUsedPowerArmor.get().get() == a_frame;
		}

		// Another actor in our game (but a_except, the stand-in the frame is for) is in this frame.
		bool FrameInUse(RE::TESObjectREFR* a_frame, const RE::Actor* a_except = nullptr)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (a_except && actor.get() == a_except) {
					continue;
				}
				const auto middle = actor && actor->currentProcess ? actor->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				if (occupied && occupied.get() == a_frame) {
					return true;
				}
			}
			return false;
		}

		// Nobody stands in this frame: no living actor here (our player, a stand-in, a companion, a settler) within
		// FRAME_CLEAR_DISTANCE of it. Its collision given back with someone in it (a hidden frame has none, so anyone can
		// walk into its spot) shoved them aside or shut them in.
		bool NobodyIn(RE::TESObjectREFR* a_frame)
		{
			const auto in = [&](RE::Actor* a_actor) {
				return a_actor && !a_actor->IsDisabled() && !a_actor->IsDead(false) && !Clear(a_frame, a_actor);
			};
			if (in(RE::PlayerCharacter::GetSingleton())) {
				return false;
			}
			if (std::ranges::any_of(players, [&](const auto& a_entry) { return in(GetActor(a_entry.second)); })) {
				return false;
			}
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return true;
			}
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					if (in(handle.get().get())) {
						return false;
					}
				}
			}
			return true;
		}

		// Frames of our world given back (drawn again) whose collision waits until nobody stands in them.
		struct PendingSolid
		{
			RE::ObjectRefHandle   ref;
			const RE::NiAVObject* off = nullptr;  // its 3D whose collision we turned off; only compared
		};
		std::vector<PendingSolid> pendingSolid;

		// A frame's collision back on: now if nobody stands in it, else as soon as nobody does (see UpdatePendingSolid).
		void RestoreCollision(const RE::NiAVObject*& a_off, RE::TESObjectREFR* a_frame)
		{
			if (!a_off) {
				return;
			}
			if (NobodyIn(a_frame)) {
				SetCollision(a_off, a_frame, true);
				return;
			}
			pendingSolid.push_back({ a_frame->GetHandle(), a_off });
			a_off = nullptr;
		}

		void UpdatePendingSolid()
		{
			std::erase_if(pendingSolid, [](PendingSolid& a_pending) {
				const auto ref = a_pending.ref.get();
				if (!ref || ref->IsDeleted() || ref->Get3D() != a_pending.off) {
					return true;  // gone, or a 3D loaded since, which comes with its own collision
				}
				if (!NobodyIn(ref.get())) {
					return false;
				}
				SetCollision(a_pending.off, ref.get(), true);
				return true;
			});
		}

		// A frame of ours hidden again (another friend got into theirs): its collision is ours to keep off again, not
		// to give back. Returns the 3D whose collision is already off, if any.
		const RE::NiAVObject* TakePendingSolid(const RE::TESObjectREFR* a_frame)
		{
			const RE::NiAVObject* off = nullptr;
			std::erase_if(pendingSolid, [&](const PendingSolid& a_pending) {
				if (a_pending.ref.get().get() != a_frame) {
					return false;
				}
				off = a_pending.off;
				return true;
			});
			return off;
		}

		// A frame made in this game goes: the persistence the engine gave it as a stand-in was switched into it is let go
		// first (nothing is done to one no stand-in got into; see Equipment::ReleasePowerArmorFrame), then it is buried.
		// Every frame of ours goes this way, whether or not its stand-in can still be found: the stand-in's own frame is
		// known from the stand-in, and a copy buried after the game had taken the stand-in from under us used to keep its
		// promotion in the persistence manager for the rest of the session.
		// a_wearer: the stand-in kept for the player's conversation while in this frame (see Grave::wearer).
		void BuryFrame(RE::TESObjectREFR* a_frame, Clock::time_point a_placedAt, RE::ObjectRefHandle a_wearer = {})
		{
			Equipment::ReleasePowerArmorFrame(a_frame);
			Bury({ .actor = a_frame->GetHandle(), .spawnedAt = a_placedAt, .wearer = a_wearer });
		}

		// The frame placed for the stand-in is removed once they are out of it a while (or the stand-in is gone).
		// a_standIn is the stand-in the copy was placed for: in it (the engine's, disabled) or standing in it where they
		// got out, it is never someone here who got into it.
		void DeleteFrame(RemotePlayer& a_player, const RE::Actor* a_standIn = nullptr, RE::ObjectRefHandle a_wearer = {})
		{
			if (const auto frame = a_player.frame.get()) {
				// Someone here may have got into the friend's frame copy (it stands drawn and solid where they got out):
				// our own player, or anyone else. Then it stays (deleting it would leave them in nothing) and is simply
				// no longer ours to manage. Only when it really is this frame they are in, or our player's last suit: a
				// player in their own armor standing next to a friend's frame copy used to count, and the copy was
				// abandoned under its stand-in. Nothing of ours needs undoing then: a copy can only be got into while it
				// is drawn and solid (a hidden one has no collision to aim at).
				if (OurFrame(frame.get()) || FrameInUse(frame.get(), a_standIn)) {
					REX::INFO("RemotePlayers: someone here is in player {}'s frame copy {:08X}; leaving it", a_player.id, frame->GetFormID());
				} else {
					BuryFrame(frame.get(), a_player.framePlacedAt, a_wearer);
				}
			}
			a_player.frame = {};
			a_player.frameCollisionOff = nullptr;
		}

		// Our copy of a pre-placed frame a friend wears (see ArmorFrames::ChooseOriginal): not drawn and without
		// collision while they wear theirs, so there is one suit where they see one. Only its 3D is touched, never
		// the reference itself: nothing of it goes into a save.
		constexpr auto ORIGINAL_CHECK_INTERVAL = 1s;  // a frame not loaded yet, not ours to hide, or hidden, is looked at again this often

		// How far the reference stands from a spot a friend's state named; none when it is in another place.
		std::optional<float> DistanceFrom(RE::TESObjectREFR* a_ref, const Spot& a_spot)
		{
			const auto cell = a_ref->GetParentCell();
			if (!cell) {
				return std::nullopt;
			}
			const bool here = cell->IsInterior() ? a_spot.cell == cell->GetFormID() :
			                                       a_spot.cell == 0 && cell->worldSpace && a_spot.worldspace == cell->worldSpace->GetFormID();
			if (!here) {
				return std::nullopt;
			}
			return a_ref->data.location.GetDistance(a_spot.at);
		}

		// How far a reference stands from where it stood before it was first moved in our world (see
		// ArmorFrames::START_TOLERANCE). TESObjectREFR::GetStartingPosition gives the position the engine noted then
		// (ExtraStartingPosition) and leaves the one it is given alone when it noted none.
		constexpr REL::ID GET_STARTING_POSITION{ 2201162 };  // TESObjectREFR::GetStartingPosition(NiPoint3&) (+516FC0)

		float FromStart(RE::TESObjectREFR* a_ref)
		{
			static REL::Relocation<void (*)(RE::TESObjectREFR*, RE::NiPoint3&)> startingPosition{ GET_STARTING_POSITION };
			RE::NiPoint3 start = a_ref->data.location;
			startingPosition(a_ref, start);
			return a_ref->data.location.GetDistance(start);
		}

		ArmorFrames::OriginalFacts OriginalFactsOf(const RemotePlayer& a_player, RE::TESObjectREFR* a_ref)
		{
			ArmorFrames::OriginalFacts facts;
			facts.found = a_ref && !a_ref->IsDeleted() && !a_ref->IsDisabled();
			if (!facts.found) {
				return facts;
			}
			const auto base = a_ref->GetObjectReference();
			facts.created = a_ref->IsCreated();
			facts.base = base ? base->GetFormID() : 0;
			facts.frame = Equipment::IsPowerArmorFrame(base);
			// A quest of ours holds it: the museum's suit fills the main quest's power armor alias (the objective's
			// target) until that step is done. The same rule as for items (WorldSync): every player keeps their own.
			facts.quest = a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			facts.loaded = a_ref->Get3D() != nullptr;
			facts.ours = OurFrame(a_ref);
			facts.inUse = !facts.ours && FrameInUse(a_ref);
			facts.moved = a_player.originalMoved;
			facts.fromStart = FromStart(a_ref);
			if (a_player.wornAt) {
				facts.fromSpot = DistanceFrom(a_ref, *a_player.wornAt);
			}
			return facts;
		}

		// Another friend wears their own copy of the same pre-placed frame and has ours hidden as well: it stays hidden
		// until the last of them is done with it.
		bool HiddenByAnother(const RemotePlayer& a_player, const RE::TESObjectREFR* a_original)
		{
			return std::ranges::any_of(players, [&](const auto& a_entry) {
				return &a_entry.second != &a_player && a_entry.second.original.get().get() == a_original;
			});
		}

		// Ours drawn again at once, and solid again once nobody stands in it.
		void ShowOriginal(RemotePlayer& a_player)
		{
			if (const auto original = a_player.original.get()) {
				if (HiddenByAnother(a_player, original.get())) {
					REX::INFO("RemotePlayers: our frame {:08X} stays hidden after player {}: another friend wears theirs", original->GetFormID(), a_player.id);
				} else {
					if (const auto root = original->Get3D(); root && root == a_player.originalHidden) {
						root->SetAppCulled(false);
					}
					RestoreCollision(a_player.originalCollisionOff, original.get());
					REX::INFO("RemotePlayers: our frame {:08X}, hidden while player {} wore theirs, is back", original->GetFormID(), a_player.id);
				}
			}
			a_player.original = {};
			a_player.originalHidden = nullptr;
			a_player.originalCollisionOff = nullptr;
		}

		void UpdateOriginal(RemotePlayer& a_player, Clock::time_point a_now)
		{
			const bool worn = a_player.powerArmorOn && a_player.powerArmorFrameRef != 0;
			auto       original = a_player.original.get();
			if (original && (original->IsDeleted() || original->IsDisabled())) {
				ShowOriginal(a_player);  // gone from our world meanwhile: nothing left to show
				original = nullptr;
			}
			if (original && (!worn || original->GetFormID() != a_player.powerArmorFrameRef)) {
				// They are out of it. Ours is shown again once the frame copy their stand-in leaves is gone or clear of it
				// (it stands where they got out, often right on ours, for a few seconds: two suits in one spot), and is
				// solid again once nobody stands in it. Not while another friend still has it hidden.
				if (!HiddenByAnother(a_player, original.get()) && !Clear(original.get(), a_player.frame.get().get())) {
					return;
				}
				ShowOriginal(a_player);
				original = nullptr;
			}
			if (!worn) {
				a_player.originalChoice = -1;
				return;
			}
			if (a_player.wornAtWanted) {
				return;  // where they got in comes with their next state
			}
			// Looked at now and then, hidden or not: a hidden one that has since moved, or that someone here got into, is
			// no longer the suit they wear where they got into it.
			if (a_now >= a_player.originalCheckAt) {
				a_player.originalCheckAt = a_now + ORIGINAL_CHECK_INTERVAL;
				const auto ref = original ? original.get() : RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_player.powerArmorFrameRef);
				const auto facts = OriginalFactsOf(a_player, ref);
				const auto choice = ArmorFrames::ChooseOriginal(a_player.powerArmorFrameRef, a_player.powerArmorFrame, facts);
				if (static_cast<int>(choice) != a_player.originalChoice) {
					a_player.originalChoice = static_cast<int>(choice);
					REX::INFO("RemotePlayers: player {} wears frame {:08X} (base {:08X}); ours, {:.0f} units from where they got in and {:.0f} from where it first stood: {}", a_player.id, a_player.powerArmorFrameRef, a_player.powerArmorFrame,
						facts.fromSpot.value_or(-1.0f), facts.fromStart,
						choice == ArmorFrames::Original::kHide      ? "hidden while they wear theirs" :
						choice == ArmorFrames::Original::kMissing   ? "none here" :
						choice == ArmorFrames::Original::kNotLoaded ? "not loaded" :
						choice == ArmorFrames::Original::kOurs      ? "our player's own, left alone" :
						choice == ArmorFrames::Original::kInUse     ? "in use here, left alone" :
						choice == ArmorFrames::Original::kNotFrame  ? "not a power armor frame, left alone" :
						choice == ArmorFrames::Original::kQuest     ? "a quest of ours holds it, left alone" :
						choice == ArmorFrames::Original::kElsewhere ? "not where they got in, left alone while they wear theirs" :
						choice == ArmorFrames::Original::kUsed      ? "moved in our world since it was placed (our player's own now), left alone" :
						                                              "-");
				}
				if (choice == ArmorFrames::Original::kElsewhere) {
					a_player.originalMoved = true;  // not hidden later in this wear, even should it come back to the spot
				}
				if (original && choice != ArmorFrames::Original::kHide && choice != ArmorFrames::Original::kNotLoaded) {
					a_player.originalMoved = true;
					ShowOriginal(a_player);
					return;
				}
				if (!original) {
					if (choice != ArmorFrames::Original::kHide) {
						return;
					}
					a_player.original = ref->GetHandle();
					a_player.originalHidden = nullptr;
					// Shown a moment ago for another friend and still waiting to be solid: its collision is still off.
					a_player.originalCollisionOff = TakePendingSolid(ref);
					original = RE::NiPointer<RE::TESObjectREFR>(ref);
					if (!a_player.originalCounted) {
						a_player.originalCounted = true;
						++originalsHidden;
					}
				}
			}
			if (!original) {
				return;
			}
			// Hidden again whenever its 3D comes back (a cell reload brings a new one, drawn and solid).
			const auto root = original->Get3D();
			if (root && root != a_player.originalHidden) {
				root->SetAppCulled(true);
			}
			a_player.originalHidden = root;
			SetCollision(a_player.originalCollisionOff, original.get(), false);
		}

		// Power armor: a friend in a frame is shown in it (see ArmorFrames). A copy of their frame is placed at the stand-in
		// and held under it until both have their 3D, and the stand-in is switched into it; from then the engine has the copy
		// (disabled, kept for the stand-in) until they get out, and their pieces go on (Outfit) once the rebuild of its 3D
		// the switch-in queued has given it the power armor skeleton. Out of it, the stand-in is replaced and a copy is left
		// where they got out (LeaveArmor).
		// A copy further than ArmorFrames::MOVE_LIMIT from its stand-in is placed again rather than moved onto it.
		// A frame copy with collision a few units off shoved the stand-in aside (in a joiner's game it flipped between two
		// spots 18 units apart while the host stood still). The copy is kept right under the stand-in, without collision.
		constexpr float FRAME_FOLLOW_DISTANCE = 1.0f;
		constexpr float FRAME_FOLLOW_TURN = 0.02f;  // radians
		std::uint32_t   switchesIn = 0;    // stand-ins the engine took into their frame copy
		std::uint32_t   switchesOut = 0;   // stand-ins replaced as their friend got out
		std::uint32_t   switchFails = 0;   // switch-ins that did not take

		// The frame copy until the stand-in is in it: not drawn (a drawn one trailed it as a second, empty suit), without
		// collision (moved onto the stand-in each time the friend walks or turns, it shoved the stand-in aside while we put
		// it back every frame) and kept right under the stand-in, in whatever cell the copy was placed: the stand-in walks on
		// across cell borders, and a copy left drawn and solid at a border stood there as an empty suit.
		void HoldWornFrame(RemotePlayer& a_player, RE::TESObjectREFR* a_frame, const RE::NiPoint3& a_at, float a_heading)
		{
			if (!a_frame->Get3D()) {
				return;
			}
			SetShown(a_frame, false);
			if (SetCollision(a_player.frameCollisionOff, a_frame, false)) {
				REX::INFO("RemotePlayers: player {}'s frame copy has no collision while it is worn", a_player.id);
			}
			// Headings are compared the short way round: one blended across north can be a full turn off the
			// frame's while facing the same way.
			if (a_frame->data.location.GetDistance(a_at) > FRAME_FOLLOW_DISTANCE || StandInHold::AngleBetween(a_frame->data.angle.z, a_heading) > FRAME_FOLLOW_TURN) {
				a_frame->data.location.x = a_at.x;
				a_frame->data.location.y = a_at.y;
				a_frame->data.location.z = a_at.z;
				a_frame->data.angle = { 0.0f, 0.0f, a_heading };
				a_frame->Update3DPosition(true);
				++framesFollowed;
			}
		}

		// The frame they are in, when they are in power armor and it is a frame here (a terminal is furniture too, but never
		// a frame).
		RE::TESFurniture* WornFrame(const RemotePlayer& a_player)
		{
			const auto base = a_player.powerArmorOn ? RE::TESForm::GetFormByID<RE::TESFurniture>(a_player.powerArmorFrame) : nullptr;
			return Equipment::IsPowerArmorFrame(base) ? base : nullptr;
		}

		// Places a copy of a frame: for the stand-in to get into, or where they got out.
		bool PlaceFrame(RemotePlayer& a_player, RE::TESBoundObject* a_base, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now)
		{
			if (!a_base || !a_cell) {
				return false;
			}
			RE::NEW_REFR_DATA data;
			data.location = a_at;
			data.direction = { 0.0f, 0.0f, a_heading };
			data.object = a_base;
			data.interior = a_cell->IsInterior() ? a_cell : nullptr;
			data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
			data.clearStillLoadingFlag = true;
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto frame = handle.get();
			if (!frame) {
				return false;
			}
			// Empty. A frame never looked into is filled from its base's container the first time its inventory is read
			// (TESObjectREFR::RemoveAllItems does so itself, then clears it; the engine's switch-in does so too as it moves
			// the frame's items onto the actor), and a pre-placed loot frame's base (the museum's T-45, 0019DED9) carries a
			// whole set of pieces: the copy left where a friend got out rolled a fresh set for whoever here stepped into it,
			// a suit nobody in the session ever had, and the copy the stand-in gets into would have had those pieces moved
			// onto it. The friend's own pieces cannot go into a copy; it stands empty.
			RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", frame->GetFormID()).c_str());
			a_player.frame = handle;
			a_player.framePlacedAt = a_now;
			a_player.frameCollisionOff = nullptr;  // a new copy has its own collision
			++framesPlaced;
			SaveGuard::Track(frame.get());
			return true;
		}

		// Into its copy (see ArmorFrames::ChooseSwitch and Equipment::SwitchIntoPowerArmor).
		void SwitchIn(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectREFR* a_frame, Clock::time_point a_now)
		{
			a_player.switchTriedAt = a_now;
			if (Equipment::SwitchIntoPowerArmor(a_actor, a_frame)) {
				// The engine disables it (its 3D, and the collision we turned off with it, go): never drawn or made solid by
				// us again, it goes with the stand-in.
				a_player.frameCollisionOff = nullptr;
				// Their pieces go on once the rebuild of its 3D the switch-in queued has run (see OnArmorSkeleton and Outfit):
				// the 3D it has now is still its own body, which the queued task takes away.
				a_player.switchedAt = a_now;
				a_player.armorSkeleton = nullptr;
				++switchesIn;
				REX::INFO("RemotePlayers: player {} stand-in {:08X} is in its frame copy {:08X}; its power armor skeleton is on its way", a_player.id, a_actor->GetFormID(), a_frame->GetFormID());
				return;
			}
			++a_player.switchTries;
			++switchFails;
			REX::WARN("RemotePlayers: player {} stand-in {:08X} did not get into its frame copy {:08X} (try {} of {}; in power armor={})", a_player.id, a_actor->GetFormID(), a_frame->GetFormID(), a_player.switchTries,
				ArmorFrames::MAX_SWITCH_TRIES, Equipment::InPowerArmor(a_actor));
		}

		// Whether the stand-in's 3D is built on the power armor skeleton (Equipment::HasPowerArmorSkeleton): the rebuild
		// the switch-in queued has run. Looked for once per 3D, and logged as it comes.
		bool OnArmorSkeleton(RemotePlayer& a_player, RE::Actor* a_actor, Clock::time_point a_now)
		{
			const auto root = a_actor->Get3D();
			if (!root) {
				return false;
			}
			if (root == a_player.armorSkeleton) {
				return true;
			}
			if (!Equipment::HasPowerArmorSkeleton(a_actor)) {
				return false;
			}
			a_player.armorSkeleton = root;
			REX::INFO("RemotePlayers: player {} stand-in {:08X} has its power armor skeleton, {} ms after the switch-in; their pieces go on", a_player.id, a_actor->GetFormID(),
				std::chrono::duration_cast<std::chrono::milliseconds>(a_now - a_player.switchedAt).count());
			return true;
		}

		// What the switch-in and the dress go by, before the copy is looked at (see UpdatePowerArmor for the rest).
		ArmorFrames::SwitchFacts SwitchFactsOf(RemotePlayer& a_player, RE::Actor* a_actor, Clock::time_point a_now)
		{
			const bool in = Equipment::InPowerArmor(a_actor);
			return {
				.worn = WornFrame(a_player) != nullptr,
				.in = in,
				.rebuilt = in && OnArmorSkeleton(a_player, a_actor, a_now),
				.sinceIn = a_now - a_player.switchedAt,
				.tries = a_player.switchTries,
				.sinceTried = a_now - a_player.switchTriedAt,
			};
		}

		void UpdatePowerArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now)
		{
			if (!a_actor) {
				return;
			}
			const auto base = WornFrame(a_player);
			const bool worn = base != nullptr;
			if (worn != a_player.inPowerArmor) {
				a_player.inPowerArmor = worn;
				if (!worn) {
					a_player.frameActionAt = a_now;  // the copy stands where they got out a while
				}
				REX::INFO("RemotePlayers: player {} is {} power armor (frame {:08X})", a_player.id, worn ? "in" : "out of", a_player.powerArmorFrame);
			}

			const bool has3D = a_actor->Get3D() != nullptr;
			const bool in = Equipment::InPowerArmor(a_actor);
			auto       frame = a_player.frame.get();
			ArmorFrames::CopyFacts facts;
			facts.worn = worn;
			facts.exists = frame != nullptr;
			if (frame) {
				facts.engaged = in && Equipment::PowerArmorFrameOf(a_actor).get() == frame.get();
				facts.occupied = OurFrame(frame.get()) || FrameInUse(frame.get(), a_actor);
				facts.loaded = frame->Get3D() != nullptr;
				facts.samePlace = SamePlace(frame.get(), a_actor);
				facts.distance = frame->data.location.GetDistance(a_actor->data.location);
				facts.sincePlaced = a_now - a_player.framePlacedAt;
				facts.sinceOut = a_now - a_player.frameActionAt;
			}
			const auto copy = ArmorFrames::ChooseCopy(facts);
			switch (copy) {
			case ArmorFrames::Copy::kNone:
				break;
			case ArmorFrames::Copy::kPlace:
				// Once the stand-in is drawn: one whose 3D never comes needs none.
				if (has3D) {
					PlaceFrame(a_player, base, a_cell, a_actor->data.location, a_actor->data.angle.z, a_now);
				}
				break;
			case ArmorFrames::Copy::kHold:
				HoldWornFrame(a_player, frame.get(), a_at, a_heading);
				break;
			case ArmorFrames::Copy::kReplace:
				{
					const auto frameCell = frame->GetParentCell();
					const auto actorCell = a_actor->GetParentCell();
					REX::INFO("RemotePlayers: player {}'s frame copy (cell {:08X}, 3d={}) is {:.0f} units from the stand-in in {:08X}, placed {} ms ago: placing it again", a_player.id, frameCell ? frameCell->GetFormID() : 0, facts.loaded, facts.distance,
						actorCell ? actorCell->GetFormID() : 0, std::chrono::duration_cast<std::chrono::milliseconds>(facts.sincePlaced).count());
					DeleteFrame(a_player, a_actor);
				}
				break;
			case ArmorFrames::Copy::kShow:
				// Where they got out: drawn, and solid only while nobody stands in it (the stand-in that took their place
				// stands right there at first).
				SetShown(frame.get(), true);
				SetCollision(a_player.frameCollisionOff, frame.get(), NobodyIn(frame.get()));
				break;
			case ArmorFrames::Copy::kDelete:
				DeleteFrame(a_player, a_actor);
				break;
			case ArmorFrames::Copy::kAbandon:
				// Someone here got into it while it stood where they got out: theirs now (see DeleteFrame).
				REX::INFO("RemotePlayers: someone here got into player {}'s frame copy {:08X}; it is theirs now", a_player.id, frame->GetFormID());
				a_player.frame = {};
				a_player.frameCollisionOff = nullptr;
				break;
			}

			const auto process = a_actor->currentProcess;
			auto       switchFacts = SwitchFactsOf(a_player, a_actor, a_now);
			switchFacts.ready = has3D && process && process->processLevel <= 1 && !a_actor->IsDead(false);
			switchFacts.held = copy == ArmorFrames::Copy::kHold;
			switch (ArmorFrames::ChooseSwitch(switchFacts)) {
			case ArmorFrames::Switch::kIn:
				SwitchIn(a_player, a_actor, frame.get(), a_now);
				break;
			case ArmorFrames::Switch::kGaveUp:
				if (!a_player.switchGaveUp) {
					a_player.switchGaveUp = true;
					REX::WARN("RemotePlayers: player {} stand-in {:08X} never got into its frame copy; shown out of power armor, the copy hidden under it", a_player.id, a_actor->GetFormID());
				}
				break;
			case ArmorFrames::Switch::kNone:
			case ArmorFrames::Switch::kOut:    // replaced before this (see LeaveArmor)
			case ArmorFrames::Switch::kStuck:  // replaced before this too (see Update)
				break;
			}
		}

		// While the friend stands still, their stand-in should be found each frame where it was put the frame
		// before; when it isn't, something in the engine moves it. Counted for net status, and logged now and
		// then with what may be holding it, such as its power armor frame copy.
		constexpr auto HOLD_LOG_INTERVAL = 10s;
		void CheckHold(RemotePlayer& a_player, RE::Actor* a_actor, const RE::NiPoint3& a_target, float a_speed, Clock::time_point a_now)
		{
			const auto last = a_player.lastTarget;
			a_player.lastTarget = a_target;
			if (!last || !a_actor->Get3D()) {
				return;
			}
			const auto& at = a_actor->data.location;
			const float moved = std::hypot(a_target.x - last->x, a_target.y - last->y);
			const float off = std::hypot(at.x - last->x, at.y - last->y);
			if (!a_player.hold.Add(moved, a_speed, off) || a_now - a_player.holdLoggedAt < HOLD_LOG_INTERVAL) {
				return;
			}
			a_player.holdLoggedAt = a_now;
			const auto frame = a_player.frame.get();
			REX::INFO("RemotePlayers: player {} stand-in stood {:.1f} units from where it was put ({:.0f},{:.0f}), at ({:.0f},{:.0f}); in power armor={}, frame copy {:.1f} units away; {} of {} still frames so", a_player.id, off, last->x, last->y, at.x, at.y,
				a_player.inPowerArmor, frame ? frame->data.location.GetDistance(at) : -1.0f, a_player.hold.off, a_player.hold.frames);
		}

		// A stand-in in its frame is about to go (never to get out of it): that frame goes with it, let go of by the engine
		// first (BuryFrame). It is the copy we hold for it (DeleteFrame buries that, stand-in or no stand-in), unless
		// something else happened; any other one made in this game is buried here.
		void ReleaseArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::ObjectRefHandle a_wearer = {})
		{
			const auto frame = a_actor ? Equipment::PowerArmorFrameOf(a_actor) : RE::NiPointer<RE::TESObjectREFR>{};
			if (frame && frame.get() != a_player.frame.get().get() && frame->IsCreated()) {
				BuryFrame(frame.get(), a_player.framePlacedAt, a_wearer);
			}
		}

		void DeleteActor(RemotePlayer& a_player)
		{
			DeleteLight(a_player);
			const auto actor = GetActor(a_player);
			// A stand-in kept for the player's conversation (see HeldForConversation) keeps the frame it is in: the frame
			// waits for it instead of being disabled from under it (it then went on held in a frame that was gone).
			const auto heldIn = actor && Dialogue::Holds(actor) ? Equipment::PowerArmorFrameOf(actor) : RE::NiPointer<RE::TESObjectREFR>{};
			const auto wearer = heldIn ? a_player.actor : RE::ObjectRefHandle{};
			ReleaseArmor(a_player, actor, wearer);
			DeleteFrame(a_player, actor, heldIn && heldIn.get() == a_player.frame.get().get() ? wearer : RE::ObjectRefHandle{});
			if (actor) {
				WeaponFire::ReleaseSoundsOf(actor->GetFormID());  // a replayed burst may still follow its 3D
				// It stays a puppet until it is disabled (see Grave::puppet), standing still where it is: no friend
				// moves it any more.
				Puppets::AllowAI(actor, 0ms);
				Puppets::SetTarget(actor, { .position = actor->data.location, .heading = actor->data.angle.z });
				Bury({ .actor = a_player.actor, .spawnedAt = a_player.range.spawnedAt, .puppet = a_player.registered, .puppetId = a_player.registeredId });
			} else if (a_player.registered) {
				Puppets::Unregister(a_player.registered, a_player.registeredId);  // gone already
			}
			a_player.registered = nullptr;
			a_player.registeredId = 0;
			a_player.actor = {};
		}

		// They are out of their armor and the stand-in is still in its frame (ArmorFrames::Switch::kOut): the stand-in goes
		// with that frame (see ArmorFrames for why it is not switched out), a fresh one is placed out of armor at once, and a
		// copy of the frame is left where the old one stood, drawn a while (ArmorFrames::ChooseCopy).
		void LeaveArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now)
		{
			const auto  frame = Equipment::PowerArmorFrameOf(a_actor);
			const auto  base = frame ? frame->GetObjectReference() : nullptr;
			const auto  at = a_actor->data.location;
			const float heading = a_actor->data.angle.z;
			REX::INFO("RemotePlayers: player {} got out of their power armor; stand-in {:08X} goes with its frame {:08X}, a copy is left where it stood", a_player.id, a_actor->GetFormID(), frame ? frame->GetFormID() : 0);
			DeleteActor(a_player);
			++switchesOut;
			if (Equipment::IsPowerArmorFrame(base) && PlaceFrame(a_player, base, a_cell, at, heading, a_now)) {
				a_player.frameActionAt = a_now;
			}
		}

		bool SameSpace(const Protocol::PlayerState& a_state, const RE::TESObjectCELL* a_localCell)
		{
			if (a_localCell->IsInterior()) {
				return a_state.cell == a_localCell->GetFormID();
			}
			return a_state.cell == 0 && a_localCell->worldSpace && a_state.worldspace == a_localCell->worldSpace->GetFormID();
		}

		// The game's uGridsToLoad (see StandInRange), read once the game has read its ini.
		int GridsToLoad()
		{
			static std::optional<int> grids;
			if (!grids) {
				const auto setting = RE::GetINISetting("uGridsToLoad:General");
				const auto type = setting ? setting->GetType() : RE::Setting::SETTING_TYPE::kNone;
				grids = type == RE::Setting::SETTING_TYPE::kUInt ? static_cast<int>(setting->GetUInt()) :
				        type == RE::Setting::SETTING_TYPE::kInt  ? setting->GetInt() :
				                                                   StandInRange::DEFAULT_GRIDS;
				const bool read = type == RE::Setting::SETTING_TYPE::kUInt || type == RE::Setting::SETTING_TYPE::kInt;
				REX::INFO("RemotePlayers: uGridsToLoad {} ({}): friends outside are drawn up to {} cells from ours", *grids, read ? "from the ini" : "default", StandInRange::LoadedRadius(*grids));
			}
			return *grids;
		}

		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kNotLoaded) == std::to_underlying(StandInRange::CellState::kNotLoaded));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kUnloading) == std::to_underlying(StandInRange::CellState::kUnloading));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kLoadingData) == std::to_underlying(StandInRange::CellState::kLoadingData));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kLoading) == std::to_underlying(StandInRange::CellState::kLoading));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kLoaded) == std::to_underlying(StandInRange::CellState::kLoaded));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kDetaching) == std::to_underlying(StandInRange::CellState::kDetaching));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kAttachQueued) == std::to_underlying(StandInRange::CellState::kAttachQueued));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kAttaching) == std::to_underlying(StandInRange::CellState::kAttaching));
		static_assert(std::to_underlying(RE::TESObjectCELL::CELL_STATE::kAttached) == std::to_underlying(StandInRange::CellState::kAttached));

		// A friend's stand-in as StandInRange looks at it: its cell, as the engine has it, and whether its 3D is in.
		std::optional<StandInRange::StandIn> StandInOf(RE::Actor* a_actor, const RE::TESObjectCELL* a_localCell)
		{
			if (!a_actor) {
				return std::nullopt;
			}
			StandInRange::StandIn standIn;
			const auto cell = a_actor->GetParentCell();
			standIn.inOurCell = cell == a_localCell;
			standIn.inCell = cell != nullptr;
			if (cell) {
				standIn.interior = cell->IsInterior();
				standIn.state = static_cast<StandInRange::CellState>(std::to_underlying(cell->cellState));
				if (const auto coords = cell->IsExterior() ? cell->GetCoordinates() : nullptr) {
					standIn.x = coords->cellX;
					standIn.y = coords->cellY;
				}
			}
			standIn.has3D = a_actor->Get3D() != nullptr;
			return standIn;
		}

		// The default stand-in bases: settlers with a fixed look and no factions, one per sex.
		using RemoteEdits::FEMALE_SETTLER;
		using RemoteEdits::MALE_SETTLER;

		RE::Actor* Spawn(RemotePlayer& a_player, const Protocol::PlayerState& a_state, RE::TESObjectCELL* a_localCell)
		{
			// The look the player picked, when it is one we place (a settler, or our own default): any other NPC
			// would bring its scripts and factions into our world on their word, so they are shown as our default.
			const auto look = RemoteEdits::LookToPlace(a_player.appearance, Config::Get().puppetBaseForm);
			if (a_player.appearance && !look && !a_player.appearanceWarned) {
				a_player.appearanceWarned = true;
				REX::WARN("RemotePlayers: player {} asked to look like NPC {:08X}, which is not a settler or our default; using the default", a_player.id, a_player.appearance);
			}
			auto npc = look ? RE::TESForm::GetFormByID<RE::TESNPC>(look) : nullptr;
			if (!npc) {
				auto baseID = Config::Get().puppetBaseForm;
				// With their face known, the base is a settler of their sex (unless the ini picked another base).
				if (a_player.face && baseID == FEMALE_SETTLER && !a_player.face->female) {
					baseID = MALE_SETTLER;
				}
				npc = RE::TESForm::GetFormByID<RE::TESNPC>(baseID);
				if (!npc) {
					REX::ERROR("RemotePlayers: puppet base form {:08X} is not an NPC", baseID);
					return nullptr;
				}
			}
			// Their own face, hair and body shape on a copy of that base (made once per look).
			if (a_player.face && !a_player.faceTried) {
				a_player.faceTried = true;
				const auto made = Face::MakeBase(npc, *a_player.face);
				a_player.faceBaseId = made ? made->GetFormID() : 0;
			}
			if (a_player.faceBaseId) {
				if (const auto faceBase = RE::TESForm::GetFormByID<RE::TESNPC>(a_player.faceBaseId)) {
					npc = faceBase;
				} else {
					a_player.faceBaseId = 0;  // gone with a load: made again next time
					a_player.faceTried = false;
				}
			}

			RE::NEW_REFR_DATA data;
			data.location = { a_state.x, a_state.y, a_state.z };
			data.direction = { 0.0f, 0.0f, a_state.heading };
			data.object = npc;
			data.interior = a_localCell->IsInterior() ? a_localCell : nullptr;
			data.world = a_localCell->IsInterior() ? nullptr : a_localCell->worldSpace;
			data.clearStillLoadingFlag = true;

			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor) {
				REX::ERROR("RemotePlayers: failed to spawn puppet for player {}", a_player.id);
				return nullptr;
			}

			Puppets::Register(actor);
			SaveGuard::Track(actor);
			// Enemies treat the stand-in like the player (it's what NPCs in this world can attack),
			// and hitting it isn't a crime. That faction and rank also tell SaveGuard a stand-in an
			// earlier version saved, so they stay as every version since 0.4.0 set them.
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} {}", actor->GetFormID(), SaveStrays::STAND_IN_FACTION, SaveStrays::STAND_IN_RANK).c_str());

			REX::INFO("RemotePlayers: spawned {:08X} for player {}", actor->GetFormID(), a_player.id);
			return actor;
		}
	}

	void Add(std::uint32_t a_id, std::string a_name, std::uint32_t a_appearance)
	{
		// A reused ID is a new player: start from scratch (old snapshots would hide their states).
		Remove(a_id);
		auto& player = players[a_id];
		if (player.knownAt == Clock::time_point{}) {
			player.knownAt = Clock::now();
		}
		player.id = a_id;
		player.name = std::move(a_name);
		player.appearance = a_appearance;
		if (a_appearance) {
			looks.insert(a_appearance);
		}
	}

	void Remove(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			DeleteActor(it->second);
			ShowOriginal(it->second);
			players.erase(it);
		}
	}

	void RemoveAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
			ShowOriginal(player);
		}
		players.clear();
	}

	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;  // state for a player we haven't been told about yet
		}
		if (!it->second.states.Push(a_state, Clock::now())) {
			return;
		}
		if (it->second.wornAtWanted) {
			TakeWornAt(it->second, a_state);  // they got into a pre-placed frame before we had a recent state of theirs
		}
	}

	void SetFace(std::uint32_t a_id, Protocol::Face a_face)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		auto& player = it->second;
		if (player.face && *player.face == a_face) {
			return;
		}
		player.face = std::move(a_face);
		player.faceBaseId = 0;  // the old copy stays in memory (forms can't be freed); a new one is made
		player.faceTried = false;
		// The stand-in is rebuilt with the new look the next time it's drawn.
		DeleteActor(player);
	}

	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items, std::vector<Protocol::ItemMods> a_mods)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		// What they have on already changes nothing; a change is put on over what the stand-in wears (see Update).
		StandInGear::Report(it->second.gear, std::move(a_items), std::move(a_mods));
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		for (const auto& [id, remote] : players) {
			Info info{ id, remote.name, std::nullopt, GetActor(remote) };
			if (!remote.states.snapshots.empty()) {
				info.state = remote.states.snapshots.back().state;
			}
			out.push_back(std::move(info));
		}
		return out;
	}

	std::string NameOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		return it != players.end() ? it->second.name : std::string{};
	}

	void SetHealth(std::uint32_t a_id, std::uint8_t a_percent, bool a_downed)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.health = a_percent;
			it->second.downed = a_downed;
		}
	}

	void PlayShot(std::uint32_t a_id, std::uint32_t a_sound)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			WeaponFire::PlayShot(GetActor(it->second), a_sound);
		}
	}

	void PlayAction(std::uint32_t a_id, std::uint8_t a_action)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		auto& player = it->second;
		if (a_action == Protocol::ShotAction::kLightOn || a_action == Protocol::ShotAction::kLightOff) {
			player.lightOn = a_action == Protocol::ShotAction::kLightOn;
			return;  // Update() keeps the light with the stand-in
		}
		const auto actor = GetActor(player);
		// The grenade the stand-in shows and throws is the one the owner holds; the last replayed
		// throw spent the one it had.
		if (a_action == Protocol::ShotAction::kThrow && player.gear.has) {
			Equipment::HoldThrowable(actor, player.gear.items);
		}
		WeaponFire::PlayAction(actor, a_action);
	}

	std::uint32_t PlayerIdFor(std::uint32_t a_actorFormId)
	{
		for (const auto& [id, remote] : players) {
			if (remote.registered && remote.registeredId == a_actorFormId) {
				return id;
			}
		}
		return 0;
	}

	bool IsStandInBase(std::uint32_t a_base)
	{
		if (!a_base) {
			return false;
		}
		if (a_base == FEMALE_SETTLER || a_base == MALE_SETTLER || a_base == Config::Get().puppetBaseForm) {
			return true;
		}
		// The ini's iMyAppearance is not one: it is only sent to friends, never placed here.
		if (!looks.contains(a_base)) {
			return false;
		}
		// Earlier versions placed any look that was an NPC here and not a unique one: their stand-ins may be in a
		// save still, and are recognised as ours to clear away (Spawn now places only the settlers or our default).
		const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
		return npc && !npc->IsUnique();
	}

	void DespawnAll()
	{
		// A frame of ours hidden while a friend wears theirs stays hidden: only its 3D was touched, which no save keeps.
		// Shown here, it stood in the world again until the next check put it away, and counted as hidden anew.
		for (auto& [id, player] : players) {
			DeleteActor(player);
		}
		// Marked for deletion earlier and still there: kept out of the save too. (Before the graveyard is
		// flushed, so the ones it adds now are not excluded twice.)
		std::erase_if(deleting, [](const Deleting& a_deleting) {
			const auto ref = a_deleting.ref.get();
			if (!ref) {
				SaveGuard::Forget(a_deleting.id);
				return true;
			}
			SaveGuard::Exclude(ref.get());
			return false;
		});
		// Before a save: whatever is only disabled, or has been waiting for its 3D, is deleted now. (A
		// reference whose 3D is still loading is left alone: deleting it crashes the loader when it
		// finishes.) The save runs between frames, so no physics job holds any of them. None of them may
		// be written into the save, deleted or not: a stand-in saved as deleted comes back from the save in
		// no cell, and every later save crashed on it. A stand-in kept for the player's
		// conversation goes too: should it be the speaker, Dialogue ends the conversation it leaves open.
		std::erase_if(graveyard, [&](Grave& a_grave) {
			const auto ref = a_grave.actor.get();
			if (!ref) {
				SaveGuard::Forget(a_grave.id);
				Release(a_grave);
				return true;
			}
			if (!a_grave.disabledAt && !DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_grave.spawnedAt)) {
				SaveGuard::Exclude(ref.get());
				return false;
			}
			Release(a_grave);
			ref->Disable();
			ref->SetDelete(true);
			SaveGuard::Exclude(ref.get());
			deleting.push_back({ a_grave.actor, a_grave.id });
			++flushedBeforeSave;
			return true;
		});
	}

	std::optional<Protocol::PlayerState> LastStateOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		if (it == players.end() || it->second.states.snapshots.empty()) {
			return std::nullopt;
		}
		return it->second.states.snapshots.back().state;
	}

	std::optional<RE::NiPoint3> PositionOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		if (it == players.end() || it->second.states.snapshots.empty()) {
			return std::nullopt;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return std::nullopt;
		}
		const auto& last = it->second.states.snapshots.back().state;
		const bool here = cell->IsInterior() ? last.cell == cell->GetFormID() : (cell->worldSpace && last.worldspace == cell->worldSpace->GetFormID());
		if (!here) {
			return std::nullopt;
		}
		return RE::NiPoint3{ last.x, last.y, last.z };
	}

	void OnGameLoaded()
	{
		// Handles and runtime forms from before the load name other things now (or nothing): a graveyard
		// entry resolved after the load could delete some unrelated object, a stale one in deleting would
		// keep an unrelated reference out of every save, and a face base made before it may be gone. A
		// stand-in among the graveyard's is no longer a puppet either (forgotten without reading it).
		for (auto& grave : graveyard) {
			Release(grave);
		}
		graveyard.clear();
		deleting.clear();
		pendingSolid.clear();
		for (auto& [id, player] : players) {
			player.faceBaseId = 0;
			player.faceTried = false;
			player.original = {};  // a hidden frame's 3D went with the old world; looked for again at once (not a new hide)
			player.originalHidden = nullptr;
			player.originalCollisionOff = nullptr;
			player.originalChoice = -1;
			player.originalCheckAt = {};
			player.originalMoved = false;  // moved in the old world; looked at afresh in this one
			// The frame copy went before the load (DespawnAll), with its stand-in; a handle from before it names something
			// else now, or nothing.
			player.frame = {};
			player.frameCollisionOff = nullptr;
			player.switchTries = 0;
			player.switchGaveUp = false;
		}
	}

	void SetLight(std::uint32_t a_id, bool a_on)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.lightOn = a_on;  // Update() keeps the light with the stand-in
		}
	}

	std::uint32_t ActorIdOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		const auto actor = it != players.end() ? it->second.actor.get() : nullptr;
		return actor ? actor->GetFormID() : 0;
	}

	void SetPowerArmor(std::uint32_t a_id, bool a_on, std::uint32_t a_frameBase, std::uint32_t a_frameRef)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			auto& player = it->second;
			// A pre-placed frame they have just got into: where they are now is where they got in (their status names the
			// frame as they climb in), and ours is looked at afresh for this wear (see ArmorFrames::ChooseOriginal). Where
			// they are is their newest state if it came just now, else the next one.
			if (a_on && a_frameBase && a_frameRef != 0 && (!player.powerArmorOn || a_frameRef != player.powerArmorFrameRef)) {
				player.wornAt.reset();
				player.wornAtWanted = true;
				player.originalMoved = false;
				player.originalChoice = -1;
				player.originalCounted = false;
				if (!player.states.snapshots.empty() && ArmorFrames::FreshEnough(Clock::now() - player.states.arrived)) {
					TakeWornAt(player, player.states.snapshots.back().state);
				}
			} else if (!a_on) {
				player.wornAtWanted = false;
			}
			if (a_on != player.powerArmorOn) {
				StandInGear::Redress(player.gear);  // dressed again (see Outfit)
				player.switchTries = 0;             // a new wear: the stand-in is tried afresh
				player.switchGaveUp = false;
			}
			// The frame is kept until they tell another (see ArmorFrames::Hear).
			const auto heard = ArmorFrames::Hear({ player.powerArmorOn, player.powerArmorFrame, player.powerArmorFrameRef }, a_on, a_frameBase, a_frameRef);
			player.powerArmorOn = heard.on;
			player.powerArmorFrame = heard.frame;
			player.powerArmorFrameRef = heard.frameRef;
		}
	}

	void SetLightForm(std::uint32_t a_formId)
	{
		friendLightForm = a_formId;
		for (auto& [id, player] : players) {
			DeleteLight(player);  // placed again with the new form
		}
	}

	void RebuildFaces()
	{
		for (auto& [id, player] : players) {
			player.faceBaseId = 0;
			player.faceTried = false;
			DeleteActor(player);
		}
	}

	void Update()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto localCell = player ? player->GetParentCell() : nullptr;
		const auto ui = RE::UI::GetSingleton();
		const bool loading = ui && ui->GetMenuOpen("LoadingMenu"sv);

		const auto now = Clock::now();
		// What waits in the graveyard, or to be let go of by the engine, and has lost its cell has its record dropped
		// each frame: any change the engine makes to it makes a new one, and the engine unloading it would write that
		// out and crash on the cell it no longer has.
		std::erase_if(graveyard, [&](Grave& a_grave) {
			const auto actor = a_grave.actor.get();
			if (!actor) {
				SaveGuard::Forget(a_grave.id);
				Release(a_grave);
				return true;
			}
			if (!actor->GetParentCell()) {
				SaveGuard::ExcludeOurs(actor.get());
			}
			if (!a_grave.disabledAt) {
				if (!DeferredDelete::Safe(actor->Get3D() != nullptr, now - a_grave.spawnedAt) || HeldForConversation(a_grave, actor.get(), now) ||
					WaitsForWearer(a_grave, actor.get())) {
					return false;
				}
				Disable(a_grave, actor.get(), now);
				return false;
			}
			if (now - *a_grave.disabledAt < DELETE_AFTER_DISABLE) {
				return false;
			}
			actor->SetDelete(true);
			SaveGuard::ExcludeOurs(actor.get());  // marking it deleted made a record of the change
			deleting.push_back({ a_grave.actor, a_grave.id });
			return true;
		});
		std::erase_if(deleting, [](const Deleting& a_deleting) {
			const auto ref = a_deleting.ref.get();
			if (!ref) {
				SaveGuard::Forget(a_deleting.id);
				return true;
			}
			if (!ref->GetParentCell()) {
				SaveGuard::ExcludeOurs(ref.get());
			}
			return false;
		});
		SaveGuard::Frame();
		UpdatePendingSolid();
		for (auto& [id, remote] : players) {
			remote.states.Frame(now);
			const auto& snaps = remote.states.snapshots;
			movingFrames += !snaps.empty() && snaps.back().state.speed > 0.0f && now - remote.states.arrived < 1s;
			const auto sampled = remote.states.Sample(now);
			heldFrames += sampled && sampled->held && sampled->state.speed > 0.0f && now - remote.states.arrived < 1s;
			const auto state = sampled ? std::optional{ sampled->state } : std::nullopt;

			// A light or frame copy the cells were cleared under (a door, a coc, a fast travel) goes, its record first: the
			// engine unloading it would write the record out and crash on the cell it no longer has. A frame copy the
			// stand-in is in is the engine's: only its record goes (it goes with the stand-in).
			if (const auto light = remote.light.get(); Adrift(light.get(), remote.lightTrack.placedAt, now)) {
				++adrift;
				DeleteLight(remote);
			}
			if (const auto frame = remote.frame.get(); Adrift(frame.get(), remote.framePlacedAt, now)) {
				const auto standIn = GetActor(remote);
				if (!standIn || Equipment::PowerArmorFrameOf(standIn).get() != frame.get()) {
					++adrift;
					DeleteFrame(remote, standIn);
				}
			}

			// In power armor with no frame here to place a copy of, for a second (the first status of a wear names none):
			// the stand-in wears their pieces as they are, and nothing else is logged of it (no "is in power armor").
			if (remote.frameUnknown.Update(remote.powerArmorOn, WornFrame(remote) != nullptr, now)) {
				REX::WARN("RemotePlayers: player {} is in power armor, but the frame they told ({:08X}) is {}; their stand-in wears the pieces with no frame copy", remote.id, remote.powerArmorFrame,
					remote.powerArmorFrame ? "no frame in this game" : "none");
			}

			// Before the stand-in is looked at: a friend out of sight can still be wearing our copy of a frame.
			UpdateOriginal(remote, now);

			auto actor = GetActor(remote);
			if (!actor && remote.registered) {
				// The game removed our puppet (e.g. cell unload); forget the stale registration. Its form may still be
				// there: it is kept out of saves while it is.
				SaveGuard::Forget(remote.registeredId);
				DeleteActor(remote);
			}
			if (actor && IsBroken(actor)) {
				if (DownRules::IsDownState(LifeState(actor))) {  // the game says dead too while down: only the life state tells
					++downedStandIns;
					REX::INFO("RemotePlayers: player {} stand-in {:08X} was put down here while its friend is {}; placed again", remote.id, actor->GetFormID(), remote.downed ? "down too" : "up");
				}
				DeleteActor(remote);
				actor = nullptr;
			}
			// Whether they are drawn here, and whether a stand-in left in a cell that is not ours (we went through a door, or
			// the engine stopped drawing its cell) stays: always replaced when we are inside, and outside when its cell is an
			// interior or the engine no longer draws it (a neighbouring exterior cell is fine, and so is one still attaching).
			// A stand-in used to stay when we stepped from an interior into the open: stuck in the unloaded interior, never
			// moving, with its power armor frame placed at that stale spot.
			StandInRange::Sight sight;
			sight.sameSpace = state && localCell && !loading && SameSpace(*state, localCell);
			if (sight.sameSpace) {
				sight.weInside = localCell->IsInterior();
				sight.x = state->x;
				sight.y = state->y;
				sight.ourX = player->data.location.x;
				sight.ourY = player->data.location.y;
				sight.gridsToLoad = GridsToLoad();
				sight.standIn = StandInOf(actor, localCell);
			}
			const auto look = StandInRange::Frame(remote.range, sight, now);
			if (!look.visible) {
				if (actor) {
					DeleteActor(remote);
				}
				continue;
			}

			if (actor && look.left != StandInRange::Left::kKeep) {
				++leftBehind;
				respawnWaits += look.left == StandInRange::Left::kReplaceLater;
				DeleteActor(remote);
				actor = nullptr;
			}

			// Out of their armor while the stand-in is still in its frame: it goes, and a fresh one is placed just below. So
			// does one the engine has in its frame but never gave the power armor skeleton (the rebuild the switch-in queued
			// did not come): the fresh one is tried afresh, and the tries of this wear add up (ArmorFrames::MAX_SWITCH_TRIES).
			if (actor) {
				switch (ArmorFrames::ChooseSwitch(SwitchFactsOf(remote, actor, now))) {
				case ArmorFrames::Switch::kOut:
					LeaveArmor(remote, actor, localCell, now);
					actor = nullptr;
					break;
				case ArmorFrames::Switch::kStuck:
					++remote.switchTries;
					++switchFails;
					REX::WARN("RemotePlayers: player {} stand-in {:08X} is still on its own skeleton {} ms after it was switched into its frame copy; placed again (try {} of {})", remote.id, actor->GetFormID(),
						std::chrono::duration_cast<std::chrono::milliseconds>(now - remote.switchedAt).count(), remote.switchTries, ArmorFrames::MAX_SWITCH_TRIES);
					DeleteActor(remote);
					actor = nullptr;
					break;
				default:
					break;
				}
			}

			if (!actor) {
				if (!remote.face && now - remote.knownAt < FACE_GRACE) {
					continue;  // their face is on its way
				}
				if (!look.mayPlace) {
					continue;  // the last one was never drawn where it stood
				}
				actor = Spawn(remote, *state, localCell);
				if (!actor) {
					continue;
				}
				++spawns;
				StandInRange::Placed(remote.range, now);
				remote.actor = actor->GetHandle();
				remote.registered = actor;
				remote.registeredId = actor->GetFormID();
				StandInGear::Placed(remote.gear);
				remote.shownName.clear();
				remote.lastTarget.reset();
				// Still in their armor (it was replaced through a door, after a load, with a new look, or for a skeleton that
				// never came): this one goes into a copy as soon as both are loaded, at once. The tries that did not take in
				// this wear are kept: placed again and again for a rebuild that never comes, it would never give up.
				remote.switchTriedAt = {};
				remote.switchedAt = {};
				remote.armorSkeleton = nullptr;
				remote.switchGaveUp = false;
			}

			UpdateLight(remote, actor, localCell, now);
			UpdatePowerArmor(remote, actor, localCell, { state->x, state->y, state->z }, state->heading, now);
			CheckHold(remote, actor, { state->x, state->y, state->z }, state->speed, now);

			// Shown when looking at them, instead of the NPC's name, with their health when hurt.
			if (auto label = NameTag(remote); label != remote.shownName && actor->extraList) {
				actor->extraList->SetOverrideName(label.c_str());
				remote.shownName = std::move(label);
			}

			// Dressed as the engine has it (ArmorFrames::ChooseDress; StandInGear::Dress says what goes on). Apply empties the
			// inventory first, which in its frame would take the frame the engine put on it, so in a frame only the pieces and
			// weapons are put on; what it wears besides them is not looked at there. In its frame but still on its own body
			// (the rebuild the switch-in queued has not run), nothing is put on or looked at: the pieces would hang off the
			// wrong skeleton. A change they reported while it is dressed is put on over what it wears (WearOnStandIn takes off what
			// they no longer have on and leaves the rest), at most once a StandInGear::CHANGE_GAP; its gun is put away and
			// drawn again only when it was taken off it or swapped: over a hat or their last grenade, their shots went silent.
			const bool inArmor = Equipment::InPowerArmor(actor);
			const auto dress = ArmorFrames::ChooseDress(inArmor, inArmor && OnArmorSkeleton(remote, actor, now));
			const auto shown = [&] { return dress == ArmorFrames::Dress::kArmor ? Kept(Equipment::Read(actor), dress) : Equipment::Read(actor); };
			const auto dressing = StandInGear::Dress(remote.gear, dress, actor->Get3D() != nullptr, now, HandWeapon,
				[&](const std::vector<std::uint32_t>& a_worn) { return Wearable(shown()) == Wearable(Kept(a_worn, dress)); });
			switch (dressing.put) {
			case StandInGear::Put::kApply:
				Equipment::Apply(actor, Outfit(remote, dress), remote.gear.mods);
				break;
			case StandInGear::Put::kWear:
				Equipment::WearOnStandIn(actor, Outfit(remote, dress), remote.gear.mods);
				break;
			case StandInGear::Put::kWearInArmor:
				Equipment::WearInArmor(actor, Outfit(remote, dress), remote.gear.mods);
				break;
			case StandInGear::Put::kNone:
				break;
			}
			gearChanges += dressing.change;
			if (dressing.redraw) {
				Puppets::RedrawWeapon(actor);
				++redraws;
			}
			if (dressing.refit) {
				++equipmentFixesTotal;
				std::string worn, wanted;
				for (const auto item : Wearable(shown())) worn += std::format("{:08X} ", item);
				for (const auto item : Wearable(Kept(remote.gear.worn, dress))) wanted += std::format("{:08X} ", item);
				REX::INFO("RemotePlayers: player {} stand-in isn't wearing their gear; dressing it again (wears {} wants {})", remote.id, worn, wanted);
			}

			Puppets::SetTarget(actor, Puppets::Motion{
				.position = { state->x, state->y, state->z },
				.heading = state->heading,
				.speed = state->speed,
				.direction = Protocol::UnpackDirection(state->direction),
				.pitch = static_cast<float>(state->pitch),
				.moveMode = state->moveMode,
				.flags = state->flags,
			});
		}
	}


	std::string Describe()
	{
		std::string out;
		for (const auto& [id, remote] : players) {
			const auto actor = GetActor(remote);
			const auto& snaps = remote.states.snapshots;
			const auto& last = snaps.empty() ? Protocol::PlayerState{} : snaps.back().state;
			// Average time between the states we hold (about the last second).
			const auto gap = snaps.size() > 1 ? std::chrono::duration_cast<std::chrono::milliseconds>(snaps.back().stamp - snaps.front().stamp).count() /
			                                        static_cast<long long>(snaps.size() - 1) :
			                                    0;
			// In power armor: whether the engine has the stand-in in its frame (and switch-ins that did not take), whether its
			// 3D is on the power armor skeleton yet, how many of their pieces it has on, and its frame copy (worn by it,
			// loading, hidden or shown).
			std::string armor;
			const bool  in = actor && Equipment::InPowerArmor(actor);
			if (actor && (remote.powerArmorOn || remote.inPowerArmor || in)) {
				const auto pieces = [](const std::vector<std::uint32_t>& a_items) { return std::ranges::count_if(Wearable(a_items), PowerArmorPiece); };
				const auto frame = remote.frame.get();
				const bool engaged = in && frame && Equipment::PowerArmorFrameOf(actor).get() == frame.get();
				const auto root = frame ? frame->Get3D() : nullptr;
				armor = std::format(" in={} skel={} tries={} pieces={}/{} copy={}", in, Equipment::HasPowerArmorSkeleton(actor), remote.switchTries, pieces(Equipment::Read(actor)), pieces(remote.gear.items),
					!frame ? "none" : engaged ? "worn" : !root ? "loading" : root->GetAppCulled() ? "hidden" : "shown");
			}
			// Their frame when every game has it, and whether ours is hidden while they wear theirs.
			if (remote.powerArmorFrameRef || remote.original) {
				const auto original = remote.original.get();
				armor += std::format(" frameRef={:08X} oursHidden={}", remote.powerArmorFrameRef, original != nullptr);
			}
			out += std::format("{}[{} '{}' puppet={:08X} x={:.0f} y={:.0f} cell={:08X} ws={:08X} gapMs={} delayMs={} pa={}/{} frame={:08X} offSpot={}/{} worst={:.1f}{}]",
				out.empty() ? "" : " ", id, remote.name, actor ? actor->GetFormID() : 0,
				last.x, last.y, last.cell, last.worldspace, gap, static_cast<int>(remote.states.delay * 1000.0f), remote.powerArmorOn, remote.inPowerArmor, remote.powerArmorFrame,
				remote.hold.off, remote.hold.frames, remote.hold.worst, armor);
		}
		if (originalsHidden) {
			out += std::format(" originalsHidden={}", originalsHidden);
		}
		if (switchesIn || switchesOut || switchFails) {
			out += std::format(" armorIn={} armorOut={} armorFails={}", switchesIn, switchesOut, switchFails);
		}
		if (movingFrames) {
			out += std::format(" held={}/{}", heldFrames, movingFrames);
			out += std::format(" lights={} frames={}/{} spawns={} leftBehind={} downed={} respawnWaits={} adrift={} graveyard={}/{}", lightsPlaced, framesPlaced, framesFollowed, spawns, leftBehind, downedStandIns, respawnWaits, adrift,
				graveyard.size(), flushedBeforeSave);
		}
		if (equipmentFixesTotal) {
			out += std::format(" gearFixes={}", equipmentFixesTotal);
		}
		if (gearChanges || redraws) {
			out += std::format(" gearChanges={} redraws={}", gearChanges, redraws);
		}
		if (const auto puppets = Puppets::Describe(); !puppets.empty()) {
			out += " " + puppets;
		}
		if (heldForConversation) {
			out += std::format(" heldForTalk={}", heldForConversation);
		}
		return out.empty() ? "none" : out;
	}
}
