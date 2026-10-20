#include "game/RemotePlayers.h"
#include "game/ArmorFrames.h"
#include "game/DeferredDelete.h"
#include "game/DownRules.h"
#include "game/Dialogue.h"
#include "game/EngineRefs.h"
#include "game/FriendLight.h"
#include "game/GraphInfo.h"
#include "game/Hud.h"
#include "game/MoveSync.h"
#include "game/NetSim.h"
#include "game/SaveGuard.h"
#include "game/SaveStrays.h"
#include "game/StandInCalm.h"
#include "game/StandInGear.h"
#include "game/StandInHold.h"
#include "game/StandInRange.h"
#include "game/StandInPipboy.h"
#include "game/StandInSeat.h"
#include "game/StandInSpace.h"
#include "game/StateBuffer.h"

#include "game/Face.h"
#include "game/FaceWatch.h"
#include "game/LooksMenu.h"
#include "Config.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/Spawns.h"
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

		// Where a friend left their frame parked.
		Spot SpotOf(const Protocol::ParkedFrame& a_parked)
		{
			return { a_parked.cell, a_parked.worldspace, { a_parked.x, a_parked.y, a_parked.z } };
		}

		struct RemotePlayer
		{
			std::uint32_t        id = 0;  // their player ID: what the log calls them (never by name)
			std::string          name;
			std::uint32_t        appearance = 0;
			bool                 appearanceWarned = false;  // told the log once that their look is not one we place
			std::optional<Protocol::Face> face;
			std::uint32_t        faceBaseId = 0;  // our copy of the base NPC wearing their face (a runtime form; resolved at use, it may not survive a load)
			bool                 faceTried = false;
			bool                 faceOwnHead = false;   // that copy has their own head (Face::OwnHead): the engine builds it, or not (FaceWatch)
			bool                 faceFallback = false;  // their own head was given up here this session: the settler's face with their body shape
			FaceWatch::StandIn   faceWatch;             // the stand-in's head, while it has their own
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
			ArmorFrames::PlayTime framePlacedPlayed{};    // the same, in the time the game ran (see ArmorFrames::PlayClock)
			ArmorFrames::PlayTime frameActionAt{};        // when they last got out (the copy stands where they did a while)
			const RE::NiAVObject* frameCollisionOff = nullptr;  // the frame's 3D whose collision we turned off; only compared
			int                  switchTries = 0;        // switch-ins in this wear that did not take, or whose skeleton never came (see ArmorFrames::ChooseSwitch)
			ArmorFrames::PlayTime switchTriedAt{};
			ArmorFrames::PlayTime switchedAt{};           // when the switch-in took for this stand-in
			const RE::NiAVObject* armorSkeleton = nullptr;  // the stand-in's 3D found built on the power armor skeleton; only compared
			ArmorFrames::Stride  stride;                  // how long the stand-in's legs stayed still in its frame while they walked (see ArmorFrames::STILL_LIMIT)
			bool                 switchGaveUp = false;   // logged
			std::optional<ArmorFrames::PlayTime> pickSince;  // when the switch-in was first due for this stand-in in this wear (see ArmorFrames::ChoosePick)
			std::optional<ArmorFrames::PlayTime> keyPutAt;   // when the item that makes the engine pick their frame item was put on it
			int                  pickLogged = -1;        // the last ArmorFrames::Pick logged for this stand-in
			std::optional<RE::NiPoint3> lastTarget;      // where the stand-in was put last frame
			StandInHold::Check   hold;                   // frames it stood still somewhere else than put (net status)
			StandInSpace::Away   away;                   // the heading they came onto our very spot with (see StandInSpace::Place)
			Clock::time_point    holdLoggedAt{};
			StandInCalm::Track   calm;                   // fights with us or another stand-in ended on it (see KeepCalm)
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
			bool                 originalAway = false;   // in this wear, they got into theirs where it stood parked away from ours, hidden then (see ArmorFrames::OriginalFacts::away)
			bool                 originalParked = false; // ours was last looked at for the frame they left parked, not one they wear
			std::optional<Protocol::ParkedFrame> parked; // out of power armor: the frame they got out of last, where it stands in their world
			RE::ObjectRefHandle  parkedCopy;             // our copy of it there (see ArmorFrames::ChoosePark)
			Clock::time_point    parkedPlacedAt{};
			const RE::NiAVObject* parkedCollisionOff = nullptr;  // its 3D whose collision we turned off; only compared
			std::optional<Protocol::ParkedFrame> parkedTaken;    // the parked frame whose copy somebody here got into: none placed for it again
			std::optional<Protocol::Seat> seat;          // the furniture they use, as their status names it (see UpdateSeat)
			RE::ObjectRefHandle  seatRef;                // ours of it, once found (StandInSeat::Pick)
			Clock::time_point    seatLookAt{};           // none found yet: looked for again then
			int                  seatLooks = 0;          // looks that found none, for this seat
			std::uint32_t        seatActor = 0;          // the stand-in the tries below are for
			int                  seatTries = 0;          // ways into it tried for this seat on that stand-in (Puppets::Entry)
			ArmorFrames::PlayTime seatTriedAt{};
			bool                 seatHeld = false;       // the stand-in is held for it (Puppets::Hold)
			bool                 seatLogged = false;     // whether it got in, or did not, was logged for this seat
			std::uint8_t         menu = Protocol::MenuKind::kNone;  // the menu they are in (their status)
			StandInPipboy::Track pipboy;                 // the Pip-Boy raised on their stand-in while they are in theirs
		};

		std::map<std::uint32_t, RemotePlayer> players;
		// The looks friends asked for this run (base NPCs; see Spawn), kept after they leave: a stand-in an
		// earlier version saved may wear one (see IsStandInBase).
		std::set<std::uint32_t> looks;
		// Our own player's head, after a stand-in with a friend's own head took it (see FaceWatch).
		FaceWatch::Ours ourHead;

		// Where a friend is now, as where they got into a pre-placed frame (see SetPowerArmor).
		void TakeWornAt(RemotePlayer& a_player, const Protocol::PlayerState& a_state)
		{
			a_player.wornAt = Spot{ a_state.cell, a_state.worldspace, { a_state.x, a_state.y, a_state.z } };
			a_player.wornAtWanted = false;
		}

		// Stand-ins to delete once their 3D is in (see DeferredDelete).
		// A stand-in (or its light or frame) goes in two steps: disabled now, and marked for deletion a moment
		// later (DeferredDelete::MayDelete), never both in one frame, not even before a save.
		struct Grave
		{
			RE::ObjectRefHandle              actor;
			Clock::time_point                spawnedAt;
			std::optional<Clock::time_point> disabledAt;
			Clock::time_point                buriedAt{};
			bool                             held = false;  // has waited for the player's conversation
			// A stand-in stays one of our puppets until it is disabled: its AI off and its flags kept (the
			// puppet update sets them again every update), kept healthy, and not to be talked to. One kept
			// for the player's conversation used to be let go first, so its AI ran and the player could
			// press E on it, start another conversation, and have it deleted in the middle of that one.
			// Never dereferenced (see RemotePlayer::registered); null for a light or frame.
			const RE::Actor*                 puppet = nullptr;
			std::uint32_t                    puppetId = 0;
			std::uint32_t                    player = 0;  // whose stand-in it was, for the log
			StandInCalm::Track               calm;        // fights with us or another stand-in ended on it while it waits (see KeepCalm)
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

		// The stand-in is being taken out of the world (a_ref; null when it is gone already): no longer one of our
		// puppets, and a conversation it leaves open as its speaker is ours to end (Dialogue::Removed).
		void Release(Grave& a_grave, RE::TESObjectREFR* a_ref)
		{
			if (a_grave.puppet) {
				Dialogue::Removed(a_grave.id, a_ref);
				Puppets::Unregister(a_grave.puppet, a_grave.puppetId);
				a_grave.puppet = nullptr;
				a_grave.puppetId = 0;
			}
		}

		void Disable(Grave& a_grave, RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			Release(a_grave, a_ref);
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
				Release(a_grave, nullptr);
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
		std::uint32_t                         calmed = 0;       // stand-ins' fights with us or another stand-in ended (see KeepCalm)
		StandInCalm::Ours                     ours;             // and the combat they left us in (see KeepUsCalm)
		// Frames drawn while a friend was moving, and how many of those ran out of states to blend
		// (the next one was late) and held them still: the interpolation delay is too short if
		// this is more than a few percent.
		std::uint32_t                         movingFrames = 0;
		std::uint32_t                         heldFrames = 0;
		// ... and how many were carried on past the newest state instead, the way the friend went (StateBuffer::EXTRAPOLATE).
		std::uint32_t                         aheadFrames = 0;
		// Frames a stand-in was drawn a body's room away from us instead of at its friend's spot, which was ours (StandInSpace).
		std::uint32_t                         spacedFrames = 0;

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
			return ArmorFrames::Kept(a_items, a_dress, KindOf);
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
			auto      tag = shown >= 100 ? a_player.name : std::format("{} ({}%)", a_player.name, shown);
			// In their Pip-Boy or another menu (the icon over them says which kind; MenuIcons).
			if (a_player.menu == Protocol::MenuKind::kPipboy) {
				return Text::Format("party.tag_pipboy", { { "name", tag } });
			}
			if (a_player.menu != Protocol::MenuKind::kNone) {
				return Text::Format("party.tag_menu", { { "name", tag } });
			}
			return tag;
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

		// a_parked: looked at for the frame they left parked (how far ours stands from it), not for one they wear.
		ArmorFrames::OriginalFacts OriginalFactsOf(const RemotePlayer& a_player, RE::TESObjectREFR* a_ref, bool a_parked)
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
			facts.fromStart = FromStart(a_ref);
			// Another player's game runs it (FrameShare.h): it stands here where their game has it.
			const auto owner = NpcSync::Owner(a_ref->GetFormID());
			facts.synced = owner != 0 && owner != NpcSync::LocalPlayer();
			if (a_parked) {
				facts.fromSpot = DistanceFrom(a_ref, SpotOf(*a_player.parked));
				return facts;
			}
			facts.moved = a_player.originalMoved;
			facts.away = a_player.originalAway;
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
			// Out of it, the frame they left parked can be ours too (ArmorFrames::ChooseParkedOriginal): theirs taken
			// somewhere else, ours still standing where the world placed it.
			const bool parked = !a_player.powerArmorOn && a_player.parked && a_player.parked->ref != 0;
			const auto named = worn ? a_player.powerArmorFrameRef : parked ? a_player.parked->ref : 0;
			auto       original = a_player.original.get();
			if (original && (original->IsDeleted() || original->IsDisabled())) {
				ShowOriginal(a_player);  // gone from our world meanwhile: nothing left to show
				original = nullptr;
			}
			if (original && original->GetFormID() != named) {
				// They are out of it, and it is not the frame they left parked either. Ours is shown again once the frame
				// copy their stand-in leaves is gone or clear of it (it stands where they got out, often right on ours, for a
				// few seconds: two suits in one spot), and is solid again once nobody stands in it. Not while another friend
				// still has it hidden.
				if (!HiddenByAnother(a_player, original.get()) && !Clear(original.get(), a_player.frame.get().get())) {
					return;
				}
				ShowOriginal(a_player);
				original = nullptr;
			}
			if (named == 0) {
				a_player.originalChoice = -1;
				return;
			}
			if (worn && a_player.wornAtWanted) {
				return;  // where they got in comes with their next state
			}
			if (parked != a_player.originalParked) {
				a_player.originalParked = parked;
				a_player.originalChoice = -1;  // logged afresh
			}
			// Looked at now and then, hidden or not: a hidden one that has since moved, or that someone here got into, is
			// no longer the suit they wear where they got into it.
			if (a_now >= a_player.originalCheckAt) {
				a_player.originalCheckAt = a_now + ORIGINAL_CHECK_INTERVAL;
				const auto ref = original ? original.get() : RE::TESForm::GetFormByID<RE::TESObjectREFR>(named);
				const auto facts = OriginalFactsOf(a_player, ref, !worn);
				const auto choice = worn ? ArmorFrames::ChooseOriginal(named, a_player.powerArmorFrame, facts) : ArmorFrames::ChooseParkedOriginal(named, a_player.parked->base, facts);
				if (static_cast<int>(choice) != a_player.originalChoice) {
					a_player.originalChoice = static_cast<int>(choice);
					REX::INFO("RemotePlayers: player {} {} frame {:08X} (base {:08X}); ours{}, {:.0f} units from where they {} and {:.0f} from where it first stood: {}", a_player.id, worn ? "wears" : "left", named,
						worn ? a_player.powerArmorFrame : a_player.parked->base, facts.synced ? " (player " + std::to_string(NpcSync::Owner(named)) + "'s game runs it)" : "", facts.fromSpot.value_or(-1.0f), worn ? "got in" : "left theirs", facts.fromStart,
						choice == ArmorFrames::Original::kHide      ? (worn ? "hidden while they wear theirs" : "hidden while theirs stands parked elsewhere") :
						choice == ArmorFrames::Original::kMissing   ? "none here" :
						choice == ArmorFrames::Original::kNotLoaded ? "not loaded" :
						choice == ArmorFrames::Original::kOurs      ? "our player's own, left alone" :
						choice == ArmorFrames::Original::kInUse     ? "in use here, left alone" :
						choice == ArmorFrames::Original::kNotFrame  ? "not a power armor frame, left alone" :
						choice == ArmorFrames::Original::kQuest     ? "a quest of ours holds it, left alone" :
						choice == ArmorFrames::Original::kElsewhere ? "not where they got in, left alone while they wear theirs" :
						choice == ArmorFrames::Original::kUsed      ? "moved in our world since it was placed (our player's own now), left alone" :
						choice == ArmorFrames::Original::kThere     ? "it stands where they left theirs, and stands for it" :
						                                              "-");
				}
				// A synced one (another game runs it) is moved to where their game has it: looked at again once it is there.
				if (worn && choice == ArmorFrames::Original::kElsewhere && !facts.synced) {
					a_player.originalMoved = true;  // not hidden later in this wear, even should it come back to the spot
				}
				if (original && choice != ArmorFrames::Original::kHide && choice != ArmorFrames::Original::kNotLoaded) {
					if (worn && !facts.synced) {
						a_player.originalMoved = true;
					}
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
		std::uint32_t   stillStandIns = 0; // of those, stand-ins in their frame placed again for legs that stayed still (ArmorFrames::Switch::kStill)
		ArmorFrames::Played played;        // the time the game ran, which the power armor limits go by

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

		// Places a copy of a frame in an interior (a_interior) or outside (a_world): for the stand-in to get into, where they
		// got out, or where they left theirs parked.
		RE::ObjectRefHandle PlaceCopy(RE::TESBoundObject* a_base, RE::TESObjectCELL* a_interior, RE::TESWorldSpace* a_world, const RE::NiPoint3& a_at, float a_heading)
		{
			if (!a_base || (!a_interior && !a_world)) {
				return {};
			}
			RE::NEW_REFR_DATA data;
			data.location = a_at;
			data.direction = { 0.0f, 0.0f, a_heading };
			data.object = a_base;
			data.interior = a_interior;
			data.world = a_interior ? nullptr : a_world;
			data.clearStillLoadingFlag = true;
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto frame = handle.get();
			if (!frame) {
				return {};
			}
			// Empty. A frame never looked into is filled from its base's container the first time its inventory is read
			// (TESObjectREFR::RemoveAllItems does so itself, then clears it; the engine's switch-in does so too as it moves
			// the frame's items onto the actor), and a pre-placed loot frame's base (the museum's T-45, 0019DED9) carries a
			// whole set of pieces: the copy left where a friend got out rolled a fresh set for whoever here stepped into it,
			// a suit nobody in the session ever had, and the copy the stand-in gets into would have had those pieces moved
			// onto it. The friend's own pieces cannot go into a copy; it stands empty.
			if (Equipment::MayChange(frame.get(), OwnGear::Change::kEmptyFrame)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", frame->GetFormID()).c_str());
			}
			++framesPlaced;
			SaveGuard::Track(frame.get());
			return handle;
		}

		// Places a copy of a frame at the stand-in, or where it stood as they got out, in our player's cell (a_cell).
		bool PlaceFrame(RemotePlayer& a_player, RE::TESBoundObject* a_base, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now, ArmorFrames::PlayTime a_played)
		{
			if (!a_cell) {
				return false;
			}
			const auto handle = PlaceCopy(a_base, a_cell->IsInterior() ? a_cell : nullptr, a_cell->IsInterior() ? nullptr : a_cell->worldSpace, a_at, a_heading);
			if (!handle) {
				return false;
			}
			a_player.frame = handle;
			a_player.framePlacedAt = a_now;
			a_player.framePlacedPlayed = a_played;
			a_player.frameCollisionOff = nullptr;  // a new copy has its own collision
			return true;
		}

		// Into its copy (see ArmorFrames::ChooseSwitch and Equipment::SwitchIntoPowerArmor).
		void SwitchIn(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectREFR* a_frame, ArmorFrames::PlayTime a_now)
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

		// Whether the stand-in may be switched in now, so that the engine's pick of its frame item is theirs (see
		// ArmorFrames::ChoosePick): the item that makes the engine pick their frame item is put on it first, or the clothes
		// it was dressed in are let come on, for at most ArmorFrames::PICK_WAIT.
		bool PickReady(RemotePlayer& a_player, RE::Actor* a_actor, ArmorFrames::PlayTime a_now)
		{
			if (!a_player.pickSince) {
				a_player.pickSince = a_now;
			}
			const auto& items = a_player.gear.items;
			const auto  named = std::ranges::find_if(items, [](std::uint32_t a_item) { return KindOf(a_item) == ArmorFrames::Item::kFrame; });
			const auto  theirs = named != items.end() ? *named : 0;
			const auto  key = theirs ? Equipment::FrameKey(theirs, items) : 0;
			const auto  worn = Equipment::Read(a_actor);
			const auto  pick = Equipment::FramePick(a_actor);
			// What it was dressed in, as an NPC's inventory shows it (Wearable), sorted as Read gives it.
			auto dressedIn = Wearable(Kept(a_player.gear.worn, ArmorFrames::Dress::kClothes));
			std::ranges::sort(dressedIn);
			ArmorFrames::PickFacts facts{
				.named = theirs != 0,
				.picksTheirs = theirs != 0 && pick == theirs,
				.key = key != 0,
				.keyOn = key != 0 && std::ranges::binary_search(worn, key),
				.dressed = a_player.gear.dressed && std::ranges::includes(worn, dressedIn),
				.waited = a_now - *a_player.pickSince,
			};
			if (a_player.keyPutAt) {
				facts.sincePut = a_now - *a_player.keyPutAt;
			}
			const auto choice = ArmorFrames::ChoosePick(facts);
			// Logged once a choice for each stand-in in a wear (a wait is not logged).
			if (static_cast<int>(choice) != a_player.pickLogged && choice != ArmorFrames::Pick::kWait) {
				a_player.pickLogged = static_cast<int>(choice);
				const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(facts.waited).count();
				if (choice == ArmorFrames::Pick::kPutOn) {
					REX::INFO("RemotePlayers: player {} stand-in {:08X} is given {:08X} so that the engine picks their frame item {:08X}, not {:08X}", a_player.id, a_actor->GetFormID(), key, theirs, pick);
				} else {
					REX::INFO("RemotePlayers: player {} stand-in {:08X} goes in after {} ms; the engine picks frame item {:08X}, theirs is {:08X}{}", a_player.id, a_actor->GetFormID(), waited, pick, theirs,
						!facts.named ? (facts.dressed ? " (not told yet; in the clothes they got in with)" : " (not told yet; its clothes not on yet)") : choice == ArmorFrames::Pick::kGiveUp ? " (not theirs)" : "");
				}
			}
			switch (choice) {
			case ArmorFrames::Pick::kPutOn:
				Equipment::PutOnForPick(a_actor, key);
				a_player.keyPutAt = a_now;
				return false;
			case ArmorFrames::Pick::kWait:
				return false;
			case ArmorFrames::Pick::kSwitch:
			case ArmorFrames::Pick::kGiveUp:
				break;
			}
			return true;
		}

		// Whether the stand-in's 3D is built on the power armor skeleton (Equipment::HasPowerArmorSkeleton): the rebuild
		// the switch-in queued has run. Looked for once per 3D, and logged as it comes.
		bool OnArmorSkeleton(RemotePlayer& a_player, RE::Actor* a_actor, ArmorFrames::PlayTime a_now)
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
			// The rebuilt 3D carries a fresh animation graph, in its base state whatever the old one was told.
			Puppets::GraphRebuilt(a_actor);
			REX::INFO("RemotePlayers: player {} stand-in {:08X} has its power armor skeleton, {} ms after the switch-in; their pieces go on", a_player.id, a_actor->GetFormID(),
				std::chrono::duration_cast<std::chrono::milliseconds>(a_now - a_player.switchedAt).count());
			return true;
		}

		// What the switch-in and the dress go by, before the copy is looked at (see UpdatePowerArmor for the rest). The
		// dress settles by the clock (StandInGear keeps its times so): a menu between a dress and the switch-in only
		// lengthens the wait.
		ArmorFrames::SwitchFacts SwitchFactsOf(RemotePlayer& a_player, RE::Actor* a_actor, Clock::time_point a_now, ArmorFrames::PlayTime a_played)
		{
			const bool in = Equipment::InPowerArmor(a_actor);
			return {
				.worn = WornFrame(a_player) != nullptr,
				.in = in,
				.rebuilt = in && OnArmorSkeleton(a_player, a_actor, a_played),
				.still = a_player.stride.Still(),
				.sinceIn = a_played - a_player.switchedAt,
				.sinceDressed = a_player.gear.wornAt == Clock::time_point{} ? ArmorFrames::Duration::max() : ArmorFrames::Duration{ a_now - a_player.gear.wornAt },
				.tries = a_player.switchTries,
				.sinceTried = a_played - a_player.switchTriedAt,
			};
		}

		// Whether the stand-in's legs move in its frame while they walk in our sight (ArmorFrames::Stride): looked at only
		// while the engine has it in its frame on the power armor skeleton.
		void UpdateStride(RemotePlayer& a_player, RE::Actor* a_actor, const Protocol::PlayerState& a_state, const RE::Actor* a_us, ArmorFrames::PlayTime a_now)
		{
			std::optional<ArmorFrames::LegPose> pose;
			if (Equipment::InPowerArmor(a_actor) && OnArmorSkeleton(a_player, a_actor, a_now)) {
				ArmorFrames::LegPose leg;
				if (Equipment::LegRotation(a_actor, leg.m)) {
					pose = leg;
				}
			}
			const auto& at = a_actor->data.location;
			const bool  seen = a_us && ArmorFrames::InSight(at.x - a_us->data.location.x, at.y - a_us->data.location.y, a_us->data.angle.z);
			const bool  walks = seen && ArmorFrames::Walks(a_state.moveMode, a_state.flags, a_state.speed) && !Dialogue::Holds(a_actor);
			a_player.stride.Update(a_actor->Get3D(), pose, walks, a_now);
		}

		void UpdatePowerArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now, ArmorFrames::PlayTime a_played)
		{
			if (!a_actor) {
				return;
			}
			const auto base = WornFrame(a_player);
			const bool worn = base != nullptr;
			if (worn != a_player.inPowerArmor) {
				a_player.inPowerArmor = worn;
				if (!worn) {
					a_player.frameActionAt = a_played;  // the copy stands where they got out a while
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
				facts.sincePlaced = a_played - a_player.framePlacedPlayed;
				facts.sinceOut = a_played - a_player.frameActionAt;
				facts.sameBase = frame->GetObjectReference() == base;
			}
			const auto copy = ArmorFrames::ChooseCopy(facts);
			switch (copy) {
			case ArmorFrames::Copy::kNone:
				break;
			case ArmorFrames::Copy::kPlace:
				// Once the stand-in is drawn: one whose 3D never comes needs none.
				if (has3D) {
					PlaceFrame(a_player, base, a_cell, a_actor->data.location, a_actor->data.angle.z, a_now, a_played);
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
			auto       switchFacts = SwitchFactsOf(a_player, a_actor, a_now, a_played);
			switchFacts.ready = has3D && process && process->processLevel <= 1 && !a_actor->IsDead(false);
			switchFacts.held = copy == ArmorFrames::Copy::kHold;
			switch (ArmorFrames::ChooseSwitch(switchFacts)) {
			case ArmorFrames::Switch::kIn:
				if (PickReady(a_player, a_actor, a_played)) {
					SwitchIn(a_player, a_actor, frame.get(), a_played);
				}
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
			case ArmorFrames::Switch::kStill:  // and so is this one
				break;
			}
		}

		// Where a friend's stand-in is put this frame: their spot, or a body's room out from us when that is ours
		// (StandInSpace), wider when either of us is in power armor. Counted for net status while it is moved.
		RE::NiPoint3 SpotOf(RemotePlayer& a_player, const Protocol::PlayerState& a_state, const RE::Actor* a_us)
		{
			const RE::NiPoint3 at{ a_state.x, a_state.y, a_state.z };
			if (!a_us) {
				return at;
			}
			const auto& us = a_us->data.location;
			const float clear = a_player.inPowerArmor || Equipment::InPowerArmor(a_us) ? StandInSpace::CLEAR_ARMOR : StandInSpace::CLEAR;
			const auto  spot = StandInSpace::Place(a_player.away, { at.x, at.y, at.z }, a_state.heading, { us.x, us.y, us.z }, clear);
			if (spot.x == at.x && spot.y == at.y) {
				return at;
			}
			++spacedFrames;
			return { spot.x, spot.y, spot.z };
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
			// The copy it wears stays (disabled) where it got in, while the stand-in walks off in it: that distance says
			// nothing of what holds it.
			const bool worn = frame && Equipment::PowerArmorFrameOf(a_actor).get() == frame.get();
			REX::INFO("RemotePlayers: player {} stand-in stood {:.1f} units from where it was put ({:.0f},{:.0f}), at ({:.0f},{:.0f}); in power armor={}, frame copy{} {:.1f} units away; {} of {} still frames so", a_player.id, off, last->x, last->y, at.x, at.y,
				a_player.inPowerArmor, worn ? " (worn, left where it got in)" : "", frame ? frame->data.location.GetDistance(at) : -1.0f, a_player.hold.off, a_player.hold.frames);
		}

		// Furniture a friend uses (StandInSeat.h): their stand-in in the same chair, bed, workbench or terminal.
		std::uint32_t seatsEntered = 0;  // stand-ins the engine put in their friend's furniture
		std::uint32_t seatsMissed = 0;   // ... and left beside it: the ways in did not take, or no such furniture here
		std::uint32_t seatsLeft = 0;     // stood up out of it as their friend got up
		constexpr auto SEAT_LOOK_INTERVAL = 1s;  // none found: looked for again this often (its 3D may still be coming)

		void ForgetSeat(RemotePlayer& a_player)
		{
			a_player.seatRef = {};
			a_player.seatLookAt = {};
			a_player.seatLooks = 0;
			a_player.seatTries = 0;
			a_player.seatLogged = false;
		}

		bool IsSeatBase(const RE::TESBoundObject* a_base)
		{
			// A plain seat or bed only, whatever a friend's game names: never a terminal, a workbench or a station (see
			// StandInSeat.h).
			const auto type = a_base ? a_base->GetFormType() : RE::ENUM_FORM_ID::kNONE;
			return type == RE::ENUM_FORM_ID::kFURN && static_cast<const RE::TESFurniture*>(a_base)->wbData.type == RE::WorkbenchData::Type::kNone;
		}

		// Furniture the stand-in can be put in: enabled, its 3D in, and where the stand-in is.
		bool UsableSeat(RE::TESObjectREFR* a_ref, const RE::Actor* a_actor)
		{
			if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D() || !IsSeatBase(a_ref->GetObjectReference())) {
				return false;
			}
			const auto cell = a_ref->GetParentCell();
			const auto ourCell = a_actor->GetParentCell();
			if (!cell || !ourCell) {
				return false;
			}
			return ourCell->IsInterior() ? cell == ourCell : !cell->IsInterior() && cell->worldSpace == ourCell->worldSpace;
		}

		// Our copy of the furniture their status names (StandInSeat::Pick), kept once found.
		RE::NiPointer<RE::TESObjectREFR> FindSeat(RemotePlayer& a_player, const RE::Actor* a_actor, Clock::time_point a_now)
		{
			const auto& seat = *a_player.seat;
			if (const auto kept = a_player.seatRef.get()) {
				const auto base = kept->GetObjectReference();
				if (base && base->GetFormID() == seat.base && UsableSeat(kept.get(), a_actor)) {
					return kept;
				}
			}
			a_player.seatRef = {};
			if (a_now < a_player.seatLookAt) {
				return {};
			}
			a_player.seatLookAt = a_now + SEAT_LOOK_INTERVAL;
			std::vector<StandInSeat::Candidate> candidates;
			const auto add = [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref->GetObjectReference();
				const auto& at = a_ref->data.location;
				candidates.push_back({ .id = a_ref->GetFormID(), .base = base ? base->GetFormID() : 0, .x = at.x, .y = at.y, .z = at.z, .usable = UsableSeat(a_ref, a_actor) });
			};
			if (seat.ref != 0) {
				if (const auto named = RE::TESForm::GetFormByID<RE::TESObjectREFR>(seat.ref)) {
					add(named);
				}
			}
			if (const auto tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(RE::NiPoint3{ seat.x, seat.y, seat.z }, StandInSeat::MATCH_RADIUS, [&](RE::TESObjectREFR* a_ref) {
					const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
					if (base && base->GetFormID() == seat.base && a_ref->GetFormID() != seat.ref) {
						add(a_ref);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			const auto picked = StandInSeat::Pick(seat, candidates);
			const auto ref = picked ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(*picked) : nullptr;
			if (!ref) {
				++a_player.seatLooks;
				return {};
			}
			a_player.seatRef = ref->GetHandle();
			return RE::NiPointer<RE::TESObjectREFR>{ ref };
		}

		// Puts the stand-in in the furniture their status names, holds it there, and stands it up as they get up
		// (StandInSeat::Choose). True while it is held: in it, on its way in, or left beside it. a_spot: where their states
		// put it.
		bool UpdateSeat(RemotePlayer& a_player, RE::Actor* a_actor, const RE::NiPoint3& a_spot, Clock::time_point a_now, ArmorFrames::PlayTime a_played)
		{
			// A new stand-in is tried afresh.
			if (a_player.seatActor != a_actor->GetFormID()) {
				a_player.seatActor = a_actor->GetFormID();
				a_player.seatTries = 0;
				a_player.seatHeld = false;
				a_player.seatLogged = false;
			}
			const auto in = Puppets::FurnitureOf(a_actor);
			const auto target = a_player.seat ? FindSeat(a_player, a_actor, a_now) : RE::NiPointer<RE::TESObjectREFR>{};
			StandInSeat::Facts facts;
			facts.wanted = a_player.seat.has_value();
			facts.found = static_cast<bool>(target);
			if (a_player.seat) {
				const auto at = target ? target->data.location : RE::NiPoint3{ a_player.seat->x, a_player.seat->y, a_player.seat->z };
				facts.near = a_spot.GetDistance(at) <= StandInSeat::NEAR;
			}
			facts.inArmor = Equipment::InPowerArmor(a_actor);
			facts.inTarget = in && target && in.get() == target.get();
			facts.inOther = in && !facts.inTarget && !in->As<RE::Actor>();
			facts.tries = a_player.seatTries;
			facts.sinceTried = a_played - a_player.seatTriedAt;

			switch (StandInSeat::Choose(facts)) {
			case StandInSeat::Act::kFollow:
				if (a_player.seatHeld) {
					Puppets::Hold(a_actor, false);
					a_player.seatHeld = false;
				}
				return false;
			case StandInSeat::Act::kStand:
				Puppets::StandUp(a_actor);
				a_player.seatHeld = false;
				++seatsLeft;
				REX::INFO("RemotePlayers: player {} {}; stand-in {:08X} stood up out of furniture {:08X}", a_player.id, facts.wanted ? "uses other furniture" : "got up", a_actor->GetFormID(), in ? in->GetFormID() : 0);
				return false;
			case StandInSeat::Act::kEnter:
				{
					const auto way = a_player.seatTries == 0 ? Puppets::Entry::kSnap : Puppets::Entry::kActivate;
					++a_player.seatTries;
					a_player.seatTriedAt = a_played;
					const bool asked = Puppets::Enter(a_actor, target.get(), way);
					Puppets::Hold(a_actor, true);
					a_player.seatHeld = true;
					const auto base = target->GetObjectReference();
					REX::INFO("RemotePlayers: player {} uses furniture {:08X} (base {:08X}, theirs {:08X}); stand-in {:08X} sent in by {} (try {} of {}{})", a_player.id, target->GetFormID(), base ? base->GetFormID() : 0,
						a_player.seat->ref, a_actor->GetFormID(), way == Puppets::Entry::kSnap ? "SnapIntoInteraction" : "activation", a_player.seatTries, StandInSeat::MAX_TRIES, asked ? "" : ", could not be asked");
					return true;
				}
			case StandInSeat::Act::kHold:
				if (!a_player.seatHeld) {
					Puppets::Hold(a_actor, true);
					a_player.seatHeld = true;
				}
				if (!a_player.seatLogged && facts.inTarget) {
					a_player.seatLogged = true;
					++seatsEntered;
					REX::INFO("RemotePlayers: player {} stand-in {:08X} is in furniture {:08X} (sit state {}) after {} way(s) in", a_player.id, a_actor->GetFormID(), in->GetFormID(), static_cast<int>(a_actor->DoGetSitSleepState()), a_player.seatTries);
				} else if (!a_player.seatLogged && ((facts.found && facts.tries >= StandInSeat::MAX_TRIES && facts.sinceTried >= StandInSeat::ENTRY_WAIT) || (!facts.found && a_player.seatLooks >= 3))) {
					a_player.seatLogged = true;
					++seatsMissed;
					REX::WARN("RemotePlayers: player {} uses furniture {:08X} (base {:08X}); {}: stand-in {:08X} is left standing where it was as they sat down", a_player.id, a_player.seat->ref, a_player.seat->base,
						facts.found ? "it never got in" : "no such furniture here", a_actor->GetFormID());
				}
				return true;
			}
			return false;
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
			// One in furniture gets up before it goes. Left snapped into a terminal as the player went to another cell, it
			// crashed the game: the engine still ran its furniture package once it had no AI process.
			if (actor && Puppets::FurnitureOf(actor)) {
				Puppets::StandUp(actor);
				a_player.seatHeld = false;
			}
			// A stand-in kept for the player's conversation (see HeldForConversation) keeps the frame it is in: the frame
			// waits for it instead of being disabled from under it (it then went on held in a frame that was gone).
			const auto heldIn = actor && Dialogue::Holds(actor) ? Equipment::PowerArmorFrameOf(actor) : RE::NiPointer<RE::TESObjectREFR>{};
			const auto wearer = heldIn ? a_player.actor : RE::ObjectRefHandle{};
			ReleaseArmor(a_player, actor, wearer);
			DeleteFrame(a_player, actor, heldIn && heldIn.get() == a_player.frame.get().get() ? wearer : RE::ObjectRefHandle{});
			if (actor) {
				WeaponFire::ReleaseSoundsOf(actor->GetFormID());  // a replayed burst may still follow its 3D
				WeaponFire::DropHandThrowable(actor->GetFormID());
				LooksMenu::Remove(actor);  // or the next reference given its form ID would have their body and tattoos
				// It stays a puppet until it is disabled (see Grave::puppet), standing still where it is: no friend
				// moves it any more.
				Puppets::AllowAI(actor, 0ms);
				Puppets::SetTarget(actor, { .position = actor->data.location, .heading = actor->data.angle.z });
				Bury({ .actor = a_player.actor, .spawnedAt = a_player.range.spawnedAt, .puppet = a_player.registered, .puppetId = a_player.registeredId, .player = a_player.id });
			} else if (a_player.registered) {
				Puppets::Unregister(a_player.registered, a_player.registeredId);  // gone already
			}
			a_player.registered = nullptr;
			a_player.registeredId = 0;
			a_player.actor = {};
			a_player.faceWatch = {};  // the next one is watched afresh
		}

		// They are out of their armor and the stand-in is still in its frame (ArmorFrames::Switch::kOut): the stand-in goes
		// with that frame (see ArmorFrames for why it is not switched out), a fresh one is placed out of armor at once, and a
		// copy of the frame is left where the old one stood, drawn a while (ArmorFrames::ChooseCopy).
		void LeaveArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now, ArmorFrames::PlayTime a_played)
		{
			const auto  frame = Equipment::PowerArmorFrameOf(a_actor);
			const auto  base = frame ? frame->GetObjectReference() : nullptr;
			const auto  at = a_actor->data.location;
			const float heading = a_actor->data.angle.z;
			REX::INFO("RemotePlayers: player {} got out of their power armor; stand-in {:08X} goes with its frame {:08X}, a copy is left where it stood", a_player.id, a_actor->GetFormID(), frame ? frame->GetFormID() : 0);
			DeleteActor(a_player);
			++switchesOut;
			// Their status told where their frame stands already, and a copy stands there (see UpdateParked): none goes
			// where the stand-in stood, some way off the frame.
			if (const auto parked = a_player.parkedCopy.get(); parked && parked->GetObjectReference() == base) {
				REX::INFO("RemotePlayers: player {}'s frame copy {:08X} stands where their frame is, {:.0f} units from where their stand-in stood", a_player.id, parked->GetFormID(), parked->data.location.GetDistance(at));
				return;
			}
			if (Equipment::IsPowerArmorFrame(base) && PlaceFrame(a_player, base, a_cell, at, heading, a_now, a_played)) {
				a_player.frameActionAt = a_played;
			}
		}

		bool SameSpace(const Protocol::PlayerState& a_state, const RE::TESObjectCELL* a_localCell)
		{
			if (a_localCell->IsInterior()) {
				return a_state.cell == a_localCell->GetFormID();
			}
			return a_state.cell == 0 && a_localCell->worldSpace && a_state.worldspace == a_localCell->worldSpace->GetFormID();
		}

		// A friend's parked frame (see ArmorFrames::ChoosePark): a copy stands where their game has the frame they got out of
		// last, for as long as they are out of it and it is in our sight, whether or not their stand-in is.
		std::uint32_t parkedPlaced = 0;  // parked copies placed, or taken over from the copy left where they got out

		// Where it stands is in the area the engine draws around our player: our interior, or outside within the cells
		// it attaches around ours (as for a stand-in, StandInRange::Visible).
		bool InView(const Protocol::ParkedFrame& a_parked, const RE::TESObjectCELL* a_localCell, const RE::PlayerCharacter* a_us, bool a_loading)
		{
			if (a_loading || !a_localCell || !a_us) {
				return false;
			}
			if (a_localCell->IsInterior()) {
				return a_parked.cell == a_localCell->GetFormID();
			}
			return a_parked.cell == 0 && a_localCell->worldSpace && a_parked.worldspace == a_localCell->worldSpace->GetFormID() &&
			       StandInRange::Visible(a_parked.x, a_parked.y, a_us->data.location.x, a_us->data.location.y, GridsToLoad());
		}

		// Our own reference of the frame they parked (a pre-placed one, by its ID) stands where they parked theirs: it
		// stands for theirs. Not one the engine has disabled (worn by our player: it stands where it was got into). One
		// their game made stands there as our copy of it when their game shares it (Spawns, FrameShare::SharesMade).
		bool OursThere(std::uint32_t a_owner, const Protocol::ParkedFrame& a_parked)
		{
			if (a_parked.ref == 0) {
				return Spawns::FrameCopyAt(a_owner, a_parked, ArmorFrames::ORIGINAL_RADIUS);
			}
			const auto ourFrame = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_parked.ref);
			if (!ourFrame || ourFrame->IsDeleted() || ourFrame->IsDisabled() || ourFrame->IsCreated()) {
				return false;
			}
			const auto base = ourFrame->GetObjectReference();
			const auto from = base && base->GetFormID() == a_parked.base ? DistanceFrom(ourFrame, SpotOf(a_parked)) : std::nullopt;
			return from && *from <= ArmorFrames::ORIGINAL_RADIUS;
		}

		// The parked copy goes, unless someone here is in it (as DeleteFrame).
		void DeleteParked(RemotePlayer& a_player)
		{
			if (const auto copy = a_player.parkedCopy.get()) {
				if (OurFrame(copy.get()) || FrameInUse(copy.get())) {
					REX::INFO("RemotePlayers: someone here is in player {}'s parked frame copy {:08X}; leaving it", a_player.id, copy->GetFormID());
				} else {
					BuryFrame(copy.get(), a_player.parkedPlacedAt);
				}
			}
			a_player.parkedCopy = {};
			a_player.parkedCollisionOff = nullptr;
		}

		void UpdateParked(RemotePlayer& a_player, RE::Actor* a_standIn, const RE::TESObjectCELL* a_localCell, const RE::PlayerCharacter* a_us, bool a_loading, Clock::time_point a_now)
		{
			const auto& parked = a_player.parked;
			const auto  base = parked && !a_player.powerArmorOn ? RE::TESForm::GetFormByID<RE::TESFurniture>(parked->base) : nullptr;
			ArmorFrames::ParkFacts facts;
			facts.told = Equipment::IsPowerArmorFrame(base);
			if (facts.told) {
				facts.inView = InView(*parked, a_localCell, a_us, a_loading);
				facts.oursThere = OursThere(a_player.id, *parked);
				facts.taken = a_player.parkedTaken == parked;
			}
			const RE::NiPoint3 at = parked ? RE::NiPoint3{ parked->x, parked->y, parked->z } : RE::NiPoint3{};
			const float        heading = parked ? parked->heading : 0.0f;
			const auto         copy = a_player.parkedCopy.get();
			facts.exists = copy != nullptr;
			if (copy) {
				facts.sameBase = facts.told && copy->GetObjectReference() == base;
				facts.occupied = OurFrame(copy.get()) || FrameInUse(copy.get());
				facts.loaded = copy->Get3D() != nullptr;
				if (const auto cell = copy->GetParentCell(); cell && parked) {
					facts.samePlace = cell->IsInterior() ? parked->cell == cell->GetFormID() :
					                                       parked->cell == 0 && cell->worldSpace && parked->worldspace == cell->worldSpace->GetFormID();
				}
				facts.distance = parked ? copy->data.location.GetDistance(at) : 0.0f;
				facts.turn = parked ? StandInHold::AngleBetween(copy->data.angle.z, heading) : 0.0f;
				facts.sincePlaced = a_now - a_player.parkedPlacedAt;
			}
			// The copy their stand-in left as they got out (LeaveArmor), when their status told where the frame stands
			// only after that: it is moved there rather than another placed.
			if (const auto left = a_player.frame.get(); facts.told && left && !WornFrame(a_player)) {
				const bool engaged = a_standIn && Equipment::PowerArmorFrameOf(a_standIn).get() == left.get();
				facts.leftCopy = !engaged && left->GetObjectReference() == base && !OurFrame(left.get()) && !FrameInUse(left.get());
			}

			switch (ArmorFrames::ChoosePark(facts)) {
			case ArmorFrames::Park::kNone:
				break;
			case ArmorFrames::Park::kPlace:
				{
					const auto interior = parked->cell ? RE::TESForm::GetFormByID<RE::TESObjectCELL>(parked->cell) : nullptr;
					const auto world = parked->cell ? nullptr : RE::TESForm::GetFormByID<RE::TESWorldSpace>(parked->worldspace);
					const auto handle = PlaceCopy(base, interior, world, at, heading);
					if (!handle) {
						break;
					}
					a_player.parkedCopy = handle;
					a_player.parkedPlacedAt = a_now;
					a_player.parkedCollisionOff = nullptr;
					++parkedPlaced;
					REX::INFO("RemotePlayers: player {} left their frame (base {:08X}, ref {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X}; a copy stands there", a_player.id, parked->base, parked->ref, at.x, at.y, at.z,
						parked->cell ? parked->cell : parked->worldspace);
				}
				break;
			case ArmorFrames::Park::kAdopt:
				{
					const auto left = a_player.frame.get();
					REX::INFO("RemotePlayers: player {}'s frame copy {:08X}, left where their stand-in stood, is moved {:.0f} units to where their frame stands", a_player.id, left->GetFormID(), left->data.location.GetDistance(at));
					a_player.parkedCopy = a_player.frame;
					a_player.parkedPlacedAt = a_player.framePlacedAt;
					a_player.parkedCollisionOff = a_player.frameCollisionOff;
					a_player.frame = {};
					a_player.frameCollisionOff = nullptr;
					++parkedPlaced;
				}
				break;
			case ArmorFrames::Park::kMove:
				if (facts.distance > 10.0f) {
					REX::INFO("RemotePlayers: player {}'s parked frame copy {:08X} is moved {:.0f} units to where their frame stands", a_player.id, copy->GetFormID(), facts.distance);
				}
				copy->data.location.x = at.x;
				copy->data.location.y = at.y;
				copy->data.location.z = at.z;
				copy->data.angle = { 0.0f, 0.0f, heading };
				copy->Update3DPosition(true);
				break;
			case ArmorFrames::Park::kShow:
				SetShown(copy.get(), true);
				SetCollision(a_player.parkedCollisionOff, copy.get(), NobodyIn(copy.get()));
				break;
			case ArmorFrames::Park::kReplace:
				REX::INFO("RemotePlayers: player {}'s parked frame copy {:08X} (3d={}) is {:.0f} units from where their frame stands{}; placing it again", a_player.id, copy->GetFormID(), facts.loaded, facts.distance,
					!facts.sameBase ? ", of another frame" : facts.samePlace && !*facts.samePlace ? ", in another place" : "");
				DeleteParked(a_player);
				break;
			case ArmorFrames::Park::kDelete:
				REX::INFO("RemotePlayers: player {}'s parked frame copy {:08X} goes: {}", a_player.id, copy->GetFormID(),
					!facts.told ? "they are in their armor, or left no frame" : facts.oursThere ? "our own copy of that frame, or the one their game shares, stands there" : "it is out of our sight");
				DeleteParked(a_player);
				break;
			case ArmorFrames::Park::kAbandon:
				REX::INFO("RemotePlayers: someone here got into player {}'s parked frame copy {:08X}; it is theirs now", a_player.id, copy->GetFormID());
				a_player.parkedTaken = parked;
				a_player.parkedCopy = {};
				a_player.parkedCollisionOff = nullptr;
				break;
			case ArmorFrames::Park::kDropLeft:
				REX::INFO("RemotePlayers: player {} left their frame where our own copy of it stands; the copy left where their stand-in stood goes", a_player.id);
				DeleteFrame(a_player, a_standIn);
				break;
			}
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

		// What a stand-in's combat is against.
		StandInCalm::Foe FoeOf(const RE::Actor* a_actor, const RE::PlayerCharacter* a_us)
		{
			const auto target = a_actor->currentCombatTarget.get();
			if (!target) {
				return StandInCalm::Foe::kNobody;
			}
			if (target.get() == a_us) {
				return StandInCalm::Foe::kUs;
			}
			return Puppets::IsPuppet(target.get()) ? StandInCalm::Foe::kStandIn : StandInCalm::Foe::kOther;
		}

		// A stand-in never fights us or another stand-in (StandInCalm): it runs no AI, so a fight the engine starts on
		// it (our punch, our stray shot) never ends, and kept us in combat for good. Its anger with us is cleared, and
		// such a fight is ended on it and on everyone in it with it (the console's stopcombatalarm on it). For a
		// stand-in in use (a_track its player's) and one kept in the graveyard for the player's conversation alike
		// (a_track the grave's): that one stands for up to a minute, hidden, with the same AI off.
		void KeepCalm(StandInCalm::Track& a_track, std::uint32_t a_playerId, RE::Actor* a_actor, const RE::PlayerCharacter* a_us, Clock::time_point a_now)
		{
			using enum RE::Actor::BOOL_FLAGS;
			const bool               inCombat = a_actor->IsInCombat();
			const StandInCalm::Facts facts{
				.inCombat = inCombat,
				.target = inCombat ? FoeOf(a_actor, a_us) : StandInCalm::Foe::kNobody,
				.angry = a_actor->boolFlags.any(kAngryWithPlayer, kAttackOnSight),
			};
			const auto calm = StandInCalm::Frame(a_track, ours, facts, a_now);
			if (calm.forgive) {
				a_actor->boolFlags.reset(kAngryWithPlayer, kAttackOnSight);
			}
			if (!calm.stop) {
				return;
			}
			a_actor->StopCombat();
			RE::Console::ExecuteCommand(std::format("{:08X}.stopcombatalarm", a_actor->GetFormID()).c_str());
			++calmed;
			if (MoveSync::Loggable(a_track.stops)) {
				REX::INFO("RemotePlayers: player {} stand-in {:08X} was in combat with {}; ended it ({} on this stand-in)", a_playerId, a_actor->GetFormID(),
					facts.target == StandInCalm::Foe::kUs ? "us" : "another stand-in", a_track.stops);
			}
		}

		// Someone besides a stand-in is in combat with us: a real fight, which is left alone.
		bool RealFoe(const RE::PlayerCharacter* a_us)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return true;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor || actor.get() == a_us || actor->IsDead(false) || Puppets::IsPuppet(actor.get()) || !actor->IsInCombat()) {
					continue;
				}
				if (actor->currentCombatTarget.get().get() == a_us) {
					return true;
				}
			}
			return false;
		}

		// We are still in combat a moment after a stand-in's fight with us was ended, and nothing real fights us: the
		// combat it left us in is ended too (the console's stopcombatalarm on us), once (StandInCalm::OursDue).
		void KeepUsCalm(RE::PlayerCharacter* a_us, Clock::time_point a_now)
		{
			if (!a_us || !ours.stoppedAt) {
				return;
			}
			const bool inCombat = a_us->IsInCombat();
			if (StandInCalm::OursDue(ours, inCombat, inCombat && RealFoe(a_us), a_now)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.stopcombatalarm", a_us->GetFormID()).c_str());
				REX::INFO("RemotePlayers: we were still in combat after a stand-in's fight with us was ended, with nobody else fighting us; ended ours ({} so far)", ours.clears);
			}
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
			// Their own face, hair and body shape on a copy of that base (made once per look): with their own head, which
			// the engine builds as the stand-in loads, unless that was given up for them (faceFallback; see FaceWatch).
			if (a_player.face && !a_player.faceTried) {
				a_player.faceTried = true;
				auto       parts = a_player.faceFallback ? Face::BODY_ONLY : Face::Parts();
				const auto made = Face::MakeBase(npc, *a_player.face, parts);
				a_player.faceBaseId = made ? made->GetFormID() : 0;
				a_player.faceOwnHead = made && Face::OwnHead(parts);
			}
			if (a_player.faceBaseId) {
				const auto faceBase = RE::TESForm::GetFormByID<RE::TESNPC>(a_player.faceBaseId);
				if (faceBase && (!a_player.faceOwnHead || Face::MarkCopy(faceBase))) {
					npc = faceBase;
				} else {
					a_player.faceBaseId = 0;  // gone with a load: made again next time
					a_player.faceTried = false;
					a_player.faceOwnHead = false;
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
			SaveGuard::KeepUnpromoted(actor);
			// Enemies treat the stand-in like the player (it's what NPCs in this world can attack), hitting it isn't a
			// crime, and it neither turns on us when we hit it nor starts or joins a fight (StandInCalm).
			for (const auto& command : StandInCalm::SetupCommands(actor->GetFormID())) {
				RE::Console::ExecuteCommand(command.c_str());
			}
			if (const auto values = RE::ActorValue::GetSingleton()) {
				const auto owner = static_cast<RE::ActorValueOwner*>(actor);
				if (values->aggression) {
					owner->SetActorValue(*values->aggression, StandInCalm::AGGRESSION);
				}
				if (values->assistance) {
					owner->SetActorValue(*values->assistance, StandInCalm::ASSISTANCE);
				}
			}
			// Their LooksMenu body sliders and overlays, when both games have LooksMenu (LooksMenu keeps them by reference,
			// not on the base: so on each stand-in).
			if (a_player.face) {
				LooksMenu::Apply(actor, a_player.face->female, a_player.face->bodyMorphs, a_player.face->overlays);
			}

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

	void Rename(std::uint32_t a_id, std::string a_name)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.name = std::move(a_name);  // the tag follows on the next Update (NameTag differs from shownName)
		}
	}

	void Remove(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			DeleteActor(it->second);
			DeleteParked(it->second);
			ShowOriginal(it->second);
			players.erase(it);
		}
	}

	void RemoveAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
			DeleteParked(player);
			ShowOriginal(player);
		}
		players.clear();
	}

	namespace
	{
		void TakeState(std::uint32_t a_id, const Protocol::PlayerState& a_state)
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

		// Dev: friends' states held back as a worse connection would (NetSim.h, dev command `netsim`), let in each frame.
		NetSim::Line<std::pair<std::uint32_t, Protocol::PlayerState>> netSim;

		void ReleaseNetSim(Clock::time_point a_now)
		{
			netSim.Release(a_now, [](const auto& a_held) { TakeState(a_held.first, a_held.second); });
		}
	}

	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state)
	{
		if (netSim.settings.Active()) {
			netSim.Push({ a_id, a_state }, Clock::now());
			return;
		}
		TakeState(a_id, a_state);
	}

	void SetNetSim(const NetSim::Settings& a_settings)
	{
		netSim.settings = a_settings;
		if (!a_settings.Active()) {
			netSim.Flush([](const auto& a_held) { TakeState(a_held.first, a_held.second); });
		}
	}

	std::string DescribeNetSim()
	{
		return std::format("{}; waiting={} lost={} spikes={}", netSim.settings.Describe(), netSim.Waiting(), netSim.lost, netSim.spikes);
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
		player.faceOwnHead = false;
		// (faceFallback stays: a new look is no new engine.)
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
			Info info{ id, remote.name, std::nullopt, GetActor(remote), remote.menu };
			if (!remote.states.snapshots.empty()) {
				info.state = remote.states.snapshots.back().state;
			}
			out.push_back(std::move(info));
		}
		return out;
	}

	bool Any()
	{
		return !players.empty();
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

	void DespawnAll(bool a_deleteNow)
	{
		// A frame of ours hidden while a friend wears theirs stays hidden: only its 3D was touched, which no save keeps.
		// Shown here, it stood in the world again until the next check put it away, and counted as hidden anew.
		for (auto& [id, player] : players) {
			DeleteActor(player);
			DeleteParked(player);  // placed again after the save, where their frame still stands
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
		// Before a save: whatever still stands in the graveyard is disabled now, and deleted by Update once
		// DeferredDelete::MayDelete lets it (one disabled long enough ago goes now). Disabled and deleted in one
		// step here, the physics jobs of the frames after the save still handled a collision with the freed
		// stand-in and crashed (CharacterCollisionHandler -> Actor::GetDesiredProcessLevel -> Get3D, VM
		// 2026-10-20, on `save` with a friend's stand-in just respawned beside the player). A reference whose 3D
		// is still loading is left alone: deleting it crashes the loader when it finishes. None of them may be
		// written into the save, deleted or not (a stand-in saved as deleted comes back from the save in no
		// cell, and every later save crashed on it): Exclude drops each one's record. A stand-in kept for the
		// player's conversation goes too: should it be the speaker, Dialogue ends the conversation it leaves open.
		const auto now = Clock::now();
		std::erase_if(graveyard, [&](Grave& a_grave) {
			const auto ref = a_grave.actor.get();
			if (!ref) {
				SaveGuard::Forget(a_grave.id);
				Release(a_grave, nullptr);
				return true;
			}
			if (!a_grave.disabledAt && !DeferredDelete::Safe(ref->Get3D() != nullptr, now - a_grave.spawnedAt)) {
				SaveGuard::Exclude(ref.get());
				return false;
			}
			if (!a_grave.disabledAt) {
				Disable(a_grave, ref.get(), now);
			}
			++flushedBeforeSave;
			if (!a_deleteNow && !DeferredDelete::MayDelete(now - *a_grave.disabledAt)) {
				SaveGuard::Exclude(ref.get());
				return false;
			}
			ref->SetDelete(true);
			SaveGuard::Exclude(ref.get());
			deleting.push_back({ a_grave.actor, a_grave.id });
			return true;
		});
	}

	void Discard(RE::ObjectRefHandle a_ref, Clock::time_point a_placedAt)
	{
		// A frame copy (Spawns' copy of a friend's frame) goes as every frame of ours does: one taken over from the parked
		// copy may be the stand-in's own, which the engine promoted when it got into it (BuryFrame).
		if (const auto ref = a_ref.get(); ref && Equipment::IsPowerArmorFrame(ref->GetObjectReference())) {
			BuryFrame(ref.get(), a_placedAt);
			return;
		}
		Bury({ .actor = a_ref, .spawnedAt = a_placedAt });
	}

	RE::ObjectRefHandle PlaceFrameCopy(std::uint32_t a_base, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading)
	{
		const auto base = RE::TESForm::GetFormByID<RE::TESFurniture>(a_base);
		if (!a_cell || !Equipment::IsPowerArmorFrame(base)) {
			return {};
		}
		return PlaceCopy(base, a_cell->IsInterior() ? a_cell : nullptr, a_cell->IsInterior() ? nullptr : a_cell->worldSpace, a_at, a_heading);
	}

	RE::ObjectRefHandle TakeParkedCopy(std::uint32_t a_owner, std::uint32_t a_base, const RemoteEdits::Spot& a_spot)
	{
		const auto it = players.find(a_owner);
		if (it == players.end()) {
			return {};
		}
		auto&      player = it->second;
		const auto copy = player.parkedCopy.get();
		const auto base = copy && !copy->IsDeleted() ? copy->GetObjectReference() : nullptr;
		if (!base || base->GetFormID() != a_base || OurFrame(copy.get()) || FrameInUse(copy.get())) {
			return {};
		}
		const auto from = DistanceFrom(copy.get(), { a_spot.cell, a_spot.worldspace, { a_spot.x, a_spot.y, a_spot.z } });
		if (!from || !(*from <= ArmorFrames::ORIGINAL_RADIUS)) {
			return {};
		}
		// Drawn and solid from now on, as a copy just placed is (solid once nobody stands in it).
		SetShown(copy.get(), true);
		RestoreCollision(player.parkedCollisionOff, copy.get());
		REX::INFO("RemotePlayers: player {}'s parked frame copy {:08X} is now our copy of the frame their game shares", a_owner, copy->GetFormID());
		const auto handle = player.parkedCopy;
		player.parkedCopy = {};
		player.parkedCollisionOff = nullptr;
		return handle;
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
		// stand-in among the graveyard's is no longer a puppet either (forgotten without reading it). The load
		// took them, not the plugin: no conversation of theirs is the plugin's to end (see Release).
		for (const auto& grave : graveyard) {
			if (grave.puppet) {
				Puppets::Unregister(grave.puppet, grave.puppetId);
			}
		}
		graveyard.clear();
		deleting.clear();
		pendingSolid.clear();
		ourHead = {};  // the head the loaded save gives our player is its own
		for (auto& [id, player] : players) {
			player.faceBaseId = 0;
			player.faceTried = false;
			player.faceOwnHead = false;
			player.faceWatch = {};
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
			player.parkedCopy = {};  // went before the load too (DespawnAll); placed again where their frame stands
			player.parkedCollisionOff = nullptr;
			player.parkedTaken.reset();
			player.switchTries = 0;
			player.switchGaveUp = false;
			player.pickSince.reset();
			player.keyPutAt = {};
			ForgetSeat(player);  // our copy of their furniture is looked for again in the loaded world
		}
	}

	void SetLight(std::uint32_t a_id, bool a_on)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.lightOn = a_on;  // Update() keeps the light with the stand-in
		}
	}

	void SetMenu(std::uint32_t a_id, std::uint8_t a_menu)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.menu = a_menu;  // Update() raises their Pip-Boy, and the tag follows (NameTag differs from shownName)
		}
	}

	std::uint32_t ActorIdOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		const auto actor = it != players.end() ? it->second.actor.get() : nullptr;
		return actor ? actor->GetFormID() : 0;
	}

	void SetPowerArmor(std::uint32_t a_id, bool a_on, std::uint32_t a_frameBase, std::uint32_t a_frameRef, const std::optional<Protocol::ParkedFrame>& a_parked)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			auto& player = it->second;
			// A pre-placed frame they have just got into: where they are now is where they got in (their status names the
			// frame as they climb in), and ours is looked at afresh for this wear (see ArmorFrames::ChooseOriginal). Where
			// they are is their newest state if it came just now, else the next one. Ours hidden while theirs stood parked
			// elsewhere, and they got into that one, stays hidden wherever they got in (ArmorFrames::OriginalFacts::away).
			if (a_on && a_frameBase && a_frameRef != 0 && (!player.powerArmorOn || a_frameRef != player.powerArmorFrameRef)) {
				// Hidden as their parked suit, or to be once its 3D comes (we have not been near it since they parked theirs).
				const auto hidden = player.original.get();
				const bool stillHidden = hidden && hidden->GetFormID() == a_frameRef;
				const bool hidesWhenLoaded = player.originalParked && player.originalChoice == static_cast<int>(ArmorFrames::Original::kNotLoaded);
				player.originalAway = player.parked && player.parked->ref == a_frameRef && (stillHidden || hidesWhenLoaded);
				player.wornAt.reset();
				player.wornAtWanted = true;
				player.originalMoved = false;
				player.originalChoice = -1;
				player.originalCounted = player.originalAway && stillHidden;  // still the hide counted as theirs was parked
				if (!player.states.snapshots.empty() && ArmorFrames::FreshEnough(Clock::now() - player.states.arrived)) {
					TakeWornAt(player, player.states.snapshots.back().state);
				}
			} else if (!a_on) {
				player.wornAtWanted = false;
			}
			// Where the frame they got out of last stands (see UpdateParked). A new wear lets any frame be shown parked again.
			player.parked = a_on ? std::nullopt : a_parked;
			if (a_on) {
				player.parkedTaken.reset();
			}
			if (a_on != player.powerArmorOn) {
				StandInGear::Redress(player.gear);  // dressed again (see Outfit)
				player.switchTries = 0;             // a new wear: the stand-in is tried afresh
				player.switchGaveUp = false;
				player.pickSince.reset();
				player.keyPutAt = {};
				player.pickLogged = -1;
			}
			// The frame is kept until they tell another (see ArmorFrames::Hear).
			const auto heard = ArmorFrames::Hear({ player.powerArmorOn, player.powerArmorFrame, player.powerArmorFrameRef }, a_on, a_frameBase, a_frameRef);
			player.powerArmorOn = heard.on;
			player.powerArmorFrame = heard.frame;
			player.powerArmorFrameRef = heard.frameRef;
		}
	}

	void SetSeat(std::uint32_t a_id, const std::optional<Protocol::Seat>& a_seat)
	{
		if (const auto it = players.find(a_id); it != players.end() && it->second.seat != a_seat) {
			it->second.seat = a_seat;
			ForgetSeat(it->second);  // other furniture, or none: looked for and tried afresh (see UpdateSeat)
		}
	}

	void SetLightForm(std::uint32_t a_formId)
	{
		friendLightForm = a_formId;
		for (auto& [id, player] : players) {
			DeleteLight(player);  // placed again with the new form
		}
	}

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

	void RebuildFaces()
	{
		for (auto& [id, player] : players) {
			player.faceBaseId = 0;
			player.faceTried = false;
			player.faceOwnHead = false;
			player.faceFallback = false;  // the dev asked for these pieces: their own head gets its chances again
			DeleteActor(player);
		}
	}

	void Update()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto localCell = player ? player->GetParentCell() : nullptr;
		const auto ui = RE::UI::GetSingleton();
		const bool loading = Hud::LoadingScreenUp();

		const auto now = Clock::now();
		ReleaseNetSim(now);
		const auto playedNow = played.Advance(now, loading || (ui && ui->menuMode > 0));
		// Our own player's head, for FaceWatch: missing from a loaded 3D (a loading screen takes all 3D; not that).
		const bool oursGone = player && !loading && player->Get3D() && !Face::HasHead(player);
		// What waits in the graveyard, or to be let go of by the engine, and has lost its cell has its record dropped
		// each frame: any change the engine makes to it makes a new one, and the engine unloading it would write that
		// out and crash on the cell it no longer has.
		std::erase_if(graveyard, [&](Grave& a_grave) {
			const auto actor = a_grave.actor.get();
			if (!actor) {
				SaveGuard::Forget(a_grave.id);
				Release(a_grave, nullptr);
				return true;
			}
			if (!actor->GetParentCell()) {
				SaveGuard::ExcludeOurs(actor.get());
			}
			if (!a_grave.disabledAt) {
				if (!DeferredDelete::Safe(actor->Get3D() != nullptr, now - a_grave.spawnedAt) || HeldForConversation(a_grave, actor.get(), now) ||
					WaitsForWearer(a_grave, actor.get())) {
					// A stand-in still standing (kept for the player's conversation, mostly) is kept calm like one in use:
					// with its AI off, a fight on it would never end, and would keep us in combat while it waits.
					if (const auto standIn = a_grave.puppet ? actor->As<RE::Actor>() : nullptr) {
						KeepCalm(a_grave.calm, a_grave.player, standIn, player, now);
					}
					return false;
				}
				Disable(a_grave, actor.get(), now);
				return false;
			}
			if (!DeferredDelete::MayDelete(now - *a_grave.disabledAt)) {
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
			// The newest state's own speed, as for movingFrames: Sample stops a long hold's speed, and those are the holds to count.
			heldFrames += sampled && sampled->held && snaps.back().state.speed > 0.0f && now - remote.states.arrived < 1s;
			aheadFrames += sampled && sampled->ahead && now - remote.states.arrived < 1s;
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

			if (const auto parked = remote.parkedCopy.get(); Adrift(parked.get(), remote.parkedPlacedAt, now)) {
				++adrift;
				DeleteParked(remote);
			}

			// Before the stand-in is looked at: a friend out of sight can still be wearing our copy of a frame, or have left
			// their frame parked where we are.
			UpdateOriginal(remote, now);
			UpdateParked(remote, GetActor(remote), localCell, player, loading, now);

			auto actor = GetActor(remote);
			// A loading screen (a door, a coc, a fast travel): a stand-in in furniture gets up while its cell is still there
			// (see DeleteActor). Its friend still in it, it sits down again once it is placed.
			if (actor && loading && remote.seatHeld) {
				Puppets::StandUp(actor);
				remote.seatHeld = false;
			}
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
			// Their spot, unless it is ours: never through our own body (StandInSpace). Placed, followed by its frame copy, checked
			// for holding still and put there each frame from this.
			const auto spot = SpotOf(remote, *state, player);

			if (actor && look.left != StandInRange::Left::kKeep) {
				++leftBehind;
				respawnWaits += look.left == StandInRange::Left::kReplaceLater;
				DeleteActor(remote);
				actor = nullptr;
			}

			// Out of their armor while the stand-in is still in its frame: it goes, and a fresh one is placed just below. So
			// does one the engine has in its frame but never gave the power armor skeleton (the rebuild the switch-in queued
			// did not come): the fresh one is tried afresh, and the tries of this wear add up (ArmorFrames::MAX_SWITCH_TRIES).
			// So does one in its frame on the power armor skeleton whose legs stay still while they walk in our sight: its
			// graph plays nothing (ArmorFrames::STILL_LIMIT).
			if (actor) {
				UpdateStride(remote, actor, *state, player, playedNow);
				switch (ArmorFrames::ChooseSwitch(SwitchFactsOf(remote, actor, now, playedNow))) {
				case ArmorFrames::Switch::kOut:
					LeaveArmor(remote, actor, localCell, now, playedNow);
					actor = nullptr;
					break;
				case ArmorFrames::Switch::kStuck:
					++remote.switchTries;
					++switchFails;
					REX::WARN("RemotePlayers: player {} stand-in {:08X} is still on its own skeleton {} ms after it was switched into its frame copy; placed again (try {} of {})", remote.id, actor->GetFormID(),
						std::chrono::duration_cast<std::chrono::milliseconds>(playedNow - remote.switchedAt).count(), remote.switchTries, ArmorFrames::MAX_SWITCH_TRIES);
					DeleteActor(remote);
					actor = nullptr;
					break;
				case ArmorFrames::Switch::kStill:
					++remote.switchTries;
					++switchFails;
					++stillStandIns;
					REX::WARN("RemotePlayers: player {} stand-in {:08X} in its frame plays no walk: its legs stayed still {} ms while they walked; placed again (try {} of {}; {})", remote.id, actor->GetFormID(),
						std::chrono::duration_cast<std::chrono::milliseconds>(remote.stride.Still()).count(), remote.switchTries, ArmorFrames::MAX_SWITCH_TRIES, GraphInfo::Describe(actor));
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
				auto placed = *state;
				placed.x = spot.x;
				placed.y = spot.y;
				placed.z = spot.z;
				actor = Spawn(remote, placed, localCell);
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
				remote.calm = {};
				// Still in their armor (it was replaced through a door, after a load, with a new look, or for a skeleton that
				// never came): this one goes into a copy as soon as both are loaded, at once. The tries that did not take in
				// this wear are kept: placed again and again for a rebuild that never comes, it would never give up.
				remote.switchTriedAt = {};
				remote.switchedAt = {};
				remote.armorSkeleton = nullptr;
				remote.stride = {};
				remote.switchGaveUp = false;
				remote.pickSince.reset();
				remote.keyPutAt = {};
				remote.pickLogged = -1;
				if (remote.faceOwnHead) {
					FaceWatch::Placed(remote.faceWatch, now);
				}
			}

			// Placed with their own head: the engine builds it as the stand-in loads, or is asked once more, or their head
			// is given up here, for the settler's (FaceWatch.h). Our own head is watched meanwhile: it once went with such a
			// copy. In power armor its 3D is the frame's (ArmorFrames), not looked at.
			if (remote.faceOwnHead) {
				FaceWatch::Facts facts;
				facts.has3D = actor->Get3D() != nullptr;
				facts.hasFace = facts.has3D && Face::HasHead(actor);
				facts.busy = Equipment::InPowerArmor(actor) || remote.inPowerArmor;
				facts.oursGone = oursGone;
				const auto outcome = FaceWatch::Frame(remote.faceWatch, facts, now);
				switch (outcome.then) {
				case FaceWatch::Then::kRebuild:
					REX::INFO("RemotePlayers: player {} stand-in {:08X} loaded with no head on it; asked the engine to build its 3D again", remote.id, actor->GetFormID());
					Face::Rebuild(actor);
					break;
				case FaceWatch::Then::kGiveUp:
					Face::GaveUp(remote.id, outcome.why == FaceWatch::Why::kOurs);
					if (outcome.why == FaceWatch::Why::kOurs) {
						FaceWatch::TakenBy(ourHead, now);
					}
					remote.faceFallback = true;
					remote.faceBaseId = 0;  // a copy with the settler's face is made at the next placement
					remote.faceTried = false;
					remote.faceOwnHead = false;
					DeleteActor(remote);
					continue;
				case FaceWatch::Then::kBuilt:
					Face::Built(remote.id, actor->GetFormID());
					break;
				case FaceWatch::Then::kNothing:
					break;
				}
			}

			UpdateLight(remote, actor, localCell, now);
			UpdatePowerArmor(remote, actor, localCell, spot, state->heading, now, playedNow);
			// Held for their furniture, it is not where its target is: no hold to check.
			if (UpdateSeat(remote, actor, spot, now, playedNow)) {
				remote.lastTarget.reset();
			} else {
				CheckHold(remote, actor, spot, sampled->OwnSpeed(), now);
			}

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
			const auto dress = ArmorFrames::ChooseDress(inArmor, inArmor && OnArmorSkeleton(remote, actor, playedNow));
			const auto shown = [&] { return dress == ArmorFrames::Dress::kArmor ? Kept(Equipment::Read(actor), dress) : Equipment::Read(actor); };
			const auto dressing = StandInGear::Dress(remote.gear, dress, actor->Get3D() != nullptr, now, HandWeapon,
				[&](const std::vector<std::uint32_t>& a_worn) { return Wearable(shown()) == Wearable(Kept(a_worn, dress)); });
			switch (dressing.put) {
			case StandInGear::Put::kApply:
				Equipment::Apply(actor, Outfit(remote, dress), remote.gear.mods);
				break;
			case StandInGear::Put::kWear:
				if (Equipment::WearOnStandIn(actor, Outfit(remote, dress), remote.gear.mods)) {
					StandInGear::TookAway(remote.gear, now);  // the gun's new copy goes on in a moment
				}
				break;
			case StandInGear::Put::kWearInArmor:
				if (Equipment::WearInArmor(actor, Outfit(remote, dress), remote.gear.mods)) {
					StandInGear::TookAway(remote.gear, now);
				}
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

			// In their Pip-Boy: the stand-in checks its own wrist, again and again, and lowers it as they close theirs
			// (StandInPipboy). Only standing on its own feet and still: not in its frame or furniture, not down.
			StandInPipboy::Facts pipboyFacts;
			pipboyFacts.inPipboy = remote.menu == Protocol::MenuKind::kPipboy;
			pipboyFacts.standIn = actor->GetFormID();
			pipboyFacts.ready = actor->Get3D() && !inArmor && !remote.inPowerArmor && !remote.powerArmorOn && !remote.seat && !remote.seatHeld && !remote.downed &&
			                    remote.health > 0 && state->speed < 1.0f;
			switch (StandInPipboy::Frame(remote.pipboy, pipboyFacts, now)) {
			case StandInPipboy::Do::kRaise:
				{
					const auto npc = actor->GetNPC();
					RE::Console::ExecuteCommand(std::format("{:08X}.playidle {}", actor->GetFormID(), StandInPipboy::RaiseFor(npc && npc->IsFemale())).c_str());
				}
				break;
			case StandInPipboy::Do::kLower:
				RE::Console::ExecuteCommand(std::format("{:08X}.playidle {}", actor->GetFormID(), StandInPipboy::LOWER).c_str());
				break;
			case StandInPipboy::Do::kNothing:
				break;
			}

			KeepCalm(remote.calm, remote.id, actor, player, now);

			// Pushed off our own spot (StandInSpace): its path, which the engine's update goes by, is moved as far.
			auto path = sampled->path;
			for (std::uint8_t i = 0; i < path.count; ++i) {
				path.points[i].x += spot.x - state->x;
				path.points[i].y += spot.y - state->y;
				path.points[i].z += spot.z - state->z;
			}
			Puppets::SetTarget(actor, Puppets::Motion{
				.position = spot,
				.heading = state->heading,
				.speed = state->speed,
				.direction = Protocol::UnpackDirection(state->direction),
				.pitch = sampled->pitch,
				.moveMode = state->moveMode,
				.flags = state->flags,
				.path = path,
			});
		}
		KeepUsCalm(player, now);
		// Our head still gone a moment after the stand-in that took it went: asked back, once.
		if (FaceWatch::RepairDue(ourHead, oursGone, now)) {
			Face::RepairOurs();
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
				armor = std::format(" in={} skel={} tries={} pieces={}/{} copy={} stillMs={}", in, Equipment::HasPowerArmorSkeleton(actor), remote.switchTries, pieces(Equipment::Read(actor)), pieces(remote.gear.items),
					!frame ? "none" : engaged ? "worn" : !root ? "loading" : root->GetAppCulled() ? "hidden" : "shown",
					std::chrono::duration_cast<std::chrono::milliseconds>(remote.stride.Still()).count());
			}
			// Their frame when every game has it, and whether ours is hidden while they wear theirs.
			if (remote.powerArmorFrameRef || remote.original) {
				const auto original = remote.original.get();
				armor += std::format(" frameRef={:08X} oursHidden={}", remote.powerArmorFrameRef, original != nullptr);
			}
			// The furniture they use: theirs (reference, base), ours of it, and whether the stand-in is held for it and in it.
			if (remote.seat) {
				const auto ourSeat = remote.seatRef.get();
				const auto seatIn = actor ? Puppets::FurnitureOf(actor) : RE::NiPointer<RE::TESObjectREFR>{};
				armor += std::format(" seat={:08X}/{:08X} ours={:08X} held={} in={:08X} tries={}", remote.seat->ref, remote.seat->base, ourSeat ? ourSeat->GetFormID() : 0, remote.seatHeld, seatIn ? seatIn->GetFormID() : 0, remote.seatTries);
			}
			// The menu they are in, and the Pip-Boy raises (and early lowers) played on their stand-ins.
			if (remote.menu != Protocol::MenuKind::kNone || remote.pipboy.raises) {
				armor += std::format(" menu={} pipboyRaises={} lowers={} raised={}", remote.menu, remote.pipboy.raises, remote.pipboy.lowers, remote.pipboy.raised);
			}
			out += std::format("{}[{} '{}' puppet={:08X} x={:.0f} y={:.0f} cell={:08X} ws={:08X} gapMs={} delayMs={} pa={}/{} frame={:08X} offSpot={}/{} worst={:.1f}{}]",
				out.empty() ? "" : " ", id, remote.name, actor ? actor->GetFormID() : 0,
				last.x, last.y, last.cell, last.worldspace, gap, static_cast<int>(remote.states.delay * 1000.0f), remote.powerArmorOn, remote.inPowerArmor, remote.powerArmorFrame,
				remote.hold.off, remote.hold.frames, remote.hold.worst, armor);
		}
		if (originalsHidden) {
			out += std::format(" originalsHidden={}", originalsHidden);
		}
		if (parkedPlaced) {
			out += std::format(" parkedFrames={}", parkedPlaced);
		}
		if (seatsEntered || seatsMissed || seatsLeft) {
			out += std::format(" seatsIn={} seatsMissed={} seatsLeft={}", seatsEntered, seatsMissed, seatsLeft);
		}
		if (switchesIn || switchesOut || switchFails) {
			out += std::format(" armorIn={} armorOut={} armorFails={} armorStill={}", switchesIn, switchesOut, switchFails, stillStandIns);
		}
		if (movingFrames) {
			out += std::format(" held={}/{} ahead={}", heldFrames, movingFrames, aheadFrames);
			out += std::format(" lights={} frames={}/{} spawns={} leftBehind={} downed={} respawnWaits={} adrift={} graveyard={}/{}", lightsPlaced, framesPlaced, framesFollowed, spawns, leftBehind, downedStandIns, respawnWaits, adrift,
				graveyard.size(), flushedBeforeSave);
		}
		if (spacedFrames) {
			out += std::format(" spaced={}", spacedFrames);
		}
		if (equipmentFixesTotal) {
			out += std::format(" gearFixes={}", equipmentFixesTotal);
		}
		if (gearChanges || redraws) {
			out += std::format(" gearChanges={} redraws={}", gearChanges, redraws);
		}
		if (calmed || ours.clears) {
			out += std::format(" calmed={} oursCalmed={}", calmed, ours.clears);
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
