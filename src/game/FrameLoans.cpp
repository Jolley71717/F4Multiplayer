#include "game/FrameLoans.h"

#include "Config.h"
#include "MainThread.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/FrameLoan.h"
#include "game/Hud.h"
#include "game/ItemCopies.h"
#include "game/NpcSync.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace FrameLoans
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using FrameLoan::Stage;

		// ---- the borrower's side ----

		// The loan of a friend's frame to our player: asked for, lent, worn. One at a time (the server holds us to one).
		struct Borrowed
		{
			Stage                       stage = Stage::kNone;
			std::uint32_t               spawnId = 0;
			std::uint32_t               owner = 0;
			std::uint32_t               copy = 0;  // our copy of it, pressed on
			std::uint32_t               base = 0;
			RE::ObjectRefHandle         handle;
			Clock::time_point           since{};    // the stage began
			std::vector<FrameLoan::Tag> tags;       // the lent items as our copies of them are named
			bool                        released = false;   // let go of by the save guard while worn: saved with our player
			bool                        ownerless = false;  // its owner left (or the session ended) while we wore it: it goes at exit
			// Our player's own fusion cores (none of the lent ones) as they got in, by base: the engine moves one into the frame
			// as the lent one runs down, and at most what they lost so is given back from it (GiveRest).
			std::unordered_map<std::uint32_t, std::int32_t> ownCores;
			// The instance mods each lent item's copy was made with, by slot: a lent item not found by its name is taken from our
			// copy only as a stack of these (FrameLoan::FitsUnnamed), never our player's own piece of its base with other mods.
			std::unordered_map<std::uint8_t, std::vector<std::uint32_t>> mods;
			// The slots of the lent items seen by their names, in our copy or on our player, while our player was in the frame
			// (FrameLoan::Hold): only those can be forfeit as the loan ends (FrameLoan::Forfeits), or noted in a save's record.
			std::vector<std::uint8_t> held;
			Clock::time_point         heldReadAt{};  // the last look for them (FrameLoan::LooksForHeld)
			Clock::time_point         inAt{};        // our player last got in
			// The kit's first seconds in our copy (WatchKit): when the lend put it in, how many frames since, and how many lent
			// items our copy held by name at the last look (-1: not looked at yet), and whether the WATCH_FOR summary is out.
			Clock::time_point lentAt{};
			std::uint32_t     framesSinceLent = 0;
			int               inCopyLast = -1;
			bool              watchSaid = false;
		};
		Borrowed borrowed;
		// The frame gate's word (KeepOut): the copy lent to us our player may get into now, as (spawn id << 32) | our copy's
		// id, or 0. The engine asks the gate as it seats our player, on its own thread, not the main one (the rig's log has
		// the ask on another thread than the loan's frames), while the main thread may be in the middle of the loan: so
		// the gate reads only this, which the main thread sets after each change (Publisher), and hands the rest of a
		// press to the main thread (Press).
		std::atomic<std::uint64_t> enterable{ 0 };

		void Publish()
		{
			const bool ours = borrowed.stage == Stage::kLent || borrowed.stage == Stage::kWorn || borrowed.stage == Stage::kOut;
			enterable = ours && borrowed.copy != 0 ? (std::uint64_t{ borrowed.spawnId } << 32) | borrowed.copy : 0;
		}

		// Publishes the loan's state as the main thread's call that may change it ends, however it ends.
		struct Publisher
		{
			Publisher() = default;
			Publisher(const Publisher&) = delete;
			Publisher& operator=(const Publisher&) = delete;
			~Publisher() { Publish(); }
		};
		// How long after the lend each change in what our copy holds of the kit is logged, frame by frame, and then summed up.
		constexpr auto WATCH_FOR = std::chrono::seconds(5);

		// ---- the owner's side ----

		// Where a lent item came from in our frame: its base and stack, and its condition then.
		struct Source
		{
			std::uint8_t  slot = 0;
			std::uint32_t base = 0;
			std::uint32_t stack = 0;  // its stack's number among its base's in the frame's inventory
			float         health = -1.0f;
			// Its stack's extra data then (none for a plain stack): how the return finds that very stack again.
			RE::BSTSmartPointer<RE::ExtraDataList> extra;
		};

		// One of our frames lent (or offered: kLendFrame sent, kFrameLent not in yet), by its spawn id.
		struct Lent
		{
			std::uint32_t       local = 0;
			std::uint32_t       borrower = 0;
			bool                granted = false;
			std::vector<Source> kit;
			Clock::time_point   since{};  // offered then
		};
		std::unordered_map<std::uint32_t, Lent> lent;
		// An offer the server never confirmed in this long (its kFrameLent lost, or the ask ended unheard) is forgotten: our
		// frame is ours to use again. The server confirms at once, or tells us the ask ended.
		constexpr auto LEND_WAIT = std::chrono::seconds(15);

		// Our frame to move to where its borrower left it, once that spot is in our sight (NpcSync::MoveOwnFrame).
		struct Move
		{
			std::uint32_t     local = 0;
			RemoteEdits::Spot at;
			float             heading = 0.0f;
			Clock::time_point since{};
		};
		std::vector<Move>    moves;
		// What came back of our frame lent, for when it has an inventory here again (PutBack).
		struct PendingBack
		{
			std::uint32_t                       local = 0;
			std::vector<Source>                 kit;
			std::vector<Protocol::ReturnedItem> items;
			Clock::time_point                   since{};
			bool                                warned = false;  // waiting a long while: logged once
		};
		std::vector<PendingBack> backs;
		constexpr auto       MOVE_LIMIT = std::chrono::minutes(30);  // a spot never come near again: it stays where it stood
		constexpr std::size_t MAX_MOVES = 16;

		// ---- everyone's ----

		std::unordered_map<std::uint32_t, std::uint32_t> lentTo;  // every live loan heard of: spawn id -> borrower
		std::unordered_map<std::uint32_t, bool>          lends;   // friends' word (their status): they lend their frames
		std::vector<std::vector<std::uint8_t>>           outgoing;

		// The LOAN record (SaveRecord, LoadRecord: the save's and the load's thread).
		std::mutex                       recordLock;
		std::optional<FrameLoan::Record> recordAtSave;  // under recordLock
		std::optional<FrameLoan::Record> recordLoaded;  // under recordLock

		// After a load of a save made in a borrowed frame: our player sent out of it, then it and the copies go.
		struct Cleanup
		{
			FrameLoan::Record    record;
			FrameLoan::AfterLoad what = FrameLoan::AfterLoad::kNothing;
			Clock::time_point    since{};
			Clock::time_point    askedAt{};
			Clock::time_point    firstAskedAt{};
			std::uint32_t        sendOuts = 0;
			bool                 asked = false;
			// Our player is (or may be) still in the frame: copies of friends' frames are kept clear of them (KeepsClear). From the
			// load until they first read as out of it.
			bool                 waitingOut = false;
			// The lent items our player wore were taken off them while they were still in it (FrameLoan::TakesWornBack), and how
			// many: the record holds only what is left (a save made now must not forfeit those at its load).
			bool                 wornTaken = false;
			std::size_t          taken = 0;

			std::optional<Clock::time_point> outSince;  // our player first read as out of the frame (FrameLoan::CleanupDue)
		};
		std::optional<Cleanup> cleanup;
		constexpr auto         CLEANUP_AFTER = std::chrono::seconds(2);   // the loaded world settles first
		constexpr auto         SEND_OUT_AGAIN = std::chrono::seconds(3);

		// The lent items of worn loans not found as they ended, one each (FrameLoan::MAX_FORFEITS): this world's, kept with its
		// saves (the LOAN record) and set from each load's.
		std::vector<FrameLoan::Tag> forfeited;
		// The forfeited items our player has on them again are looked for this often (SweepForfeits).
		constexpr auto                   SWEEP_EVERY = std::chrono::seconds(1);
		Clock::time_point                lastSweep{};
		Clock::time_point                loadedAt{};       // the last load (or new game): the loaded world settles first
		std::optional<Clock::time_point> outOfArmorSince;  // our player out of power armor since then
		bool                             sweepStuck = false;  // an item did not go: not tried again until the next load

		std::string       lastSaid;
		Clock::time_point lastSaidAt{};
		constexpr auto    SAY_AGAIN = std::chrono::seconds(3);

		std::uint32_t asked = 0, refusedHere = 0, lentHere = 0, worn = 0, returned = 0, ownerlessEnds = 0, lentOut = 0, givenBack = 0, piecesWorn = 0, piecesKept = 0, piecesGiven = 0, movedBack = 0, cleanedUp = 0,
		              unnamedTaken = 0,  // lent items gathered by their base, their names lost (GatherUnnamed)
		              forfeitsTaken = 0,
		              stuckIn = 0;  // clean-ups after a load whose send-out never got our player out (FrameLoan::TakesWornBack)

		// A notice, not again while it is the last one said a moment ago (a press is asked about more than once).
		void Say(const std::string& a_text)
		{
			const auto now = Clock::now();
			if (a_text == lastSaid && now - lastSaidAt < SAY_AGAIN) {
				return;
			}
			lastSaid = a_text;
			lastSaidAt = now;
			Hud::Notify(a_text);
		}

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		// Our player is in the frame, or getting in (the seat comes before the armor).
		bool PlayerIn(const RE::TESObjectREFR* a_frame)
		{
			const auto player = Player();
			if (!player || !a_frame) {
				return false;
			}
			const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
			const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
			return (occupied && occupied.get() == a_frame) || Equipment::PowerArmorFrameOf(player).get() == a_frame;
		}

		// The same by the frame's form id, for our copy while its handle does not resolve.
		bool PlayerIn(std::uint32_t a_frame)
		{
			const auto player = Player();
			if (!player || a_frame == 0) {
				return false;
			}
			const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
			const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
			const auto frame = Equipment::PowerArmorFrameOf(player);
			return (occupied && occupied->GetFormID() == a_frame) || (frame && frame->GetFormID() == a_frame);
		}

		// Our copy of the frame lent to us: by the handle it was pressed on, or by its id. The engine frees a reference as its
		// cell unloads and makes it anew from its record as the cell loads again (the one our player wears, or one they got
		// out of and walked away from), and the old handle names nothing then, though the copy stands there with the lent
		// items in it. Taken by its id only while what stands under it is still that copy: ours by the save guard's books,
		// or, let go of them while worn (released), a created frame of its base, not deleted. The handle is set to it again.
		RE::NiPointer<RE::TESObjectREFR> CopyRef()
		{
			if (auto held = borrowed.handle.get()) {
				return held;
			}
			if (borrowed.copy == 0) {
				return {};
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(borrowed.copy);
			if (!ref || ref->IsDeleted()) {
				return {};
			}
			const auto base = ref->GetObjectReference();
			const bool ours = borrowed.released ? ref->IsCreated() && base && base->GetFormID() == borrowed.base : SaveGuard::Placed(ref);
			if (!ours) {
				return {};
			}
			borrowed.handle = ref->GetHandle();
			REX::INFO("FrameLoans: our copy {:08X} of the frame lent to us is found again by its id (its handle named nothing)", borrowed.copy);
			return RE::NiPointer<RE::TESObjectREFR>(ref);
		}

		std::optional<RemoteEdits::Spot> SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return std::nullopt;
			}
			RemoteEdits::Spot spot;
			if (cell->IsInterior()) {
				spot.cell = cell->GetFormID();
			} else if (cell->worldSpace) {
				spot.worldspace = cell->worldSpace->GetFormID();
			} else {
				return std::nullopt;
			}
			spot.x = a_ref->data.location.x;
			spot.y = a_ref->data.location.y;
			spot.z = a_ref->data.location.z;
			return spot;
		}

		// The name a stack shows: one given to it (as our copies of lent items are), or empty.
		std::string NameOf(RE::ExtraDataList* a_extra)
		{
			const auto text = a_extra ? a_extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
			return text && !text->displayName.empty() ? std::string{ text->displayName.c_str() } : std::string{};
		}

		// The fusion core a frame runs on (Fallout4.esm's AmmoFusionCore; the console's `player.additem 75FE4`). The engine
		// strips editor ids at runtime, so it is known by its id. Never any other ammunition: a frame holds whatever its owner
		// stored in it (the Transfer menu), and a stack of rounds is not a core.
		constexpr std::uint32_t FUSION_CORE = 0x00075FE4;

		bool IsCore(const RE::TESBoundObject* a_object)
		{
			return a_object && a_object->GetFormType() == RE::ENUM_FORM_ID::kAMMO && a_object->GetFormID() == FUSION_CORE;
		}

		// A frame's power armor piece, or a fusion core.
		bool PieceOrCore(const RE::TESBoundObject* a_object)
		{
			return IsCore(a_object) || (a_object && Equipment::IsPowerArmorPiece(a_object));
		}

		struct Hit
		{
			RE::TESBoundObject* object = nullptr;
			std::uint32_t       stack = 0;
			std::int32_t        count = 0;
			std::uint8_t        slot = 0;
			float               health = -1.0f;
		};

		// The first stack in a_ref's inventory that a_match takes (given its base, name and extra data).
		template <class F>
		std::optional<Hit> FindStack(RE::TESObjectREFR* a_ref, F&& a_match)
		{
			const auto list = a_ref ? a_ref->inventoryList : nullptr;
			if (!list) {
				return std::nullopt;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object) {
					continue;
				}
				std::uint32_t index = 0;
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get(), ++index) {
					if (auto hit = a_match(item.object, stack->extra.get())) {
						hit->object = item.object;
						hit->stack = index;
						hit->count = static_cast<std::int32_t>(stack->count);
						return hit;
					}
				}
			}
			return std::nullopt;
		}

		// That stack out of a_ref: gone, or into a_to.
		void RemoveStack(RE::TESObjectREFR* a_ref, const Hit& a_hit, RE::TESObjectREFR* a_to = nullptr)
		{
			RE::TESObjectREFR::RemoveItemData data{ a_hit.object, a_hit.count };
			data.stackData.push_back(a_hit.stack);
			if (a_to) {
				data.otherContainer = a_to;
				data.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
			}
			a_ref->RemoveItem(data);
		}

		constexpr int MAX_STACKS = 64;  // stacks looked at once each at most, in case one does not go

		// Every lent item in a_ref (our copy, or our player) found, its condition noted, and removed.
		void Gather(RE::TESObjectREFR* a_ref, const std::vector<FrameLoan::Tag>& a_tags, std::vector<FrameLoan::Found>& a_found)
		{
			if (!Equipment::MayChange(a_ref, OwnGear::Change::kLoan)) {
				return;  // never our player's frame, nor a reference this game did not place
			}
			for (int i = 0; i < MAX_STACKS; ++i) {
				const auto hit = FindStack(a_ref, [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
					const auto base = a_object->GetFormID();
					if (std::ranges::none_of(a_tags, [&](const FrameLoan::Tag& a_tag) { return a_tag.base == base; })) {
						return std::nullopt;
					}
					const auto name = NameOf(a_extra);
					const auto tag = std::ranges::find_if(a_tags, [&](const FrameLoan::Tag& a_tag) { return FrameLoan::IsTagged(a_tag, base, name); });
					if (tag == a_tags.end()) {
						return std::nullopt;
					}
					Hit found;
					found.slot = tag->slot;
					if (a_extra && a_extra->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
						found.health = a_extra->GetHealthPerc();
					}
					return found;
				});
				if (!hit) {
					return;
				}
				a_found.push_back({ .slot = hit->slot, .health = hit->health });
				RemoveStack(a_ref, *hit);
			}
			REX::WARN("FrameLoans: lent items in {:08X} did not all go", a_ref->GetFormID());
		}

		// The stacks of a lent item's base a_ref holds, as the log tells them: each one's count, and its name as against the
		// one the lent item was given (never the name itself: it holds the owner's), and whether the engine has it as a name
		// given to it (a custom name: ours are). How a lent item not found by its name stands in our copy or on our player.
		std::string DescribeStacks(RE::TESObjectREFR* a_ref, const FrameLoan::Tag& a_tag)
		{
			const auto list = a_ref ? a_ref->inventoryList : nullptr;
			if (!list) {
				return "no inventory";
			}
			std::string        out;
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object || item.object->GetFormID() != a_tag.base) {
					continue;
				}
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					const auto extra = stack->extra.get();
					const auto text = extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
					const auto shown = NameOf(extra);
					const char* name = !text ? "no name data" : shown.empty() ? "no name" : shown == a_tag.name ? "the lent name" : "another name";
					const bool  given = text && text->ownerInstance.get() == RE::ExtraTextDisplayData::DisplayDataType::kCustomName;
					out += std::format("{}x{} ({}{})", out.empty() ? "" : ", ", stack->count, name, text && !given ? ", not a given one" : "");
				}
			}
			return out.empty() ? "none" : out;
		}

		// A stack's instance mods (their ids), as FrameLoan::FitsUnnamed compares them. None when more than the wire carries.
		std::optional<std::vector<std::uint32_t>> ModsOf(const RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra)
		{
			const auto item = ItemCopies::ItemOf(a_object, a_extra);
			if (!item) {
				return std::nullopt;
			}
			std::vector<std::uint32_t> out;
			for (const auto& mod : item->mods) {
				out.push_back(mod.mod);
			}
			return out;
		}

		// The slots of the lent items a_ref holds by their names, each once, and how many stacks it holds in all.
		struct Seen
		{
			std::vector<std::uint8_t> slots;
			std::uint32_t             stacks = 0;
		};
		Seen SeenIn(RE::TESObjectREFR* a_ref, const std::vector<FrameLoan::Tag>& a_tags)
		{
			Seen out;
			const auto list = a_ref ? a_ref->inventoryList : nullptr;
			if (!list) {
				return out;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object) {
					continue;
				}
				const auto base = item.object->GetFormID();
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					++out.stacks;
					if (std::ranges::none_of(a_tags, [&](const FrameLoan::Tag& a_tag) { return a_tag.base == base; })) {
						continue;
					}
					const auto name = NameOf(stack->extra.get());
					for (const auto& tag : a_tags) {
						if (FrameLoan::IsTagged(tag, base, name) && std::ranges::find(out.slots, tag.slot) == out.slots.end()) {
							out.slots.push_back(tag.slot);
						}
					}
				}
			}
			return out;
		}

		// Every stack in a_ref that a_keep does not take (given its base and extra data) removed, there and then: the
		// engine's own RemoveItem, as the return's Gather removes. Not the console's removeallitems: by all the rig shows it
		// empties the reference a moment after it is called, as the console's disable does (HavokGuard), so the kit put in
		// right after it went with it (see OnFrameLent). How many stacks went.
		template <class F>
		std::uint32_t RemoveAllBut(RE::TESObjectREFR* a_ref, F&& a_keep)
		{
			std::uint32_t removed = 0;
			for (int i = 0; i < MAX_STACKS; ++i) {
				const auto hit = FindStack(a_ref, [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
					return a_keep(a_object, a_extra) ? std::nullopt : std::optional<Hit>{ Hit{} };
				});
				if (!hit) {
					return removed;
				}
				RemoveStack(a_ref, *hit);
				++removed;
			}
			REX::WARN("FrameLoans: {:08X} still holds stacks after {} were removed", a_ref->GetFormID(), removed);
			return removed;
		}

		// The lent items seen while our player is in the frame (Borrowed::held), looked for in our copy and on our player
		// (FrameLoan::LooksForHeld: as they get in, just before a save, and for a while after they got in until each has
		// been seen). A piece our player had of a forfeited item of the same name (an earlier loan of this frame) counts
		// too, but it is also gathered at the return as this loan's, so it is found, never forfeit.
		void ConfirmHeld(RE::TESObjectREFR* a_copy, Clock::time_point a_now, bool a_force)
		{
			if (!FrameLoan::LooksForHeld(borrowed.held.size(), borrowed.tags.size(), a_force, a_now - borrowed.inAt, a_now - borrowed.heldReadAt)) {
				return;
			}
			borrowed.heldReadAt = a_now;
			const auto before = borrowed.held.size();
			FrameLoan::Hold(borrowed.held, borrowed.tags, SeenIn(a_copy, borrowed.tags).slots);
			FrameLoan::Hold(borrowed.held, borrowed.tags, SeenIn(Player(), borrowed.tags).slots);
			if (borrowed.held.size() != before) {
				REX::INFO("FrameLoans: {} of {} lent items seen in our copy {:08X} or on our player while they are in it", borrowed.held.size(), borrowed.tags.size(), borrowed.copy);
			}
		}

		// The kit in our copy in its first WATCH_FOR after the lend, looked at every frame: each change in how many lent
		// items it holds by name is logged with the time and the frames since the lend, the stage, and whether our player is
		// in it, and then what it and our player hold at the end. The rig's batch 12 saw all six read back at the lend and
		// none 0.3 s later, before our player got in: this says in which frame they went.
		void WatchKit(RE::TESObjectREFR* a_copy, Clock::time_point a_now)
		{
			if (borrowed.watchSaid || borrowed.tags.empty() || (borrowed.stage != Stage::kLent && borrowed.stage != Stage::kWorn && borrowed.stage != Stage::kOut)) {
				return;
			}
			++borrowed.framesSinceLent;
			const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(a_now - borrowed.lentAt).count();
			const auto inCopy = SeenIn(a_copy, borrowed.tags);
			const auto count = static_cast<int>(inCopy.slots.size());
			const bool in = PlayerIn(borrowed.copy);
			if (count != borrowed.inCopyLast) {
				REX::INFO("FrameLoans: our copy {:08X} holds {} of {} lent items by name ({} stacks in all{}), {} ms and {} frames after the lend; stage {}, our player {}", borrowed.copy, count,
					borrowed.tags.size(), inCopy.stacks, a_copy && !a_copy->inventoryList ? ", no inventory list" : "", since, borrowed.framesSinceLent, static_cast<int>(borrowed.stage),
					in ? "in it" : "not in it");
				borrowed.inCopyLast = count;
			}
			if (a_now - borrowed.lentAt >= WATCH_FOR) {
				borrowed.watchSaid = true;
				const auto onPlayer = SeenIn(Player(), borrowed.tags);
				REX::INFO("FrameLoans: {} s after the lend our copy {:08X} holds {} of {} lent items by name and our player {}; our player {}, {} seen while in it",
					std::chrono::duration_cast<std::chrono::seconds>(WATCH_FOR).count(), borrowed.copy, count, borrowed.tags.size(), onPlayer.slots.size(), in ? "in it" : "not in it",
					borrowed.held.size());
			}
		}

		// How many of a_base a_ref holds, every stack of it.
		std::int32_t CountOf(RE::TESObjectREFR* a_ref, std::uint32_t a_base)
		{
			const auto list = a_ref ? a_ref->inventoryList : nullptr;
			if (!list) {
				return 0;
			}
			std::int32_t count = 0;
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object || item.object->GetFormID() != a_base) {
					continue;
				}
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					count += static_cast<std::int32_t>(stack->count);
				}
			}
			return count;
		}

		// Our player's own fusion cores as they get in (Borrowed::ownCores): the vanilla core and each lent core's base.
		void NoteOwnCores(RE::PlayerCharacter* a_player)
		{
			borrowed.ownCores.clear();
			borrowed.ownCores[FUSION_CORE] = CountOf(a_player, FUSION_CORE);
			for (const auto& tag : borrowed.tags) {
				if (tag.core) {
					borrowed.ownCores[tag.base] = CountOf(a_player, tag.base);
				}
			}
		}

		void GiveOwn(RE::TESObjectREFR* a_copy, RE::PlayerCharacter* a_player, bool a_cores);

		// The lent items no name found (Gather from the copy and from our player first), taken from our copy all the same by
		// their base (FrameLoan::TakeUnnamed): one stack of it, one item, each, and only a stack with the mods its copy was made
		// with (a_mods, by slot; none after a load: FrameLoan::FitsUnnamed). a_reserved: how many of a base are our player's
		// own and go back to them (their fusion cores the engine moved in). What each one not found looks like in the copy
		// and on our player is logged, so the rig tells why its name did not hold. Never anything of a base no lent item
		// has, and never more of one than lent items of it are missing.
		template <class F>
		void GatherUnnamed(RE::TESObjectREFR* a_copy, RE::TESObjectREFR* a_player, const std::vector<FrameLoan::Tag>& a_tags, std::vector<FrameLoan::Found>& a_found,
			const std::unordered_map<std::uint8_t, std::vector<std::uint32_t>>& a_mods, F&& a_reserved)
		{
			const auto unfound = FrameLoan::NotFound(a_tags, a_found);
			if (unfound.empty()) {
				return;
			}
			for (const auto& tag : unfound) {
				REX::INFO("FrameLoans: lent item {} ({:08X}) not found by its name; of its base our copy holds {}; our player {}", tag.slot, tag.base, DescribeStacks(a_copy, tag),
					DescribeStacks(a_player, tag));
			}
			if (!a_copy || !Equipment::MayChange(a_copy, OwnGear::Change::kLoan)) {
				return;
			}
			std::unordered_map<std::uint32_t, std::int32_t> taken;  // by base, taken so far
			for (const auto& tag : unfound) {
				const auto missing = std::ranges::count_if(unfound, [&](const FrameLoan::Tag& a_other) { return a_other.base == tag.base; });
				if (FrameLoan::TakeUnnamed(static_cast<std::int32_t>(missing), CountOf(a_copy, tag.base) + taken[tag.base], a_reserved(tag)) <= taken[tag.base]) {
					continue;
				}
				const auto made = a_mods.find(tag.slot);
				const auto mods = made != a_mods.end() ? std::optional<std::vector<std::uint32_t>>{ made->second } : std::nullopt;
				auto       hit = FindStack(a_copy, [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
					if (a_object->GetFormID() != tag.base) {
						return std::nullopt;
					}
					const auto stack = ModsOf(a_object, a_extra);
					if (!stack || !FrameLoan::FitsUnnamed(mods, *stack)) {
						return std::nullopt;  // our player's own piece of its base, put on from the Pip-Boy: theirs (GiveOwn)
					}
					Hit found;
					if (a_extra && a_extra->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
						found.health = a_extra->GetHealthPerc();
					}
					return found;
				});
				if (!hit) {
					REX::INFO("FrameLoans: lent item {} ({:08X}) is not taken by its base: no stack of it in our copy {:08X} has the mods it was made with", tag.slot, tag.base, a_copy->GetFormID());
					continue;
				}
				hit->count = 1;
				RemoveStack(a_copy, *hit);
				++taken[tag.base];
				++unnamedTaken;
				a_found.push_back({ .slot = tag.slot, .health = hit->health });
				REX::INFO("FrameLoans: lent item {} ({:08X}) taken from our copy {:08X} by its base", tag.slot, tag.base, a_copy->GetFormID());
			}
		}

		// What of our player's own the engine moved into our copy goes back to them, once the lent items are gathered (none
		// of those is counted): their own fusion cores, as the lent one ran down (of each base, at most as many as they have
		// fewer of now than as they got in, and only what the copy still holds of it), and their own power armor pieces,
		// put on the frame from the Pip-Boy in place of lent ones (those came off onto our player, and are gathered from
		// there), which the engine puts into the frame as they get out: every piece left in it, as the lent ones are gone by
		// now, and the copy was emptied before the kit went in (OnFrameLent), so no piece in it is from anywhere else.
		// Nothing else of the copy goes to them: whatever more it holds goes with it.
		void GiveRest(RE::TESObjectREFR* a_copy, RE::PlayerCharacter* a_player)
		{
			if (!Equipment::MayChange(a_player, OwnGear::Change::kLoan) || !Equipment::MayChange(a_copy, OwnGear::Change::kLoan)) {
				return;
			}
			for (const auto& [base, before] : borrowed.ownCores) {
				const auto lost = before - CountOf(a_player, base);
				const auto there = CountOf(a_copy, base);
				const auto give = (std::min)(lost, there);
				const auto form = RE::TESForm::GetFormByID(base);
				const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
				if (give <= 0 || !object) {
					continue;
				}
				REX::INFO("FrameLoans: {} of {:08X}, our player's own fusion cores, go from the borrowed frame back to them", give, base);
				RE::TESObjectREFR::RemoveItemData data{ object, give };
				data.otherContainer = a_player;
				data.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
				a_copy->RemoveItem(data);
			}
			GiveOwn(a_copy, a_player, false);
		}

		// Our player's own power armor pieces left in our copy (and, a_cores, their own fusion cores: after a load, with no
		// count of what they got in with) go back to them. Run once the lent items are gathered: none of what is left is
		// one of those, the copy was emptied before the kit went in, and nothing gets into it but off our player.
		void GiveOwn(RE::TESObjectREFR* a_copy, RE::PlayerCharacter* a_player, bool a_cores)
		{
			if (!Equipment::MayChange(a_player, OwnGear::Change::kLoan) || !Equipment::MayChange(a_copy, OwnGear::Change::kLoan)) {
				return;
			}
			for (int i = 0; i < MAX_STACKS; ++i) {
				const auto hit = FindStack(a_copy, [&](RE::TESBoundObject* a_object, RE::ExtraDataList*) -> std::optional<Hit> {
					const bool own = (Equipment::IsPowerArmorPiece(a_object) && !Equipment::IsFrameItem(a_object)) || (a_cores && IsCore(a_object));
					return own ? std::optional<Hit>{ Hit{} } : std::nullopt;
				});
				if (!hit) {
					return;
				}
				REX::INFO("FrameLoans: {} of {:08X}, our player's own, left in the borrowed frame, go back to them", hit->count, hit->object->GetFormID());
				RemoveStack(a_copy, *hit, a_player);
				piecesGiven += static_cast<std::uint32_t>((std::max)(hit->count, 1));
			}
			REX::WARN("FrameLoans: our player's own items in {:08X} did not all go back to them", a_copy->GetFormID());
		}

		// A frame of ours nobody is in, gone (one the session's book of spawns no longer holds), as every copy of ours goes
		// (RemotePlayers::Discard): its persistence let go of, disabled once its 3D is in, deleted a moment later and kept
		// out of saves meanwhile. Disabled and deleted in one frame, as it was, a physics job of the frames after still
		// handled a collision with the frame our player had just got out of (DeferredDelete). Anyone here in it after
		// all keeps it, never left in a frame that is gone.
		void DeleteFrame(RE::TESObjectREFR* a_frame)
		{
			RemotePlayers::Discard(a_frame->GetHandle(), Clock::now());
		}

		// Our player out of the frame they are in: Actor.SwitchToPowerArmor with None, as a companion is sent out of a
		// friend's frame (RemotePlayers).
		bool SendOut(RE::PlayerCharacter* a_player)
		{
			const auto self = Papyrus::BoundObject(static_cast<RE::Actor*>(a_player), "Actor", false);
			const auto none = [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
				a_out[0] = nullptr;  // None: out of the one they are in
			};
			return self && Papyrus::CallOn(self, "SwitchToPowerArmor", none);
		}

		// The loan to our player is over: the lent items gathered from our copy and from our player and removed, what else
		// of ours is in the copy given to our player, its owner told (a_send) where it stands and what came back, and our
		// copy let go of, to be placed again where its owner's game has the frame. a_ended: never worn (nothing of it
		// moved or wore down).
		void Finish(bool a_send, bool a_ended, const char* a_why)
		{
			enterable = 0;  // the gate lets nobody into it from here on (Publisher sets it again as the call ends)
			const auto copy = CopyRef();
			const auto player = Player();
			// Our player in it after all (got in as the loan ended unworn): never taken from under them. The owner's game is
			// told it ended (its frame stays as it was), and ours goes, with the copies, as they get out.
			if (PlayerIn(borrowed.copy)) {
				if (a_send && !borrowed.ownerless) {
					Protocol::FrameReturn back;
					back.spawnId = borrowed.spawnId;
					back.ended = true;
					outgoing.push_back(Protocol::Encode(back, Protocol::MessageType::kReturnFrame));
				}
				REX::INFO("FrameLoans: player {}'s frame {:08X} given back ({}) with our player in it; ours goes as they get out", borrowed.owner, borrowed.spawnId, a_why);
				borrowed.ownerless = true;
				borrowed.stage = Stage::kWorn;
				borrowed.since = Clock::now();
				if (!borrowed.released) {
					borrowed.released = true;
					SaveGuard::Release(borrowed.copy);
				}
				return;
			}
			std::vector<FrameLoan::Found> found;
			if (copy) {
				Gather(copy.get(), borrowed.tags, found);
			}
			// Never from our player while they are in power armor (another frame): its pieces on them are its own.
			if (player && !Equipment::InPowerArmor(player)) {
				Gather(player, borrowed.tags, found);
			}
			// A lent item whose name did not hold is taken from the copy by its base: never one of our player's own cores the
			// engine moved in as the lent one ran down (GiveRest gives those back).
			GatherUnnamed(copy.get(), player, borrowed.tags, found, borrowed.mods, [&](const FrameLoan::Tag& a_tag) {
				const auto own = borrowed.ownCores.find(a_tag.base);
				return own != borrowed.ownCores.end() && player ? own->second - CountOf(player, a_tag.base) : 0;
			});
			if (copy && player) {
				GiveRest(copy.get(), player);
			}
			// Never our player's own suit (RemotePlayers::OurFrame, Spawns' Occupied).
			if (player && copy && player->lastUsedPowerArmor.get().get() == copy.get()) {
				player->lastUsedPowerArmor = RE::ObjectRefHandle{};
			}
			const auto items = FrameLoan::ComposeReturn(borrowed.tags, found, !copy);
			// The lent items not found now, by name or by base (FrameLoan::MAX_FORFEITS): the owner keeps every original (a
			// return never takes one out, and an ownerless end says nothing), so a copy of one our player put somewhere while
			// they wore the frame is forfeit, one each. Never worn, nothing of it left the copy (the transfer menu is kept out of
			// a lent frame). A core run down and used up by the engine is forfeit too: no item answers to it. Only one seen while
			// our player was in the frame (FrameLoan::Forfeits): one gone from the copy before that was never in our world.
			if (borrowed.released) {
				const auto lost = FrameLoan::Forfeits(borrowed.tags, found, borrowed.held);
				const auto unseen = FrameLoan::NotFound(borrowed.tags, found).size() - lost.size();
				if (!lost.empty()) {
					FrameLoan::Forfeit(forfeited, lost);
					REX::INFO("FrameLoans: {} lent items of player {}'s frame were not found as the loan ended, their owner keeping theirs; forfeit, taken from our player if they have them again ({} forfeit now)",
						lost.size(), borrowed.owner, forfeited.size());
				}
				if (unseen > 0) {
					REX::INFO("FrameLoans: {} lent items of player {}'s frame not found were never seen in our copy or on our player while they wore it; not forfeit", unseen, borrowed.owner);
				}
			}
			if (a_send && !borrowed.ownerless) {
				Protocol::FrameReturn back;
				back.spawnId = borrowed.spawnId;
				back.ended = a_ended;
				if (!a_ended) {
					if (const auto at = copy ? SpotOf(copy.get()) : std::nullopt) {
						back.cell = at->cell;
						back.worldspace = at->worldspace;
						back.x = at->x;
						back.y = at->y;
						back.z = at->z;
						back.heading = copy->data.angle.z;
					}
					back.items = items;
				}
				outgoing.push_back(Protocol::Encode(back, Protocol::MessageType::kReturnFrame));
			}
			const auto foundCount = std::ranges::count_if(items, &Protocol::ReturnedItem::found);
			REX::INFO("FrameLoans: player {}'s frame {:08X} (our copy {:08X}) given back ({}): {} of {} lent items found{}", borrowed.owner, borrowed.spawnId, borrowed.copy, a_why, foundCount, borrowed.tags.size(),
				borrowed.ownerless ? "; its owner is gone, so it goes" : "");
			if (borrowed.released) {
				// Ours again: kept out of saves as every copy is. One not found now (freed with its cell, its record kept: the lent
				// items are in it still) is put back on the books by its id, and forgotten as any copy lost is, its record
				// dropped so that no save keeps it and no cell loading again brings it back; Spawns does that itself for one it
				// still holds (Refresh), and a form of that id again is found by Spawns, never kept by us.
				if (copy) {
					SaveGuard::Track(copy.get());
				} else {
					// Forgotten here even while Spawns still lists it: its own handle died with ours, and its Refresh then let the
					// id go unheard (released, it was not on the books), so it would never drop the record.
					SaveGuard::Track(borrowed.copy, borrowed.base);
					SaveGuard::Forget(borrowed.copy);
					REX::INFO("FrameLoans: our copy {:08X} of the frame lent to us is not found as the loan ends; it is ours to keep out of saves again, and forgotten", borrowed.copy);
				}
			}
			if (copy) {
				// Spawns buries it as it drops it, when the handle Spawns holds still names it; one Spawns lost (its handle died
				// with its cell, and ours found it again by its id) is deleted here, or it would stand on, a frame to keep.
				const bool spawnsCopy = Spawns::LocalOf(borrowed.spawnId) == copy->GetFormID();
				const bool spawnsHolds = spawnsCopy && std::ranges::any_of(Spawns::FrameCopies(), [&](const RE::ObjectRefHandle& a_handle) { return a_handle.get().get() == copy.get(); });
				if (spawnsCopy) {
					Spawns::DropLentCopy(borrowed.spawnId);
				}
				if (!spawnsHolds && !PlayerIn(copy.get()) && !NpcSync::InFrame(copy.get())) {
					DeleteFrame(copy.get());
				}
			}
			if (borrowed.ownerless) {
				++ownerlessEnds;
			} else {
				++returned;
			}
			borrowed = {};
		}

		// ---- the owner's side ----

		// Someone here is in our frame: our player, or anyone else.
		bool InUse(RE::TESObjectREFR* a_frame)
		{
			return PlayerIn(a_frame) || NpcSync::InFrame(a_frame);
		}

		// Our frame's kit as the wire carries it (FrameLoan::PickKit), and where each item came from. None: no inventory to
		// read (its cell is not loaded).
		std::optional<std::vector<Protocol::LentItem>> ReadKit(RE::TESObjectREFR* a_frame, std::vector<Source>& a_sources)
		{
			const auto list = a_frame ? a_frame->inventoryList : nullptr;
			if (!list) {
				return std::nullopt;
			}
			struct Candidate
			{
				Protocol::DroppedItem item;
				std::uint32_t         stack = 0;
				bool                  core = false;
				RE::BSTSmartPointer<RE::ExtraDataList> extra;
			};
			std::vector<FrameLoan::KitStack>      stacks;
			std::vector<std::optional<Candidate>> candidates;
			{
				RE::BSAutoReadLock l{ list->rwLock };
				for (auto& entry : list->data) {
					const auto object = entry.object;
					if (!PieceOrCore(object) || Equipment::IsFrameItem(object)) {
						continue;
					}
					const bool    core = IsCore(object);
					std::uint32_t index = 0;
					for (auto stack = entry.stackData.get(); stack; stack = stack->nextStack.get(), ++index) {
						auto       item = ItemCopies::ItemOf(object, stack->extra.get());
						const bool fits = item && Protocol::IsShareableRef(item->base) &&
						                  std::ranges::all_of(item->mods, [](const Protocol::ItemMod& a_mod) { return Protocol::IsShareableRef(a_mod.mod); });
						stacks.push_back({ .piece = !core, .core = core, .fits = fits });
						if (fits) {
							candidates.push_back(Candidate{ std::move(*item), index, core, stack->extra });
						} else {
							candidates.push_back(std::nullopt);
						}
					}
				}
			}
			std::vector<Protocol::LentItem> kit;
			for (const auto i : FrameLoan::PickKit(stacks)) {
				const auto& candidate = *candidates[i];
				const auto  slot = static_cast<std::uint8_t>(kit.size());
				kit.push_back({ .slot = slot,
					.base = candidate.item.base,
					.templateIndex = candidate.item.templateIndex,
					.mods = candidate.item.mods,
					.health = candidate.item.health,
					.name = candidate.item.name,
					.core = candidate.core });
				a_sources.push_back({ .slot = slot, .base = candidate.item.base, .stack = candidate.stack, .health = candidate.item.health, .extra = candidate.extra });
			}
			return kit;
		}

		// The stack in our frame a lent item came from: the one with its extra data then, else one of its base at the
		// condition it was lent at, else the one at its number then (the engine may have merged or split stacks since).
		std::optional<Hit> SourceStack(RE::TESObjectREFR* a_frame, const Source& a_source, RE::BSTSmartPointer<RE::ExtraDataList>& a_extra)
		{
			const auto list = a_frame ? a_frame->inventoryList : nullptr;
			if (!list) {
				return std::nullopt;
			}
			const auto healthOf = [](RE::ExtraDataList* a_list) { return a_list && a_list->HasType(RE::EXTRA_DATA_TYPE::kHealth) ? a_list->GetHealthPerc() : -1.0f; };
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& entry : list->data) {
				if (!entry.object || entry.object->GetFormID() != a_source.base) {
					continue;
				}
				std::optional<Hit> byHealth, byIndex;
				std::uint32_t      index = 0;
				for (auto stack = entry.stackData.get(); stack; stack = stack->nextStack.get(), ++index) {
					const Hit hit{ .object = entry.object, .stack = index, .count = static_cast<std::int32_t>(stack->count) };
					if (a_source.extra && stack->extra.get() == a_source.extra.get()) {
						a_extra = stack->extra;
						return hit;
					}
					const float health = healthOf(stack->extra.get());
					if (!byHealth && std::abs(health - a_source.health) < 0.0001f) {
						byHealth = hit;
						a_extra = stack->extra;
					}
					if (!byIndex && index == a_source.stack) {
						byIndex = hit;
						if (!byHealth) {
							a_extra = stack->extra;
						}
					}
				}
				return byHealth ? byHealth : byIndex;
			}
			return std::nullopt;
		}

		// What came back of the lent items put on our frame (FrameLoan::AfterReturn): each worn down to what came back, never
		// up; one the borrower's game did not find is left as it is (nothing of ours goes on a return's word). One item
		// of its stack only: one of a stack of the same (fusion cores at full charge share one) is split off with its own
		// extra data first, so the others keep theirs. False when our frame has no inventory here yet, or our player is in it
		// (its pieces are on them, and the own-gear rule never changes a frame they are in): tried again.
		bool PutBack(RE::TESObjectREFR* a_frame, const std::vector<Source>& a_kit, const std::vector<Protocol::ReturnedItem>& a_items)
		{
			if (!a_frame || !a_frame->inventoryList || PlayerIn(a_frame)) {
				return false;
			}
			if (!Equipment::MayChange(a_frame, OwnGear::Change::kLoan)) {
				return false;  // a frame of ours made here, nobody in it: refused only while gone (counted and logged there)
			}
			std::uint32_t wornDown = 0, notBack = 0;
			for (const auto& source : a_kit) {
				const auto back = std::ranges::find(a_items, source.slot, &Protocol::ReturnedItem::slot);
				if (back != a_items.end() && !back->found) {
					++notBack;  // kept as it is (AfterReturn)
				}
				const auto after = FrameLoan::AfterReturn(source.health, back != a_items.end() ? &*back : nullptr);
				if (after.what == FrameLoan::Back::kKeep) {
					continue;
				}
				RE::BSTSmartPointer<RE::ExtraDataList> extra;
				auto                                    hit = SourceStack(a_frame, source, extra);
				if (!hit) {
					REX::WARN("FrameLoans: lent item {} ({:08X}) is no longer in our frame {:08X}", source.slot, source.base, a_frame->GetFormID());
					continue;
				}
				if (hit->count == 1 && extra) {
					extra->SetHealthPerc(after.health);
				} else {
					auto item = ItemCopies::ItemOf(hit->object, extra.get());
					if (!item) {
						continue;
					}
					item->count = 1;
					item->health = after.health;
					hit->count = 1;
					RemoveStack(a_frame, *hit);
					a_frame->AddObjectToContainer(hit->object, ItemCopies::ExtraFor(*item, hit->object), 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
				}
				++wornDown;
			}
			piecesWorn += wornDown;
			piecesKept += notBack;
			REX::INFO("FrameLoans: of our frame {:08X}'s lent items, {} came back worn down and {} were not found by the borrower's game (ours kept as they are)", a_frame->GetFormID(), wornDown, notBack);
			return true;
		}

		void OnReturnedToUs(const Protocol::FrameReturn& a_return)
		{
			const auto it = lent.find(a_return.spawnId);
			if (it == lent.end()) {
				return;
			}
			const auto loan = std::move(it->second);
			lent.erase(it);
			const auto frame = RE::TESForm::GetFormByID<RE::TESObjectREFR>(loan.local);
			const auto name = RemotePlayers::NameOf(a_return.borrowerId);
			if (a_return.ended) {
				// Shown again where it stood as the borrower's status stops naming it (RemotePlayers' frame hide).
				REX::INFO("FrameLoans: the loan of our frame {:08X} to player {} ended without a return", loan.local, a_return.borrowerId);
				if (loan.granted) {
					Say(Text::Format("party.frame_given_back", { { "player", name } }));
				}
				return;
			}
			++givenBack;
			if (!PutBack(frame, loan.kit, a_return.items)) {
				// Not loaded here now: put on it once it is (Frame), before anyone here can take from it.
				if (backs.size() >= MAX_MOVES) {
					backs.erase(backs.begin());
				}
				backs.push_back({ .local = loan.local, .kit = loan.kit, .items = a_return.items, .since = Clock::now() });
				REX::INFO("FrameLoans: our frame {:08X} has no inventory here now; what came back is put on it once it has", loan.local);
			}
			// Queued with no frame here too (freed with its cell while our player was away): moved once it is loaded again.
			if (FrameLoan::ChoosePlace(a_return) == FrameLoan::Place::kMove) {
				if (moves.size() >= MAX_MOVES) {
					moves.erase(moves.begin());
				}
				std::erase_if(moves, [&](const Move& a_move) { return a_move.local == loan.local; });
				moves.push_back({ .local = loan.local, .at = { a_return.cell, a_return.worldspace, a_return.x, a_return.y, a_return.z }, .heading = a_return.heading, .since = Clock::now() });
			}
			REX::INFO("FrameLoans: player {} gave our frame {:08X} back at ({:.0f},{:.0f},{:.0f}) in {:08X}", a_return.borrowerId, loan.local, a_return.x, a_return.y, a_return.z, a_return.cell ? a_return.cell : a_return.worldspace);
			Say(Text::Format("party.frame_given_back", { { "player", name } }));
		}

		// ---- after a load ----

		// What stands around our player as the clean-up sends them out of the frame (the log's, for the rig): where they are,
		// how the engine has them in it, and the copies of friends' frames near them (FrameLoan::KeepsClear).
		std::string DescribeAround(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_copy)
		{
			const auto at = SpotOf(a_player);
			if (!at) {
				return "our player in no cell";
			}
			const auto middle = a_player->currentProcess ? a_player->currentProcess->middleHigh : nullptr;
			const auto seat = middle ? middle->occupiedFurniture.get() : nullptr;
			const auto frame = Equipment::PowerArmorFrameOf(a_player);
			std::string out = std::format("our player at ({:.0f},{:.0f},{:.0f}), in power armor={}, seated in {:08X}, frame {:08X}; the frame disabled={} 3d={}", at->x, at->y, at->z,
				Equipment::InPowerArmor(a_player), seat ? seat->GetFormID() : 0, frame ? frame->GetFormID() : 0, a_copy && a_copy->IsDisabled(), a_copy && a_copy->Get3D() != nullptr);
			float         nearest = -1.0f;
			std::uint32_t nearestId = 0;
			bool          nearestLoaded = false;
			int           within = 0;
			for (const auto& handle : Spawns::FrameCopies()) {
				const auto ref = handle.get();
				const auto spot = ref ? SpotOf(ref.get()) : std::nullopt;
				const float distance = spot ? RemoteEdits::Distance(*at, *spot) : -1.0f;
				if (!(distance >= 0.0f)) {
					continue;
				}
				within += distance <= FrameLoan::EXIT_CLEARANCE ? 1 : 0;
				if (nearest < 0.0f || distance < nearest) {
					nearest = distance;
					nearestId = ref->GetFormID();
					nearestLoaded = ref->Get3D() != nullptr;
				}
			}
			if (nearestId == 0) {
				return out + "; no copy of a friend's frame in its space";
			}
			return out + std::format("; the nearest copy of a friend's frame {:08X} {:.0f} units off (3d={}), {} within {:.0f}", nearestId, nearest, nearestLoaded, within, FrameLoan::EXIT_CLEARANCE);
		}

		// The send-out did not get our player out (FrameLoan::TakesWornBack): the lent items on them are taken off them where
		// they stand, as the return gathers them. What is left of the kit (in the frame, which is never changed while they are
		// in it) stays on the record, and goes as they get out, or with the load of a save made meanwhile.
		void TakeWornBack(Cleanup& a_job, RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_copy, Clock::time_point a_now)
		{
			std::vector<FrameLoan::Found> found;
			Gather(a_player, a_job.record.tags, found);
			a_job.wornTaken = true;
			a_job.taken += found.size();
			a_job.record.tags = FrameLoan::NotFound(a_job.record.tags, found);
			++stuckIn;
			REX::WARN("FrameLoans: our player is still in {:08X}, the frame borrowed from player {} as this save was made, {} s after the first of {} send-outs; {} lent items are taken off them now, and the {} left go as they get out ({})",
				a_job.record.copy, a_job.record.owner, std::chrono::duration_cast<std::chrono::seconds>(a_now - a_job.firstAskedAt).count(), a_job.sendOuts, found.size(), a_job.record.tags.size(),
				DescribeAround(a_player, a_copy));
			Say(Text::Get("party.frame_loan_over"));
		}

		void RunCleanup(Clock::time_point a_now)
		{
			auto&      job = *cleanup;
			const auto player = Player();
			if (!player || a_now - job.since < CLEANUP_AFTER) {
				return;
			}
			const auto copy = job.what == FrameLoan::AfterLoad::kAll ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(job.record.copy) : nullptr;
			if (copy && PlayerIn(copy)) {
				job.outSince.reset();  // back in before the settle was over
				job.waitingOut = true;
				const auto sinceFirst = job.asked ? std::optional<Clock::duration>{ a_now - job.firstAskedAt } : std::nullopt;
				if (FrameLoan::TakesWornBack(sinceFirst, job.wornTaken)) {
					TakeWornBack(job, player, copy, a_now);
				}
				if (!job.asked || a_now - job.askedAt >= SEND_OUT_AGAIN) {
					const bool first = !job.asked;
					job.asked = true;
					job.askedAt = a_now;
					++job.sendOuts;
					if (first) {
						job.firstAskedAt = a_now;
					}
					const bool sent = SendOut(player);
					if (first) {
						REX::INFO("FrameLoans: this save was made in {:08X}, the frame borrowed from player {}; our player is sent out of it{} ({})", job.record.copy, job.record.owner,
							sent ? "" : " (the call could not be made)", DescribeAround(player, copy));
					}
				}
				return;
			}
			if (job.waitingOut) {
				job.waitingOut = false;  // out: copies of friends' frames stand where they are again
				if (job.asked) {
					REX::INFO("FrameLoans: our player is out of {:08X} {} ms after the first of {} send-outs", job.record.copy,
						std::chrono::duration_cast<std::chrono::milliseconds>(a_now - job.firstAskedAt).count(), job.sendOuts);
				}
			}
			if (Equipment::InPowerArmor(player)) {
				job.outSince.reset();
				return;  // in another frame: nothing is gathered from them until they are out of it
			}
			if (copy && !job.outSince) {
				job.outSince = a_now;
			}
			if (!FrameLoan::CleanupDue(copy != nullptr, job.outSince ? std::optional<Clock::duration>{ a_now - *job.outSince } : std::nullopt)) {
				return;  // the engine's exit runs on: the frame re-enabled, its 3D rebuilt, our player's own pieces moved back into it
			}
			std::vector<FrameLoan::Found> found;
			if (copy) {
				Gather(copy, job.record.tags, found);
			}
			Gather(player, job.record.tags, found);
			// No count of our player's own cores as they got in (a load since): a lent core whose name did not hold stays,
			// and goes back to them with their own.
			GatherUnnamed(copy, player, job.record.tags, found, {}, [](const FrameLoan::Tag& a_tag) { return a_tag.core ? (std::numeric_limits<std::int32_t>::max)() : 0; });
			if (copy) {
				GiveOwn(copy, player, true);  // their own pieces and cores, moved into it as they were sent out
				if (player->lastUsedPowerArmor.get().get() == copy) {
					player->lastUsedPowerArmor = RE::ObjectRefHandle{};
				}
				DeleteFrame(copy);
			}
			// The loan this save was made in ends here with no return (the server ended it as our game asked for the world
			// again): its owner keeps every original, and a lent item not found, by name or by base (put somewhere before the
			// save), is forfeit, one each.
			if (const auto lost = FrameLoan::NotFound(job.record.tags, found); !lost.empty()) {
				FrameLoan::Forfeit(forfeited, lost);
				REX::INFO("FrameLoans: {} lent items of the frame borrowed as this save was made were not found; forfeit, taken from our player if they have them again ({} forfeit now)", lost.size(), forfeited.size());
			}
			++cleanedUp;
			REX::INFO("FrameLoans: after the load, the frame borrowed from player {} ({:08X}) and {} lent items are gone{}", job.record.owner, job.record.copy, found.size() + job.taken,
				job.wornTaken ? std::format(" ({} of them taken off our player while they were still in it)", job.taken) : std::string{});
			if (!job.wornTaken) {
				Say(Text::Get("party.frame_loan_over"));  // said already as the worn ones were taken
			}
			cleanup.reset();
		}

		// ---- forfeits ----

		// The forfeited lent items our player has on them again (taken back out of a chest, bought back) removed: an item
		// of a forfeited tag's base and very name, one for each forfeit, which is then spent. At
		// most once a second, and only while nothing else moves what they carry: no loan or clean-up after a load under way
		// (those gather their own), our player alive and out of power armor for EXIT_SETTLE (the engine moves a frame's
		// pieces between it and its wearer as they get in and out, and the frame they are in is theirs), no menu up that
		// shows or moves their inventory, no loading screen, and the loaded world settled. Saves and loads hold the frame's
		// work (SaveLoadHold), so this never runs inside one.
		void SweepForfeits(Clock::time_point a_now)
		{
			if (forfeited.empty() || sweepStuck || borrowed.stage != Stage::kNone || cleanup || a_now - lastSweep < SWEEP_EVERY) {
				return;
			}
			lastSweep = a_now;
			const auto player = Player();
			if (!player || player->IsDead(false) || !outOfArmorSince || a_now - *outOfArmorSince < FrameLoan::EXIT_SETTLE || a_now - loadedAt < CLEANUP_AFTER) {
				return;
			}
			using OpenMenus::Menu;
			if (Hud::LoadingScreenUp() || OpenMenus::AnyOpen({ Menu::kPipboy, Menu::kContainer, Menu::kBarter, Menu::kExamine, Menu::kCooking, Menu::kPowerArmorMod, Menu::kWorkshop,
											  Menu::kFader, Menu::kPause, Menu::kConsole })) {
				return;
			}
			// One item for each forfeit, which is then spent: a name is no lent item's alone (the same owner's same frame lent
			// again names its copies the same), so never more of a name is taken than copies of it were left behind. Only by
			// that name: a lent item whose name did not hold is taken by its base and mods from the copy alone, at the loan's
			// end (GatherUnnamed), never here, so the two never take the same piece, and nothing of our player's own answers.
			for (int i = 0; i < MAX_STACKS && !forfeited.empty(); ++i) {
				const auto hit = FindStack(player, [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
					const auto base = a_object->GetFormID();
					if (std::ranges::none_of(forfeited, [&](const FrameLoan::Tag& a_tag) { return a_tag.base == base; })) {
						return std::nullopt;
					}
					const auto name = NameOf(a_extra);
					const auto tag = std::ranges::find_if(forfeited, [&](const FrameLoan::Tag& a_tag) { return FrameLoan::IsTagged(a_tag, base, name); });
					if (tag == forfeited.end()) {
						return std::nullopt;
					}
					Hit found;
					found.slot = static_cast<std::uint8_t>(tag - forfeited.begin());
					return found;
				});
				if (!hit || !Equipment::MayChange(player, OwnGear::Change::kLoan)) {
					return;
				}
				const auto tag = forfeited[hit->slot];
				const auto before = CountOf(player, tag.base);
				RemoveStack(player, Hit{ .object = hit->object, .stack = hit->stack, .count = 1 });
				if (CountOf(player, tag.base) >= before) {
					sweepStuck = true;
					REX::WARN("FrameLoans: forfeited {:08X} would not go from our player; not tried again until the next load", tag.base);
					return;
				}
				forfeited.erase(forfeited.begin() + hit->slot);
				++forfeitsTaken;
				REX::INFO("FrameLoans: forfeited {:08X}, a copy of a friend's lent piece whose loan ended with its owner keeping theirs, is taken from our player ({} forfeit now)", tag.base, forfeited.size());
			}
		}
	}

	std::uint32_t WornSpawnOf(std::uint32_t a_local)
	{
		// Not once its owner is gone (ownerless): their spawn ended with them, and no game has a frame of it to hide.
		const bool ours = (borrowed.stage == Stage::kLent || borrowed.stage == Stage::kWorn || borrowed.stage == Stage::kOut) && !borrowed.ownerless;
		return ours && a_local != 0 && a_local == borrowed.copy ? borrowed.spawnId : 0;
	}

	std::uint32_t LentOutTo(std::uint32_t a_local)
	{
		if (a_local == 0) {
			return 0;
		}
		const auto it = std::ranges::find_if(lent, [&](const auto& a_entry) { return a_entry.second.local == a_local; });
		return it != lent.end() ? it->second.borrower : 0;
	}

	bool LentOut(std::uint32_t a_local)
	{
		return LentOutTo(a_local) != 0;
	}

	bool LentToUs(std::uint32_t a_id)
	{
		return FrameLoan::HoldsSpawn(borrowed.stage, borrowed.spawnId, a_id);
	}

	bool BorrowedCopy(std::uint32_t a_local)
	{
		if (a_local == 0) {
			return false;
		}
		// Or the copy a loaded save was made in, while the clean-up waits out our player's exit from it (RunCleanup).
		if (cleanup && cleanup->what == FrameLoan::AfterLoad::kAll && a_local == cleanup->record.copy) {
			return true;
		}
		return a_local == borrowed.copy && borrowed.stage != Stage::kNone && borrowed.stage != Stage::kAsked;
	}

	bool KeepsClear(const RemoteEdits::Spot& a_at)
	{
		if (!cleanup || !cleanup->waitingOut) {
			return false;
		}
		const auto at = SpotOf(Player());
		return at && FrameLoan::KeepsClear(true, RemoteEdits::Distance(*at, a_at));
	}

	namespace
	{
		// Our player's press on a_local, our copy of a_owner's frame shared as a_spawnId (KeepOut), on the main thread: asks
		// its owner for it, or says why not. Nothing, should it be no such copy by now (gone, or another spawn's since).
		void Press(std::uint32_t a_local, std::uint32_t a_spawnId, std::uint32_t a_owner)
		{
			const Publisher publish;
			const auto      frame = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_local);
			if (!frame || frame->IsDeleted() || Spawns::CopyIdOf(a_local) != a_spawnId || Spawns::FrameCopyOwnerOf(a_local) != a_owner) {
				return;
			}
			const bool mine = borrowed.stage != Stage::kNone && borrowed.spawnId == a_spawnId;
			const auto status = lends.find(a_owner);
			const auto loan = lentTo.find(a_spawnId);
			FrameLoan::PressFacts facts;
			facts.ownerHere = status != lends.end();
			facts.ownerLends = facts.ownerHere && status->second;
			facts.ours = mine && borrowed.stage != Stage::kAsked;
			facts.filled = facts.ours && borrowed.copy == a_local;
			facts.asked = mine && borrowed.stage == Stage::kAsked;
			facts.otherLoan = borrowed.stage != Stage::kNone && !mine;
			facts.lentToOther = loan != lentTo.end() && loan->second != NpcSync::LocalPlayer();
			const auto name = RemotePlayers::NameOf(a_owner);
			switch (FrameLoan::ChoosePress(facts)) {
			case FrameLoan::Press::kEnter:
				return;  // lent to us since the press (the gate's word was a moment behind): the next press gets in
			case FrameLoan::Press::kAsk:
				{
					const auto base = frame->GetObjectReference();
					borrowed = {};
					borrowed.stage = Stage::kAsked;
					borrowed.spawnId = a_spawnId;
					borrowed.owner = a_owner;
					borrowed.copy = a_local;
					borrowed.base = base ? base->GetFormID() : 0;
					borrowed.handle = frame->GetHandle();
					borrowed.since = Clock::now();
					outgoing.push_back(Protocol::Encode(Protocol::FrameBorrow{ 0, a_spawnId }, Protocol::MessageType::kBorrowFrame));
					++asked;
					REX::INFO("FrameLoans: asking player {} for their frame {:08X} (our copy {:08X})", a_owner, a_spawnId, a_local);
					Say(Text::Format("party.frame_asking", { { "player", name } }));
					return;
				}
			case FrameLoan::Press::kWait:
				Say(Text::Format("party.frame_asking", { { "player", name } }));
				return;
			case FrameLoan::Press::kNotLending:
				Say(Text::Format("party.frame_not_lending", { { "player", name } }));
				return;
			case FrameLoan::Press::kBusy:
				Say(Text::Get("party.frame_busy"));
				return;
			case FrameLoan::Press::kLent:
			case FrameLoan::Press::kOwnerGone:
				Say(Text::Format("party.frame_refused", { { "player", name } }));
				return;
			}
		}
	}

	bool KeepOut(RE::TESObjectREFR* a_frame, std::uint32_t a_spawnId, std::uint32_t a_owner)
	{
		// Asked on the thread the engine seats our player on (not the main one): it reads only the gate's word, and the
		// press (an ask, or the HUD's word why not) is the main thread's, before its next frame's work.
		if (!a_frame) {
			return true;
		}
		const auto local = a_frame->GetFormID();
		if (enterable.load() == ((std::uint64_t{ a_spawnId } << 32) | local)) {
			return false;
		}
		MainThread::Post([local, a_spawnId, a_owner] { Press(local, a_spawnId, a_owner); });
		return true;
	}

	bool IsLentItem(std::uint32_t a_base, std::string_view a_name)
	{
		const auto tagged = [&](const FrameLoan::Tag& a_tag) { return FrameLoan::IsTagged(a_tag, a_base, a_name); };
		const bool lentNow = borrowed.stage != Stage::kNone && borrowed.stage != Stage::kAsked;
		// Or forfeit (a loan ended with its owner keeping theirs): no more ours to give than while it was lent.
		return (lentNow && std::ranges::any_of(borrowed.tags, tagged)) || std::ranges::any_of(forfeited, tagged);
	}

	bool KeepsThroughSave(const RE::TESObjectREFR* a_copy)
	{
		const bool wearing = borrowed.stage == Stage::kWorn || borrowed.stage == Stage::kOut;
		return wearing && a_copy && a_copy->GetFormID() == borrowed.copy;
	}

	void BeforeSave()
	{
		std::optional<FrameLoan::Record> record;
		if (borrowed.stage == Stage::kWorn || borrowed.stage == Stage::kOut) {
			// Only the lent items seen while our player was in it (FrameLoan::Held): the load's clean-up forfeits whatever of
			// these it does not find, and one never seen was never here to find. Looked for once more first: the load's
			// clean-up also gathers only these, and one in the saved world but not seen yet would be left with our player.
			const auto copy = CopyRef();
			ConfirmHeld(copy && !copy->IsDeleted() ? copy.get() : nullptr, Clock::now(), true);
			record = FrameLoan::Record{ .copy = borrowed.copy, .base = borrowed.base, .spawnId = borrowed.spawnId, .owner = borrowed.owner, .tags = FrameLoan::Held(borrowed.tags, borrowed.held) };
			REX::INFO("FrameLoans: this save holds {:08X}, the frame our player wears on loan from player {}, and {} lent items ({} seen while worn)", borrowed.copy, borrowed.owner, borrowed.tags.size(),
				record->tags.size());
		} else if (cleanup) {
			// The clean-up after the last load is not done yet (our player still in the frame, or the world settling): this
			// save holds what that one did, and its load cleans it up the same way.
			record = cleanup->record;
			REX::INFO("FrameLoans: this save still holds {:08X}, the frame borrowed from player {} as the save loaded was made; noted again", cleanup->record.copy, cleanup->record.owner);
		}
		// This world's forfeits, with a loan's record or alone.
		if (!forfeited.empty()) {
			if (!record) {
				record.emplace();
			}
			record->forfeited = forfeited;
		} else if (record) {
			record->forfeited.clear();
		}
		std::scoped_lock l{ recordLock };
		recordAtSave = std::move(record);
	}

	void EndUnwornBeforeSave()
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		if (borrowed.stage == Stage::kLent) {
			Finish(true, true, "a save came before our player got in");
		}
	}

	void OnStatus(std::uint32_t a_playerId, bool a_lends)
	{
		lends[a_playerId] = a_lends;
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		lends.erase(a_playerId);
	}

	void OnLoanAsked(const Protocol::FrameBorrow& a_ask)
	{
		const auto local = Spawns::LocalOf(a_ask.spawnId);
		const auto frame = local != 0 && Spawns::OwnIdOf(local) == a_ask.spawnId ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(local) : nullptr;
		const bool here = frame && !frame->IsDeleted() && !frame->IsDisabled() && Equipment::IsPowerArmorFrame(frame->GetObjectReference());
		FrameLoan::LendFacts facts{ .lends = Config::Get().lendPowerArmor, .here = here, .inUse = here && InUse(frame), .lentOut = lent.contains(a_ask.spawnId) };
		Protocol::FrameLending answer;
		answer.borrowerId = a_ask.playerId;
		answer.spawnId = a_ask.spawnId;
		answer.refusal = FrameLoan::ChooseLend(facts);
		std::vector<Source> sources;
		if (answer.refusal == Protocol::LoanRefusal::kNone) {
			if (auto kit = ReadKit(frame, sources)) {
				answer.kit = std::move(*kit);
			} else {
				answer.refusal = Protocol::LoanRefusal::kUnavailable;
			}
		}
		if (answer.refusal == Protocol::LoanRefusal::kNone) {
			lent[a_ask.spawnId] = { .local = local, .borrower = a_ask.playerId, .granted = false, .kit = std::move(sources), .since = Clock::now() };
		}
		REX::INFO("FrameLoans: player {} asks for our frame {:08X} ({:08X}): {}", a_ask.playerId, a_ask.spawnId, local,
			answer.refusal == Protocol::LoanRefusal::kNone ? std::format("lent, with {} items", answer.kit.size()) : std::format("refused ({})", static_cast<int>(answer.refusal)));
		outgoing.push_back(Protocol::Encode(answer, Protocol::MessageType::kLendFrame));
	}

	void OnFrameLent(const Protocol::FrameLending& a_lending)
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		const auto us = NpcSync::LocalPlayer();
		if (a_lending.refusal == Protocol::LoanRefusal::kNone) {
			lentTo[a_lending.spawnId] = a_lending.borrowerId;
		}
		// Ours, lent.
		if (a_lending.ownerId == us && us != 0) {
			if (a_lending.refusal != Protocol::LoanRefusal::kNone) {
				return;
			}
			auto it = lent.find(a_lending.spawnId);
			if (it == lent.end()) {
				// Our offer, confirmed after LEND_WAIT (ours again meanwhile; a loading screen of ours held the word up): taken up all
				// the same, since the borrower's game fills its copy on this very word and wears it. Ignored, the return would be
				// ignored too, and a piece whose copy stayed in their world (dropped, sold) would be in our frame as well. The
				// kit as the server kept it: each item found again by its base and the condition it was lent at (SourceStack),
				// not by its stack then. Only for a frame still ours and shared under that spawn.
				const auto local = Spawns::LocalOf(a_lending.spawnId);
				if (local == 0 || Spawns::OwnIdOf(local) != a_lending.spawnId) {
					REX::WARN("FrameLoans: frame {:08X} is lent to player {} on our word, but it is no frame of ours shared now; the return will not be put on it", a_lending.spawnId, a_lending.borrowerId);
					return;
				}
				std::vector<Source> kit;
				for (const auto& item : a_lending.kit) {
					kit.push_back({ .slot = item.slot, .base = item.base, .stack = 0, .health = item.health });
				}
				it = lent.emplace(a_lending.spawnId, Lent{ .local = local, .borrower = a_lending.borrowerId, .granted = true, .kit = std::move(kit), .since = Clock::now() }).first;
				++lentOut;
				REX::INFO("FrameLoans: our frame {:08X} ({:08X}) is lent to player {}, confirmed late (our offer had been given up); the {} items are found again by base and condition at the return", a_lending.spawnId, local,
					a_lending.borrowerId, it->second.kit.size());
				Say(Text::Format("party.frame_borrowed", { { "player", RemotePlayers::NameOf(a_lending.borrowerId) } }));
				return;
			}
			if (!it->second.granted) {
				it->second.granted = true;
				++lentOut;
				REX::INFO("FrameLoans: our frame {:08X} ({:08X}) is lent to player {}", a_lending.spawnId, it->second.local, a_lending.borrowerId);
				Say(Text::Format("party.frame_borrowed", { { "player", RemotePlayers::NameOf(a_lending.borrowerId) } }));
			}
			return;
		}
		if (a_lending.borrowerId != us || us == 0) {
			return;
		}
		const bool askedFor = borrowed.stage == Stage::kAsked && borrowed.spawnId == a_lending.spawnId;
		if (a_lending.refusal != Protocol::LoanRefusal::kNone) {
			if (askedFor) {
				REX::INFO("FrameLoans: player {} does not lend us frame {:08X} ({})", a_lending.ownerId, a_lending.spawnId, static_cast<int>(a_lending.refusal));
				const auto name = RemotePlayers::NameOf(a_lending.ownerId);
				Say(a_lending.refusal == Protocol::LoanRefusal::kNotLending ? Text::Format("party.frame_not_lending", { { "player", name } }) : Text::Format("party.frame_refused", { { "player", name } }));
				++refusedHere;
				borrowed = {};
			}
			return;
		}
		if (borrowed.spawnId == a_lending.spawnId && borrowed.stage != Stage::kNone && !askedFor) {
			return;  // told again (we asked again while it was lent to us)
		}
		const auto copy = borrowed.handle.get();
		if (!askedFor || !copy || copy->IsDeleted() || !Equipment::MayChange(copy.get(), OwnGear::Change::kLoan)) {
			// Not asked for now (asked before a load), or our copy is gone (or not one ours to fill): given back as it is.
			REX::INFO("FrameLoans: frame {:08X} lent to us by player {}, but {}; given back", a_lending.spawnId, a_lending.ownerId, askedFor ? "our copy of it is gone" : "we no longer ask for it");
			Protocol::FrameReturn back;
			back.spawnId = a_lending.spawnId;
			back.ended = true;
			outgoing.push_back(Protocol::Encode(back, Protocol::MessageType::kReturnFrame));
			if (askedFor) {
				borrowed = {};
			}
			return;
		}
		// Copies of its kit in our copy, each named as its owner's: never stacked with our player's own, and found again at
		// the return.
		// Emptied first: a frame never looked into is filled from its base's container as it is first opened (as for a
		// stand-in's, Equipment::SwitchIntoPowerArmor), and those would be pieces and a core from nowhere. Emptied here and
		// now, stack by stack (RemoveAllBut). This was the console's removeallitems: the kit went in right after it, read back
		// whole, and was gone from the copy within 0.3 s, before our player got in (the rig's batch 12), with nothing else of
		// ours taking from the copy then, so every borrower wore an empty frame. The console's command, it seems, empties
		// the reference a moment after it is called, as its disable is queued (HavokGuard); WatchKit logs the frame the kit
		// goes in, should it go again. Our copy was emptied as it was placed (RemotePlayers' PlaceCopy), long before a press
		// could ask for it; this takes what got in since.
		const bool hadList = copy->inventoryList != nullptr;
		const auto emptied = RemoveAllBut(copy.get(), [](RE::TESBoundObject*, RE::ExtraDataList*) { return false; });
		const auto owner = RemotePlayers::NameOf(a_lending.ownerId);
		bool       coreMade = false;
		for (const auto& item : a_lending.kit) {
			Protocol::DroppedItem made;
			made.base = item.base;
			made.count = 1;
			made.templateIndex = item.templateIndex;
			made.mods = item.mods;
			made.health = item.health;
			const auto form = RE::TESForm::GetFormByID(item.base);
			const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!object || !ItemCopies::Placeable(made)) {
				continue;  // the return says nothing of it: the owner's stays as it is
			}
			// Only what a frame is lent with (FrameLoan::PickKit): power armor pieces, and one fusion core (ammunition), each
			// slot once. Anything else a kit names is not made.
			const bool core = object->GetFormType() == RE::ENUM_FORM_ID::kAMMO;
			const bool kind = item.core ? core && !coreMade : !core && Equipment::IsPowerArmorPiece(object) && !Equipment::IsFrameItem(object);
			if (!kind || std::ranges::any_of(borrowed.tags, [&](const FrameLoan::Tag& a_tag) { return a_tag.slot == item.slot; })) {
				REX::WARN("FrameLoans: player {}'s kit names {:08X} in slot {}, which a frame is not lent with; not made", a_lending.ownerId, item.base, item.slot);
				continue;
			}
			coreMade = coreMade || item.core;
			const std::string own = item.name.empty() ? std::string{ RE::TESFullName::GetFullName(*form) } : item.name;
			made.name = std::string{ Protocol::Utf8Prefix(Text::Format("party.lent_item", { { "player", owner }, { "item", own } }), Protocol::MAX_ITEM_NAME_LENGTH) };
			const auto extra = ItemCopies::ExtraFor(made, object);
			if (const auto mods = ModsOf(object, extra.get())) {
				borrowed.mods[item.slot] = *mods;
			}
			copy->AddObjectToContainer(object, extra, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
			borrowed.tags.push_back({ .slot = item.slot, .base = item.base, .name = made.name, .core = item.core });
		}
		// Whatever else is in it now goes too (FrameLoan::KeepsAfterLend): a list the engine made only as the first copy went
		// in may have been filled from the base's container then, with pieces of a lent item's base as well, which the return
		// would give our player as their own (GiveOwn). Should a name not have held, every stack of a lent item's base stays:
		// that is the kit (the read-back below says so, and the return takes it by its base).
		const bool allNamed = std::ranges::all_of(borrowed.tags, [&](const FrameLoan::Tag& a_tag) {
			return FindStack(copy.get(), [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
				return FrameLoan::IsTagged(a_tag, a_object->GetFormID(), NameOf(a_extra)) ? std::optional<Hit>{ Hit{} } : std::nullopt;
			}).has_value();
		});
		const auto strays = RemoveAllBut(copy.get(), [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) {
			return FrameLoan::KeepsAfterLend(borrowed.tags, a_object->GetFormID(), NameOf(a_extra), allNamed);
		});
		borrowed.owner = a_lending.ownerId;
		borrowed.stage = Stage::kLent;
		borrowed.since = Clock::now();
		borrowed.lentAt = borrowed.since;
		++lentHere;
		REX::INFO("FrameLoans: player {} lent us frame {:08X}; {} of its {} items are in our copy {:08X} ({} stacks taken out before{}, {} after)", a_lending.ownerId, a_lending.spawnId, borrowed.tags.size(),
			a_lending.kit.size(), borrowed.copy, emptied, hadList ? "" : ", which had no inventory list", strays);
		// Read back at once: each copy made must be found by its name, as the return finds it (Gather).
		for (const auto& tag : borrowed.tags) {
			const auto named = FindStack(copy.get(), [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) -> std::optional<Hit> {
				return FrameLoan::IsTagged(tag, a_object->GetFormID(), NameOf(a_extra)) ? std::optional<Hit>{ Hit{} } : std::nullopt;
			});
			if (!named) {
				REX::WARN("FrameLoans: lent item {} ({:08X}) is not found by its name in our copy {:08X} just after it was made; of its base it holds {}", tag.slot, tag.base, borrowed.copy,
					DescribeStacks(copy.get(), tag));
			}
		}
		Say(Text::Format("party.frame_lent", { { "player", owner } }));
	}

	void OnFrameReturned(const Protocol::FrameReturn& a_return)
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		lentTo.erase(a_return.spawnId);
		const auto us = NpcSync::LocalPlayer();
		if (us == 0) {
			return;
		}
		if (a_return.ownerId == us) {
			OnReturnedToUs(a_return);
			return;
		}
		if (a_return.borrowerId != us || borrowed.spawnId != a_return.spawnId || !a_return.ended) {
			return;  // our own return, heard back; or someone else's
		}
		// Ended by the server: its owner left, or their game let the frame go (a load).
		switch (borrowed.stage) {
		case Stage::kAsked:
			borrowed = {};
			break;
		case Stage::kLent:
			Finish(false, true, "its owner's game let it go");
			break;
		case Stage::kWorn:
		case Stage::kOut:
			if (!borrowed.ownerless) {
				borrowed.ownerless = true;
				REX::INFO("FrameLoans: player {}'s frame {:08X}, which our player wears, is theirs no more; it goes as they get out", borrowed.owner, borrowed.spawnId);
				Say(Text::Format("party.frame_owner_left", { { "player", RemotePlayers::NameOf(borrowed.owner) } }));
			}
			break;
		case Stage::kNone:
			break;
		}
	}

	void Frame()
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		const auto now = Clock::now();
		if (cleanup) {
			RunCleanup(now);
		}
		if (borrowed.stage != Stage::kNone) {
			const auto copy = CopyRef();
			const FrameLoan::WearFacts facts{ .copyThere = copy && !copy->IsDeleted(), .in = PlayerIn(borrowed.copy), .inArmor = Equipment::InPowerArmor(Player()) };
			const auto                 live = facts.copyThere ? copy.get() : nullptr;  // looked into only while not deleted
			switch (FrameLoan::NextStep(borrowed.stage, now - borrowed.since, facts)) {
			case FrameLoan::Step::kGiveUp:
				{
					REX::INFO("FrameLoans: player {} never answered our ask for frame {:08X}", borrowed.owner, borrowed.spawnId);
					// The server is told the ask is over (it tells the owner's game): a late grant is then dropped there.
					Protocol::FrameReturn back;
					back.spawnId = borrowed.spawnId;
					back.ended = true;
					outgoing.push_back(Protocol::Encode(back, Protocol::MessageType::kReturnFrame));
					borrowed = {};
					break;
				}
			case FrameLoan::Step::kWorn:
				if (borrowed.stage == Stage::kLent) {
					++worn;
					REX::INFO("FrameLoans: our player got into {:08X}, player {}'s frame lent to us; it holds {} of {} lent items by name, our player {}", borrowed.copy, borrowed.owner,
						SeenIn(live, borrowed.tags).slots.size(), borrowed.tags.size(), SeenIn(Player(), borrowed.tags).slots.size());
					NoteOwnCores(Player());
				}
				borrowed.stage = Stage::kWorn;
				borrowed.since = now;
				borrowed.inAt = now;
				ConfirmHeld(live, now, true);  // as they get in: the kit is in the copy still, or moving onto them
				// A save made now must hold it, with our player in it (a save in a frame it lacks does not load).
				if (!borrowed.released) {
					borrowed.released = true;
					SaveGuard::Release(borrowed.copy);
				}
				break;
			case FrameLoan::Step::kOut:
				borrowed.stage = Stage::kOut;
				borrowed.since = now;
				break;
			case FrameLoan::Step::kReturn:
				Finish(true, borrowed.stage == Stage::kLent, borrowed.stage == Stage::kLent ? (facts.copyThere ? "never got into" : "our copy went") : "our player got out");
				break;
			case FrameLoan::Step::kNone:
				break;
			}
			// Finish (kReturn, kGiveUp) may have ended the loan: nothing of it is looked at then.
			if (borrowed.stage == Stage::kWorn || borrowed.stage == Stage::kOut) {
				ConfirmHeld(live, now, false);
			}
			if (borrowed.stage != Stage::kNone && borrowed.stage != Stage::kAsked) {
				WatchKit(live, now);
			}
		}
		// The forfeited lent items our player has on them again go (SweepForfeits). Whether they are in power armor is read
		// every frame, so that no getting in and out between two sweeps is missed; seated in any furniture counts as in it
		// (a frame's seat comes before its armor, and the engine moves the pieces as they get in).
		if (!forfeited.empty()) {
			const auto player = Player();
			const auto middle = player && player->currentProcess ? player->currentProcess->middleHigh : nullptr;
			const bool seated = middle && middle->occupiedFurniture.get();
			if (!player || seated || Equipment::InPowerArmor(player)) {
				outOfArmorSince.reset();
			} else if (!outOfArmorSince) {
				outOfArmorSince = now;
			}
			SweepForfeits(now);
		}
		// Ours offered and never confirmed: ours again.
		std::erase_if(lent, [&](const auto& a_entry) {
			if (a_entry.second.granted || now - a_entry.second.since < LEND_WAIT) {
				return false;
			}
			REX::INFO("FrameLoans: our frame {:08X} offered to player {} was never confirmed; ours again", a_entry.first, a_entry.second.borrower);
			return true;
		});
		// Ours given back while it had no inventory here, or our player was in it: what came back put on it once it can be,
		// however long that takes (a piece that did not come back is in the borrower's world now: left in our frame as well,
		// it would be two). Until a load (the loaded world's frame is as it was saved).
		std::erase_if(backs, [&](PendingBack& a_back) {
			if (PutBack(RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_back.local), a_back.kit, a_back.items)) {
				return true;
			}
			if (!a_back.warned && now - a_back.since >= MOVE_LIMIT) {
				a_back.warned = true;
				REX::WARN("FrameLoans: our frame {:08X} has had no inventory here (or our player in it) since its loan's return; what came back of it waits", a_back.local);
			}
			return false;
		});
		// Ours given back: moved to where its borrower left it once that spot is in our sight.
		std::erase_if(moves, [&](const Move& a_move) {
			if (now - a_move.since >= MOVE_LIMIT) {
				REX::INFO("FrameLoans: our frame {:08X} stays where it stood; where it was given back never came in sight", a_move.local);
				return true;
			}
			const auto frame = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_move.local);
			if (!frame) {
				return false;  // unloaded with its cell: tried again once it is back (until MOVE_LIMIT)
			}
			if (!Equipment::IsPowerArmorFrame(frame->GetObjectReference())) {
				// Its id names something else now (made at runtime, freed with its cell, the id handed out again): never moved
				// in its place (MoveOwnFrame moves whatever it is given).
				REX::INFO("FrameLoans: our frame {:08X}, given back, is no frame here now; nothing is moved", a_move.local);
				return true;
			}
			if (!NpcSync::MoveOwnFrame(frame, a_move.at, a_move.heading)) {
				return false;
			}
			++movedBack;
			return true;
		});
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	std::vector<std::uint8_t> SaveRecord()
	{
		std::scoped_lock l{ recordLock };
		return recordAtSave ? FrameLoan::Encode(*recordAtSave) : std::vector<std::uint8_t>{};
	}

	void LoadRecord(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_version)
	{
		auto record = FrameLoan::Decode(a_bytes, a_version);
		std::scoped_lock l{ recordLock };
		recordLoaded = std::move(record);
	}

	void RevertRecord()
	{
		std::scoped_lock l{ recordLock };
		recordLoaded.reset();
	}

	void BeforeLoad()
	{
		if (!lent.empty() || !backs.empty() || !moves.empty()) {
			REX::INFO("FrameLoans: the load ends {} loans of our frames, and lets go of {} gives back and {} moves waiting", lent.size(), backs.size(), moves.size());
		}
		lent.clear();
		backs.clear();
		moves.clear();
	}

	void OnGameLoaded()
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		// The old world's loans are over: the server ends them as our game asks for the world again, and lets our
		// spawns go (Spawns::OnGameLoaded).
		if (borrowed.stage != Stage::kNone) {
			REX::INFO("FrameLoans: the loan of player {}'s frame {:08X} ends with the load", borrowed.owner, borrowed.spawnId);
		}
		borrowed = {};
		lent.clear();
		moves.clear();
		backs.clear();  // the frames they were for are the loaded world's now
		std::optional<FrameLoan::Record> record;
		{
			std::scoped_lock l{ recordLock };
			record = std::exchange(recordLoaded, std::nullopt);
		}
		cleanup.reset();
		// This world's forfeits are the loaded save's (none in a new game, or a save without them): a load of an older save
		// has the world before the loans that forfeited the newer ones.
		forfeited = record ? record->forfeited : std::vector<FrameLoan::Tag>{};
		loadedAt = Clock::now();
		outOfArmorSince.reset();
		sweepStuck = false;
		if (!forfeited.empty()) {
			REX::INFO("FrameLoans: this save holds {} forfeited lent items; taken from our player if they have them", forfeited.size());
		}
		if (!record || record->copy == 0) {
			return;
		}
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(record->copy);
		const auto base = ref ? ref->GetObjectReference() : nullptr;
		const auto what = FrameLoan::ChooseAfterLoad(*record, ref && !ref->IsDeleted(), base ? base->GetFormID() : 0, ref && ref->IsCreated());
		REX::INFO("FrameLoans: this save was made in {:08X}, a frame borrowed from player {}: {}", record->copy, record->owner,
			what == FrameLoan::AfterLoad::kAll ? "our player is sent out of it, and it goes with the lent items" : what == FrameLoan::AfterLoad::kItems ? "it is gone; the lent items go" : "nothing to do");
		if (what != FrameLoan::AfterLoad::kNothing) {
			cleanup = Cleanup{ .record = std::move(*record), .what = what, .since = Clock::now(), .waitingOut = what == FrameLoan::AfterLoad::kAll };
		}
	}

	void Reset()
	{
		const Publisher publish;  // the gate's word (KeepOut) as this ends
		lends.clear();
		lentTo.clear();
		lent.clear();
		moves.clear();
		outgoing.clear();
		switch (borrowed.stage) {
		case Stage::kWorn:
		case Stage::kOut:
			if (!borrowed.ownerless) {
				borrowed.ownerless = true;
				REX::INFO("FrameLoans: the session ended while our player wears player {}'s frame {:08X}; it goes as they get out", borrowed.owner, borrowed.spawnId);
				Say(Text::Format("party.frame_owner_left", { { "player", RemotePlayers::NameOf(borrowed.owner) } }));
			}
			// The ids of the next session are not this one's: a new spawn under the id we hold would be taken for the frame lent
			// to us (LentToUs), its copy never placed or moved while we wear on, and every press on a frame copy called busy.
			// The copy and the tags are all Finish still needs.
			borrowed.spawnId = 0;
			borrowed.owner = 0;
			break;
		case Stage::kLent:
			Finish(false, true, "the session ended");  // before Spawns lets the copy go
			break;
		case Stage::kAsked:
			borrowed = {};
			break;
		case Stage::kNone:
			break;
		}
	}

	std::string Describe()
	{
		return std::format("loans: stage={} spawn={:08X} copy={:08X} tags={} held={} ownerless={} lentOut={} heard={} moves={} backs={} asked={} refused={} lent={} worn={} returned={} ended={} given={} unnamed={} forfeit={} forfeitsTaken={}{} ours: lent={} back={} worn={} kept={} moved={} cleaned={} stuckIn={}{}",
			static_cast<int>(borrowed.stage), borrowed.spawnId, borrowed.copy, borrowed.tags.size(), borrowed.held.size(), borrowed.ownerless, lent.size(), lentTo.size(), moves.size(), backs.size(), asked, refusedHere, lentHere, worn, returned, ownerlessEnds,
			piecesGiven, unnamedTaken, forfeited.size(), forfeitsTaken, sweepStuck ? " (stuck)" : "", lentOut, givenBack, piecesWorn, piecesKept, movedBack, cleanedUp, stuckIn, cleanup && cleanup->waitingOut ? " (sending out)" : "");
	}
}
