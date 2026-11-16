#include "game/RackDisplays.h"

#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/HavokNan.h"
#include "game/ItemCopies.h"
#include "game/OwnGear.h"
#include "game/Papyrus.h"
#include "game/RackShare.h"
#include "game/RefTag.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RackDisplays
{
	namespace
	{
		using namespace std::chrono_literals;
		using Clock = std::chrono::steady_clock;
		using Protocol::RackWeapon;

		// Contraptions Workshop's weapon rack script (DLCworkshop02): OnItemAdded drops the weapon put in the rack and holds it
		// at one of the rack's nodes, linked to the rack with WorkshopStackedItemParentKEYWORD.
		constexpr std::string_view RACK_SCRIPT = "DLC05:WeaponRackScript";
		// Fallout4.esm's WorkshopStackedItemParentKEYWORD (the rack script's property), and the engine's own stack keyword
		// (a BGSKeyword*, the default object WorkshopSync reads stacked pieces by), tried after it.
		constexpr std::uint32_t STACKED_PARENT_KEYWORD = 0x001C5EDD;
		constexpr REL::ID       STACK_KEYWORD{ 4797181 };
		// The default object WorkshopBlockInteractionKeyword's keyword (a BGSKeyword*): workshop mode won't pick a
		// reference that has it, so a copy can't be stored, scrapped or moved there (WorkshopSync's copies have it too).
		constexpr REL::ID BLOCK_INTERACTION_KEYWORD{ 4797360 };

		constexpr auto SCAN_EVERY = 2s;       // our racks looked at
		constexpr auto RESEND_EVERY = 30s;    // our list said again while it holds anything (a list the server dropped heals)
		constexpr auto PLACE_EVERY = 500ms;   // friends' copies placed, checked and removed
		constexpr auto BLOCK_WAIT = 5s;       // a copy whose activation isn't blocked by then goes (and is placed again)
		constexpr auto NO_RACK_WAIT = 5s;     // a friend's weapon with no rack of ours there is looked at again after this
		constexpr int  PLACE_PER_TICK = 4;
		constexpr std::size_t MAX_INBOX = 256;
		constexpr std::size_t MAX_PICKUPS = 16;
		constexpr float       PICKUP_REACH = 512.0f;  // our player this near a copy that left the world may have picked it up
		constexpr int         LOGGED = 40;

		struct Copy
		{
			RE::ObjectRefHandle      ref;
			std::uint32_t            refId = 0;
			std::uint32_t            serial = 0;  // its BlockActivation's: a late answer for an earlier copy is not this one's
			Clock::time_point        placedAt{};
			RackWeapon               entry;
			bool                     blocked = false;  // the VM ran its BlockActivation
			bool                     shown = false;    // enabled
			bool                     held = false;     // held still, at its place
			const RE::NiAVObject*    root = nullptr;   // the 3D it was held with (compared only: a new one is held again)
		};

		std::vector<std::vector<std::uint8_t>> outgoing;

		// Ours: the list last sent, and whether it is sent again (a friend joined, a load, RESEND_EVERY).
		std::vector<RackWeapon>                         ours;
		bool                                            sentOnce = false;
		bool                                            resend = false;
		Clock::time_point                               nextScan{};
		Clock::time_point                               nextResend{};
		// A reference our weapons are linked to, or one near a friend's weapon: a rack (its script)? By its id and its base,
		// so an id the game gives another reference after a scrap isn't taken for the rack it was.
		std::unordered_map<std::uint64_t, bool>         racks;

		// Theirs.
		std::map<std::uint32_t, std::vector<RackWeapon>> lists;  // by player
		std::unordered_map<std::uint64_t, Copy>          copies;  // by RackShare::CopyKey
		std::unordered_set<std::uint64_t>                unplaceable;
		std::unordered_map<std::uint64_t, Clock::time_point> noRackUntil;  // no rack of ours there: not shown until then
		Clock::time_point                                nextPlace{};
		std::uint32_t                                    blockSerial = 0;

		std::mutex                                                inboxLock;
		std::vector<std::pair<std::uint32_t, std::uint32_t>>      blockedInbox;  // (reference, serial)
		std::vector<std::pair<std::uint32_t, Clock::time_point>> pickups;       // weapons our player took from the world: (base, when)

		// Counters (Describe).
		std::uint32_t sent = 0, received = 0, scanned = 0, placed = 0, removed = 0, lost = 0, placeFailed = 0, blockLate = 0, held = 0, reheld = 0, missing = 0,
					  hidden = 0, noRack = 0, pickedUp = 0, takenBack = 0, duplicates = 0, refused = 0;
		int           logged = 0;

		bool Log() { return logged++ < LOGGED; }

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		RackShare::Spot SpotOf(RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref->GetParentCell();
			RackShare::Spot spot{ 0, 0, a_ref->data.location.x, a_ref->data.location.y, a_ref->data.location.z };
			if (cell && cell->IsInterior()) {
				spot.cell = cell->GetFormID();
			} else if (cell && cell->worldSpace) {
				spot.worldspace = cell->worldSpace->GetFormID();
			}
			return spot;
		}

		bool Loose(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !a_ref->IsDeleted() && a_ref->GetParentCell() != nullptr;
		}

		// Whether a_ref is a weapon rack (it has the rack script): looked up once a load per reference and base, never made
		// (a_create false), only found.
		bool IsRack(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || a_ref->IsDeleted()) {
				return false;
			}
			const auto key = (std::uint64_t{ a_ref->GetFormID() } << 32) | base->GetFormID();
			if (const auto it = racks.find(key); it != racks.end()) {
				return it->second;
			}
			const bool isRack = Papyrus::BoundObject(a_ref, RACK_SCRIPT, false) != nullptr;
			racks[key] = isRack;
			if (isRack && Log()) {
				REX::INFO("RackDisplays: rack {} found", RefTag::Of(a_ref));
			}
			return isRack;
		}

		// ---- ours ----

		// The rack a_weapon is held on: the reference it is linked to as a stacked item, when that has the rack script.
		RE::TESObjectREFR* RackOf(RE::TESObjectREFR* a_weapon)
		{
			static const auto                              parent = RE::TESForm::GetFormByID<RE::BGSKeyword>(STACKED_PARENT_KEYWORD);
			static REL::Relocation<RE::BGSKeyword**>       stack{ STACK_KEYWORD };
			RE::TESObjectREFR*                             rack = parent ? a_weapon->GetLinkedRef(parent) : nullptr;
			if (!rack && *stack && *stack != parent) {
				rack = a_weapon->GetLinkedRef(*stack);
			}
			return rack && IsRack(rack) ? rack : nullptr;
		}

		// A weapon one of our racks shows: dropped by the rack's script (made at runtime), there, and linked to the rack.
		std::optional<RackWeapon> Shown(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || base->GetFormType() != RE::ENUM_FORM_ID::kWEAP || !a_ref->IsCreated() || a_ref->IsDeleted() || a_ref->IsDisabled() ||
				SaveGuard::Placed(a_ref) || Spawns::IdOf(a_ref->GetFormID()) != 0 || !RackOf(a_ref)) {
				return std::nullopt;
			}
			auto item = ItemCopies::ItemOf(base, a_ref->extraList.get());
			if (!item || !Protocol::IsShareableRef(item->base) ||
				std::ranges::any_of(item->mods, [](const Protocol::ItemMod& a_mod) { return !Protocol::IsShareableRef(a_mod.mod); })) {
				return std::nullopt;  // made at runtime here, or a mod that was: no other game has it
			}
			const auto spot = SpotOf(a_ref);
			if (spot.cell == 0 && spot.worldspace == 0) {
				return std::nullopt;
			}
			RackWeapon entry;
			entry.key = a_ref->GetFormID();
			entry.item = std::move(*item);
			entry.item.cell = spot.cell;
			entry.item.worldspace = spot.worldspace;
			entry.item.x = spot.x;
			entry.item.y = spot.y;
			entry.item.z = spot.z;
			entry.angleX = a_ref->data.angle.x;
			entry.angleY = a_ref->data.angle.y;
			entry.angleZ = a_ref->data.angle.z;
			return entry;
		}

		void Send(const std::vector<RackWeapon>& a_list)
		{
			Protocol::RackWeapons msg;
			msg.entries = a_list;
			outgoing.push_back(Protocol::Encode(msg, Protocol::MessageType::kReportRackWeapons));
			++sent;
		}

		void ScanOurs(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			if (a_now < nextScan) {
				return;
			}
			nextScan = a_now + SCAN_EVERY;
			const auto tes = RE::TES::GetSingleton();
			if (!tes || !a_player->GetParentCell()) {
				return;
			}
			std::vector<RackWeapon> found;
			tes->ForEachReferenceInRange(a_player->data.location, RackShare::SCAN_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				if (auto entry = Shown(a_ref)) {
					found.push_back(std::move(*entry));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			++scanned;
			auto now = RackShare::Merge(ours, std::move(found), SpotOf(a_player));
			// The server passes a list on only as its event budget allows, and we send on a change: one it dropped is healed
			// by saying a list that holds anything again now and then.
			if (sentOnce && !ours.empty() && a_now >= nextResend) {
				resend = true;
			}
			if (RackShare::SameList(now, ours) && sentOnce && !resend) {
				return;
			}
			if (now.empty() && !sentOnce && !resend) {
				return;  // nothing shown, and nothing ever said
			}
			if (!resend && Log()) {
				REX::INFO("RackDisplays: our racks show {} weapon(s); told the others", now.size());
			}
			ours = std::move(now);
			Send(ours);
			sentOnce = true;
			resend = false;
			nextResend = a_now + RESEND_EVERY;
		}

		// ---- theirs ----

		class Blocked final :
			public RE::BSScript::IStackCallbackFunctor
		{
		public:
			Blocked(std::uint32_t a_refId, std::uint32_t a_serial) :
				refId(a_refId),
				serial(a_serial)
			{}

			void CallQueued() override {}
			void CallCanceled() override {}
			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}
			void operator()(RE::BSScript::Variable) override
			{
				std::scoped_lock l{ inboxLock };
				if (blockedInbox.size() < MAX_INBOX) {
					blockedInbox.emplace_back(refId, serial);
				}
			}

		private:
			std::uint32_t refId;
			std::uint32_t serial;
		};

		RE::ObjectRefHandle PlaceCopy(const RackWeapon& a_entry, RE::TESObjectCELL* a_cell, std::uint32_t a_serial)
		{
			const auto form = RE::TESForm::GetFormByID(a_entry.item.base);
			const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!object) {
				return {};
			}
			RE::NEW_REFR_DATA data;
			data.location = { a_entry.item.x, a_entry.item.y, a_entry.item.z };
			data.direction = { a_entry.angleX, a_entry.angleY, a_entry.angleZ };
			data.object = object;
			data.interior = a_cell->IsInterior() ? a_cell : nullptr;
			data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
			data.extra = ItemCopies::ExtraFor(a_entry.item, object);
			data.clearStillLoadingFlag = true;
			data.initiallyDisabled = true;  // shown once its activation is blocked
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref) {
				return {};
			}
			SaveGuard::Track(ref.get());
			static REL::Relocation<RE::BGSKeyword**> keyword{ BLOCK_INTERACTION_KEYWORD };
			if (*keyword) {
				ref->AddKeyword(*keyword);
			}
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> blocked{ new Blocked(ref->GetFormID(), a_serial) };
			if (!Papyrus::CallMethodThen(blocked, ref.get(), "ObjectReference", "BlockActivation", true, true, false)) {
				RemotePlayers::Discard(handle, Clock::now());
				return {};
			}
			return handle;
		}

		// How long ago our player took a weapon of a_base from the world (-1: not lately). The pickup paired with a copy is
		// used up, so one pickup never stands for two copies.
		float SincePickup(std::uint32_t a_base, Clock::time_point a_now, bool a_take)
		{
			std::scoped_lock l{ inboxLock };
			std::erase_if(pickups, [&](const auto& a_pickup) { return a_now - a_pickup.second > 30s; });
			for (auto it = pickups.rbegin(); it != pickups.rend(); ++it) {
				if (it->first == a_base) {
					const float since = std::chrono::duration<float>(a_now - it->second).count();
					if (a_take && since <= RackShare::PICKUP_PAIRING_SECONDS) {
						pickups.erase(std::next(it).base());
					}
					return since;
				}
			}
			return -1.0f;
		}

		// A copy that left the world past its blocked activation (a_ref: its reference, when it still resolves): an inventory
		// took it. Racks give nothing (a weapon in a rack stays its owner's), so the weapon is taken back out of the inventory
		// the engine names, or our player's when they just picked up one of its kind beside it; else both games have it, which
		// is counted and said. False: nothing says it was taken (its cell went, say).
		bool TakeBack(const Copy& a_copy, RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			RE::NiPointer<RE::TESObjectREFR> named;
			if (a_ref && a_ref->extraList) {
				if (const auto handles = a_ref->extraList->GetByType<RE::ExtraReferenceHandles>()) {
					if (auto holder = handles->containerRef.get(); holder && holder.get() != a_ref) {
						named = std::move(holder);
					}
				}
			}
			const auto player = Player();
			const bool near = player && player->GetParentCell() && RackShare::SameSpace(SpotOf(player), RackShare::SpotOf(a_copy.entry)) &&
			                  RackShare::Distance3(SpotOf(player), RackShare::SpotOf(a_copy.entry)) <= PICKUP_REACH;
			const float since = near && !named ? SincePickup(a_copy.entry.item.base, a_now, true) : -1.0f;
			RE::TESObjectREFR* from = nullptr;
			switch (RackShare::HolderOf(named != nullptr, since)) {
			case RackShare::Holder::kNamed:
				from = named.get();
				break;
			case RackShare::Holder::kPlayer:
				from = player;
				break;
			case RackShare::Holder::kUnknown:
				if (!a_ref) {
					return false;  // its handle went with nothing to say a pickup took it: its cell went
				}
				++pickedUp;
				++duplicates;
				REX::WARN("RackDisplays: copy {:08X} of a friend's rack weapon {:08X} left the world past its block into an inventory we can't name; both games have it",
					a_copy.refId, a_copy.entry.item.base);
				return true;
			}
			++pickedUp;
			if (from && Equipment::MayChange(from, OwnGear::Change::kRackCopy) && ItemCopies::TakeOut(from, a_copy.entry.item, 1)) {
				++takenBack;
				REX::WARN("RackDisplays: copy {:08X} of a friend's rack weapon {:08X} was picked up past its block; taken back out of {:08X}", a_copy.refId,
					a_copy.entry.item.base, from->GetFormID());
			} else {
				++duplicates;
				REX::WARN("RackDisplays: copy {:08X} of a friend's rack weapon {:08X} was picked up past its block and isn't in {:08X} to take back; both games have it",
					a_copy.refId, a_copy.entry.item.base, from ? from->GetFormID() : 0);
			}
			return true;
		}

		void RemoveCopy(std::unordered_map<std::uint64_t, Copy>::iterator a_it, Clock::time_point a_now)
		{
			auto&      copy = a_it->second;
			const auto ref = copy.ref.get();
			if (!ref) {
				if (!TakeBack(copy, nullptr, a_now)) {
					++lost;  // the game took it from under us (its cell went)
				}
				SaveGuard::Forget(copy.refId);
			} else if (Loose(ref.get())) {
				if (Equipment::MayChange(ref.get(), OwnGear::Change::kDropCopy)) {
					RemotePlayers::Discard(copy.ref, copy.placedAt);
					++removed;
				} else {
					++refused;  // left as it is: SaveGuard still keeps it out of saves
				}
			} else {
				TakeBack(copy, ref.get(), a_now);
				if (!ref->IsDeleted()) {
					SaveGuard::Release(copy.refId);  // a reference an inventory holds now: the game's to save as it has it
				}
			}
			copies.erase(a_it);
		}

		// Every body under the copy's 3D (a walk of at most HavokNan::NODE_LIMIT nodes), as MovedObjects walks them.
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

		// The copy as the rack holds the weapon: still (keyframed, as the rack script sets it), touching nothing, at the
		// owner's place and angles. False until its 3D and its bodies are there.
		bool Hold(Copy& a_copy, RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root)
		{
			int bodies = 0;
			ForEachCollision(a_root, [&](RE::bhkNPCollisionObject* a_collision) {
				a_collision->SetMotionType(RE::hknpMotionPropertiesId::Preset::kKeyframed);
				if (const auto body = a_collision->GetBody()) {
					++bodies;
					RE::CFilter filter{ body->m_collisionFilterInfo };
					filter.SetNoCollision(true);
					a_collision->SetCollisionFilterInfo(filter);
				}
			});
			if (bodies == 0) {
				return false;  // its physics not made yet: held on the next frame
			}
			const auto& entry = a_copy.entry;
			a_ref->SetLocationOnReference({ entry.item.x, entry.item.y, entry.item.z });
			a_ref->SetAngleOnReference({ entry.angleX, entry.angleY, entry.angleZ });
			a_ref->Update3DPosition(true);
			return true;
		}

		// Every frame: each shown copy is held as soon as its bodies are made (a dynamic weapon left a moment longer falls to
		// the floor and knocks what it lands on), and held again when its 3D is made anew in place.
		void HoldShown()
		{
			for (auto& [key, copy] : copies) {
				if (!copy.shown) {
					continue;
				}
				const auto ref = copy.ref.get();
				if (!ref || !Loose(ref.get())) {
					continue;  // ShowTheirs sees to it
				}
				const auto root = ref->Get3D();
				if (copy.held && root != copy.root) {
					copy.held = false;
					++reheld;
				}
				if (!copy.held && root && Hold(copy, ref.get(), root)) {
					copy.held = true;
					copy.root = root;
					++held;
				}
			}
		}

		const RackWeapon* EntryOf(std::uint64_t a_key)
		{
			const auto list = lists.find(static_cast<std::uint32_t>(a_key >> 32));
			if (list == lists.end()) {
				return nullptr;
			}
			const auto key = static_cast<std::uint32_t>(a_key & 0xFFFFFFFFu);
			const auto it = std::ranges::find(list->second, key, &RackWeapon::key);
			return it != list->second.end() ? &*it : nullptr;
		}

		// Whether our game has a rack where a friend's weapon hangs (RackShare::RackThere). Looked at again NO_RACK_WAIT after
		// a miss (its cell may still be loading).
		bool RackHere(std::uint64_t a_key, const RackWeapon& a_entry, Clock::time_point a_now)
		{
			if (const auto until = noRackUntil.find(a_key); until != noRackUntil.end() && a_now < until->second) {
				return false;
			}
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return false;
			}
			std::vector<RackShare::Spot> found;
			tes->ForEachReferenceInRange(RE::NiPoint3{ a_entry.item.x, a_entry.item.y, a_entry.item.z }, RackShare::RACK_NEAR, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref->GetParentCell() && IsRack(a_ref)) {
					found.push_back(SpotOf(a_ref));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			if (RackShare::RackThere(a_entry, found)) {
				noRackUntil.erase(a_key);
				return true;
			}
			if (!noRackUntil.contains(a_key) && Log()) {
				REX::INFO("RackDisplays: no rack here for player {}'s rack weapon {:08X}: not shown (it would hang in mid air)", static_cast<std::uint32_t>(a_key >> 32),
					a_entry.item.base);
			}
			noRackUntil[a_key] = a_now + NO_RACK_WAIT;
			++noRack;
			return false;
		}

		void ShowTheirs(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> blockedNow;
			{
				std::scoped_lock l{ inboxLock };
				blockedNow.swap(blockedInbox);
			}
			for (const auto& [refId, serial] : blockedNow) {
				if (const auto it = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second.refId == refId && a_entry.second.serial == serial; });
					it != copies.end()) {
					it->second.blocked = true;
				}
			}
			const auto cell = a_player->GetParentCell();
			if (!cell || a_now < nextPlace) {
				return;
			}
			nextPlace = a_now + PLACE_EVERY;
			const auto here = SpotOf(a_player);
			for (auto it = copies.begin(); it != copies.end();) {
				auto&      copy = it->second;
				const auto ref = copy.ref.get();
				if (!ref || !Loose(ref.get())) {
					const auto gone = it++;
					RemoveCopy(gone, a_now);  // its cell went, or an inventory took it (taken back)
					continue;
				}
				const auto entry = EntryOf(it->first);
				const bool keep = entry && RackShare::Same(*entry, copy.entry) && RackShare::Near(RackShare::SpotOf(*entry), here, RackShare::REMOVE_RANGE) &&
				                  !RackShare::OursThere(*entry, ours);
				const bool late = !copy.blocked && a_now - copy.placedAt >= BLOCK_WAIT;
				if (!keep || late) {
					if (late) {
						++blockLate;
						if (Log()) {
							REX::WARN("RackDisplays: copy {:08X} wasn't blocked in time; placed again", copy.refId);
						}
					}
					const auto gone = it++;
					RemoveCopy(gone, a_now);
					continue;
				}
				if (copy.blocked && !copy.shown) {
					ref->Enable(false);
					copy.shown = true;  // held from the next frame on, as soon as its bodies are there (HoldShown)
				}
				++it;
			}
			int placedNow = 0;
			for (const auto& [playerId, list] : lists) {
				for (const auto& entry : list) {
					if (placedNow >= PLACE_PER_TICK) {
						return;
					}
					const auto key = RackShare::CopyKey(playerId, entry.key);
					if (copies.contains(key) || unplaceable.contains(key) || !RackShare::Near(RackShare::SpotOf(entry), here, RackShare::PLACE_RANGE)) {
						continue;
					}
					if (RackShare::OursThere(entry, ours)) {
						++hidden;
						continue;
					}
					if (!ItemCopies::Placeable(entry.item)) {
						unplaceable.insert(key);
						++missing;
						if (Log()) {
							REX::WARN("RackDisplays: player {}'s rack weapon {:08X} ({} mods) can't be shown here: it or a mod is missing from this game", playerId, entry.item.base,
								entry.item.mods.size());
						}
						continue;
					}
					if (!RackHere(key, entry, a_now)) {
						continue;
					}
					++placedNow;
					const auto serial = ++blockSerial;
					const auto handle = PlaceCopy(entry, cell, serial);
					const auto ref = handle.get();
					if (!ref) {
						++placeFailed;
						unplaceable.insert(key);
						continue;
					}
					copies[key] = { handle, ref->GetFormID(), serial, a_now, entry };
					++placed;
					if (Log()) {
						REX::INFO("RackDisplays: placed copy {:08X} of player {}'s rack weapon {:08X} ({} mods)", ref->GetFormID(), playerId, entry.item.base, entry.item.mods.size());
					}
				}
			}
		}
	}

	void Frame()
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		const auto now = Clock::now();
		ScanOurs(player, now);
		ShowTheirs(player, now);
		HoldShown();
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnList(const Protocol::RackWeapons& a_list)
	{
		++received;
		if (a_list.playerId == 0) {
			return;
		}
		if (a_list.entries.empty()) {
			lists.erase(a_list.playerId);
		} else {
			lists[a_list.playerId] = a_list.entries;
		}
		// A weapon that changed may be shown again, even one that couldn't be.
		const auto theirs = [&](std::uint64_t a_key) { return static_cast<std::uint32_t>(a_key >> 32) == a_list.playerId; };
		std::erase_if(unplaceable, theirs);
		std::erase_if(noRackUntil, [&](const auto& a_entry) { return theirs(a_entry.first); });
	}

	void OnPlayerPickedUp(std::uint32_t a_base)
	{
		std::scoped_lock l{ inboxLock };
		if (pickups.size() >= MAX_PICKUPS) {
			pickups.erase(pickups.begin());
		}
		pickups.emplace_back(a_base, Clock::now());
	}

	void OnPlayerJoined()
	{
		if (sentOnce && !ours.empty()) {
			resend = true;
			nextScan = {};
		}
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		lists.erase(a_playerId);
		const auto now = Clock::now();
		for (auto it = copies.begin(); it != copies.end();) {
			if (static_cast<std::uint32_t>(it->first >> 32) == a_playerId) {
				const auto gone = it++;
				RemoveCopy(gone, now);
			} else {
				++it;
			}
		}
	}

	void DiscardCopies()
	{
		const auto now = Clock::now();
		for (auto it = copies.begin(); it != copies.end();) {
			const auto gone = it++;
			RemoveCopy(gone, now);
		}
		std::scoped_lock l{ inboxLock };
		blockedInbox.clear();
	}

	void OnGameLoaded()
	{
		// What we said before stays said until the walk sees otherwise; the places out of sight are another world's now.
		if (sentOnce) {
			ours.clear();
			resend = true;
		}
		racks.clear();
		unplaceable.clear();
		noRackUntil.clear();
		nextScan = {};
		nextPlace = {};
		std::scoped_lock l{ inboxLock };
		pickups.clear();
	}

	void Reset()
	{
		DiscardCopies();
		lists.clear();
		unplaceable.clear();
		noRackUntil.clear();
		ours.clear();
		racks.clear();
		sentOnce = false;
		resend = false;
		outgoing.clear();
		logged = 0;  // each session's log says what its racks did
		{
			std::scoped_lock l{ inboxLock };
			pickups.clear();
		}
	}

	std::string Describe()
	{
		return std::format(
			"racks=ours:{} theirs:{} copies={} sent={} got={} scans={} placed={} removed={} held={} reheld={} lost={} failed={} late={} missing={} hidden={} noRack={} picked={} takenBack={} dup={} refused={}",
			ours.size(), lists.size(), copies.size(), sent, received, scanned, placed, removed, held, reheld, lost, placeFailed, blockLate, missing, hidden, noRack, pickedUp,
			takenBack, duplicates, refused);
	}
}
