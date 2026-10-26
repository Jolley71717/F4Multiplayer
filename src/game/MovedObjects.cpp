#include "game/MovedObjects.h"

#include "game/Equipment.h"
#include "game/HavokNan.h"
#include "game/Hud.h"
#include "game/NpcSync.h"
#include "game/ObjectShare.h"
#include "game/OpenMenus.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"
#include "game/WeaponFire.h"

#include <atomic>
#include <charconv>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MovedObjects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using ObjectShare::Pose;
		using ObjectShare::Vec3;

		constexpr auto  SCAN_INTERVAL = std::chrono::milliseconds(250);
		constexpr float SCAN_RADIUS = 1500.0f;        // loose objects this close to our player are watched for knocks
		constexpr std::size_t MAX_SCANNED = 256;      // of them, at most this many a scan
		constexpr float FRIEND_NEAR = 6000.0f;        // a knock is reported only with a friend this close
		constexpr auto  HIT_MEMORY = std::chrono::seconds(3);  // a hit of ours moves it for that long
		constexpr auto  FLIGHT = std::chrono::seconds(12);     // a friend's let-go flies here until their rest, at most
		constexpr float SNAP_DISTANCE = 1.0f;          // our copy this far from their rest is put where theirs lies
		constexpr float SNAP_TURN = 0.02f;
		constexpr auto  RELEASE_AGAIN = std::chrono::milliseconds(150);  // a refused grab's copy is back on after this

		ObjectShare::Carry      carry;
		ObjectShare::Carried    carried;
		ObjectShare::KnockWatch knocks;
		ObjectShare::BlastBook  blasts;
		std::vector<Packet>     outgoing;
		Clock::time_point       lastFrame{};
		Clock::time_point       nextScan{};

		// Objects a friend let go of, flying here until their rest comes.
		std::unordered_map<std::uint32_t, Clock::time_point> flying;
		// Objects we made keyframed (held still for a friend's hands), to make physics again: by handle, so it is that
		// very reference.
		struct Still
		{
			RE::ObjectRefHandle        handle;
			std::vector<std::uint32_t> filters;  // each body's collision filter before: it collides with nothing while held here
		};
		std::unordered_map<std::uint32_t, Still> keyframed;
		// Our player grabbed what a friend holds (the server said so): its copy is turned off and on again so the game
		// lets go of it (the engine drops a grab whose 3D goes).
		std::unordered_map<std::uint32_t, std::pair<RE::ObjectRefHandle, Clock::time_point>> released;

		// References our player hit lately (TESHitEvent, any thread).
		std::mutex                                             hitLock;
		std::unordered_map<std::uint32_t, Clock::time_point> hits;

		struct Counters
		{
			std::uint64_t holdsSent = 0, holdsApplied = 0, holdsIgnored = 0;
			std::uint64_t letGosSent = 0, letGosApplied = 0;
			std::uint64_t restsSent = 0, restsApplied = 0, restsAlready = 0, restsIgnored = 0;
			std::uint64_t knocksSent = 0, refused = 0, releases = 0, badPoses = 0, blastsSeen = 0;
		} counts;

		// The base forms whose references are loose objects a player can grab or knock about.
		bool LooseType(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			switch (a_base->GetFormType()) {
			case RE::ENUM_FORM_ID::kMISC:  // junk: brooms, bottles, cans, tins
			case RE::ENUM_FORM_ID::kMSTT:  // moveable statics: tires, buckets, barrels
			case RE::ENUM_FORM_ID::kALCH:
			case RE::ENUM_FORM_ID::kAMMO:
			case RE::ENUM_FORM_ID::kBOOK:
			case RE::ENUM_FORM_ID::kWEAP:
			case RE::ENUM_FORM_ID::kARMO:
			case RE::ENUM_FORM_ID::kNOTE:
			case RE::ENUM_FORM_ID::kKEYM:
			case RE::ENUM_FORM_ID::kINGR:
				return true;
			default:
				return false;
			}
		}

		// Every physics body under the reference's 3D (a walk of at most HavokNan::NODE_LIMIT nodes).
		template <class F>
		void ForEachCollision(RE::NiAVObject* a_root, F&& a_each)
		{
			std::vector<RE::NiAVObject*> stack{ a_root };
			std::size_t                  walked = 0;
			while (!stack.empty() && walked++ < HavokNan::NODE_LIMIT) {
				const auto node = stack.back();
				stack.pop_back();
				if (const auto object = node->collisionObject.get()) {
					if (const auto collision = object->IsbhkNPCollisionObject()) {
						a_each(collision);
					}
				}
				if (const auto parent = node->IsNode()) {
					for (const auto& child : parent->children) {
						if (child.get()) {
							stack.push_back(child.get());
						}
					}
				}
			}
		}

		// Whether the reference has a body physics moves (a static one has no motion of its own: motion id 0).
		bool HasMovingBody(RE::NiAVObject* a_root)
		{
			bool moving = false;
			ForEachCollision(a_root, [&](RE::bhkNPCollisionObject* a_collision) {
				if (const auto body = a_collision->GetBody()) {
					std::uint32_t motion = 0;
					std::memcpy(&motion, &body->m_motionId, sizeof(motion));
					moving = moving || motion != 0;
				}
			});
			return moving;
		}

		// The cheap part of Movable, for a scan's walk: never an actor, the player or a runtime reference.
		bool MaybeLoose(RE::TESObjectREFR* a_ref)
		{
			return a_ref && Protocol::IsShareableRef(a_ref->GetFormID()) && !a_ref->IsDeleted() && !a_ref->IsDisabled() && !a_ref->As<RE::Actor>() &&
			       LooseType(a_ref->GetObjectReference()) && a_ref->Get3D();
		}

		// A loose object of the world, ours to share: a plugin's reference of a loose type, loaded with a body physics
		// moves, in no quest's alias, and nothing another part of the session placed or runs.
		bool Movable(RE::TESObjectREFR* a_ref)
		{
			if (!MaybeLoose(a_ref)) {
				return false;
			}
			const auto extra = a_ref->extraList.get();
			if (extra && extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray)) {
				return false;  // a quest item
			}
			if (SaveGuard::Placed(a_ref) || Spawns::IdOf(a_ref->GetFormID()) != 0 || Equipment::IsPowerArmorFrame(a_ref->GetObjectReference())) {
				return false;
			}
			return HasMovingBody(a_ref->Get3D());
		}

		RE::NiPointer<RE::TESObjectREFR> Lookup(std::uint32_t a_id)
		{
			return RE::NiPointer<RE::TESObjectREFR>{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) };
		}

		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return { 0, 0 };
			}
			if (cell->IsInterior()) {
				return { cell->GetFormID(), 0 };
			}
			return { 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0 };
		}

		// Where the reference is: where its 3D is drawn (physics moves that first), turned as its 3D is.
		std::optional<Pose> PoseOf(RE::TESObjectREFR* a_ref)
		{
			const auto root = a_ref ? a_ref->Get3D() : nullptr;
			if (!root) {
				return std::nullopt;
			}
			const auto [cell, worldspace] = SpaceOf(a_ref);
			if (cell == 0 && worldspace == 0) {
				return std::nullopt;
			}
			const auto& t = root->world.translate;
			// Its angles from its 3D too: the reference's own lag behind physics while it is held (0.03 rad seen).
			const auto& r = root->world.rotate.entry;
			const ObjectShare::Mat3 rotation{ Vec3{ r[0].x, r[0].y, r[0].z }, Vec3{ r[1].x, r[1].y, r[1].z }, Vec3{ r[2].x, r[2].y, r[2].z } };
			Pose pose{ .cell = cell, .worldspace = worldspace, .position = { t.x, t.y, t.z }, .angles = ObjectShare::ToAngles(rotation) };
			if (!ObjectShare::Finite(pose.position) || !ObjectShare::Finite(pose.angles)) {
				return std::nullopt;
			}
			return pose;
		}

		std::optional<Pose> PoseOfId(std::uint32_t a_id)
		{
			const auto ref = Lookup(a_id);
			return ref && MaybeLoose(ref.get()) ? PoseOf(ref.get()) : std::nullopt;
		}

		Vec3 Position(const RE::TESObjectREFR* a_ref)
		{
			return { a_ref->data.location.x, a_ref->data.location.y, a_ref->data.location.z };
		}

		std::uint32_t OurHeld()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto ref = player ? player->grabbedObject.get() : RE::NiPointer<RE::TESObjectREFR>{};
			return ref ? ref->GetFormID() : 0;
		}

		void SetMotion(RE::TESObjectREFR* a_ref, RE::hknpMotionPropertiesId::Preset a_motion)
		{
			if (const auto root = a_ref->Get3D()) {
				ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) { a_collision->SetMotionType(a_motion); });
				if (a_motion == RE::hknpMotionPropertiesId::Preset::kDynamic) {
					RE::bhkWorld::Activate(root, true, true, true);
				}
			}
		}

		void HoldStill(RE::TESObjectREFR* a_ref)
		{
			const auto id = a_ref->GetFormID();
			if (keyframed.contains(id)) {
				return;
			}
			SetMotion(a_ref, RE::hknpMotionPropertiesId::Preset::kKeyframed);
			// And it touches nothing: drawn in a friend's stand-in's hands it would push the stand-in away (and anything
			// else in the way, which the holder's game moves itself, and reports).
			Still still{ .handle = a_ref->GetHandle() };
			if (const auto root = a_ref->Get3D()) {
				ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) {
					const auto body = a_collision->GetBody();
					RE::CFilter filter{ body ? body->m_collisionFilterInfo : 0u };
					still.filters.push_back(filter.filter);
					if (body) {
						filter.SetNoCollision(true);
						a_collision->SetCollisionFilterInfo(filter);
					}
				});
			}
			keyframed.emplace(id, std::move(still));
		}

		// Physics again, as the game made it (a reference we held still for a friend).
		void LetLoose(std::uint32_t a_id)
		{
			const auto it = keyframed.find(a_id);
			if (it == keyframed.end()) {
				return;
			}
			const auto ref = it->second.handle.get();
			const auto filters = std::move(it->second.filters);
			keyframed.erase(it);
			if (ref && ref->GetFormID() == a_id && !ref->IsDeleted()) {
				if (const auto root = ref->Get3D()) {
					std::size_t i = 0;
					ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) {
						const auto body = a_collision->GetBody();
						if (body && i < filters.size()) {
							RE::CFilter filter{ filters[i] };
							if (filter.filter != 0 && filter.filter != body->m_collisionFilterInfo) {
								a_collision->SetCollisionFilterInfo(filter);
							}
						}
						++i;
					});
				}
				SetMotion(ref.get(), RE::hknpMotionPropertiesId::Preset::kDynamic);
			}
		}

		void LetAllLoose()
		{
			std::vector<std::uint32_t> ids;
			for (const auto& [id, still] : keyframed) {
				ids.push_back(id);
			}
			for (const auto id : ids) {
				LetLoose(id);
			}
		}

		// Puts the reference there: its place and angles, and its 3D and physics with them. Only a finite pose in the
		// world is ever written (HavokGuard: a NaN in a body freezes the physics step).
		bool Place(RE::TESObjectREFR* a_ref, const Vec3& a_position, const Vec3& a_angles)
		{
			if (!ObjectShare::Writable(a_position, a_angles)) {
				++counts.badPoses;
				return false;
			}
			a_ref->SetLocationOnReference({ a_position[0], a_position[1], a_position[2] });
			a_ref->SetAngleOnReference({ a_angles[0], a_angles[1], a_angles[2] });
			a_ref->Update3DPosition(true);
			return true;
		}

		// Sets the reference's bodies moving at a_velocity (units a second; zero stops them).
		void SetVelocity(RE::TESObjectREFR* a_ref, const Vec3& a_velocity)
		{
			const auto cell = a_ref->GetParentCell();
			const auto world = cell ? cell->GetbhkWorld() : nullptr;
			const auto physics = world ? world->m_worldNP.get() : nullptr;
			const auto root = a_ref->Get3D();
			if (!physics || !root || !ObjectShare::Finite(a_velocity)) {
				return;
			}
			const RE::hkVector4f velocity{ a_velocity[0] * RE::BS_TO_HK_SCALE, a_velocity[1] * RE::BS_TO_HK_SCALE, a_velocity[2] * RE::BS_TO_HK_SCALE, 0.0f };
			ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) {
				RE::hknpBodyId id{};
				a_collision->GetBodyId(id);
				if (world->IsBodyAdded(id)) {
					physics->SetBodyLinearVelocity(id, velocity);
				}
			});
		}

		void Send(std::vector<std::uint8_t> a_data, bool a_reliable)
		{
			if (outgoing.size() < 256) {
				outgoing.push_back({ std::move(a_data), a_reliable });
			}
		}

		bool HitByUs(std::uint32_t a_ref, Clock::time_point a_now)
		{
			std::scoped_lock l{ hitLock };
			const auto       it = hits.find(a_ref);
			return it != hits.end() && a_now - it->second < HIT_MEMORY;
		}

		void PruneHits(Clock::time_point a_now)
		{
			std::scoped_lock l{ hitLock };
			std::erase_if(hits, [&](const auto& a_entry) { return a_now - a_entry.second >= HIT_MEMORY; });
		}

		// Our player's grab of what a friend holds ends: its copy goes off now and comes back on RELEASE_AGAIN later,
		// where its place is, as the engine made it (HavokGuard's reset does the same).
		void Release(RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			const auto id = a_ref->GetFormID();
			if (released.contains(id)) {
				return;
			}
			released.emplace(id, std::pair{ a_ref->GetHandle(), a_now + RELEASE_AGAIN });
			a_ref->Disable();
			++counts.releases;
			REX::INFO("MovedObjects: a friend holds {:08X}, which our player grabbed after them: our player lets go", id);
		}

		void EnableReleased(Clock::time_point a_now, bool a_all)
		{
			for (auto it = released.begin(); it != released.end();) {
				if (!a_all && a_now < it->second.second) {
					++it;
					continue;
				}
				const auto ref = it->second.first.get();
				if (ref && ref->GetFormID() == it->first && !ref->IsDeleted() && ref->IsDisabled()) {
					ref->Enable(false);
				}
				it = released.erase(it);
			}
		}

		// Draws each object a friend holds in their stand-in's hands.
		void DrawHeld(std::uint32_t a_ourHeld, float a_seconds, Clock::time_point a_now)
		{
			for (const auto id : carried.TakeStale(a_now)) {
				LetLoose(id);
			}
			for (const auto id : carried.Refs()) {
				auto* const entry = carried.Find(id);
				const auto  ref = Lookup(id);
				if (!entry || !ref || !Movable(ref.get()) || released.contains(id)) {
					continue;  // not loaded here (it may be soon), or not a loose object of ours
				}
				const auto standIn = RE::TESForm::GetFormByID<RE::Actor>(RemotePlayers::ActorIdOf(entry->holder));
				if (!standIn || !standIn->Get3D()) {
					continue;
				}
				const auto player = RE::PlayerCharacter::GetSingleton();
				ObjectShare::Facts facts{
					.loaded = true,
					.sameSpace = SpaceOf(standIn) == SpaceOf(ref.get()),
					.oursHeld = a_ourHeld == id,
					.fromUs = player ? ObjectShare::Distance(Position(player), Position(ref.get())) : 0.0f,
					.fromPlace = entry->placed ? 0.0f : ObjectShare::Distance(Position(standIn), Position(ref.get())),
				};
				const auto verdict = ObjectShare::JudgeHold(facts);
				if (verdict != ObjectShare::Verdict::kApply) {
					if (verdict == ObjectShare::Verdict::kOurs) {
						LetLoose(id);  // the engine's grab moves it; the server says whose it is
					}
					continue;
				}
				const auto relative = carried.Step(id, a_seconds);
				if (!relative) {
					continue;
				}
				const ObjectShare::Holder holder{ .position = Position(standIn), .heading = standIn->data.angle.z };
				const auto [position, angles] = ObjectShare::FromHolderFrame(*relative, holder);
				HoldStill(ref.get());
				if (Place(ref.get(), position, angles)) {
					if (!entry->placed) {
						entry->placed = true;
						++counts.holdsApplied;
					}
					const auto [cell, worldspace] = SpaceOf(ref.get());
					knocks.Placed(id, Pose{ .cell = cell, .worldspace = worldspace, .position = position, .angles = angles }, a_now);
				}
			}
		}

		bool FriendNear(const RE::PlayerCharacter* a_player)
		{
			for (const auto& info : RemotePlayers::List()) {
				if (const auto at = RemotePlayers::PositionOf(info.id); at && at->GetDistance(a_player->data.location) <= FRIEND_NEAR) {
					return true;
				}
			}
			return false;
		}

		// Whether our game runs this actor (not our player, not a friend's stand-in): the server made our player its owner
		// (ObjectShare::Answers), so what it shoves is ours to report.
		bool RunsHere(RE::Actor* a_actor)
		{
			if (!a_actor || a_actor->IsPlayerRef() || Puppets::IsPuppet(a_actor)) {
				return false;
			}
			const auto id = NpcSync::SessionId(a_actor->GetFormID());
			return ObjectShare::Answers(id, NpcSync::Owner(id), NpcSync::LocalPlayer());
		}

		// An explosion in our game: whose it is by what caused it (ObjectShare::RoleOf), kept in the blast book for what it
		// throws about. One our game set off for a friend's throw is theirs; so is one a friend's stand-in or an NPC another
		// game runs caused. A hazard's is nobody's.
		void NoteBlast(RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			const auto base = a_ref->GetObjectReference();
			const auto form = base && base->Is(RE::ENUM_FORM_ID::kEXPL) ? base->As<RE::BGSExplosion>() : nullptr;
			// The reference itself must be an explosion, by the game's own type info: one of an explosion base that is
			// something else (a mod's static of it, a reference half made) has no owner to read. As<RE::Explosion>() can't
			// tell (an explosion's form type is a plain reference's), so the cast is the engine's checked one.
			const auto explosion = form ? RE::fallout_cast<RE::Explosion*>(a_ref) : nullptr;
			if (!explosion || a_ref->IsDeleted()) {
				return;
			}
			ObjectShare::Cause cause = ObjectShare::Cause::kNobody;
			bool               answered = false;
			if (!WeaponFire::SetOffForFriend(a_ref)) {
				if (const auto actor = explosion->GetActorOwner()) {
					if (actor->IsPlayerRef()) {
						cause = ObjectShare::Cause::kPlayer;
					} else if (Puppets::IsPuppet(actor)) {
						cause = ObjectShare::Cause::kStandIn;
					} else {
						cause = ObjectShare::Cause::kNpc;
						answered = RunsHere(actor);
					}
				}
			}
			const auto role = ObjectShare::RoleOf(WeaponFire::SetOffForFriend(a_ref), cause, answered);
			if (!role) {
				return;
			}
			const auto& at = a_ref->data.location;
			if (blasts.Note(a_ref->GetHandle().get_handle(), { at.x, at.y, at.z }, form->data.outerRadius, *role, a_now)) {
				++counts.blastsSeen;
			}
		}

		// The loose objects near our player: what moved with no hand on it, and came to rest, where our player, an NPC of
		// ours or a blast of ours moved it.
		void ScanKnocks(RE::PlayerCharacter* a_player, std::uint32_t a_ourHeld, Clock::time_point a_now)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			std::vector<RE::NiPointer<RE::TESObjectREFR>> found;
			std::vector<RE::NiPointer<RE::TESObjectREFR>> blastRefs;
			std::vector<Vec3>                             shovers;  // NPCs our game runs, alive
			found.reserve(MAX_SCANNED);
			tes->ForEachReferenceInRange(a_player->data.location, SCAN_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				if (found.size() >= MAX_SCANNED) {
					return RE::BSContainer::ForEachResult::kStop;
				}
				if (MaybeLoose(a_ref)) {
					found.emplace_back(a_ref);
				} else if (const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr) {
					if (shovers.size() < ObjectShare::MAX_SHOVERS && !actor->IsDead(false) && RunsHere(actor)) {
						shovers.push_back(Position(actor));
					}
				} else if (const auto base = a_ref ? a_ref->GetObjectReference() : nullptr; base && base->Is(RE::ENUM_FORM_ID::kEXPL) && blastRefs.size() < ObjectShare::MAX_BLASTS) {
					blastRefs.emplace_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto& blast : blastRefs) {
				NoteBlast(blast.get(), a_now);
			}
			const Vec3                                  player = Position(a_player);
			std::vector<ObjectShare::KnockWatch::Seen> seen;
			seen.reserve(found.size());
			for (const auto& ref : found) {
				if (!Movable(ref.get())) {
					continue;
				}
				const auto pose = PoseOf(ref.get());
				if (!pose) {
					continue;
				}
				const auto id = ref->GetFormID();
				const bool busy = carried.Find(id) || flying.contains(id) || keyframed.contains(id) || a_ourHeld == id || carry.IsSettling(id) || carry.IsRefused(id);
				const auto reach = blasts.Reaching(pose->position, a_now);
				const bool byNpc = std::ranges::any_of(shovers, [&](const Vec3& a_npc) { return ObjectShare::NpcNear(a_npc, pose->position); });
				seen.push_back({ .refId = id, .pose = *pose, .byUs = ObjectShare::Near(player, pose->position) || HitByUs(id, a_now), .busy = busy, .byNpc = byNpc,
					.blast = reach.serial, .theirs = reach.theirs });
			}
			for (const auto& rest : knocks.Scan(seen, FriendNear(a_player), a_now)) {
				Send(Protocol::Encode(rest, Protocol::MessageType::kReportObjectRest), true);
				++counts.knocksSent;
			}
			PruneHits(a_now);
		}
	}

	void Frame(bool a_welcomed)
	{
		const auto now = Clock::now();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const bool inWorld = player && player->GetParentCell() && !Hud::LoadingScreenUp();
		if (!a_welcomed || !inWorld) {
			if (!keyframed.empty() || !released.empty() || carried.Size() || carry.Holding()) {
				EnableReleased(now, true);
				LetAllLoose();
				carried.Reset();
				carry.Reset();
				flying.clear();
			}
			lastFrame = now;
			return;
		}
		const float seconds = lastFrame == Clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		EnableReleased(now, false);

		// What our player holds: nothing of it in workshop mode (WorkshopSync's), and only a loose object of ours.
		auto       ourHeld = OurHeld();
		const auto heldRef = ourHeld ? Lookup(ourHeld) : RE::NiPointer<RE::TESObjectREFR>{};
		const bool workshop = OpenMenus::Open(OpenMenus::Menu::kWorkshop);
		const auto shared = !workshop && heldRef && Movable(heldRef.get()) ? ourHeld : 0;
		const ObjectShare::Holder holder{ .position = Position(player), .heading = player->data.angle.z };
		carry.Frame(shared, holder, PoseOfId, now);
		for (const auto& hold : carry.TakeHolds()) {
			Send(Protocol::Encode(hold, Protocol::MessageType::kReportObjectHold), false);
			++counts.holdsSent;
		}
		for (const auto& rest : carry.TakeRests()) {
			Send(Protocol::Encode(rest, Protocol::MessageType::kReportObjectRest), true);
			++(rest.kind == Protocol::ObjectRestKind::kLetGo ? counts.letGosSent : counts.restsSent);
		}
		if (shared && carry.IsRefused(shared)) {
			Release(heldRef.get(), now);
		}

		DrawHeld(ourHeld, seconds, now);
		std::erase_if(flying, [&](const auto& a_entry) { return now >= a_entry.second; });

		if (now >= nextScan) {
			nextScan = now + SCAN_INTERVAL;
			ScanKnocks(player, ourHeld, now);
		}
	}

	std::vector<Packet> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnHeld(const Protocol::ObjectHold& a_hold)
	{
		const auto now = Clock::now();
		if (carry.Holding() == a_hold.refId) {
			// A friend grabbed it first: the server gives it to them. Our player lets go (Frame).
			carry.Refused(a_hold.refId);
			++counts.refused;
			REX::INFO("MovedObjects: player {} grabbed {:08X} first", a_hold.playerId, a_hold.refId);
		}
		carry.TakenOver(a_hold.refId);
		flying.erase(a_hold.refId);
		if (!carried.OnHold(a_hold, now)) {
			++counts.holdsIgnored;
		}
	}

	void OnRested(const Protocol::ObjectRest& a_rest)
	{
		const auto now = Clock::now();
		const bool wasHeld = carried.OnLetGo(a_rest, now);
		carry.TakenOver(a_rest.refId);
		const auto ref = Lookup(a_rest.refId);
		const auto player = RE::PlayerCharacter::GetSingleton();
		ObjectShare::Facts facts;
		if (ref && player && Movable(ref.get())) {
			facts = {
				.loaded = true,
				.sameSpace = SpaceOf(ref.get()) == std::pair{ a_rest.cell, a_rest.worldspace },
				.oursHeld = OurHeld() == a_rest.refId,
				.fromUs = ObjectShare::Distance(Position(player), a_rest.position),
				.fromPlace = ObjectShare::Distance(Position(ref.get()), a_rest.position),
			};
		}
		const auto verdict = ObjectShare::JudgeRest(facts);
		if (verdict != ObjectShare::Verdict::kApply) {
			if (wasHeld) {
				LetLoose(a_rest.refId);
			}
			++counts.restsIgnored;
			return;
		}
		LetLoose(a_rest.refId);
		const Pose pose{ .cell = a_rest.cell, .worldspace = a_rest.worldspace, .position = a_rest.position, .angles = a_rest.rotation };
		if (a_rest.kind == Protocol::ObjectRestKind::kLetGo) {
			if (Place(ref.get(), a_rest.position, a_rest.rotation)) {
				SetVelocity(ref.get(), ObjectShare::Launch(a_rest.velocity));
				flying[a_rest.refId] = now + FLIGHT;
				knocks.Placed(a_rest.refId, pose, now);
				++counts.letGosApplied;
			}
			return;
		}
		flying.erase(a_rest.refId);
		const auto here = PoseOf(ref.get());
		if (here && ObjectShare::Distance(here->position, a_rest.position) <= SNAP_DISTANCE && ObjectShare::Turned(here->angles, a_rest.rotation) <= SNAP_TURN) {
			++counts.restsAlready;  // ours came to rest where theirs did
		} else if (Place(ref.get(), a_rest.position, a_rest.rotation)) {
			SetVelocity(ref.get(), {});
			++counts.restsApplied;
		}
		knocks.Placed(a_rest.refId, pose, now);
	}

	void OnHit(std::uint32_t a_ref)
	{
		if (!Protocol::IsShareableRef(a_ref)) {
			return;
		}
		std::scoped_lock l{ hitLock };
		if (hits.size() < 1024) {
			hits[a_ref] = Clock::now();
		}
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		for (const auto id : carried.Leave(a_playerId)) {
			LetLoose(id);
		}
	}

	void BeforeSaveOrLoad()
	{
		EnableReleased(Clock::now(), true);
		LetAllLoose();
		carried.Reset();
		flying.clear();
	}

	void Reset()
	{
		EnableReleased(Clock::now(), true);
		LetAllLoose();
		carried.Reset();
		carry.Reset();
		knocks.Reset();
		blasts.Reset();
		flying.clear();
		outgoing.clear();
		std::scoped_lock l{ hitLock };
		hits.clear();
	}

	std::string Status()
	{
		const auto& c = counts;
		return std::format("objects: held={:08X} holds={}/{} ignored={} letGo={}/{} rests={}/{} already={} ignored={} knocks={} refused={} releases={} badPoses={} "
		                   "carried={} flying={} stillForFriends={} watched={} moving={} blasts={}/{}",
			carry.Holding(), c.holdsSent, c.holdsApplied, c.holdsIgnored, c.letGosSent, c.letGosApplied, c.restsSent, c.restsApplied, c.restsAlready, c.restsIgnored,
			c.knocksSent, c.refused, c.releases, c.badPoses, carried.Size(), flying.size(), keyframed.size(), knocks.Watching(), knocks.Moving(), c.blastsSeen, blasts.Size());
	}

	std::string Command(std::string_view a_args)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (a_args.empty() || a_args == "status") {
			return Status();
		}
		if (a_args == "grab") {
			if (!player) {
				return "error: no player";
			}
			const auto ref = player->grabbedObject.get();
			return std::format("grabbed={:08X} type={} weight={:.1f} distance={:.1f} heading={:.3f} shared={}", ref ? ref->GetFormID() : 0,
				static_cast<int>(player->grabType.get()), player->grabObjectWeight, player->grabDistance, player->data.angle.z, ref && Movable(ref.get()));
		}
		if (a_args.starts_with("probe ")) {
			const auto    hex = a_args.substr(6);
			std::uint32_t id = 0;
			if (std::from_chars(hex.data(), hex.data() + hex.size(), id, 16).ec != std::errc{}) {
				return "error: usage: objects probe <refHex>";
			}
			const auto ref = Lookup(id);
			if (!ref) {
				return "error: no such reference";
			}
			const auto  root = ref->Get3D();
			const auto& l = ref->data.location;
			const auto& a = ref->data.angle;
			std::string out = std::format("ref={:08X} type={} loose={} movable={} loc=({:.2f},{:.2f},{:.2f}) angle=({:.4f},{:.4f},{:.4f})", id,
				ref->GetObjectReference() ? static_cast<int>(ref->GetObjectReference()->GetFormType()) : -1, MaybeLoose(ref.get()), Movable(ref.get()), l.x, l.y, l.z, a.x, a.y, a.z);
			if (root) {
				const auto& t = root->world.translate;
				const auto& r = root->world.rotate.entry;
				float       ex = 0, ey = 0, ez = 0;
				root->world.rotate.ToEulerAnglesXYZ(ex, ey, ez);
				out += std::format(" 3d=({:.2f},{:.2f},{:.2f}) rot=[{:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}] xyz=({:.4f},{:.4f},{:.4f})", t.x, t.y, t.z,
					r[0].x, r[0].y, r[0].z, r[1].x, r[1].y, r[1].z, r[2].x, r[2].y, r[2].z, ex, ey, ez);
				int bodies = 0, moving = 0;
				ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) {
					++bodies;
					if (const auto body = a_collision->GetBody()) {
						std::uint32_t motion = 0;
						std::memcpy(&motion, &body->m_motionId, sizeof(motion));
						moving += motion != 0;
					}
				});
				out += std::format(" bodies={} moving={} keyframedByUs={}", bodies, moving, keyframed.contains(id));
			}
			if (player) {
				out += std::format(" player=({:.1f},{:.1f},{:.1f}) heading={:.4f}", player->data.location.x, player->data.location.y, player->data.location.z, player->data.angle.z);
			}
			return out;
		}
		return "error: usage: objects [status | grab | probe <refHex>]";
	}
}
