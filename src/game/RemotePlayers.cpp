#include "game/RemotePlayers.h"
#include "game/ArmorFrames.h"
#include "game/DeferredDelete.h"
#include "game/Dialogue.h"
#include "game/EngineRefs.h"
#include "game/SaveGuard.h"
#include "game/SaveStrays.h"
#include "game/Prediction.h"
#include "game/StandInHold.h"

#include "game/Face.h"
#include "Config.h"
#include "game/Equipment.h"
#include "game/Puppets.h"
#include "game/WeaponFire.h"

namespace RemotePlayers
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Render remote players slightly in the past so there are always two states to blend.
		// States arrive about every 33 ms; two of them, plus slack for the relay and network jitter.
		// That is the least: a slow game reads the network once a frame, so it needs one of its frames
		// more, and a friend in a slow game sends less often. Each player's delay is eased toward one gap
		// between their states plus one of our frames plus slack, so the render time never jumps.
		constexpr auto  INTERPOLATION_DELAY = 70ms;
		constexpr auto  MAX_INTERPOLATION_DELAY = 250ms;
		constexpr float DELAY_SLACK = 0.02f;  // seconds
		constexpr float DELAY_SLEW = 0.002f;  // seconds per frame
		float             frameSeconds = 1.0f / 60.0f;  // ours, smoothed
		Clock::time_point lastFrame{};
		constexpr auto STALE_AFTER = 5s;
		constexpr std::size_t MAX_SNAPSHOTS = 32;

		// Exterior puppets further than this are outside the loaded area and are despawned.
		constexpr float MAX_EXTERIOR_DISTANCE = 9000.0f;

		// The game sometimes puts the NPC's own outfit back on a stand-in (when its inventory is
		// set up late, or its 3D reloads). What it wears is checked this often and put right.
		constexpr auto EQUIPMENT_CHECK_INTERVAL = 3s;
		constexpr int  MAX_EQUIPMENT_FIXES = 3;  // in a row without it sticking, then give up

		struct Snapshot
		{
			Clock::time_point     received;
			Protocol::PlayerState state;
		};

		// A place as a friend's state names it.
		struct Spot
		{
			std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
			std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
			RE::NiPoint3  at;
		};

		struct RemotePlayer
		{
			std::string          name;
			std::uint32_t        appearance = 0;
			std::vector<std::uint32_t> equipment;
			std::optional<Protocol::Face> face;
			std::uint32_t        faceBaseId = 0;  // our copy of the base NPC wearing their face (a runtime form; resolved at use, it may not survive a load)
			bool                 faceTried = false;
			bool                 hasEquipment = false;
			bool                 equipmentApplied = false;
			Clock::time_point    equipmentCheckAt{};
			int                  equipmentFixes = 0;
			std::deque<Snapshot> snapshots;
			Prediction::ArrivalClock arrivals;     // even stamps for states read in bursts
			float                delay = 0.07f;  // seconds behind now it is rendered
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
			Clock::time_point    lightPlacedAt{};
			Clock::time_point    spawnedAt{};  // when the current stand-in was placed
			bool                 powerArmorOn = false;   // they are in power armor
			std::uint32_t        powerArmorFrame = 0;    // the base form of their frame
			std::uint32_t        powerArmorFrameRef = 0; // their frame itself when every game has it (a pre-placed one), else 0
			bool                 inPowerArmor = false;   // the stand-in is shown in its armor, its frame copy kept under it (see ArmorFrames)
			RE::ObjectRefHandle  frame;                  // the frame we placed for it
			Clock::time_point    framePlacedAt{};
			Clock::time_point    frameActionAt{};        // when they last got out (the copy stands where they did a while)
			const RE::NiAVObject* frameCollisionOff = nullptr;  // the frame's 3D whose collision we turned off; only compared
			std::optional<RE::NiPoint3> lastTarget;      // where the stand-in was put last frame
			StandInHold::Check   hold;                   // frames it stood still somewhere else than put (net status)
			Clock::time_point    holdLoggedAt{};
			RE::ObjectRefHandle  original;               // our copy of their pre-placed frame, hidden while they wear theirs
			const RE::NiAVObject* originalHidden = nullptr;  // its 3D we stopped drawing; only compared
			const RE::NiAVObject* originalCollisionOff = nullptr;  // its 3D whose collision we turned off; only compared
			Clock::time_point    originalCheckAt{};
			int                  originalChoice = -1;    // the last ArmorFrames::Original logged
			bool                 originalCounted = false;  // a hide counted for this wear (one hidden again after a load is not a new one)
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
		};
		std::vector<Grave> graveyard;
		std::uint32_t      flushedBeforeSave = 0;
		// Marked for deletion, and still there until the engine lets go of them: a save in that moment
		// would write them too.
		std::vector<RE::ObjectRefHandle> deleting;
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

		// Disables the reference now if it is safe to, and leaves it to the graveyard to delete.
		void Bury(Grave a_grave)
		{
			const auto ref = a_grave.actor.get();
			if (!ref) {
				Release(a_grave);
				return;
			}
			const auto now = Clock::now();
			a_grave.buriedAt = now;
			if (DeferredDelete::Safe(ref->Get3D() != nullptr, now - a_grave.spawnedAt) && !HeldForConversation(a_grave, ref.get(), now)) {
				Disable(a_grave, ref.get(), now);
			}
			graveyard.push_back(a_grave);
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
		std::uint32_t   originalsHidden = 0; // our copies of pre-placed frames hidden while a friend wore theirs (once a wear)
		std::uint32_t                         equipmentFixesTotal = 0;
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

		// A power armor piece.
		bool PowerArmorPiece(std::uint32_t a_item)
		{
			const auto armor = RE::TESForm::GetFormByID<RE::TESObjectARMO>(a_item);
			return armor && armor->HasKeywordString("ArmorTypePower");
		}

		// What a stand-in should wear now: everything the owner has on, less the power armor pieces once their status
		// says they are out of their armor (their gear list can still name the pieces for a moment). In it, the pieces
		// go on at once: the engine never puts the stand-in into a frame (see ArmorFrames), and waiting for it to left
		// the friend shown without their armor.
		std::vector<std::uint32_t> Outfit(const RemotePlayer& a_player)
		{
			if (a_player.powerArmorOn) {
				return a_player.equipment;
			}
			std::vector<std::uint32_t> out;
			for (const auto item : a_player.equipment) {
				if (!PowerArmorPiece(item)) {
					out.push_back(item);
				}
			}
			return out;
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

		float LerpAngle(float a_from, float a_to, float a_t)
		{
			return a_from + StandInHold::TurnBetween(a_from, a_to) * a_t;
		}

		struct Sampled
		{
			Protocol::PlayerState state;
			float                 motionAngle = 0.0f;  // world heading of travel, radians
		};

		// Movement direction relative to facing, as a fraction of a turn in [0, 1).
		float RelativeDirection(float a_motionAngle, float a_heading)
		{
			const float rel = StandInHold::TurnBetween(a_heading, a_motionAngle) / (2.0f * std::numbers::pi_v<float>);
			return rel < 0.0f ? rel + 1.0f : rel;
		}

		// Interpolated state at (now - delay), or nullopt if we have nothing recent.
		std::optional<Sampled> Sample(const RemotePlayer& a_player, Clock::time_point a_now)
		{
			const auto& snaps = a_player.snapshots;
			if (snaps.empty() || a_now - snaps.back().received > STALE_AFTER) {
				return std::nullopt;
			}

			const auto renderTime = a_now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_player.delay));
			if (renderTime <= snaps.front().received) {
				return Sampled{ snaps.front().state, snaps.front().state.heading };
			}

			for (std::size_t i = 1; i < snaps.size(); ++i) {
				const auto& a = snaps[i - 1];
				const auto& b = snaps[i];
				if (renderTime > b.received) {
					continue;
				}

				// Don't blend across a cell or worldspace change (doors, fast travel).
				if (a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace) {
					return Sampled{ b.state, b.state.heading };
				}

				const float span = std::chrono::duration<float>(b.received - a.received).count();
				const float t = span > 0.0f ? std::chrono::duration<float>(renderTime - a.received).count() / span : 1.0f;

				auto out = b.state;
				out.x = std::lerp(a.state.x, b.state.x, t);
				out.y = std::lerp(a.state.y, b.state.y, t);
				out.z = std::lerp(a.state.z, b.state.z, t);
				out.heading = LerpAngle(a.state.heading, b.state.heading, t);

				const float dx = b.state.x - a.state.x;
				const float dy = b.state.y - a.state.y;
				const float motionAngle = dx * dx + dy * dy > 1.0f ? std::atan2(dx, dy) : out.heading;
				return Sampled{ out, motionAngle };
			}

			// Past the newest snapshot: hold position rather than guessing ahead, and stop
			// animating if updates have dried up.
			auto held = snaps.back().state;
			heldFrames += held.speed > 0.0f && a_now - snaps.back().received < 1s;
			if (a_now - snaps.back().received > INTERPOLATION_DELAY + 250ms) {
				held.speed = 0.0f;
			}
			return Sampled{ held, held.heading };
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

		// A dead puppet (killed despite protection, e.g. by a script) is replaced by a fresh one.
		bool IsBroken(RE::Actor* a_actor)
		{
			return a_actor->IsDead(false) || a_actor->IsDisabled();
		}

		// Removes the stand-in's light, once its 3D is in (the same rule as for the stand-in itself).
		void DeleteLight(RemotePlayer& a_player)
		{
			if (const auto light = a_player.light.get()) {
				Bury({ .actor = a_player.light, .spawnedAt = a_player.lightPlacedAt });
			}
			a_player.light = {};
		}

		// Two references in one place: the same cell, or both outside in the same worldspace (a neighbouring cell
		// outside is the same place: a stand-in walks on across cell borders). Not known while either is still
		// attaching (no cell yet).
		std::optional<bool> SamePlace(RE::TESObjectREFR* a_one, RE::TESObjectREFR* a_other)
		{
			const auto cell = a_one->GetParentCell();
			const auto otherCell = a_other->GetParentCell();
			if (!cell || !otherCell) {
				return std::nullopt;
			}
			return cell == otherCell || (cell->IsExterior() && otherCell->IsExterior() && cell->worldSpace == otherCell->worldSpace);
		}

		// Keeps a light at the stand-in while their Pip-Boy light is on, and removes it when it goes off.
		void UpdateLight(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now)
		{
			auto light = a_player.light.get();
			if (!a_player.lightOn || !a_actor || !a_actor->Get3D()) {
				if (light) {
					DeleteLight(a_player);
				}
				return;
			}
			// The stand-in walked into another cell: the light is remade there (a reference stays in the
			// cell it was created in).
			if (light && light->GetParentCell() != a_actor->GetParentCell()) {
				DeleteLight(a_player);
				light = nullptr;
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
				a_player.lightPlacedAt = a_now;
				++lightsPlaced;
			}
			light->SetLocationOnReference(at);
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

		// Another actor in our game is in this frame.
		bool FrameInUse(RE::TESObjectREFR* a_frame)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
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

		// The frame placed for the stand-in is removed once they are out of it a while (or the stand-in is gone).
		void DeleteFrame(RemotePlayer& a_player)
		{
			if (const auto frame = a_player.frame.get()) {
				// Someone here may have got into the friend's frame copy (it stands drawn and solid where they got out):
				// our own player, or anyone else. Then it stays (deleting it would leave them in nothing) and is simply
				// no longer ours to manage. Only when it really is this frame they are in, or our player's last suit: a
				// player in their own armor standing next to a friend's frame copy used to count, and the copy was
				// abandoned under its stand-in. Nothing of ours needs undoing then: a copy can only be got into while it
				// is drawn and solid (a hidden one has no collision to aim at).
				if (OurFrame(frame.get()) || FrameInUse(frame.get())) {
					REX::INFO("RemotePlayers: someone here is in '{}'s frame copy {:08X}; leaving it", a_player.name, frame->GetFormID());
				} else {
					Bury({ .actor = a_player.frame, .spawnedAt = a_player.framePlacedAt });
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
					REX::INFO("RemotePlayers: our frame {:08X} stays hidden after '{}': another friend wears theirs", original->GetFormID(), a_player.name);
				} else {
					if (const auto root = original->Get3D(); root && root == a_player.originalHidden) {
						root->SetAppCulled(false);
					}
					RestoreCollision(a_player.originalCollisionOff, original.get());
					REX::INFO("RemotePlayers: our frame {:08X}, hidden while '{}' wore theirs, is back", original->GetFormID(), a_player.name);
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
					REX::INFO("RemotePlayers: '{}' wears frame {:08X} (base {:08X}); ours, {:.0f} units from where they got in: {}", a_player.name, a_player.powerArmorFrameRef, a_player.powerArmorFrame, facts.fromSpot.value_or(-1.0f),
						choice == ArmorFrames::Original::kHide      ? "hidden while they wear theirs" :
						choice == ArmorFrames::Original::kMissing   ? "none here" :
						choice == ArmorFrames::Original::kNotLoaded ? "not loaded" :
						choice == ArmorFrames::Original::kOurs      ? "our player's own, left alone" :
						choice == ArmorFrames::Original::kInUse     ? "in use here, left alone" :
						choice == ArmorFrames::Original::kNotFrame  ? "not a power armor frame, left alone" :
						choice == ArmorFrames::Original::kQuest     ? "a quest of ours holds it, left alone" :
						choice == ArmorFrames::Original::kElsewhere ? "not where they got in, left alone while they wear theirs" :
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

		// Power armor: a friend in a frame is shown in their armor (see ArmorFrames). The stand-in wears the pieces they
		// wear (Outfit), and a copy of their frame is kept under it, hidden and without collision, to stand where they
		// get out. It is never sent into the copy: the engine starts nothing for an NPC that activates a frame.
		// A copy further than ArmorFrames::MOVE_LIMIT from its stand-in is placed again rather than moved onto it.
		// The engine holds an actor in furniture at the furniture, and a frame copy with collision a few units off
		// shoved the stand-in aside (in a joiner's game it flipped between two spots 18 units apart while the host
		// stood still). The copy is kept right under the stand-in, without collision.
		constexpr float FRAME_FOLLOW_DISTANCE = 1.0f;
		constexpr float FRAME_FOLLOW_TURN = 0.02f;  // radians

		// The frame copy while it is worn: not drawn (the stand-in's own pieces are what shows; a drawn one trailed it as
		// a second, empty suit), without collision (moved onto the stand-in each time the friend walks or turns, it
		// shoved the stand-in aside while we put it back every frame) and kept right under the stand-in, in whatever cell
		// the copy was placed: the stand-in walks on in it across cell borders, and a copy left drawn and solid at a
		// border stood there as an empty suit.
		void HoldWornFrame(RemotePlayer& a_player, RE::TESObjectREFR* a_frame, const RE::NiPoint3& a_at, float a_heading)
		{
			if (!a_frame->Get3D()) {
				return;
			}
			SetShown(a_frame, false);
			if (SetCollision(a_player.frameCollisionOff, a_frame, false)) {
				REX::INFO("RemotePlayers: '{}'s frame copy has no collision while it is worn", a_player.name);
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

		void UpdatePowerArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now)
		{
			if (!a_actor || !a_actor->Get3D()) {
				return;
			}
			const auto base = a_player.powerArmorOn ? RE::TESForm::GetFormByID<RE::TESFurniture>(a_player.powerArmorFrame) : nullptr;
			const bool worn = Equipment::IsPowerArmorFrame(base);  // a terminal is furniture too, but never a frame
			if (worn != a_player.inPowerArmor) {
				a_player.inPowerArmor = worn;
				if (!worn) {
					a_player.frameActionAt = a_now;  // the copy stands where they got out a while
				}
				REX::INFO("RemotePlayers: '{}' stand-in is shown {} power armor (frame {:08X})", a_player.name, worn ? "in" : "out of", a_player.powerArmorFrame);
			}

			auto                     frame = a_player.frame.get();
			ArmorFrames::CopyFacts facts;
			facts.worn = worn;
			facts.exists = frame != nullptr;
			if (frame) {
				facts.occupied = OurFrame(frame.get()) || FrameInUse(frame.get());
				facts.loaded = frame->Get3D() != nullptr;
				facts.samePlace = SamePlace(frame.get(), a_actor);
				facts.distance = frame->data.location.GetDistance(a_actor->data.location);
				facts.sincePlaced = a_now - a_player.framePlacedAt;
				facts.sinceOut = a_now - a_player.frameActionAt;
			}
			switch (ArmorFrames::ChooseCopy(facts)) {
			case ArmorFrames::Copy::kNone:
				break;
			case ArmorFrames::Copy::kPlace:
				{
					RE::NEW_REFR_DATA data;
					data.location = a_actor->data.location;
					data.direction = { 0.0f, 0.0f, a_actor->data.angle.z };
					data.object = base;
					data.interior = a_cell->IsInterior() ? a_cell : nullptr;
					data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
					data.clearStillLoadingFlag = true;
					const auto handle = EngineRefs::CreateReferenceAtLocation(data);
					if (handle.get()) {
						a_player.frame = handle;
						a_player.framePlacedAt = a_now;
						a_player.frameCollisionOff = nullptr;  // a new copy has its own collision
						++framesPlaced;
					}
				}
				break;
			case ArmorFrames::Copy::kHold:
				HoldWornFrame(a_player, frame.get(), a_at, a_heading);
				break;
			case ArmorFrames::Copy::kReplace:
				{
					const auto frameCell = frame->GetParentCell();
					const auto actorCell = a_actor->GetParentCell();
					REX::INFO("RemotePlayers: '{}'s frame copy (cell {:08X}, 3d={}) is {:.0f} units from the stand-in in {:08X}, placed {} ms ago: placing it again", a_player.name, frameCell ? frameCell->GetFormID() : 0, facts.loaded, facts.distance,
						actorCell ? actorCell->GetFormID() : 0, std::chrono::duration_cast<std::chrono::milliseconds>(facts.sincePlaced).count());
					DeleteFrame(a_player);
				}
				break;
			case ArmorFrames::Copy::kShow:
				// Where they got out: drawn, and solid again once nobody stands in it (the stand-in starts out in it).
				SetShown(frame.get(), true);
				if (a_player.frameCollisionOff && NobodyIn(frame.get())) {
					SetCollision(a_player.frameCollisionOff, frame.get(), true);
				}
				break;
			case ArmorFrames::Copy::kDelete:
				DeleteFrame(a_player);
				break;
			case ArmorFrames::Copy::kAbandon:
				// Someone here got into it while it stood where they got out: theirs now (see DeleteFrame).
				REX::INFO("RemotePlayers: someone here got into '{}'s frame copy {:08X}; it is theirs now", a_player.name, frame->GetFormID());
				a_player.frame = {};
				a_player.frameCollisionOff = nullptr;
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
			REX::INFO("RemotePlayers: '{}' stand-in stood {:.1f} units from where it was put ({:.0f},{:.0f}), at ({:.0f},{:.0f}); in power armor={}, frame copy {:.1f} units away; {} of {} still frames so", a_player.name, off, last->x, last->y, at.x, at.y,
				a_player.inPowerArmor, frame ? frame->data.location.GetDistance(at) : -1.0f, a_player.hold.off, a_player.hold.frames);
		}

		void DeleteActor(RemotePlayer& a_player)
		{
			DeleteLight(a_player);
			DeleteFrame(a_player);
			const auto actor = GetActor(a_player);
			if (actor) {
				WeaponFire::ReleaseSoundsOf(actor->GetFormID());  // a replayed burst may still follow its 3D
				// It stays a puppet until it is disabled (see Grave::puppet), standing still where it is: no friend
				// moves it any more.
				Puppets::AllowAI(actor, 0ms);
				Puppets::SetTarget(actor, { .position = actor->data.location, .heading = actor->data.angle.z });
				Bury({ .actor = a_player.actor, .spawnedAt = a_player.spawnedAt, .puppet = a_player.registered, .puppetId = a_player.registeredId });
			} else if (a_player.registered) {
				Puppets::Unregister(a_player.registered, a_player.registeredId);  // gone already
			}
			a_player.registered = nullptr;
			a_player.registeredId = 0;
			a_player.actor = {};
		}

		bool SameSpace(const Protocol::PlayerState& a_state, const RE::TESObjectCELL* a_localCell)
		{
			if (a_localCell->IsInterior()) {
				return a_state.cell == a_localCell->GetFormID();
			}
			return a_state.cell == 0 && a_localCell->worldSpace && a_state.worldspace == a_localCell->worldSpace->GetFormID();
		}

		// The default stand-in bases: settlers with a fixed look and no factions, one per sex.
		constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
		constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;

		RE::Actor* Spawn(RemotePlayer& a_player, const Protocol::PlayerState& a_state, RE::TESObjectCELL* a_localCell)
		{
			// Prefer the look the player picked; fall back to our default if it isn't a valid NPC here.
			// Unique NPCs (companions, quest characters) would bring their scripts along, so not those.
			auto npc = a_player.appearance ? RE::TESForm::GetFormByID<RE::TESNPC>(a_player.appearance) : nullptr;
			if (npc && npc->IsUnique()) {
				REX::WARN("RemotePlayers: '{}' asked to look like unique NPC {:08X}; using the default", a_player.name, a_player.appearance);
				npc = nullptr;
			}
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
				REX::ERROR("RemotePlayers: failed to spawn puppet for '{}'", a_player.name);
				return nullptr;
			}

			Puppets::Register(actor);
			SaveGuard::Track(actor);
			// Enemies treat the stand-in like the player (it's what NPCs in this world can attack),
			// and hitting it isn't a crime. That faction and rank also tell SaveGuard a stand-in an
			// earlier version saved, so they stay as every version since 0.4.0 set them.
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} {}", actor->GetFormID(), SaveStrays::STAND_IN_FACTION, SaveStrays::STAND_IN_RANK).c_str());

			REX::INFO("RemotePlayers: spawned {:08X} for '{}'", actor->GetFormID(), a_player.name);
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
		auto& snaps = it->second.snapshots;
		if (!snaps.empty() && a_state.sequence <= snaps.back().state.sequence) {
			return;
		}
		const auto steps = snaps.empty() ? 1u : a_state.sequence - snaps.back().state.sequence;
		snaps.push_back({ it->second.arrivals.Next(Clock::now(), steps), a_state });
		while (snaps.size() > MAX_SNAPSHOTS) {
			snaps.pop_front();
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

	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		it->second.equipment = std::move(a_items);
		it->second.hasEquipment = true;
		it->second.equipmentApplied = false;
		it->second.equipmentFixes = 0;
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		for (const auto& [id, remote] : players) {
			Info info{ id, remote.name, std::nullopt, GetActor(remote) };
			if (!remote.snapshots.empty()) {
				info.state = remote.snapshots.back().state;
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
		if (a_action == Protocol::ShotAction::kThrow && player.hasEquipment) {
			Equipment::HoldThrowable(actor, player.equipment);
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
		// A look is used only when it is an NPC here and not a unique one (see Spawn).
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
		std::erase_if(deleting, [](const RE::ObjectRefHandle& a_handle) {
			const auto ref = a_handle.get();
			if (!ref) {
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
			deleting.push_back(a_grave.actor);
			++flushedBeforeSave;
			return true;
		});
	}

	std::optional<RE::NiPoint3> PositionOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		if (it == players.end() || it->second.snapshots.empty()) {
			return std::nullopt;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return std::nullopt;
		}
		const auto& last = it->second.snapshots.back().state;
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
				if (!player.snapshots.empty() && ArmorFrames::FreshEnough(Clock::now() - player.arrivals.arrival)) {
					TakeWornAt(player, player.snapshots.back().state);
				}
			} else if (!a_on) {
				player.wornAtWanted = false;
			}
			if (a_on != player.powerArmorOn) {
				player.equipmentApplied = false;  // dressed again: with the pieces in, without them out (see Outfit)
			}
			player.powerArmorOn = a_on;
			if (a_frameBase) {
				player.powerArmorFrame = a_frameBase;
				player.powerArmorFrameRef = a_frameRef;
			} else if (!a_on) {
				player.powerArmorFrameRef = 0;  // the next frame is told with its base
			}
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
		if (lastFrame != Clock::time_point{}) {
			const float seconds = std::chrono::duration<float>(now - lastFrame).count();
			if (seconds < 0.5f) {  // a load or a pause is not the frame rate
				frameSeconds = frameSeconds * 0.95f + seconds * 0.05f;
			}
		}
		lastFrame = now;
		std::erase_if(graveyard, [&](Grave& a_grave) {
			const auto actor = a_grave.actor.get();
			if (!actor) {
				Release(a_grave);
				return true;
			}
			if (!a_grave.disabledAt) {
				if (!DeferredDelete::Safe(actor->Get3D() != nullptr, now - a_grave.spawnedAt) || HeldForConversation(a_grave, actor.get(), now)) {
					return false;
				}
				Disable(a_grave, actor.get(), now);
				return false;
			}
			if (now - *a_grave.disabledAt < DELETE_AFTER_DISABLE) {
				return false;
			}
			actor->SetDelete(true);
			deleting.push_back(a_grave.actor);
			return true;
		});
		std::erase_if(deleting, [](const RE::ObjectRefHandle& a_handle) { return !a_handle.get(); });
		UpdatePendingSolid();
		for (auto& [id, remote] : players) {
			// Drop snapshots that are too old to ever be rendered again.
			while (remote.snapshots.size() > 2 && now - remote.snapshots[1].received > INTERPOLATION_DELAY + 1s) {
				remote.snapshots.pop_front();
			}

			movingFrames += !remote.snapshots.empty() && remote.snapshots.back().state.speed > 0.0f && now - remote.snapshots.back().received < 1s;
			const float delayWanted = std::clamp(remote.arrivals.gap + frameSeconds + DELAY_SLACK, std::chrono::duration<float>(INTERPOLATION_DELAY).count(),
				std::chrono::duration<float>(MAX_INTERPOLATION_DELAY).count());
			remote.delay += std::clamp(delayWanted - remote.delay, -DELAY_SLEW, DELAY_SLEW);
			const auto sampled = Sample(remote, now);
			const auto state = sampled ? std::optional{ sampled->state } : std::nullopt;
			bool       visible = state && localCell && !loading && SameSpace(*state, localCell);
			if (visible && localCell->IsExterior()) {
				const auto& p = player->data.location;
				const float dx = state->x - p.x;
				const float dy = state->y - p.y;
				visible = dx * dx + dy * dy < MAX_EXTERIOR_DISTANCE * MAX_EXTERIOR_DISTANCE;
			}

			// Before the stand-in is looked at: a friend out of sight can still be wearing our copy of a frame.
			UpdateOriginal(remote, now);

			auto actor = GetActor(remote);
			if (!actor && remote.registered) {
				// The game removed our puppet (e.g. cell unload); forget the stale registration.
				DeleteActor(remote);
			}
			if (actor && IsBroken(actor)) {
				DeleteActor(remote);
				actor = nullptr;
			}
			if (!visible) {
				if (actor) {
					DeleteActor(remote);
				}
				continue;
			}

			// A puppet left behind in another cell (we went through a door) is replaced: always when we are
			// inside, and outside when its cell is an interior or not attached (a neighbouring exterior cell is
			// fine). It used to stay when we stepped from an interior into the open: stuck in the unloaded
			// interior, never moving, with its power armor frame placed at that stale spot.
			if (actor && actor->GetParentCell() != localCell) {
				const auto puppetCell = actor->GetParentCell();
				if (localCell->IsInterior() || !puppetCell || puppetCell->IsInterior() || !puppetCell->IsAttached()) {
					++leftBehind;
					DeleteActor(remote);
					actor = nullptr;
				}
			}

			if (!actor) {
				if (!remote.face && now - remote.knownAt < FACE_GRACE) {
					continue;  // their face is on its way
				}
				actor = Spawn(remote, *state, localCell);
				if (!actor) {
					continue;
				}
				remote.spawnedAt = now;
				remote.actor = actor->GetHandle();
				remote.registered = actor;
				remote.registeredId = actor->GetFormID();
				remote.equipmentApplied = false;
				remote.equipmentFixes = 0;
				remote.shownName.clear();
				remote.lastTarget.reset();
			}

			UpdateLight(remote, actor, localCell, now);
			UpdatePowerArmor(remote, actor, localCell, { state->x, state->y, state->z }, state->heading, now);
			CheckHold(remote, actor, { state->x, state->y, state->z }, state->speed, now);

			// Shown when looking at them, instead of the NPC's name, with their health when hurt.
			if (auto label = NameTag(remote); label != remote.shownName && actor->extraList) {
				actor->extraList->SetOverrideName(label.c_str());
				remote.shownName = std::move(label);
			}

			if (remote.hasEquipment && !remote.equipmentApplied && actor->Get3D()) {
				Equipment::Apply(actor, Outfit(remote));
				Puppets::RedrawWeapon(actor);
				remote.equipmentApplied = true;
				remote.equipmentCheckAt = now + EQUIPMENT_CHECK_INTERVAL;
			} else if (remote.equipmentApplied && now >= remote.equipmentCheckAt && actor->Get3D()) {
				remote.equipmentCheckAt = now + EQUIPMENT_CHECK_INTERVAL;
				if (Wearable(Equipment::Read(actor)) == Wearable(Outfit(remote))) {
					remote.equipmentFixes = 0;
				} else if (remote.equipmentFixes < MAX_EQUIPMENT_FIXES) {
					++remote.equipmentFixes;
					++equipmentFixesTotal;
					remote.equipmentApplied = false;  // applied again next frame
					{
						std::string worn, wanted;
						for (const auto id : Wearable(Equipment::Read(actor))) worn += std::format("{:08X} ", id);
						for (const auto id : Wearable(Outfit(remote))) wanted += std::format("{:08X} ", id);
						REX::INFO("RemotePlayers: '{}' stand-in isn't wearing their gear; dressing it again (wears {} wants {})", remote.name, worn, wanted);
					}
				}
			}

			Puppets::SetTarget(actor, Puppets::Motion{
				.position = { state->x, state->y, state->z },
				.heading = state->heading,
				.speed = state->speed,
				.direction = RelativeDirection(sampled->motionAngle, state->heading),
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
			const auto& snaps = remote.snapshots;
			const auto& last = snaps.empty() ? Protocol::PlayerState{} : snaps.back().state;
			// Average time between the states we hold (about the last second).
			const auto gap = snaps.size() > 1 ? std::chrono::duration_cast<std::chrono::milliseconds>(snaps.back().received - snaps.front().received).count() /
			                                        static_cast<long long>(snaps.size() - 1) :
			                                    0;
			// In power armor: how many of the pieces it should wear it has on, and its frame copy (loading, hidden or shown).
			std::string armor;
			if (actor && (remote.powerArmorOn || remote.inPowerArmor)) {
				const auto pieces = [](const std::vector<std::uint32_t>& a_items) { return std::ranges::count_if(Wearable(a_items), PowerArmorPiece); };
				const auto frame = remote.frame.get();
				const auto root = frame ? frame->Get3D() : nullptr;
				armor = std::format(" pieces={}/{} copy={}", pieces(Equipment::Read(actor)), pieces(Outfit(remote)), !frame ? "none" : !root ? "loading" : root->GetAppCulled() ? "hidden" : "shown");
			}
			// Their frame when every game has it, and whether ours is hidden while they wear theirs.
			if (remote.powerArmorFrameRef || remote.original) {
				const auto original = remote.original.get();
				armor += std::format(" frameRef={:08X} oursHidden={}", remote.powerArmorFrameRef, original != nullptr);
			}
			out += std::format("{}[{} '{}' puppet={:08X} x={:.0f} y={:.0f} cell={:08X} ws={:08X} gapMs={} delayMs={} pa={}/{} frame={:08X} offSpot={}/{} worst={:.1f}{}]",
				out.empty() ? "" : " ", id, remote.name, actor ? actor->GetFormID() : 0,
				last.x, last.y, last.cell, last.worldspace, gap, static_cast<int>(remote.delay * 1000.0f), remote.powerArmorOn, remote.inPowerArmor, remote.powerArmorFrame,
				remote.hold.off, remote.hold.frames, remote.hold.worst, armor);
		}
		if (originalsHidden) {
			out += std::format(" originalsHidden={}", originalsHidden);
		}
		if (movingFrames) {
			out += std::format(" held={}/{}", heldFrames, movingFrames);
			out += std::format(" lights={} frames={}/{} leftBehind={} graveyard={}/{}", lightsPlaced, framesPlaced, framesFollowed, leftBehind, graveyard.size(), flushedBeforeSave);
		}
		if (equipmentFixesTotal) {
			out += std::format(" gearFixes={}", equipmentFixesTotal);
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
