#include "game/RackDisplays.h"

#include "game/EngineRefs.h"
#include "game/HavokNan.h"
#include "game/ItemCopies.h"
#include "game/Papyrus.h"
#include "game/RackShare.h"
#include "game/RefTag.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"

#include <chrono>
#include <format>
#include <algorithm>
#include <map>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

		constexpr auto SCAN_EVERY = 2s;      // our racks looked at
		constexpr auto PLACE_EVERY = 500ms;  // friends' copies placed, checked and removed
		constexpr auto BLOCK_WAIT = 5s;      // a copy whose activation isn't blocked by then goes (and is placed again)
		constexpr int  PLACE_PER_TICK = 4;
		constexpr std::size_t MAX_INBOX = 256;
		constexpr int  LOGGED = 40;

		struct Copy
		{
			RE::ObjectRefHandle ref;
			std::uint32_t       refId = 0;
			Clock::time_point   placedAt{};
			RackWeapon          entry;
			bool                blocked = false;  // the VM ran its BlockActivation
			bool                shown = false;    // enabled
			bool                held = false;     // held still, at its place
		};

		std::vector<std::vector<std::uint8_t>> outgoing;

		// Ours: the list last sent, and whether it is sent again (a friend joined, a load).
		std::vector<RackWeapon>                         ours;
		bool                                            sentOnce = false;
		bool                                            resend = false;
		Clock::time_point                               nextScan{};
		std::unordered_map<std::uint32_t, bool>         racks;  // a reference our weapons are linked to: a rack (its script)?

		// Theirs.
		std::map<std::uint32_t, std::vector<RackWeapon>> lists;  // by player
		std::unordered_map<std::uint64_t, Copy>          copies;  // by RackShare::CopyKey
		std::unordered_set<std::uint64_t>                unplaceable;
		Clock::time_point                                nextPlace{};

		std::mutex                 inboxLock;
		std::vector<std::uint32_t> blockedInbox;

		// Counters (Describe).
		std::uint32_t sent = 0, received = 0, scanned = 0, placed = 0, removed = 0, lost = 0, placeFailed = 0, blockLate = 0, held = 0, missing = 0,
					  hidden = 0, pickedUp = 0;
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
			if (!rack || rack->IsDeleted()) {
				return nullptr;
			}
			const auto id = rack->GetFormID();
			if (const auto it = racks.find(id); it != racks.end()) {
				return it->second ? rack : nullptr;
			}
			// Looked up once a load: never made (a_create false), only found.
			const bool isRack = Papyrus::BoundObject(rack, RACK_SCRIPT, false) != nullptr;
			racks[id] = isRack;
			if (isRack && Log()) {
				REX::INFO("RackDisplays: rack {} holds weapons", RefTag::Of(rack));
			}
			return isRack ? rack : nullptr;
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
			if (RackShare::SameList(now, ours) && sentOnce && !resend) {
				return;
			}
			if (now.empty() && !sentOnce && !resend) {
				return;  // nothing shown, and nothing ever said
			}
			if (Log()) {
				REX::INFO("RackDisplays: our racks show {} weapon(s); told the others", now.size());
			}
			ours = std::move(now);
			Send(ours);
			sentOnce = true;
			resend = false;
		}

		// ---- theirs ----

		class Blocked final :
			public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Blocked(std::uint32_t a_refId) :
				refId(a_refId)
			{}

			void CallQueued() override {}
			void CallCanceled() override {}
			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}
			void operator()(RE::BSScript::Variable) override
			{
				std::scoped_lock l{ inboxLock };
				if (blockedInbox.size() < MAX_INBOX) {
					blockedInbox.push_back(refId);
				}
			}

		private:
			std::uint32_t refId;
		};

		RE::ObjectRefHandle PlaceCopy(const RackWeapon& a_entry, RE::TESObjectCELL* a_cell)
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
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> blocked{ new Blocked(ref->GetFormID()) };
			if (!Papyrus::CallMethodThen(blocked, ref.get(), "ObjectReference", "BlockActivation", true, true, false)) {
				RemotePlayers::Discard(handle, Clock::now());
				return {};
			}
			return handle;
		}

		void RemoveCopy(std::unordered_map<std::uint64_t, Copy>::iterator a_it)
		{
			if (const auto ref = a_it->second.ref.get()) {
				if (Loose(ref.get())) {
					RemotePlayers::Discard(a_it->second.ref, a_it->second.placedAt);
					++removed;
				} else if (!ref->IsDeleted()) {
					// Taken although its activation was blocked: an item in an inventory, ours to keep as the game has it.
					++pickedUp;
					SaveGuard::Release(a_it->second.refId);
					REX::WARN("RackDisplays: copy {:08X} was picked up although its activation was blocked", a_it->second.refId);
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
		bool Hold(Copy& a_copy)
		{
			const auto ref = a_copy.ref.get();
			const auto root = ref ? ref->Get3D() : nullptr;
			if (!root) {
				return false;
			}
			int bodies = 0;
			ForEachCollision(root, [&](RE::bhkNPCollisionObject* a_collision) {
				a_collision->SetMotionType(RE::hknpMotionPropertiesId::Preset::kKeyframed);
				if (const auto body = a_collision->GetBody()) {
					++bodies;
					RE::CFilter filter{ body->m_collisionFilterInfo };
					filter.SetNoCollision(true);
					a_collision->SetCollisionFilterInfo(filter);
				}
			});
			if (bodies == 0) {
				return false;  // its physics not made yet: held on a later look
			}
			const auto& entry = a_copy.entry;
			ref->SetLocationOnReference({ entry.item.x, entry.item.y, entry.item.z });
			ref->SetAngleOnReference({ entry.angleX, entry.angleY, entry.angleZ });
			ref->Update3DPosition(true);
			return true;
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

		void ShowTheirs(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			std::vector<std::uint32_t> blockedNow;
			{
				std::scoped_lock l{ inboxLock };
				blockedNow.swap(blockedInbox);
			}
			for (const auto refId : blockedNow) {
				if (const auto it = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second.refId == refId; }); it != copies.end()) {
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
				if (!ref) {
					++lost;  // the game took it from under us (its cell went)
					SaveGuard::Forget(copy.refId);
					it = copies.erase(it);
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
					RemoveCopy(gone);
					continue;
				}
				if (copy.blocked && !copy.shown) {
					ref->Enable(false);
					copy.shown = true;
				}
				if (copy.shown && !copy.held && Hold(copy)) {
					copy.held = true;
					++held;
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
					++placedNow;
					const auto handle = PlaceCopy(entry, cell);
					const auto ref = handle.get();
					if (!ref) {
						++placeFailed;
						unplaceable.insert(key);
						continue;
					}
					copies[key] = { handle, ref->GetFormID(), a_now, entry };
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
		std::erase_if(unplaceable, [&](std::uint64_t a_key) { return static_cast<std::uint32_t>(a_key >> 32) == a_list.playerId; });
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
		for (auto it = copies.begin(); it != copies.end();) {
			if (static_cast<std::uint32_t>(it->first >> 32) == a_playerId) {
				const auto gone = it++;
				RemoveCopy(gone);
			} else {
				++it;
			}
		}
	}

	void DiscardCopies()
	{
		for (auto it = copies.begin(); it != copies.end();) {
			const auto gone = it++;
			RemoveCopy(gone);
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
		nextScan = {};
		nextPlace = {};
	}

	void Reset()
	{
		DiscardCopies();
		lists.clear();
		unplaceable.clear();
		ours.clear();
		racks.clear();
		sentOnce = false;
		resend = false;
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("racks=ours:{} theirs:{} copies={} sent={} got={} scans={} placed={} removed={} held={} lost={} failed={} late={} missing={} hidden={} picked={}",
			ours.size(), lists.size(), copies.size(), sent, received, scanned, placed, removed, held, lost, placeFailed, blockLate, missing, hidden, pickedUp);
	}
}
