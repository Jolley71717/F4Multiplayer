#include "game/Drops.h"

#include "game/DropBook.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/FrameLoans.h"
#include "game/Hud.h"
#include "game/ItemCopies.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"
#include "game/StandInGear.h"
#include "Text.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace Drops
{
	namespace
	{
		using namespace std::chrono_literals;
		using Clock = DropBook::Clock;

		// A copy stands while our player is this near where its item lies (well inside the cells the game has loaded
		// around them), and goes once they are farther than REMOVE_RANGE (it is placed again when they come back).
		constexpr float PLACE_RANGE = 5000.0f;
		constexpr float REMOVE_RANGE = 7000.0f;
		// Where an item our player dropped is looked for when the engine's event names no reference.
		constexpr float FIND_RADIUS = 400.0f;
		// A copy is placed hidden and shown once the script call blocking its activation has run (it is queued to the script
		// VM, which says when it has): never a moment in which Take would pick it up without asking. One whose call hasn't
		// run by BLOCK_WAIT goes (it is placed again).
		constexpr auto  BLOCK_WAIT = 5s;
		constexpr auto  PLACE_EVERY = 500ms;
		constexpr int   PLACE_PER_FRAME = 2;
		// A granted take is checked the frames after: our player's inventory has the item, or it is tried again, then given
		// back to the session.
		constexpr int TAKE_TRIES = 4;
		constexpr auto TAKE_EVERY = 250ms;
		constexpr std::size_t MAX_PENDING = 32;
		constexpr std::size_t MAX_INBOX = 64;
		constexpr std::size_t MAX_SEEN = 256;
		constexpr std::uint32_t LOGGED = 40;  // lines about single drops a session (the counters go on)

		// ---- the engine's events (any thread) ----

		struct Left
		{
			std::uint32_t base = 0;
			std::int32_t  count = 0;
			std::uint32_t ref = 0;
			bool          inMenu = false;  // the Pip-Boy or the console was open: where items are dropped
		};
		std::mutex                 inboxLock;
		std::vector<Left>          leftInbox;
		std::vector<std::uint32_t> activatedInbox;
		std::vector<std::uint32_t> equippedInbox;  // bases our player equipped, or used up (food, a stim)
		std::vector<std::uint32_t> blockedInbox;   // our copies (form IDs) whose activation is blocked now (the VM's threads)
		std::vector<std::uint32_t> scrappedInbox;  // our copies (form IDs) the workshop's scrap event named
		// What the workshop took away (any reference, with its workbench), and what came into a container from nothing while
		// workshop mode was up: what a scrap of our copy put in the workbench (DropBook::ScrapYields).
		struct Destroyed
		{
			std::uint32_t     ref = 0;
			std::uint32_t     bench = 0;
			Clock::time_point at{};
		};
		struct Added
		{
			std::uint32_t     container = 0;
			std::uint32_t     base = 0;
			std::int32_t      count = 0;
			Clock::time_point at{};
		};
		std::vector<Destroyed>     destroyedInbox;
		std::vector<Added>         addedInbox;
		std::atomic<bool>          workshopUp{ false };  // workshop mode up, or closed within SCRAP_GRACE (the main thread says)
		// Our copies' form IDs as of the last frame, for the workshop's event sinks (any thread): a copy is never reported as
		// a workshop piece, and its scrap comes here.
		std::unordered_set<std::uint32_t> copyRefIds;  // under inboxLock
		std::atomic<bool>          listening{ false };  // welcomed: our player's drops are the session's
		std::atomic<bool>          keptInContainer{ false };
		std::atomic<bool>          devDrop{ false };

		// ---- the record (the co-save's thread too) ----

		std::mutex       recordLock;
		DropBook::Record record;

		// ---- main thread ----

		std::uint32_t localId = 0;

		// An item our player dropped, from the drop until the server's answer.
		struct Pending
		{
			RE::ObjectRefHandle ref;
			std::uint32_t       refId = 0;
			std::uint32_t       base = 0;
			std::int32_t        count = 0;
			std::uint64_t       nonce = 0;
			Clock::time_point   droppedAt{};
			Clock::time_point   movedAt{};
			RE::NiPoint3        last{};
			bool                reported = false;
		};
		std::vector<Pending> pending;

		// What lies in the session, as the server says.
		std::map<std::uint32_t, Protocol::DroppedItem> lying;
		std::vector<Protocol::DroppedItem>             listing;  // a kDropList's packets until its last

		// Our copies of what lies there, by drop ID.
		struct Copy
		{
			RE::ObjectRefHandle ref;
			std::uint32_t       refId = 0;
			Clock::time_point   placedAt{};
			bool                blocked = false;  // its activation is (the VM ran the call)
			bool                shown = false;
		};
		std::unordered_map<std::uint32_t, Copy> copies;
		std::unordered_set<std::uint32_t>       unplaceable;  // an item with a base or a mod this game lacks

		DropBook::Claims claims;
		// Copies our player scrapped in workshop mode, claimed as their take until the server answers.
		DropBook::Scraps  scraps;
		Clock::time_point lastWorkshop{};  // when workshop mode was last seen up
		// What our scraps put in their workbenches, to take back out of one lost to a friend's claim: yields known while the
		// answer waits, and scraps lost before their yield was known.
		DropBook::ScrapYields                                     yields;
		std::unordered_map<std::uint32_t, DropBook::ScrapYields::Yield> yieldsKept;
		std::unordered_set<std::uint32_t>                         lostScraps;
		// ... and each lost scrap's item, as the session had it: an item stored whole comes out of the workbench as that item.
		std::unordered_map<std::uint32_t, Protocol::DroppedItem>  lostItems;
		constexpr auto                                            SCRAP_GRACE = std::chrono::seconds(1);  // the menu may close in the frame of the scrap
		// Our drops swapped for a copy that the server hasn't said lie there yet: given back to our world if we leave first.
		DropBook::Unconfirmed unconfirmed;
		bool                  devLosePlaced = false;  // dev only: the next kPlaced isn't sent (as one lost with the link)
		// Copies our player picked up although their activation should have been blocked: claimed as any take, and taken
		// out of their inventory again if someone else gets it.
		std::unordered_set<std::uint32_t> pickedUnblocked;

		// A take the server granted us, being finished.
		struct Take
		{
			std::uint32_t       dropId = 0;
			RE::ObjectRefHandle ref;
			std::uint32_t       refId = 0;
			std::uint32_t       base = 0;
			std::int32_t        count = 0;
			std::uint32_t       before = 0;
			int                 tries = 0;
			Clock::time_point   placedAt{};
			Clock::time_point   triedAt{};
		};
		std::optional<Take> taking;

		// References of an item's base seen near our player at a drop the event named none for, that weren't it: not taken
		// for a later one.
		std::unordered_set<std::uint32_t> seenLoose;
		// Our player's count of each base they carry, a frame before the events at hand (DropBook::MayScan): kept while the
		// Pip-Boy or the console is open, where items are dropped.
		std::optional<std::unordered_map<std::uint32_t, std::uint32_t>> carriedBefore;
		// Bases our player used up (an equip event) or we took out of their inventory, since the last frame: not scanned for.
		std::unordered_set<std::uint32_t> usedNow;

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextPlace{};
		bool                                   hinted = false;     // the container hint, once a session
		bool                                   refusalSaid = false;  // the refusal, once a session

		std::uint32_t dropped = 0, notGiven = 0, reported = 0, refused = 0, swapped = 0, received = 0, placed = 0, placeFailed = 0, removed = 0;
		std::uint32_t claimed = 0, taken = 0, takeFailed = 0, givenBack = 0, lost = 0, reclaimed = 0, unblocked = 0, dupRisk = 0, takenFirst = 0, foundByScan = 0;
		std::uint32_t notScanned = 0, blockLate = 0, takenBack = 0, restored = 0, restoreFailed = 0;
		std::uint32_t scrapped = 0, scrapLost = 0;  // copies scrapped in workshop mode: granted to us / someone else had it first
		std::uint32_t scrapTakenBack = 0, scrapShort = 0;  // lost scraps whose yield was taken out of the workbench / with none seen
		std::uint32_t scrapRefused = 0;  // items of lost scraps left in the workbench: stored whole, not to be told from our player's own
		std::uint32_t logged = 0;

		bool Log() { return logged++ < LOGGED; }

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		std::uint64_t NewNonce()
		{
			static std::mt19937_64 random{ (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}() };
			std::uint64_t          nonce = 0;
			while (nonce == 0) {
				nonce = random();
			}
			return nonce;
		}

		void Send(std::vector<std::uint8_t> a_packet)
		{
			outgoing.push_back(std::move(a_packet));
		}

		void Claim(std::uint32_t a_dropId, Protocol::DropClaim a_op)
		{
			Send(Protocol::Encode(Protocol::ClaimDrop{ a_dropId, a_op }));
		}

		std::uint32_t Carried(std::uint32_t a_base)
		{
			const auto player = Player();
			const auto form = RE::TESForm::GetFormByID(a_base);
			std::uint32_t count = 0;
			if (player && form) {
				player->GetItemCount(count, form, false);
			}
			return count;
		}

		bool Loose(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !a_ref->IsDeleted() && a_ref->GetParentCell() != nullptr;
		}

		// An item lies there: loose and switched on. Workshop mode's scrap may only switch it off until the game deletes it,
		// and a scrapped item is no drop to swap for a copy (the player has its components).
		bool Lies(RE::TESObjectREFR* a_ref)
		{
			return Loose(a_ref) && !a_ref->IsDisabled();
		}

		// Our player's count of every base they carry.
		std::unordered_map<std::uint32_t, std::uint32_t> CarriedAll()
		{
			std::unordered_map<std::uint32_t, std::uint32_t> out;
			const auto                                       player = Player();
			const auto                                       list = player ? player->inventoryList : nullptr;
			if (!list) {
				return out;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object) {
					continue;
				}
				auto& count = out[item.object->GetFormID()];
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					count += stack->count;
				}
			}
			return out;
		}

		// A reference another of our systems placed or keeps (a stand-in's, a copy of a friend's NPC or frame, a light): never
		// taken for an item our player dropped, nor swapped out for a copy.
		bool OtherSystems(const RE::TESObjectREFR* a_ref)
		{
			return SaveGuard::Placed(a_ref) || Spawns::IdOf(a_ref->GetFormID()) != 0;
		}

		LootScope::Item KindOf(const RE::TESForm* a_form)
		{
			using Item = LootScope::Item;
			using Type = RE::ENUM_FORM_ID;
			switch (a_form ? a_form->GetFormType() : Type::kNONE) {
			case Type::kKEYM:
				return Item::kKey;
			case Type::kNOTE:
				return Item::kNote;
			case Type::kBOOK:
				return Item::kBook;
			case Type::kWEAP:
				return Item::kWeapon;
			case Type::kARMO:
				return Item::kArmor;
			case Type::kMISC:
				return Item::kMisc;
			case Type::kAMMO:
				return Item::kAmmo;
			case Type::kALCH:
				return Item::kAid;
			case Type::kINGR:
				return Item::kIngredient;
			case Type::kCMPO:
				return Item::kComponent;
			default:
				return Item::kOther;
			}
		}

		constexpr std::uint32_t FEATURED_ITEM_KEYWORD = 0x001B3FAC;  // bobbleheads and magazines

		const RE::BGSObjectInstanceExtra* InstanceOf(RE::TESObjectREFR* a_ref)
		{
			return a_ref->extraList ? a_ref->extraList->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
		}

		DropBook::ItemFacts FactsOf(RE::TESObjectREFR* a_ref, std::int32_t a_count)
		{
			DropBook::ItemFacts facts;
			const auto          base = a_ref->GetObjectReference();
			facts.kind = KindOf(base);
			facts.count = a_count;
			if (base) {
				const auto keywords = base->As<RE::BGSKeywordForm>();
				facts.featured = keywords && keywords->HasKeywordID(FEATURED_ITEM_KEYWORD);
				facts.runtimeBase = base->IsCreated();
			}
			facts.questItem = a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			if (const auto instance = InstanceOf(a_ref); instance && instance->values) {
				for (const auto& data : instance->GetIndexData()) {
					if (!data.disabled && (data.objectID >> 24) == 0xFF) {
						facts.runtimeMod = true;
					}
				}
			}
			return facts;
		}

		// The item as it lies, for the others to place theirs: its mods and template combination (as Equipment reads a
		// worn item's), a power armor piece's condition and a name the player gave it.
		std::optional<Protocol::DroppedItem> ItemOf(RE::TESObjectREFR* a_ref, std::int32_t a_count, std::uint64_t a_nonce)
		{
			const auto base = a_ref->GetObjectReference();
			const auto cell = a_ref->GetParentCell();
			if (!base || !cell) {
				return std::nullopt;
			}
			Protocol::DroppedItem item;
			item.nonce = a_nonce;
			item.base = base->GetFormID();
			item.count = a_count;
			item.templateIndex = StandInGear::NO_TEMPLATE;
			if (const auto instance = InstanceOf(a_ref); instance && instance->values) {
				item.templateIndex = instance->itemIndex;
				for (const auto& data : instance->GetIndexData()) {
					if (data.disabled || data.objectID == 0) {
						continue;
					}
					if (item.mods.size() >= Protocol::MAX_ITEM_MODS) {
						return std::nullopt;  // more than the wire carries: not that item
					}
					item.mods.push_back({ data.objectID, data.index, data.rank });
				}
			}
			if (a_ref->extraList) {
				if (a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
					if (const auto health = a_ref->extraList->GetHealthPerc(); std::isfinite(health)) {
						item.health = std::clamp(health, 0.0f, 1.0f);
					}
				}
				if (const auto text = a_ref->extraList->GetByType<RE::ExtraTextDisplayData>();
					text && text->ownerInstance.get() == RE::ExtraTextDisplayData::DisplayDataType::kCustomName && !text->displayName.empty()) {
					item.name = Protocol::Utf8Prefix(text->displayName.c_str(), Protocol::MAX_ITEM_NAME_LENGTH);
				}
			}
			const auto& at = a_ref->data.location;
			item.x = at.x;
			item.y = at.y;
			item.z = at.z;
			if (cell->IsInterior()) {
				item.cell = cell->GetFormID();
			} else if (cell->worldSpace) {
				item.worldspace = cell->worldSpace->GetFormID();
			} else {
				return std::nullopt;
			}
			if (!Protocol::InWorld(item.x) || !Protocol::InWorld(item.y) || !Protocol::InWorld(item.z)) {
				return std::nullopt;
			}
			return item;
		}

		bool Known(std::uint32_t a_refId)
		{
			if (std::ranges::any_of(pending, [&](const Pending& a_pending) { return a_pending.refId == a_refId; })) {
				return true;
			}
			return std::ranges::any_of(copies, [&](const auto& a_entry) { return a_entry.second.refId == a_refId; });
		}

		// The reference our player's item became: the one the event names, or else (in the Pip-Boy or the console only,
		// where items are dropped, and when it can only have been a drop: a_mayScan, DropBook::MayScan) the newest reference
		// of that item near them that we haven't seen before. Never one another of our systems keeps.
		RE::TESObjectREFR* FindDropped(const Left& a_left, bool a_mayScan)
		{
			const auto player = Player();
			if (!player) {
				return nullptr;
			}
			if (a_left.ref != 0) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_left.ref);
				const auto base = ref ? ref->GetObjectReference() : nullptr;
				return ref && base && base->GetFormID() == a_left.base && ref->IsCreated() && Loose(ref) && !Known(a_left.ref) && !OtherSystems(ref) ? ref : nullptr;
			}
			if (!a_left.inMenu) {
				return nullptr;  // an item used up (ammo fired, food eaten)
			}
			if (!a_mayScan) {
				++notScanned;  // used up in the Pip-Boy (food, a stim), or its count changed some other way too
				return nullptr;
			}
			RE::TESObjectREFR*         found = nullptr;
			std::vector<std::uint32_t> others;
			RE::TES::GetSingleton()->ForEachReferenceInRange(player->data.location, FIND_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->GetFormID() == a_left.base && a_ref->IsCreated() && Loose(a_ref) && !a_ref->IsDisabled() && !Known(a_ref->GetFormID()) &&
					!seenLoose.contains(a_ref->GetFormID()) && !OtherSystems(a_ref)) {
					if (found) {
						others.push_back((std::min)(found->GetFormID(), a_ref->GetFormID()));
					}
					if (!found || a_ref->GetFormID() > found->GetFormID()) {
						found = a_ref;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			if (seenLoose.size() + others.size() > MAX_SEEN) {
				seenLoose.clear();
			}
			seenLoose.insert(others.begin(), others.end());
			if (found) {
				++foundByScan;
			}
			return found;
		}

		void SayRefused()
		{
			if (!std::exchange(refusalSaid, true)) {
				Hud::Notify(Text::Get("drops.not_given"));
			}
		}

		// ---- copies ----

		// Made as a lent frame's pieces are (ItemCopies).
		using ItemCopies::ExtraFor;
		using ItemCopies::Placeable;

		// Told when the VM has run a copy's BlockActivation (on one of its threads): from then on Take only asks the server.
		// A cancelled call says nothing; the copy goes after BLOCK_WAIT.
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

		RE::ObjectRefHandle PlaceCopy(const Protocol::DroppedItem& a_drop, RE::TESObjectCELL* a_cell)
		{
			const auto form = RE::TESForm::GetFormByID(a_drop.base);
			const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!object) {
				return {};
			}
			RE::NEW_REFR_DATA data;
			data.location = { a_drop.x, a_drop.y, a_drop.z };
			data.object = object;
			data.interior = a_cell->IsInterior() ? a_cell : nullptr;
			data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
			data.extra = ExtraFor(a_drop, object);
			data.clearStillLoadingFlag = true;
			data.initiallyDisabled = true;  // shown once its activation is blocked (ShowCopies)
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref) {
				return {};
			}
			SaveGuard::Track(ref.get());
			// Take only asks the server (OnActivated); the engine's own pickup runs once the server grants it (StartTake).
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> blocked{ new Blocked(ref->GetFormID()) };
			if (!Papyrus::CallMethodThen(blocked, ref.get(), "ObjectReference", "BlockActivation", true, true, false)) {
				RemotePlayers::Discard(handle, Clock::now());
				return {};
			}
			return handle;
		}

		void RemoveCopy(std::uint32_t a_dropId)
		{
			const auto it = copies.find(a_dropId);
			if (it == copies.end()) {
				return;
			}
			if (const auto ref = it->second.ref.get(); ref && Loose(ref.get()) && Equipment::MayChange(ref.get(), OwnGear::Change::kDropCopy)) {
				RemotePlayers::Discard(it->second.ref, it->second.placedAt);
				++removed;
			}
			copies.erase(it);
		}

		// Whether a_drop lies within a_range of our player.
		bool Near(const Protocol::DroppedItem& a_drop, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_range)
		{
			if (a_cell->IsInterior()) {
				if (a_drop.cell != a_cell->GetFormID()) {
					return false;
				}
			} else if (!a_cell->worldSpace || a_drop.worldspace != a_cell->worldSpace->GetFormID()) {
				return false;
			}
			const float dx = a_drop.x - a_at.x;
			const float dy = a_drop.y - a_at.y;
			return dx * dx + dy * dy <= a_range * a_range;
		}

		void PlaceCopies(Clock::time_point a_now)
		{
			const auto player = Player();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell || a_now < nextPlace) {
				return;
			}
			nextPlace = a_now + PLACE_EVERY;
			const auto at = player->data.location;
			for (auto it = copies.begin(); it != copies.end();) {
				const auto drop = lying.find(it->first);
				const auto ref = it->second.ref.get();
				if (!ref) {
					++lost;  // the game took it from under us (its cell went)
					SaveGuard::Forget(it->second.refId);
					it = copies.erase(it);
					continue;
				}
				if (drop == lying.end() || !Near(drop->second, cell, at, REMOVE_RANGE)) {
					const auto id = it->first;
					++it;
					RemoveCopy(id);
					continue;
				}
				++it;
			}
			int placedNow = 0;
			for (const auto& [id, drop] : lying) {
				if (placedNow >= PLACE_PER_FRAME) {
					break;
				}
				if (copies.contains(id) || unplaceable.contains(id) || pickedUnblocked.contains(id) || scraps.Waiting(id) || (taking && taking->dropId == id) ||
					!Near(drop, cell, at, PLACE_RANGE)) {
					continue;
				}
				if (!Placeable(drop)) {
					unplaceable.insert(id);
					if (Log()) {
						REX::WARN("Drops: drop {} ({:08X} with {} mods) can't be placed here: its item or a mod is missing from this game", id, drop.base, drop.mods.size());
					}
					continue;
				}
				++placedNow;
				const auto handle = PlaceCopy(drop, cell);
				const auto ref = handle.get();
				if (!ref) {
					++placeFailed;
					unplaceable.insert(id);  // not tried every half second
					continue;
				}
				copies[id] = { handle, ref->GetFormID(), a_now, false };
				++placed;
				if (Log()) {
					REX::INFO("Drops: placed copy {:08X} of drop {} ({:08X} x{}, {} mods, player {}'s)", ref->GetFormID(), id, drop.base, drop.count, drop.mods.size(), drop.ownerId);
				}
			}
		}

		// A copy is shown once the VM has blocked its activation (Blocked), and goes if it hasn't by BLOCK_WAIT.
		void ShowCopies(const std::vector<std::uint32_t>& a_blocked, Clock::time_point a_now)
		{
			for (const auto refId : a_blocked) {
				if (const auto it = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second.refId == refId; }); it != copies.end()) {
					it->second.blocked = true;
				}
			}
			std::vector<std::uint32_t> late;
			for (auto& [id, copy] : copies) {
				if (copy.shown) {
					continue;
				}
				if (!copy.blocked) {
					if (a_now - copy.placedAt >= BLOCK_WAIT) {
						late.push_back(id);
					}
					continue;
				}
				if (const auto ref = copy.ref.get()) {
					ref->Enable(false);
				}
				copy.shown = true;
			}
			for (const auto id : late) {
				++blockLate;
				if (Log()) {
					REX::WARN("Drops: copy of drop {} wasn't blocked in time; placed again", id);
				}
				RemoveCopy(id);
			}
		}

		// Our own drop the server may never have heard we swapped out (DropBook::Unconfirmed), as we leave the session: the
		// item goes back into our world as the ordinary item it was, lying where it lay if our player is near it (no block,
		// never removed), else into their inventory.
		bool GiveBackOwn(const Protocol::DroppedItem& a_drop)
		{
			const auto player = Player();
			const auto cell = player ? player->GetParentCell() : nullptr;
			const auto form = RE::TESForm::GetFormByID(a_drop.base);
			const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!player || !object) {
				return false;
			}
			if (cell && Near(a_drop, cell, player->data.location, REMOVE_RANGE)) {
				RE::NEW_REFR_DATA data;
				data.location = { a_drop.x, a_drop.y, a_drop.z };
				data.object = object;
				data.interior = cell->IsInterior() ? cell : nullptr;
				data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
				data.extra = ExtraFor(a_drop, object);
				data.clearStillLoadingFlag = true;
				if (EngineRefs::CreateReferenceAtLocation(data).get()) {
					return true;
				}
			}
			auto inInventory = a_drop;
			inInventory.count = 1;  // the stack's count is the container's, not its extra data's
			player->AddObjectToContainer(object, ExtraFor(inInventory, object), a_drop.count, nullptr, RE::ITEM_REMOVE_REASON::kNone);
			return true;
		}

		// We are leaving the session: each of our drops the server never said lies there is given back to our world.
		void GiveBackUnconfirmed()
		{
			for (const auto& drop : unconfirmed.TakeAll()) {
				if (!GiveBackOwn(drop)) {
					++restoreFailed;
					REX::WARN("Drops: drop {} may not have reached the server before we left, and couldn't be given back", drop.dropId);
					continue;
				}
				{
					std::scoped_lock l{ recordLock };
					record.Unmade(drop.nonce);
				}
				++restored;
				REX::INFO("Drops: drop {} ({:08X} x{}) may not have reached the server before we left; it is ours again", drop.dropId, drop.base, drop.count);
			}
		}

		// ---- our player's drops ----

		void TakeInbox(std::vector<Left>& a_left, std::vector<std::uint32_t>& a_activated, std::vector<std::uint32_t>& a_blocked, std::vector<std::uint32_t>& a_scrapped,
			std::vector<Destroyed>& a_destroyed, std::vector<Added>& a_added)
		{
			std::scoped_lock l{ inboxLock };
			a_left.swap(leftInbox);
			a_activated.swap(activatedInbox);
			a_blocked.swap(blockedInbox);
			a_scrapped.swap(scrappedInbox);
			a_destroyed.swap(destroyedInbox);
			a_added.swap(addedInbox);
			usedNow.insert(equippedInbox.begin(), equippedInbox.end());
			equippedInbox.clear();
		}

		void NoticeDrops(const std::vector<Left>& a_left, Clock::time_point a_now)
		{
			// What left our player's inventory of each base, against their count of it a frame before: a reference the
			// events don't name is only looked for when that is all that changed (DropBook::MayScan).
			std::unordered_map<std::uint32_t, std::int64_t>                 leftOf;
			std::optional<std::unordered_map<std::uint32_t, std::uint32_t>> carriedNow;
			for (const auto& left : a_left) {
				leftOf[left.base] += left.count;
			}
			for (const auto& left : a_left) {
				bool mayScan = false;
				if (left.ref == 0 && left.inMenu) {
					if (!carriedNow) {
						carriedNow = CarriedAll();
					}
					std::optional<std::uint32_t> before;
					if (carriedBefore) {
						const auto it = carriedBefore->find(left.base);
						before = it != carriedBefore->end() ? it->second : 0;
					}
					const auto now = carriedNow->find(left.base);
					mayScan = DropBook::MayScan(before, now != carriedNow->end() ? now->second : 0, leftOf[left.base], usedNow.contains(left.base));
				}
				const auto ref = FindDropped(left, mayScan);
				if (!ref) {
					continue;
				}
				++dropped;
				const auto verdict = DropBook::Judge(FactsOf(ref, left.count));
				if (verdict != DropBook::Givable::kGive) {
					if (verdict != DropBook::Givable::kNotItem) {
						++notGiven;
						SayRefused();
					}
					if (Log()) {
						REX::INFO("Drops: {:08X} ({:08X} x{}) dropped; not given (verdict {})", ref->GetFormID(), left.base, left.count, static_cast<int>(verdict));
					}
					continue;
				}
				if (pending.size() >= MAX_PENDING) {
					continue;
				}
				pending.push_back({ ref->GetHandle(), ref->GetFormID(), left.base, left.count, NewNonce(), a_now, a_now, ref->data.location, false });
				if (Log()) {
					REX::INFO("Drops: {:08X} ({:08X} x{}) dropped{}; reported once it rests", ref->GetFormID(), left.base, left.count, left.ref ? "" : " (found near the player)");
				}
			}
			usedNow.clear();
		}

		// Our player's counts while a menu items are dropped from is open: the next frame's "before" (NoticeDrops).
		void NoteCarried()
		{
			if (OpenMenus::AnyOpen({ OpenMenus::Menu::kPipboy, OpenMenus::Menu::kConsole })) {
				carriedBefore = CarriedAll();
			} else {
				carriedBefore.reset();
			}
		}

		void SettleDrops(Clock::time_point a_now)
		{
			std::erase_if(pending, [&](Pending& a_pending) {
				if (a_pending.reported) {
					return false;  // waiting for the server
				}
				const auto ref = a_pending.ref.get();
				if (!ref || !Lies(ref.get())) {
					return true;  // picked up again, or scrapped, before it was reported: nothing to tell anyone
				}
				const auto& at = ref->data.location;
				if (at.GetDistance(a_pending.last) > DropBook::SETTLE_MOVE) {
					a_pending.last = at;
					a_pending.movedAt = a_now;
				}
				if (!DropBook::Settled(a_now - a_pending.droppedAt, a_now - a_pending.movedAt)) {
					return false;
				}
				const auto item = ItemOf(ref.get(), a_pending.count, a_pending.nonce);
				if (!item) {
					++notGiven;
					return true;
				}
				// A copy of a friend's lent piece (FrameLoans): never given, as it is not ours. Left lying in our world, and
				// its owner takes theirs out at the return, as for one sold or stored.
				if (FrameLoans::IsLentItem(item->base, item->name)) {
					++notGiven;
					if (Log()) {
						REX::INFO("Drops: {:08X} ({:08X}) dropped is a copy of a friend's lent piece; not given", ref->GetFormID(), item->base);
					}
					return true;
				}
				Send(Protocol::Encode(*item, Protocol::MessageType::kReportDrop));
				a_pending.reported = true;
				++reported;
				return false;
			});
		}

		// ---- takes ----

		void Activate(RE::TESObjectREFR* a_ref, std::int32_t a_count)
		{
			// As our player pressing Take, past the block (the default processing only, as Papyrus' Activate with it on).
			a_ref->ActivateRef(Player(), nullptr, a_count, true, false, false);
		}

		void StartTake(std::uint32_t a_dropId, const Protocol::DroppedItem* a_drop)
		{
			const auto it = copies.find(a_dropId);
			const auto ref = it != copies.end() ? it->second.ref.get() : RE::NiPointer<RE::TESObjectREFR>{};
			if (!ref || !Loose(ref.get()) || !a_drop || !Equipment::MayChange(ref.get(), OwnGear::Change::kDropCopy)) {
				// Our copy went in the meantime: it lies there again for anyone.
				Claim(a_dropId, Protocol::DropClaim::kGiveBack);
				++givenBack;
				if (it != copies.end()) {
					copies.erase(it);
				}
				return;
			}
			taking = Take{ a_dropId, it->second.ref, it->second.refId, a_drop->base, a_drop->count, Carried(a_drop->base), 0, it->second.placedAt, Clock::now() };
			copies.erase(it);
			Activate(ref.get(), a_drop->count);
		}

		// Whether the take at hand is done: our copy left the world and came to our player (DropBook::JudgeTake).
		bool TakeDone(const Take& a_take)
		{
			const auto ref = a_take.ref.get();
			return DropBook::JudgeTake(ref && Loose(ref.get()), a_take.before, Carried(a_take.base), a_take.count) == DropBook::TakeState::kTaken;
		}

		void RecordTake(const Take& a_take)
		{
			SaveGuard::Release(a_take.refId);  // in our player's inventory: its id may name the next item they drop
			{
				std::scoped_lock l{ recordLock };
				record.Took(a_take.dropId);
			}
			++taken;
			if (Log()) {
				REX::INFO("Drops: took drop {} ({:08X} x{})", a_take.dropId, a_take.base, a_take.count);
			}
		}

		// The take at hand didn't finish: our copy goes, wherever it is, and the item lies there again for anyone (the
		// server tells every game, ours too, and a fresh copy is placed).
		void FailTake(const Take& a_take)
		{
			if (const auto ref = a_take.ref.get(); ref && Loose(ref.get())) {
				RemotePlayers::Discard(a_take.ref, a_take.placedAt);
			}
			Claim(a_take.dropId, Protocol::DropClaim::kGiveBack);
			++takeFailed;
		}

		void FinishTake(Clock::time_point a_now)
		{
			if (!taking) {
				return;
			}
			auto& take = *taking;
			if (TakeDone(take)) {
				RecordTake(take);
				taking.reset();
				return;
			}
			if (a_now - take.triedAt < TAKE_EVERY) {
				return;  // the engine's pickup may take a frame or two
			}
			take.triedAt = a_now;
			const auto ref = take.ref.get();
			if (++take.tries < TAKE_TRIES) {
				if (!ref || !Loose(ref.get())) {
					return;  // gone from the world, but not (yet) to us: looked at again
				}
				if (take.tries == TAKE_TRIES - 1) {
					Player()->PickUpObject(ref.get(), take.count, true);  // the engine's pickup itself, once
				} else {
					Activate(ref.get(), take.count);
				}
				return;
			}
			REX::WARN("Drops: the take of drop {} ({:08X} x{}) didn't finish (copy {}, carried {} of {} before); it lies there again", take.dropId, take.base, take.count,
				ref && Loose(ref.get()) ? "still lying" : "gone", Carried(take.base), take.before);
			FailTake(take);
			taking.reset();
		}

		// The stacks of a_from's items of a_object, as DropBook::PickStack reads them (stack numbers as RemoveItem takes them).
		std::vector<DropBook::StackInfo> StacksOf(RE::TESObjectREFR* a_from, const RE::TESBoundObject* a_object)
		{
			std::vector<DropBook::StackInfo> out;
			const auto                       list = a_from ? a_from->inventoryList : nullptr;
			if (!list || !a_object) {
				return out;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (item.object != a_object) {
					continue;
				}
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					DropBook::StackInfo info{ .count = stack->count };
					const auto          extra = stack->extra.get();
					const auto          instance = extra ? extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
					if (instance && instance->values) {
						info.instanced = true;
						for (const auto& data : instance->GetIndexData()) {
							if (!data.disabled && data.objectID != 0) {
								info.mods.push_back(data.objectID);
							}
						}
					}
					std::ranges::sort(info.mods);
					const auto text = extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
					if (text && !text->displayName.empty()) {
						info.name = text->displayName.c_str();
					}
					out.push_back(std::move(info));
				}
				break;  // one entry per base
			}
			return out;
		}

		// a_count of the drop taken out of a_from: from the stack that is that item (its mods and name), never another of
		// its base (DropBook::PickStack). False, and nothing taken, when none is. a_before runs just before the removal.
		template <class Before>
		bool RemoveDrop(RE::TESObjectREFR* a_from, const Protocol::DroppedItem& a_drop, std::int32_t a_count, Before&& a_before)
		{
			const auto form = RE::TESForm::GetFormByID(a_drop.base);
			const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!a_from || !object) {
				return false;
			}
			const auto pick = DropBook::PickStack(StacksOf(a_from, object), a_drop, a_count);
			if (pick.kind == DropBook::StackPick::Kind::kNone) {
				return false;
			}
			RE::TESObjectREFR::RemoveItemData data{ object, a_count };
			if (pick.kind == DropBook::StackPick::Kind::kStack) {
				data.stackData.push_back(pick.index);
			}
			a_before();
			a_from->RemoveItem(data);
			return true;
		}

		// Our player has an item someone else was granted (picked up past a block that hadn't taken): it goes out of their
		// inventory again, from the stack that is that item (its mods and name) where one is, so it is in one inventory only.
		bool TakeBack(const Protocol::DroppedItem& a_drop)
		{
			return RemoveDrop(Player(), a_drop, a_drop.count, [&] {
				usedNow.insert(a_drop.base);  // its leaving their inventory is no drop
			});
		}

		void NoticeActivations(const std::vector<std::uint32_t>& a_activated, Clock::time_point a_now)
		{
			for (const auto refId : a_activated) {
				const auto it = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second.refId == refId; });
				if (it == copies.end()) {
					continue;
				}
				const auto dropId = it->first;
				if (const auto ref = it->second.ref.get(); !ref || !Loose(ref.get())) {
					// Picked up all the same: its block hadn't taken. Claimed like any take; counted if someone else gets it.
					pickedUnblocked.insert(dropId);
					SaveGuard::Release(refId);
					copies.erase(it);
					++unblocked;
					REX::WARN("Drops: our player picked up copy {:08X} of drop {} although its activation was blocked", refId, dropId);
				}
				if (claims.Start(dropId, a_now)) {
					Claim(dropId, Protocol::DropClaim::kTake);
					++claimed;
				}
			}
		}

		// ---- scraps (see DropBook::Scraps) ----

		// Our copy of a_dropId was scrapped in workshop mode: our world consumed the item, so it is our take. Recorded as taken
		// at once (a link lost before the answer is settled by our next record), never given back to our world as we leave
		// (Unconfirmed: it is in it, as components), and not placed again while the claim waits.
		void ScrappedCopy(std::uint32_t a_dropId, Clock::time_point a_now)
		{
			const auto it = copies.find(a_dropId);
			if (it == copies.end()) {
				return;
			}
			const auto refId = it->second.refId;
			SaveGuard::Release(refId);  // the game deletes it: its id may name the next item our player drops
			copies.erase(it);
			if (!lying.contains(a_dropId) || !scraps.Scrapped(a_dropId)) {
				return;
			}
			unconfirmed.Heard(a_dropId);
			{
				std::scoped_lock l{ recordLock };
				record.Took(a_dropId);
			}
			if (claims.Start(a_dropId, a_now)) {
				Claim(a_dropId, Protocol::DropClaim::kTake);
				++claimed;
			}
			REX::INFO("Drops: our copy {:08X} of drop {} was scrapped in workshop mode; claimed as our take", refId, a_dropId);
		}

		// What a lost scrap put in its workbench taken back out: the item is a friend's, our player keeps nothing of it.
		void TakeBackYield(const DropBook::ScrapYields::Yield& a_yield)
		{
			const auto lostIt = lostItems.find(a_yield.dropId);
			const auto lost = lostIt != lostItems.end() ? std::optional{ lostIt->second } : std::nullopt;
			if (lostIt != lostItems.end()) {
				lostItems.erase(lostIt);
			}
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_yield.bench);
			if (!bench || a_yield.items.empty()) {
				++scrapShort;
				REX::WARN("Drops: drop {} went to someone else as our player scrapped our copy of it, and nothing it put in workbench {:08X} was seen; "
						  "our player keeps it", a_yield.dropId, a_yield.bench);
				return;
			}
			// What has no instance of its own comes out by base; an item stored whole as its own stack, never another of
			// its base, which may be our player's own modded or legendary one (DropBook::ScrapYields::PlanTakeBack).
			const auto plan = DropBook::ScrapYields::PlanTakeBack(a_yield, [](std::uint32_t a_base) {
				const auto form = RE::TESForm::GetFormByID(a_base);
				const auto type = form ? form->GetFormType() : RE::ENUM_FORM_ID::kNONE;
				return type == RE::ENUM_FORM_ID::kMISC || type == RE::ENUM_FORM_ID::kAMMO || type == RE::ENUM_FORM_ID::kALCH || type == RE::ENUM_FORM_ID::kINGR;
			});
			for (const auto& item : plan.out) {
				const auto object = RE::TESForm::GetFormByID<RE::TESBoundObject>(item.base);
				if (!object) {
					continue;
				}
				RE::TESObjectREFR::RemoveItemData data{ object, item.count };
				bench->RemoveItem(data);
				REX::INFO("Drops: {:08X} x{} taken back out of workbench {:08X}: the scrap of drop {} was someone else's", item.base, item.count, a_yield.bench,
					a_yield.dropId);
			}
			bool tookWhole = false;
			for (const auto& item : plan.whole) {
				if (lost && lost->base == item.base && RemoveDrop(bench, *lost, item.count, [] {})) {
					tookWhole = true;
					REX::INFO("Drops: {:08X} x{} stored whole taken back out of workbench {:08X}, as that item: drop {} went to someone else", item.base, item.count,
						a_yield.bench, a_yield.dropId);
					continue;
				}
				++scrapRefused;
				REX::WARN("Drops: {:08X} x{} left in workbench {:08X}, though drop {} went to someone else: no stack there is that item (its mods and name), and "
						  "our player's own of the same base is never taken",
					item.base, item.count, a_yield.bench, a_yield.dropId);
			}
			if (!plan.out.empty() || tookWhole) {
				++scrapTakenBack;
			}
		}

		// Our copies the workshop took away this frame (its destroyed event named them), and what our scraps put in their
		// workbenches.
		void NoticeScraps(const std::vector<std::uint32_t>& a_named, const std::vector<Destroyed>& a_destroyed, const std::vector<Added>& a_added, Clock::time_point a_now)
		{
			if (OpenMenus::Open(OpenMenus::Menu::kWorkshop)) {
				lastWorkshop = a_now;
			}
			workshopUp = lastWorkshop != Clock::time_point{} && a_now - lastWorkshop <= SCRAP_GRACE;
			for (const auto& added : a_added) {
				yields.Added(added.container, added.base, added.count, added.at);
			}
			for (const auto& destroyed : a_destroyed) {
				const auto copy = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second.refId == destroyed.ref; });
				yields.Destroyed(copy != copies.end() ? copy->first : 0, destroyed.bench, destroyed.at);
			}
			if (!a_named.empty()) {
				std::vector<std::uint32_t> gone;
				for (const auto& [id, copy] : copies) {
					const bool named = std::ranges::find(a_named, copy.refId) != a_named.end();
					if (DropBook::JudgeCopy(static_cast<bool>(copy.ref.get()), named) == DropBook::CopyGone::kScrapped) {
						gone.push_back(id);
					}
				}
				for (const auto id : gone) {
					ScrappedCopy(id, a_now);
				}
			}
			for (auto& yield : yields.Due(a_now)) {
				if (lostScraps.erase(yield.dropId)) {
					TakeBackYield(yield);
				} else if (scraps.Waiting(yield.dropId) && yieldsKept.size() < DropBook::ScrapYields::MAX_SCRAPS) {
					yieldsKept[yield.dropId] = std::move(yield);
				}
			}
			if (yields.Waiting() == 0) {
				for (const auto id : lostScraps) {
					TakeBackYield({ id, 0, {} });  // its scrap's event was never noted: nothing to take back
				}
				lostScraps.clear();
			}
			// A claim whose answer went with a reconnect is made again (our next record settles it too).
			for (const auto id : scraps.Unanswered()) {
				if (lying.contains(id) && claims.Start(id, a_now)) {
					Claim(id, Protocol::DropClaim::kTake);
				}
			}
		}

		// The workshop's sinks read our copies' ids from another thread.
		void PublishCopyIds()
		{
			std::unordered_set<std::uint32_t> ids;
			for (const auto& [id, copy] : copies) {
				ids.insert(copy.refId);
			}
			std::scoped_lock l{ inboxLock };
			copyRefIds = std::move(ids);
		}

		Protocol::DropRecord RecordForServer()
		{
			std::scoped_lock l{ recordLock };
			return record.ForServer();
		}
	}

	void OnLeftInventory(std::uint32_t a_base, std::int32_t a_count, std::uint32_t a_ref)
	{
		if (!listening || a_count <= 0) {
			return;
		}
		const bool inMenu = OpenMenus::AnyOpen({ OpenMenus::Menu::kPipboy, OpenMenus::Menu::kConsole }) || devDrop.exchange(false);
		if (a_ref == 0 && !inMenu) {
			return;
		}
		std::scoped_lock l{ inboxLock };
		if (leftInbox.size() < MAX_INBOX) {
			leftInbox.push_back({ a_base, a_count, a_ref, inMenu });
		}
	}

	void OnActivated(std::uint32_t a_ref)
	{
		if (!listening) {
			return;
		}
		std::scoped_lock l{ inboxLock };
		if (activatedInbox.size() < MAX_INBOX) {
			activatedInbox.push_back(a_ref);
		}
	}

	void OnEquipped(std::uint32_t a_base)
	{
		if (!listening) {
			return;
		}
		std::scoped_lock l{ inboxLock };
		if (equippedInbox.size() < MAX_INBOX) {
			equippedInbox.push_back(a_base);
		}
	}

	bool IsCopyRef(std::uint32_t a_ref)
	{
		std::scoped_lock l{ inboxLock };
		return copyRefIds.contains(a_ref);
	}

	bool OnScrapped(std::uint32_t a_ref, std::uint32_t a_bench)
	{
		const auto now = Clock::now();
		std::scoped_lock l{ inboxLock };
		if (a_ref != 0 && a_bench != 0 && destroyedInbox.size() < MAX_INBOX) {
			destroyedInbox.push_back({ a_ref, a_bench, now });  // ours or not: what it put in the workbench is its own
		}
		if (!copyRefIds.contains(a_ref)) {
			return false;
		}
		if (scrappedInbox.size() < MAX_INBOX) {
			scrappedInbox.push_back(a_ref);
		}
		return true;
	}

	void OnAddedFromNowhere(std::uint32_t a_container, std::uint32_t a_base, std::int32_t a_count)
	{
		if (!workshopUp || a_container == 0 || a_container == Protocol::PLAYER_REF_ID || a_count <= 0) {
			return;
		}
		const auto now = Clock::now();
		std::scoped_lock l{ inboxLock };
		if (addedInbox.size() < DropBook::ScrapYields::MAX_ADDED) {
			addedInbox.push_back({ a_container, a_base, a_count, now });
		}
	}

	void DevDropNext()
	{
		devDrop = true;
	}

	void DevLoseNextPlaced()
	{
		devLosePlaced = true;
	}

	void OnKeptInContainer()
	{
		if (listening) {
			keptInContainer = true;
		}
	}

	void OnPlaced(const Protocol::DroppedItem& a_drop)
	{
		++received;
		const auto mine = a_drop.ownerId == localId && localId != 0;
		const auto it = mine ? std::ranges::find(pending, a_drop.nonce, &Pending::nonce) : pending.end();
		if (a_drop.dropId == 0) {
			// Not kept by the server (the session has too many lying about): the item stays on our ground, ours alone.
			if (it != pending.end()) {
				pending.erase(it);
				++refused;
				SayRefused();
			}
			return;
		}
		if (it == pending.end()) {
			lying[a_drop.dropId] = a_drop;
			unplaceable.erase(a_drop.dropId);
			unconfirmed.Heard(a_drop.dropId);  // one of ours: the server has our kPlaced
			return;
		}
		// Our drop, offered to us alone: the item we dropped makes way for a copy like everyone's, and only once it has
		// does the server tell the others (kPlaced), so nobody can take it while it is still in our world.
		const auto ref = it->ref.get();
		if (ref && Lies(ref.get()) && Equipment::MayChange(ref.get(), OwnGear::Change::kDropCopy)) {
			RemotePlayers::Discard(it->ref, it->droppedAt);
			{
				std::scoped_lock l{ recordLock };
				record.Made(a_drop.nonce);
			}
			if (!std::exchange(devLosePlaced, false)) {
				Claim(a_drop.dropId, Protocol::DropClaim::kPlaced);
			}
			unconfirmed.Swapped(a_drop);
			lying[a_drop.dropId] = a_drop;
			unplaceable.erase(a_drop.dropId);
			++swapped;
			if (Log()) {
				REX::INFO("Drops: drop {} is the session's: {:08X} makes way for a copy", a_drop.dropId, it->refId);
			}
		} else {
			// Our player picked it up again before the offer came (or scrapped it, or it is gone): it never lay there for anyone.
			Claim(a_drop.dropId, Protocol::DropClaim::kKept);
			++reclaimed;
			if (Log()) {
				REX::INFO("Drops: drop {} kept: {:08X} was picked up again before the server's answer", a_drop.dropId, it->refId);
			}
		}
		pending.erase(it);
	}

	void OnGone(const Protocol::DropGone& a_gone)
	{
		unconfirmed.Heard(a_gone.dropId);
		const auto drop = lying.find(a_gone.dropId);
		std::optional<Protocol::DroppedItem> item;
		if (drop != lying.end()) {
			item = drop->second;
			lying.erase(drop);
		}
		const bool ours = claims.Answered(a_gone.dropId);
		switch (scraps.Answered(a_gone.dropId, a_gone.takerId, localId)) {
		case DropBook::Scraps::Outcome::kOurs:
			++scrapped;
			yieldsKept.erase(a_gone.dropId);
			if (Log()) {
				REX::INFO("Drops: drop {} scrapped here is ours; gone for everyone", a_gone.dropId);
			}
			return;
		case DropBook::Scraps::Outcome::kLost:
			// Our copy went in a scrap, but someone else's claim came first: the item is theirs, and not in our world. What
			// the scrap put in the workbench comes back out of it, now or once it is known (NoticeScraps).
			{
				std::scoped_lock l{ recordLock };
				record.Untook(a_gone.dropId);
			}
			++scrapLost;
			REX::WARN("Drops: drop {} went to player {} as our player scrapped our copy of it; the item is theirs, and the components go back", a_gone.dropId,
				a_gone.takerId);
			if (item && lostItems.size() < DropBook::ScrapYields::MAX_SCRAPS) {
				lostItems[a_gone.dropId] = *item;
			}
			if (const auto kept = yieldsKept.find(a_gone.dropId); kept != yieldsKept.end()) {
				TakeBackYield(kept->second);
				yieldsKept.erase(kept);
			} else {
				lostScraps.insert(a_gone.dropId);
			}
			return;
		case DropBook::Scraps::Outcome::kNotScrapped:
			break;
		}
		if (a_gone.takerId == localId && localId != 0) {
			if (pickedUnblocked.erase(a_gone.dropId)) {
				std::scoped_lock l{ recordLock };
				record.Took(a_gone.dropId);
				++taken;
				return;
			}
			if (taking && taking->dropId == a_gone.dropId) {
				return;  // already being taken
			}
			if (!ours) {
				bool held = false;
				{
					std::scoped_lock l{ recordLock };
					held = record.HasTaken(a_gone.dropId);
				}
				RemoveCopy(a_gone.dropId);
				if (!held) {
					// Granted to a claim we no longer make (from before a load): it lies there again for anyone.
					Claim(a_gone.dropId, Protocol::DropClaim::kGiveBack);
					++givenBack;
				}
				return;  // else our record says our world has it (the server made the session agree with it)
			}
			StartTake(a_gone.dropId, item ? &*item : nullptr);
			return;
		}
		// Someone else's now, or back with its owner: our copy goes, and the item too if our player picked it up all the same.
		if (pickedUnblocked.erase(a_gone.dropId)) {
			if (item && TakeBack(*item)) {
				++takenBack;
				REX::WARN("Drops: drop {} went to player {} while our player had picked it up; taken out of their inventory again", a_gone.dropId, a_gone.takerId);
			} else {
				++dupRisk;
				REX::WARN("Drops: drop {} went to player {} while our player had picked it up, and it isn't there to take back; both have it", a_gone.dropId,
					a_gone.takerId);
			}
		}
		RemoveCopy(a_gone.dropId);
		if (ours && a_gone.takerId != 0) {
			++takenFirst;
			Hud::Notify(Text::Get("drops.taken_first"));
		}
	}

	void OnList(const Protocol::DropList& a_list)
	{
		listing.insert(listing.end(), a_list.drops.begin(), a_list.drops.end());
		if (!a_list.last) {
			return;
		}
		std::map<std::uint32_t, Protocol::DroppedItem> now;
		for (auto& drop : listing) {
			unconfirmed.Heard(drop.dropId);
			now[drop.dropId] = std::move(drop);
		}
		listing.clear();
		for (const auto& [id, drop] : lying) {
			if (!now.contains(id)) {
				RemoveCopy(id);
			}
		}
		lying = std::move(now);
	}

	void Frame()
	{
		if (!listening) {
			return;
		}
		const auto                 now = Clock::now();
		std::vector<Left>          left;
		std::vector<std::uint32_t> activated;
		std::vector<std::uint32_t> blocked;
		std::vector<std::uint32_t> scrappedNow;
		std::vector<Destroyed>     destroyed;
		std::vector<Added>         added;
		TakeInbox(left, activated, blocked, scrappedNow, destroyed, added);
		NoticeDrops(left, now);
		SettleDrops(now);
		NoticeActivations(activated, now);
		FinishTake(now);
		NoticeScraps(scrappedNow, destroyed, added, now);
		PlaceCopies(now);
		ShowCopies(blocked, now);
		PublishCopyIds();
		NoteCarried();
		if (keptInContainer.exchange(false) && !std::exchange(hinted, true)) {
			Hud::Notify(Text::Get("drops.container_hint"));
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnWelcome(std::uint64_t a_session, std::uint32_t a_playerId)
	{
		localId = a_playerId;
		{
			std::scoped_lock l{ recordLock };
			record.Bind(a_session);
		}
		hinted = false;
		refusalSaid = false;
		listening = true;
		Send(Protocol::Encode(RecordForServer()));
		REX::INFO("Drops: welcomed; our record of this session sent");
	}

	void SendRecord()
	{
		if (listening) {
			Send(Protocol::Encode(RecordForServer()));
		}
	}

	void DiscardCopies()
	{
		for (auto& [id, copy] : copies) {
			if (const auto ref = copy.ref.get(); ref && Loose(ref.get())) {
				RemotePlayers::Discard(copy.ref, copy.placedAt);
			}
		}
		copies.clear();
		PublishCopyIds();
		if (taking) {
			// A take not checked yet: settled now, so the save's record says what its inventory holds. In it, the take is
			// recorded; not, the item lies there again for anyone (our copy goes with the others).
			if (TakeDone(*taking)) {
				RecordTake(*taking);
			} else {
				REX::WARN("Drops: the take of drop {} ({:08X}) hadn't finished by the save; it lies there again", taking->dropId, taking->base);
				FailTake(*taking);
			}
			taking.reset();
		}
	}

	void OnGameLoaded()
	{
		// Our references went with the world before the load. Our drops not answered yet are in the loaded save or not:
		// either way our record (the save's) says what our world holds now.
		copies.clear();
		pending.clear();
		taking.reset();
		pickedUnblocked.clear();
		claims.Clear();
		scraps.Clear();  // the loaded world's record says whether it took them
		lastWorkshop = {};
		workshopUp = false;
		yields.Clear();
		yieldsKept.clear();
		lostScraps.clear();
		lostItems.clear();
		unconfirmed.Clear();  // the loaded world's record says whether they left it
		seenLoose.clear();
		unplaceable.clear();
		carriedBefore.reset();
		usedNow.clear();
		std::scoped_lock l{ inboxLock };
		leftInbox.clear();
		activatedInbox.clear();
		equippedInbox.clear();
		blockedInbox.clear();
		scrappedInbox.clear();
		destroyedInbox.clear();
		addedInbox.clear();
		copyRefIds.clear();
	}

	std::vector<std::uint8_t> SaveRecord()
	{
		std::scoped_lock l{ recordLock };
		return record.Empty() ? std::vector<std::uint8_t>{} : record.Save();
	}

	void LoadRecord(std::span<const std::uint8_t> a_bytes)
	{
		auto loaded = DropBook::Record::Load(a_bytes);
		if (!loaded) {
			REX::WARN("Drops: ignoring an unreadable record of items given");
		}
		std::scoped_lock l{ recordLock };
		record.Replace(loaded ? std::move(*loaded) : DropBook::Record{});
	}

	void RevertRecord()
	{
		std::scoped_lock l{ recordLock };
		record.Replace(DropBook::Record{});
	}

	void Reset()
	{
		listening = false;
		GiveBackUnconfirmed();  // before our copies go: the session may never have heard those left our world
		DiscardCopies();
		OnGameLoaded();
		lying.clear();
		listing.clear();
		outgoing.clear();
		localId = 0;
		std::scoped_lock l{ recordLock };
		record.Bind(0);
	}

	std::string List()
	{
		std::string out = std::format("lying={} copies={} pending={}", lying.size(), copies.size(), pending.size());
		for (const auto& [id, drop] : lying) {
			const auto copy = copies.find(id);
			out += std::format("; drop {} base={:08X} x{} mods={} tpl={} owner={} at {:08X}/{:08X} ({:.0f},{:.0f},{:.0f}) copy={}", id, drop.base, drop.count, drop.mods.size(),
				drop.templateIndex, drop.ownerId, drop.cell, drop.worldspace, drop.x, drop.y, drop.z, copy != copies.end() ? std::format("{:08X}", copy->second.refId) : "none");
		}
		for (const auto& entry : pending) {
			out += std::format("; pending {:08X} base={:08X} x{} {}", entry.refId, entry.base, entry.count, entry.reported ? "reported" : "settling");
		}
		const auto ours = RecordForServer();
		out += std::format("; record: session={:016X} made={} taken={}", ours.sessionId, ours.made.size(), ours.taken.size());
		return out;
	}

	std::string ItemMods(std::uint32_t a_base)
	{
		const auto player = Player();
		const auto list = player ? player->inventoryList : nullptr;
		if (!list) {
			return "error: no inventory";
		}
		std::string out;
		int         stacks = 0;
		RE::BSAutoReadLock l{ list->rwLock };
		for (auto& item : list->data) {
			if (!item.object || item.object->GetFormID() != a_base) {
				continue;
			}
			for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
				++stacks;
				out += std::format("; stack x{}", stack->count);
				const auto extra = stack->extra.get();
				const auto instance = extra ? extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
				if (instance && instance->values) {
					out += std::format(" tpl={} mods=", instance->itemIndex);
					for (const auto& data : instance->GetIndexData()) {
						if (!data.disabled && data.objectID != 0) {
							out += std::format("{:08X},", data.objectID);
						}
					}
				}
				if (extra && extra->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
					out += std::format(" health={:.2f}", extra->GetHealthPerc());
				}
				if (const auto text = extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr; text && !text->displayName.empty()) {
					out += std::format(" name='{}'", text->displayName.c_str());
				}
			}
		}
		return std::format("{:08X}: stacks={}", a_base, stacks) + out;
	}

	std::string Describe()
	{
		return std::format("drops: lying={} copies={} dropped={} notGiven={} reported={} refused={} swapped={} received={} placed={} placeFailed={} removed={} claimed={} taken={} "
		                   "takeFailed={} givenBack={} takenFirst={} reclaimed={} unblocked={} takenBack={} dupRisk={} lost={} byScan={} notScanned={} blockLate={} unconfirmed={} restored={} restoreFailed={} "
		                   "scrapped={} scrapLost={} scrapWaiting={} scrapTakenBack={} scrapShort={} scrapRefused={}",
			lying.size(), copies.size(), dropped, notGiven, reported, refused, swapped, received, placed, placeFailed, removed, claimed, taken, takeFailed, givenBack, takenFirst,
			reclaimed, unblocked, takenBack, dupRisk, lost, foundByScan, notScanned, blockLate, unconfirmed.Size(), restored, restoreFailed, scrapped, scrapLost, scraps.Size(), scrapTakenBack, scrapShort, scrapRefused);
	}
}
