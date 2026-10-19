#include "game/Drops.h"

#include "game/DropBook.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/StandInGear.h"
#include "Text.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
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
		// A copy is placed hidden and shown once the script call blocking its activation has had time to run (it is
		// queued to the script VM): never a moment in which Take would pick it up without asking.
		constexpr auto  SHOW_AFTER = 300ms;
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
			bool                shown = false;
		};
		std::unordered_map<std::uint32_t, Copy> copies;
		std::unordered_set<std::uint32_t>       unplaceable;  // an item with a base or a mod this game lacks

		DropBook::Claims claims;
		// Drops of ours whose item our player picked up again before the server's answer came: claimed back, by drop ID
		// (the nonce goes into the record with the take).
		std::unordered_map<std::uint32_t, std::uint64_t> reclaiming;
		// Copies our player picked up although their activation should have been blocked: claimed as any take.
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

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextPlace{};
		bool                                   hinted = false;     // the container hint, once a session
		bool                                   refusalSaid = false;  // the refusal, once a session

		std::uint32_t dropped = 0, notGiven = 0, reported = 0, refused = 0, swapped = 0, received = 0, placed = 0, placeFailed = 0, removed = 0;
		std::uint32_t claimed = 0, taken = 0, takeFailed = 0, givenBack = 0, lost = 0, reclaimed = 0, unblocked = 0, dupRisk = 0, takenFirst = 0, foundByScan = 0;
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
					item.name = std::string{ text->displayName.c_str() }.substr(0, Protocol::MAX_ITEM_NAME_LENGTH);
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
		// where items are dropped) the newest reference of that item near them that we haven't seen before.
		RE::TESObjectREFR* FindDropped(const Left& a_left)
		{
			const auto player = Player();
			if (!player) {
				return nullptr;
			}
			if (a_left.ref != 0) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_left.ref);
				const auto base = ref ? ref->GetObjectReference() : nullptr;
				return ref && base && base->GetFormID() == a_left.base && ref->IsCreated() && Loose(ref) && !Known(a_left.ref) ? ref : nullptr;
			}
			if (!a_left.inMenu) {
				return nullptr;  // an item used up (ammo fired, food eaten)
			}
			RE::TESObjectREFR*         found = nullptr;
			std::vector<std::uint32_t> others;
			RE::TES::GetSingleton()->ForEachReferenceInRange(player->data.location, FIND_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->GetFormID() == a_left.base && a_ref->IsCreated() && Loose(a_ref) && !a_ref->IsDisabled() && !Known(a_ref->GetFormID()) &&
					!seenLoose.contains(a_ref->GetFormID())) {
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

		bool ModHere(std::uint32_t a_mod)
		{
			return RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_mod) != nullptr;
		}

		// Whether our game can place that very item: its base and every mod on it are here.
		bool Placeable(const Protocol::DroppedItem& a_drop)
		{
			const auto form = RE::TESForm::GetFormByID(a_drop.base);
			return form && form->As<RE::TESBoundObject>() && std::ranges::all_of(a_drop.mods, [](const Protocol::ItemMod& a_mod) { return ModHere(a_mod.mod); });
		}

		// The extra data our copy is made with: the item's instance (its mods, as Equipment::Give builds a stand-in's), its
		// count, condition and name.
		RE::BSTSmartPointer<RE::ExtraDataList> ExtraFor(const Protocol::DroppedItem& a_drop, RE::TESBoundObject* a_object)
		{
			RE::BSTSmartPointer<RE::ExtraDataList> extra{ new RE::ExtraDataList() };
			const auto type = a_object->GetFormType();
			const bool gear = type == RE::ENUM_FORM_ID::kWEAP || type == RE::ENUM_FORM_ID::kARMO;
			if (gear && a_drop.mods.empty() && !a_object->objectTemplate.items.empty()) {
				// A plain one (its dropper's had no mods): its base's default combination, as Equipment::Give does, so the
				// engine doesn't roll one for our level.
				RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*a_object, *extra, nullptr, true);
			} else if (gear && a_drop.templateIndex != StandInGear::NO_TEMPLATE) {
				std::unique_ptr<RE::BGSObjectInstanceExtra> instance{ new RE::BGSObjectInstanceExtra() };
				instance->itemIndex = a_drop.templateIndex;
				for (const auto& entry : a_drop.mods) {
					if (const auto mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.mod)) {
						instance->AddMod(*mod, entry.index, entry.rank, false);
					}
				}
				if (instance->values) {
					RE::BSTSmartPointer<RE::TBO_InstanceData> data;
					instance->CreateBaseInstanceData(*a_object, data);
					extra->AddExtra(instance.release());  // the list owns it from here
					if (data) {
						extra->AddExtra(new RE::ExtraInstanceData(a_object, data));
					}
				}
			}
			if (a_drop.count > 1) {
				extra->SetCount(static_cast<std::int16_t>(a_drop.count));
			}
			if (a_drop.health >= 0.0f) {
				extra->SetHealthPerc(a_drop.health);
			}
			if (!a_drop.name.empty()) {
				extra->SetOverrideName(a_drop.name.c_str());
			}
			return extra;
		}

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
			data.initiallyDisabled = true;  // shown once its activation is blocked (SHOW_AFTER)
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref) {
				return {};
			}
			SaveGuard::Track(ref.get());
			// Take only asks the server (OnActivated); the engine's own pickup runs once the server grants it (StartTake).
			if (!Papyrus::CallMethod(ref.get(), "ObjectReference", "BlockActivation", true, true, false)) {
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
				if (copies.contains(id) || unplaceable.contains(id) || reclaiming.contains(id) || (taking && taking->dropId == id) || !Near(drop, cell, at, PLACE_RANGE)) {
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

		void ShowCopies(Clock::time_point a_now)
		{
			for (auto& [id, copy] : copies) {
				if (copy.shown || a_now - copy.placedAt < SHOW_AFTER) {
					continue;
				}
				if (const auto ref = copy.ref.get()) {
					ref->Enable(false);
				}
				copy.shown = true;
			}
		}

		// ---- our player's drops ----

		void TakeInbox(std::vector<Left>& a_left, std::vector<std::uint32_t>& a_activated)
		{
			std::scoped_lock l{ inboxLock };
			a_left.swap(leftInbox);
			a_activated.swap(activatedInbox);
		}

		void NoticeDrops(const std::vector<Left>& a_left, Clock::time_point a_now)
		{
			for (const auto& left : a_left) {
				const auto ref = FindDropped(left);
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
		}

		void SettleDrops(Clock::time_point a_now)
		{
			std::erase_if(pending, [&](Pending& a_pending) {
				if (a_pending.reported) {
					return false;  // waiting for the server
				}
				const auto ref = a_pending.ref.get();
				if (!ref || !Loose(ref.get())) {
					return true;  // picked up again (or gone) before it was reported: nothing to tell anyone
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

		void FinishTake(Clock::time_point a_now)
		{
			if (!taking) {
				return;
			}
			auto&      take = *taking;
			const auto ref = take.ref.get();
			// In our inventory: its count grew, or our copy left the world (the engine moved it in; the count of a stack can
			// fall meanwhile, ammo fired).
			if (Carried(take.base) > take.before || !ref || !Loose(ref.get())) {
				{
					std::scoped_lock l{ recordLock };
					record.Took(take.dropId);
				}
				++taken;
				if (Log()) {
					REX::INFO("Drops: took drop {} ({:08X} x{})", take.dropId, take.base, take.count);
				}
				taking.reset();
				return;
			}
			if (a_now - take.triedAt < TAKE_EVERY) {
				return;  // the engine's pickup may take a frame or two
			}
			take.triedAt = a_now;
			if (++take.tries < TAKE_TRIES) {
				if (take.tries == TAKE_TRIES - 1) {
					Player()->PickUpObject(ref.get(), take.count, true);  // the engine's pickup itself, once
				} else {
					Activate(ref.get(), take.count);
				}
				return;
			}
			// Still lying there: it is there again for anyone (our copy stands again).
			++takeFailed;
			REX::WARN("Drops: the take of drop {} ({:08X}) didn't finish; it lies there again", take.dropId, take.base);
			Claim(take.dropId, Protocol::DropClaim::kGiveBack);
			copies[take.dropId] = { take.ref, take.refId, take.placedAt, true };
			taking.reset();
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

	void DevDropNext()
	{
		devDrop = true;
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
		lying[a_drop.dropId] = a_drop;
		unplaceable.erase(a_drop.dropId);
		if (it == pending.end()) {
			return;
		}
		// Ours: the item we dropped makes way for a copy like everyone's.
		const auto ref = it->ref.get();
		if (ref && Loose(ref.get()) && Equipment::MayChange(ref.get(), OwnGear::Change::kDropCopy)) {
			RemotePlayers::Discard(it->ref, it->droppedAt);
			{
				std::scoped_lock l{ recordLock };
				record.Made(a_drop.nonce);
			}
			++swapped;
			if (Log()) {
				REX::INFO("Drops: drop {} is the session's: {:08X} makes way for a copy", a_drop.dropId, it->refId);
			}
		} else {
			// Our player picked it up again before the answer came: it is in our inventory, so it is claimed back.
			reclaiming[a_drop.dropId] = a_drop.nonce;
			if (claims.Start(a_drop.dropId, Clock::now())) {
				Claim(a_drop.dropId, Protocol::DropClaim::kTake);
			}
		}
		pending.erase(it);
	}

	void OnGone(const Protocol::DropGone& a_gone)
	{
		const auto drop = lying.find(a_gone.dropId);
		std::optional<Protocol::DroppedItem> item;
		if (drop != lying.end()) {
			item = drop->second;
			lying.erase(drop);
		}
		const bool ours = claims.Answered(a_gone.dropId);
		if (a_gone.takerId == localId && localId != 0) {
			if (const auto back = reclaiming.find(a_gone.dropId); back != reclaiming.end()) {
				{
					std::scoped_lock l{ recordLock };
					record.Made(back->second);
					record.Took(a_gone.dropId);
				}
				reclaiming.erase(back);
				RemoveCopy(a_gone.dropId);
				++reclaimed;
				return;
			}
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
		// Someone else's now, or back with its owner: our copy goes.
		if (reclaiming.erase(a_gone.dropId) || pickedUnblocked.erase(a_gone.dropId)) {
			++dupRisk;
			REX::WARN("Drops: drop {} went to player {} while our player had picked it up; both have it", a_gone.dropId, a_gone.takerId);
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
		TakeInbox(left, activated);
		NoticeDrops(left, now);
		SettleDrops(now);
		NoticeActivations(activated, now);
		FinishTake(now);
		PlaceCopies(now);
		ShowCopies(now);
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
		if (taking) {
			// A take not checked yet: settled now, so the save's record says what its inventory holds. In it, the take is
			// recorded; not, the item lies there again for anyone (our copy goes with the others).
			const auto ref = taking->ref.get();
			if (Carried(taking->base) > taking->before || !ref || !Loose(ref.get())) {
				std::scoped_lock l{ recordLock };
				record.Took(taking->dropId);
				++taken;
			} else {
				RemotePlayers::Discard(taking->ref, taking->placedAt);
				Claim(taking->dropId, Protocol::DropClaim::kGiveBack);
				++takeFailed;
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
		reclaiming.clear();
		pickedUnblocked.clear();
		claims.Clear();
		seenLoose.clear();
		unplaceable.clear();
		std::scoped_lock l{ inboxLock };
		leftInbox.clear();
		activatedInbox.clear();
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
		                   "takeFailed={} givenBack={} takenFirst={} reclaimed={} unblocked={} dupRisk={} lost={} byScan={}",
			lying.size(), copies.size(), dropped, notGiven, reported, refused, swapped, received, placed, placeFailed, removed, claimed, taken, takeFailed, givenBack, takenFirst,
			reclaimed, unblocked, dupRisk, lost, foundByScan);
	}
}
