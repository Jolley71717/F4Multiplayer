#include "game/RemotePlayers.h"
#include "game/ArmorClimb.h"
#include "game/DeferredDelete.h"
#include "game/EngineRefs.h"
#include "game/SaveGuard.h"
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
			bool                 inPowerArmor = false;   // the stand-in was sent into our frame (it may still be climbing in)
			RE::ObjectRefHandle  frame;                  // the frame we placed for it
			Clock::time_point    framePlacedAt{};
			Clock::time_point    frameActionAt{};        // when the stand-in last climbed in or out (the animation takes a moment)
			const RE::NiAVObject* frameCollisionOff = nullptr;  // the frame's 3D whose collision we turned off; only compared
			std::optional<RE::NiPoint3> lastTarget;      // where the stand-in was put last frame
			StandInHold::Check   hold;                   // frames it stood still somewhere else than put (net status)
			Clock::time_point    holdLoggedAt{};
			std::optional<Clock::time_point> climbStartedAt;  // a climb in or out is under way (see ArmorClimb)
			bool                 climbedIn = false;      // the engine has the stand-in in the frame: its armor pieces go on
			int                  climbFailures = 0;      // climbs in that never got there, since they last got out
		};

		std::map<std::uint32_t, RemotePlayer> players;

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
		};
		std::vector<Grave> graveyard;
		std::uint32_t      flushedBeforeSave = 0;
		// Marked for deletion, and still there until the engine lets go of them: a save in that moment
		// would write them too.
		std::vector<RE::ObjectRefHandle> deleting;

		// Disables the reference now if it is safe to, and leaves it to the graveyard to delete.
		void Bury(RE::ObjectRefHandle a_handle, Clock::time_point a_placedAt)
		{
			const auto ref = a_handle.get();
			if (!ref) {
				return;
			}
			if (DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_placedAt)) {
				ref->Disable();
				graveyard.push_back({ a_handle, a_placedAt, Clock::now() });
			} else {
				graveyard.push_back({ a_handle, a_placedAt, std::nullopt });
			}
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
		std::uint32_t   climbsDone = 0;      // climbs in or out of a frame the engine finished
		std::uint32_t   climbsCutShort = 0;  // climbs that ran out of time (ArmorClimb::LIMIT)
		std::uint32_t   climbsGivenUp = 0;   // friends left out of their armor after climbs in that never got there
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

		// A power armor piece: put on a stand-in that is not in its frame, it hangs off a body of normal
		// size (a report: "a dwarf in a frame with the parts in the air"). The engine sizes the actor
		// for the pieces only once it is in the frame.
		bool PowerArmorPiece(std::uint32_t a_item)
		{
			const auto armor = RE::TESForm::GetFormByID<RE::TESObjectARMO>(a_item);
			return armor && armor->HasKeywordString("ArmorTypePower");
		}

		// What a stand-in should wear now: everything the owner has on, less the power armor pieces
		// while the engine does not have the stand-in in a frame (sent in is not enough: see ArmorClimb).
		std::vector<std::uint32_t> Outfit(const RemotePlayer& a_player)
		{
			if (a_player.climbedIn) {
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

		constexpr std::uint32_t PLAYER_FACTION = 0x0001C21C;

		float LerpAngle(float a_from, float a_to, float a_t)
		{
			constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
			float delta = std::fmod(a_to - a_from, twoPi);
			if (delta > std::numbers::pi_v<float>) {
				delta -= twoPi;
			} else if (delta < -std::numbers::pi_v<float>) {
				delta += twoPi;
			}
			return a_from + delta * a_t;
		}

		struct Sampled
		{
			Protocol::PlayerState state;
			float                 motionAngle = 0.0f;  // world heading of travel, radians
		};

		// Movement direction relative to facing, as a fraction of a turn in [0, 1).
		float RelativeDirection(float a_motionAngle, float a_heading)
		{
			constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
			float rel = std::fmod(a_motionAngle - a_heading, twoPi);
			if (rel < 0.0f) {
				rel += twoPi;
			}
			return rel / twoPi;
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
				Bury(a_player.light, a_player.lightPlacedAt);
			}
			a_player.light = {};
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

		// While the stand-in wears its frame copy, the copy is kept right under it (see UpdatePowerArmor), and
		// hiding it leaves its collision in place: moved onto the stand-in each time the friend walks or turns,
		// it would shove the stand-in aside while we put it back every frame. So the copy has no collision while
		// it is worn, and gets it back when the stand-in climbs out and leaves it standing.
		void SetFrameCollision(RemotePlayer& a_player, RE::TESObjectREFR* a_frame, bool a_on)
		{
			const auto root = a_frame ? a_frame->Get3D() : nullptr;
			if (!root) {
				a_player.frameCollisionOff = nullptr;  // a new 3D comes with its collision
				return;
			}
			if (a_on) {
				if (a_player.frameCollisionOff == root) {
					RE::bhkWorld::EnableCollision(root, true, true, true);
				}
				a_player.frameCollisionOff = nullptr;
			} else if (a_player.frameCollisionOff != root) {
				const bool done = RE::bhkWorld::EnableCollision(root, false, true, true);
				a_player.frameCollisionOff = root;
				REX::INFO("RemotePlayers: '{}'s frame copy has no collision while it is worn (done={})", a_player.name, done);
			}
		}

		// The frame placed for the stand-in is removed once the stand-in is out of it (or gone).
		void DeleteFrame(RemotePlayer& a_player)
		{
			if (const auto frame = a_player.frame.get()) {
				// Our own player may have climbed into the friend's frame copy: then it stays (deleting it would
				// leave them in nothing) and is simply no longer ours to manage.
				// Only when it really is this frame they occupy: a player in their own armor standing next to a
				// friend's frame copy used to count, and the copy was abandoned under its stand-in.
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto middle = player && player->currentProcess ? player->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				if (occupied && occupied.get() == frame.get()) {
					REX::INFO("RemotePlayers: the player is in '{}'s frame copy; leaving it", a_player.name);
					SetFrameCollision(a_player, frame.get(), true);  // theirs now, solid like any frame
					a_player.frame = {};
					a_player.inPowerArmor = false;
					a_player.climbStartedAt.reset();
					a_player.climbedIn = false;
					return;
				}
				Bury(a_player.frame, a_player.framePlacedAt);
			}
			a_player.frame = {};
			a_player.frameCollisionOff = nullptr;
			a_player.inPowerArmor = false;
			a_player.climbStartedAt.reset();  // a climb still under way ends with its frame (its AI runs out within a second)
			a_player.climbedIn = false;
		}

		// Whether the engine has the actor in power armor: the ExtraPowerArmor it puts on an actor as the
		// actor gets into a frame and takes off as it gets out (what our own player's status reads too).
		bool InArmor(RE::Actor* a_actor)
		{
			return a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
		}

		// Whether the actor is done with a frame as furniture: neither getting in, sitting, nor getting out.
		bool Settled(RE::Actor* a_actor)
		{
			return a_actor->DoGetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal;
		}

		// The engine's side of a stand-in and its armor, for the dev status and the climb log: in the armor,
		// the race it is drawn as (PowerArmorRace 0001D31E once in), its furniture state (0 none, 1-4 getting
		// in, sitting, getting out) and which animation graph it runs (each has a state variable the other lacks).
		std::string DescribeArmor(RE::Actor* a_actor)
		{
			static const RE::BSFixedString powerArmorState{ "iState_PowerArmorNPCDefault" };
			static const RE::BSFixedString humanState{ "iState_NPCDefault" };
			const auto   graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			std::int32_t value = 0;
			const char*  graphName = graph->GetGraphVariableImplInt(powerArmorState, value) ? "armor" :
			                         graph->GetGraphVariableImplInt(humanState, value)      ? "human" :
			                                                                                  "none";
			const auto race = a_actor->GetVisualsRace();
			return std::format("in={} race={:08X} sit={} graph={}", InArmor(a_actor), race ? race->GetFormID() : 0, static_cast<int>(a_actor->DoGetSitSleepState()), graphName);
		}

		// Power armor: a friend in a frame is shown in one. A frame of their frame's base is placed at
		// the stand-in and the stand-in activates it, the way an NPC takes power armor; the armor pieces
		// come with their equipment once the engine has it in the frame. When they climb out, the stand-in
		// activates the frame again and the frame is removed.
		constexpr auto FRAME_ACTION_GAP = 4s;
		constexpr auto FRAME_AI_WINDOW = 6s;  // after a climb in or out starts, the hold check (CheckHold) waits this long
		// A climb's AI is kept on this much at a time while the climb lasts, so it runs out on its own if
		// the stand-in stops being updated.
		constexpr auto CLIMB_AI_SLICE = 1s;
		// After this many climbs in that never got there, the stand-in is left out of the armor until they get
		// out and in again: in its own clothes rather than with the pieces hung on a body that is not in a frame.
		constexpr int MAX_CLIMB_FAILURES = 2;
		// The engine holds an actor in furniture at the furniture: a frame copy even a few units off pulled the
		// stand-in back toward it on alternate frames (in a joiner's game it flipped between two spots 18 units
		// apart while the host stood still). The copy is kept under the stand-in, where it is placed this frame.
		constexpr float FRAME_FOLLOW_DISTANCE = 1.0f;
		constexpr float FRAME_FOLLOW_TURN = 0.02f;  // radians
		void UpdatePowerArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading, Clock::time_point a_now)
		{
			// A climb is followed even while the stand-in has no 3D: getting into the armor gives it the armor's
			// body, which is loaded afresh, and its AI must not run out meanwhile.
			if (!a_actor || (!a_actor->Get3D() && !a_player.climbStartedAt)) {
				return;
			}
			auto frame = a_player.frame.get();
			// The engine moves a frame along with the actor walking in it, but a stand-in is placed each
			// frame, which left its frame standing where it climbed in: the friend saw a spare suit of
			// power armor, with the pieces in it, at that spot (Nexus report on 0.7.0). The frame is
			// brought along whenever it falls behind.
			if (a_player.inPowerArmor && frame && frame->Get3D() && frame->GetParentCell() == a_actor->GetParentCell()) {
				// While it is worn, the frame object itself is hidden (the stand-in's own armor is what shows,
				// as for any actor in power armor); a visible one trailed the stand-in as a second, empty suit.
				frame->Get3D()->SetAppCulled(true);
				SetFrameCollision(a_player, frame.get(), false);
				// Headings are compared the short way round: one blended across north can be a full turn off the
				// frame's while facing the same way.
				if (frame->data.location.GetDistance(a_at) > FRAME_FOLLOW_DISTANCE || StandInHold::AngleBetween(frame->data.angle.z, a_heading) > FRAME_FOLLOW_TURN) {
					frame->data.location.x = a_at.x;
					frame->data.location.y = a_at.y;
					frame->data.location.z = a_at.z;
					frame->data.angle = { 0.0f, 0.0f, a_heading };
					frame->Update3DPosition(true);
					++framesFollowed;
				}
			}
			// A climb under way: the stand-in's own AI runs until the engine has finished it (see ArmorClimb).
			// It is dressed only then, for in or out of the frame: stripped mid-climb (a gear fix) it fell out.
			if (a_player.climbStartedAt) {
				const bool engineIn = InArmor(a_actor);
				const auto step = ArmorClimb::Next(a_player.inPowerArmor, engineIn, Settled(a_actor), a_now - *a_player.climbStartedAt);
				if (step == ArmorClimb::Step::kWait) {
					Puppets::AllowAI(a_actor, CLIMB_AI_SLICE);
					return;
				}
				Puppets::AllowAI(a_actor, 0ms);  // its own update ends now; it follows its owner again
				const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(a_now - *a_player.climbStartedAt).count();
				const bool done = step == ArmorClimb::Step::kDone;
				if (done) {
					++climbsDone;
				} else {
					++climbsCutShort;
				}
				REX::INFO("RemotePlayers: '{}' stand-in's climb {} its frame {} after {} ms ({})", a_player.name, a_player.inPowerArmor ? "into" : "out of",
					done ? "finished" : "was cut short", took, DescribeArmor(a_actor));
				a_player.climbStartedAt.reset();
				a_player.frameActionAt = a_now;
				a_player.climbedIn = a_player.inPowerArmor && engineIn;  // out of it, it is undressed with a strip, which also takes it out
				a_player.equipmentApplied = false;
				if (a_player.inPowerArmor && !engineIn) {
					// It never got in: tried again (the frame is still there), and in the end left in its own clothes.
					a_player.inPowerArmor = false;
					if (++a_player.climbFailures >= MAX_CLIMB_FAILURES) {
						++climbsGivenUp;
						REX::WARN("RemotePlayers: '{}' stand-in did not get into its frame {} times; shown out of power armor until they get out and in again", a_player.name, a_player.climbFailures);
						DeleteFrame(a_player);
					}
				}
				return;
			}
			if (!a_player.powerArmorOn) {
				a_player.climbFailures = 0;  // a fresh start the next time they get in
			}
			if (a_now - a_player.frameActionAt < FRAME_ACTION_GAP) {
				return;
			}
			if (a_player.powerArmorOn && !a_player.inPowerArmor && a_player.climbFailures < MAX_CLIMB_FAILURES) {
				const auto base = RE::TESForm::GetFormByID<RE::TESFurniture>(a_player.powerArmorFrame);
				if (!base || base->GetFormType() != RE::ENUM_FORM_ID::kFURN) {
					return;  // a terminal is furniture too, but never a frame
				}
				if (frame && InArmor(a_actor)) {
					// In after all (a climb that finished after it was cut short): dressed for it, not sent in again,
					// since activating the frame it is in takes it out.
					a_player.inPowerArmor = true;
					a_player.climbedIn = true;
					a_player.equipmentApplied = false;
					a_player.frameActionAt = a_now;
					REX::INFO("RemotePlayers: '{}' stand-in is in its frame after all ({})", a_player.name, DescribeArmor(a_actor));
					return;
				}
				// A frame left in another cell is no use; one still attaching (no cell yet) or standing right at the
				// stand-in is kept, else a frame placed on a cell seam was deleted and placed again for ever.
				if (frame && frame->GetParentCell() && frame->GetParentCell() != a_actor->GetParentCell() &&
					frame->data.location.GetDistance(a_actor->data.location) > 256.0f) {
					REX::INFO("RemotePlayers: '{}'s frame copy is in cell {:08X}, the stand-in in {:08X}: placing it again", a_player.name, frame->GetParentCell()->GetFormID(), a_actor->GetParentCell() ? a_actor->GetParentCell()->GetFormID() : 0);
					DeleteFrame(a_player);
					frame = nullptr;
				}
				if (!frame) {
					RE::NEW_REFR_DATA data;
					data.location = a_actor->data.location;
					data.direction = a_actor->data.angle;
					data.object = base;
					data.interior = a_cell->IsInterior() ? a_cell : nullptr;
					data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
					data.clearStillLoadingFlag = true;
					const auto handle = EngineRefs::CreateReferenceAtLocation(data);
					frame = handle.get().get();
					if (!frame) {
						return;
					}
					a_player.frame = handle;
					a_player.framePlacedAt = a_now;
					a_player.frameActionAt = a_now;  // let it load before the stand-in uses it
					++framesPlaced;
					return;
				}
				if (!frame->Get3D()) {
					if (a_now - a_player.framePlacedAt > std::chrono::seconds(10)) {
						const auto frameCell = frame->GetParentCell();
						REX::INFO("RemotePlayers: '{}'s frame copy has had no 3D for ten seconds; placing it again (frame at ({:.0f},{:.0f},{:.0f}) cell {:08X} attached={}, stand-in at ({:.0f},{:.0f},{:.0f}) cell {:08X} 3d={}, we are in cell {:08X})", a_player.name, frame->data.location.x, frame->data.location.y, frame->data.location.z, frameCell ? frameCell->GetFormID() : 0, frameCell ? frameCell->IsAttached() : false, a_actor->data.location.x, a_actor->data.location.y, a_actor->data.location.z, a_actor->GetParentCell() ? a_actor->GetParentCell()->GetFormID() : 0, a_actor->Get3D() != nullptr, a_cell ? a_cell->GetFormID() : 0);
						DeleteFrame(a_player);
					}
					return;
				}
				Puppets::AllowAI(a_actor, CLIMB_AI_SLICE);
				frame->ActivateRef(a_actor, nullptr, 0, false, false, false);
				a_player.inPowerArmor = true;
				a_player.climbStartedAt = a_now;  // the pieces go on once the engine has it in the frame
				a_player.frameActionAt = a_now;
				REX::INFO("RemotePlayers: '{}' stand-in gets into power armor (frame {:08X}; {})", a_player.name, a_player.powerArmorFrame, DescribeArmor(a_actor));
			} else if (!a_player.powerArmorOn && a_player.inPowerArmor) {
				a_player.inPowerArmor = false;
				a_player.frameActionAt = a_now;
				if (frame && frame->Get3D()) {
					SetFrameCollision(a_player, frame.get(), true);
					frame->Get3D()->SetAppCulled(false);  // it stands where they got out, until it is deleted
					Puppets::AllowAI(a_actor, CLIMB_AI_SLICE);
					frame->ActivateRef(a_actor, nullptr, 0, false, false, false);
					a_player.climbStartedAt = a_now;  // its own clothes go back on once the engine has it out
				} else {
					a_player.climbedIn = false;
					a_player.equipmentApplied = false;
				}
				REX::INFO("RemotePlayers: '{}' stand-in gets out of power armor ({})", a_player.name, DescribeArmor(a_actor));
			} else if (!a_player.powerArmorOn && frame && a_now - a_player.frameActionAt >= FRAME_ACTION_GAP) {
				DeleteFrame(a_player);  // out of it and the animation is done
			}
		}

		// While the friend stands still, their stand-in should be found each frame where it was put the frame
		// before; when it isn't, something in the engine moves it. Counted for net status, and logged now and
		// then with what may be holding it, such as its power armor frame copy.
		constexpr auto HOLD_LOG_INTERVAL = 10s;
		void CheckHold(RemotePlayer& a_player, RE::Actor* a_actor, const RE::NiPoint3& a_target, Clock::time_point a_now)
		{
			const auto last = a_player.lastTarget;
			a_player.lastTarget = a_target;
			// A climb into or out of a frame is the engine's to run; the stand-in isn't put anywhere meanwhile.
			if (!last || !a_actor->Get3D() || a_now - a_player.frameActionAt < FRAME_AI_WINDOW || a_player.climbStartedAt) {
				return;
			}
			const auto& at = a_actor->data.location;
			const float moved = std::hypot(a_target.x - last->x, a_target.y - last->y);
			const float off = std::hypot(at.x - last->x, at.y - last->y);
			if (!a_player.hold.Add(moved, off) || a_now - a_player.holdLoggedAt < HOLD_LOG_INTERVAL) {
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
			a_player.climbFailures = 0;  // a new stand-in gets its own tries
			if (a_player.registered) {
				Puppets::Unregister(a_player.registered, a_player.registeredId);
				a_player.registered = nullptr;
				a_player.registeredId = 0;
			}
			if (const auto actor = GetActor(a_player)) {
				WeaponFire::ReleaseSoundsOf(actor->GetFormID());  // a replayed burst may still follow its 3D
				Bury(a_player.actor, a_player.spawnedAt);
			}
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
			// Enemies treat the stand-in like the player (it's what NPCs in this world can attack),
			// and hitting it isn't a crime.
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} 1", actor->GetFormID(), PLAYER_FACTION).c_str());

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
	}

	void Remove(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			DeleteActor(it->second);
			players.erase(it);
		}
	}

	void RemoveAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
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

	void PlayShot(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			WeaponFire::PlayShot(GetActor(it->second));
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

	void DespawnAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
		}
		// Before a save: whatever is only disabled, or has been waiting for its 3D, is deleted now. (A
		// reference whose 3D is still loading is left alone: deleting it crashes the loader when it
		// finishes.) The save runs between frames, so no physics job holds any of them. None of them may
		// be written into the save, deleted or not: a stand-in saved as deleted comes back from the save in
		// no cell, and every later save crashed on it.
		std::erase_if(graveyard, [&](const Grave& a_grave) {
			const auto ref = a_grave.actor.get();
			if (!ref) {
				return true;
			}
			if (!a_grave.disabledAt && !DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_grave.spawnedAt)) {
				SaveGuard::Exclude(ref.get());
				return false;
			}
			ref->Disable();
			ref->SetDelete(true);
			SaveGuard::Exclude(ref.get());
			deleting.push_back(a_grave.actor);
			++flushedBeforeSave;
			return true;
		});
		std::erase_if(deleting, [](const RE::ObjectRefHandle& a_handle) {
			const auto ref = a_handle.get();
			if (!ref) {
				return true;
			}
			SaveGuard::Exclude(ref.get());
			return false;
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
		// entry resolved after the load could delete some unrelated object, and a face base made before
		// it may be gone.
		graveyard.clear();
		for (auto& [id, player] : players) {
			player.faceBaseId = 0;
			player.faceTried = false;
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

	void SetPowerArmor(std::uint32_t a_id, bool a_on, std::uint32_t a_frameBase)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.powerArmorOn = a_on;
			if (a_frameBase) {
				it->second.powerArmorFrame = a_frameBase;
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
				return true;
			}
			if (!a_grave.disabledAt) {
				if (!DeferredDelete::Safe(actor->Get3D() != nullptr, now - a_grave.spawnedAt)) {
					return false;
				}
				actor->Disable();
				a_grave.disabledAt = now;
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
			CheckHold(remote, actor, { state->x, state->y, state->z }, now);

			// Shown when looking at them, instead of the NPC's name, with their health when hurt.
			if (auto label = NameTag(remote); label != remote.shownName && actor->extraList) {
				actor->extraList->SetOverrideName(label.c_str());
				remote.shownName = std::move(label);
			}

			// Not while it climbs in or out of its frame: it is dressed for the result once the engine is done.
			const bool climbing = remote.climbStartedAt.has_value();
			if (remote.hasEquipment && !remote.equipmentApplied && !climbing && actor->Get3D()) {
				// In a frame it is dressed without a strip first: a strip takes it out of the frame.
				if (remote.climbedIn) {
					Equipment::Wear(actor, Outfit(remote));
				} else {
					Equipment::Apply(actor, Outfit(remote));
				}
				Puppets::RedrawWeapon(actor);
				remote.equipmentApplied = true;
				remote.equipmentCheckAt = now + EQUIPMENT_CHECK_INTERVAL;
			} else if (remote.equipmentApplied && !climbing && now >= remote.equipmentCheckAt && actor->Get3D()) {
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
			// In power armor: whether its pieces are on (once the engine has it in the frame), how long the climb
			// under way has taken, and the engine's own view of the stand-in (see DescribeArmor).
			std::string armor;
			if (actor && (remote.powerArmorOn || remote.inPowerArmor || remote.climbedIn)) {
				const auto climbMs = remote.climbStartedAt ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *remote.climbStartedAt).count() : -1;
				armor = std::format(" pieces={} climbMs={} {}", remote.climbedIn, climbMs, DescribeArmor(actor));
			}
			out += std::format("{}[{} '{}' puppet={:08X} x={:.0f} y={:.0f} cell={:08X} ws={:08X} gapMs={} delayMs={} pa={}/{} frame={:08X} offSpot={}/{} worst={:.1f}{}]",
				out.empty() ? "" : " ", id, remote.name, actor ? actor->GetFormID() : 0,
				last.x, last.y, last.cell, last.worldspace, gap, static_cast<int>(remote.delay * 1000.0f), remote.powerArmorOn, remote.inPowerArmor, remote.powerArmorFrame,
				remote.hold.off, remote.hold.frames, remote.hold.worst, armor);
		}
		if (climbsDone || climbsCutShort) {
			out += std::format(" climbs={}/{}/{}", climbsDone, climbsCutShort, climbsGivenUp);  // finished / cut short / friends left out of their armor
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
		return out.empty() ? "none" : out;
	}
}
