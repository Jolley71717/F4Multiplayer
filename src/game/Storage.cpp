#include "game/Storage.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/LootScope.h"
#include "game/LootSync.h"
#include "game/WorldSync.h"

#include <charconv>
#include <chrono>
#include <format>
#include <unordered_map>
#include <vector>

namespace Storage
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using StorageView::Keeper;

		// The host's scan: how often, how far from our player, and how many it lists at most each time (a settlement's
		// many boxes go over a few scans rather than in one frame).
		constexpr auto  SCAN_INTERVAL = std::chrono::seconds{ 2 };
		constexpr float SCAN_RADIUS = 8192.0f;
		constexpr int   LIST_PER_SCAN = 16;
		// A container the scan found not to be storage is looked at again after this (a settlement taken, a home bought).
		constexpr auto  NOT_STORAGE_FOR = std::chrono::seconds{ 30 };

		StorageView::Side side;  // main thread
		bool              hosting = false;
		Clock::time_point scannedAt{};
		std::unordered_map<std::uint32_t, Clock::time_point> notStorage;  // the host's scan: since when

		struct Counts
		{
			std::uint32_t stashed = 0;      // stacks of our copy moved to our player as a view opened
			std::uint32_t shown = 0;        // contents shown in our copy
			std::uint32_t adjusts = 0;      // stacks our copy was changed by to show them
			std::uint32_t givenBack = 0;    // refused or short takes taken back out of our player
			std::int64_t  usedUp = 0;       // of those, items our player no longer had
			std::uint32_t returned = 0;     // refused puts given back to our player
			std::uint32_t refusedOwn = 0;   // our player changes OwnGear refused (must stay 0)
			std::uint32_t listFailed = 0;   // the host's listings put off (the ledger still asking)
		} counts;

		thread_local bool applying = false;

		struct Guard
		{
			Guard() { applying = true; }
			~Guard() { applying = false; }
			Guard(const Guard&) = delete;
			Guard& operator=(const Guard&) = delete;
		};

		RE::TESBoundObject* Object(std::uint32_t a_item)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			return form ? form->As<RE::TESBoundObject>() : nullptr;
		}

		std::int64_t CountIn(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_object)
		{
			return a_ref && a_object ? static_cast<std::int64_t>(a_ref->GetInventoryObjectCount(a_object)) : 0;
		}

		// Adds to (a_count > 0) or takes out of (< 0) an inventory, as one of Storage's own changes; what was changed (a take
		// takes no more than there is). a_change: OwnGear's for that reference (our player: kStorageUndo only).
		std::int64_t Change(RE::TESObjectREFR* a_ref, std::uint32_t a_item, std::int64_t a_count, OwnGear::Change a_change)
		{
			const auto object = Object(a_item);
			if (!a_ref || !object || a_count == 0) {
				return 0;
			}
			if (!Equipment::MayChange(a_ref, a_change)) {
				++counts.refusedOwn;
				return 0;
			}
			Guard guard;
			if (a_count > 0) {
				const auto n = static_cast<std::int32_t>((std::min<std::int64_t>)(a_count, Protocol::MAX_ITEM_COUNT));
				a_ref->AddObjectToContainer(object, nullptr, n, nullptr, RE::ITEM_REMOVE_REASON::kNone);
				return n;
			}
			const auto n = static_cast<std::int32_t>((std::min)(-a_count, CountIn(a_ref, object)));
			if (n <= 0) {
				return 0;
			}
			RE::TESObjectREFR::RemoveItemData data{ object, n };
			a_ref->RemoveItem(data);
			return -n;
		}

		RE::TESObjectREFR* Ref(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			return ref && !ref->IsDeleted() ? ref : nullptr;
		}

		RE::TESObjectREFR* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		void Send(std::vector<std::uint8_t> a_packet)
		{
			WorldSync::QueueEvent(std::move(a_packet));
		}

		// The session's things leave our copy of the container we viewed (the menu closed, a save, the session's end).
		void EndView(bool a_tellServer)
		{
			const auto container = side.Close();
			if (!container) {
				return;
			}
			if (const auto ref = Ref(*container)) {
				for (const auto& [item, count] : WorldSync::SharedIn(*container)) {
					Change(ref, item, -count, OwnGear::Change::kContainer);
				}
			}
			if (a_tellServer) {
				Send(Protocol::Encode(Protocol::StorageView{ *container, false }));
			}
		}

		// Our menu shows a_container: a friend's storage is viewed.
		void Open(std::uint32_t a_container)
		{
			if (!side.Rule() || KeeperOf(a_container) != Keeper::kFriend) {
				return;
			}
			// Our moves before it (at another container, or this one before it was a friend's) go first, as theirs.
			WorldSync::FlushLoot();
			if (side.Viewing() && *side.Viewing() != a_container) {
				EndView(true);
			}
			if (!side.Open(a_container)) {
				return;
			}
			const auto ref = Ref(a_container);
			const auto player = Player();
			if (ref && player) {
				// What our copy held of the session's things (our own stash from before, a roll of loot) is our player's now.
				for (const auto& [item, count] : WorldSync::SharedIn(a_container)) {
					const auto taken = -Change(ref, item, -count, OwnGear::Change::kContainer);
					if (taken > 0) {
						Change(player, item, taken, OwnGear::Change::kStorageUndo);
						++counts.stashed;
					}
				}
			}
			Send(Protocol::Encode(Protocol::StorageView{ a_container, true }));
		}

		void Close()
		{
			// The moves made in the menu go first: they were made while it showed the session's things.
			WorldSync::FlushLoot();
			EndView(true);
		}

		void Scan()
		{
			const auto now = Clock::now();
			if (!hosting || !side.Rule() || now - scannedAt < SCAN_INTERVAL || !WorldSync::InWorld()) {
				return;
			}
			scannedAt = now;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return;
			}
			std::erase_if(notStorage, [&](const auto& a_entry) { return now - a_entry.second >= NOT_STORAGE_FOR; });
			std::vector<std::uint32_t> found;
			tes->ForEachReferenceInRange(player->data.location, SCAN_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (!base || !base->Is(RE::ENUM_FORM_ID::kCONT)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto id = a_ref->GetFormID();
				if (Protocol::IsShareableRef(id) && !side.Listed(id) && !notStorage.contains(id)) {
					found.push_back(id);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			int listed = 0;
			for (const auto id : found) {
				if (listed >= LIST_PER_SCAN) {
					break;
				}
				const auto facts = WorldSync::HolderFactsOf(id);
				if (!facts.storage || facts.workshop || facts.vendorChest) {
					notStorage[id] = now;
					continue;
				}
				const auto listing = WorldSync::StorageListing(id);
				if (!listing) {
					++counts.listFailed;  // the ledger is still asking: next time
					return;
				}
				Send(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
				side.MarkListed(id);
				++listed;
			}
		}

		std::uint32_t ParseHex(std::string_view a_text)
		{
			std::uint32_t value = 0;
			std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			return value;
		}

		std::string_view Name(Keeper a_keeper)
		{
			switch (a_keeper) {
			case Keeper::kUs:
				return "ours";
			case Keeper::kFriend:
				return "a friend's";
			case Keeper::kLocal:
				return "each player's own";
			default:
				return "not shared storage";
			}
		}
	}

	void OnWelcome(bool a_rule, bool a_hosting)
	{
		hosting = a_hosting;
		side.OnWelcome(a_rule, a_hosting);
		notStorage.clear();
		scannedAt = {};
		REX::INFO("Storage: shared storage {}{}", a_rule ? "on" : "off", a_rule && a_hosting ? " (our game keeps the true copies)" : "");
	}

	void OnDisconnected()
	{
		EndView(false);  // nobody's truth now: the session's things leave our copy
		side.OnDisconnected();
	}

	void OnOwners(const Protocol::StorageOwners& a_owners)
	{
		side.Owners(a_owners);
	}

	void OnContents(const Protocol::StorageListing& a_contents)
	{
		const auto want = side.Contents(a_contents);
		const auto ref = want ? Ref(a_contents.container) : nullptr;
		if (!ref) {
			return;
		}
		++counts.shown;
		for (const auto& adjust : StorageView::Reconcile(*want, WorldSync::SharedIn(a_contents.container))) {
			Change(ref, adjust.item, adjust.count, OwnGear::Change::kContainer);
			++counts.adjusts;
		}
	}

	void OnRefused(const Protocol::StorageRefusal& a_refusal)
	{
		const auto undo = side.Refused(a_refusal);
		if (undo.closeView) {
			// The server closed it already. The menu stays open on our own copy: what our player does there now stays in our game
			// (LootSync keeps a move at a friend's storage made without a view), and the next open asks again.
			EndView(false);
			Hud::Notify(Text::Get("storage.closed"));
			return;
		}
		if (!undo.matched) {
			return;
		}
		if (const auto player = Player(); player && undo.player != 0) {
			const auto done = Change(player, undo.item, undo.player, OwnGear::Change::kStorageUndo);
			if (undo.player < 0) {
				++counts.givenBack;
				counts.usedUp += done - undo.player;  // what our player no longer had (used, dropped)
			} else {
				++counts.returned;
			}
		}
		if (undo.copy != 0) {
			Change(Ref(undo.container), undo.item, undo.copy, OwnGear::Change::kContainer);
		}
		if (undo.refresh && side.Refresh(undo.container)) {
			Send(Protocol::Encode(Protocol::StorageView{ undo.container, true }));
		}
		Hud::Notify(Text::Get(a_refusal.reason == Protocol::StorageReason::kShort ? "storage.short" :
		                      undo.player < 0                                     ? "storage.take_refused" :
		                                                                            "storage.put_refused"));
	}

	void BeforeSaveOrLoad()
	{
		EndView(true);
	}

	void OnGameLoaded()
	{
		side.OnLoad();
		notStorage.clear();
	}

	void Frame()
	{
		Scan();
	}

	void OnMenuShown(RE::ObjectRefHandle a_container)
	{
		MainThread::Post([a_container] {
			if (const auto ref = a_container.get()) {
				Open(ref->GetFormID());
			}
		});
	}

	void OnMenuClosed()
	{
		MainThread::Post([] { Close(); });
	}

	Keeper KeeperOf(std::uint32_t a_container)
	{
		if (!side.Rule()) {
			return Keeper::kNone;
		}
		const bool storageHere = !side.OwnerOf(a_container) && !hosting && WorldSync::HolderFactsOf(a_container).storage;
		return side.KeeperOf(a_container, storageHere, [](std::uint32_t a_id) { return WorldSync::Loot().Ours(a_id); });
	}

	std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container)
	{
		return side.ViewFrom(a_container);
	}

	void Reporting(const Protocol::ContainerChange& a_change)
	{
		side.Reported(a_change);
	}

	void Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing)
	{
		Send(Protocol::Encode(Protocol::StorageShort{ a_index, a_missing }));
		side.Unlist(a_change.container);  // listed again, as it is now
		side.NoteShort();
		REX::INFO("Storage: change {} took {} of {:08X} from {:08X}, which held {} fewer here", a_index, -a_change.count, a_change.item, a_change.container, a_missing);
	}

	void OwnNumbered(const Protocol::IndexedContainerChange& a_change)
	{
		if (side.Rule() && WorldSync::Loot().Ours(a_change.playerId)) {
			side.OwnNumbered(a_change);
		}
	}

	void UnlistAll()
	{
		side.UnlistAll();
	}

	bool Applying()
	{
		return applying;
	}

	std::string Describe()
	{
		const auto& s = side.Stats();
		return std::format("storage={}{} owners={} viewing={} inFlight={} views={} shown={} stashed={} refusedTakes={} refusedPuts={} shortTakes={} usedUp={} closedByHost={} unmatched={} listed={} shorts={} listPutOff={} ownRefused={}",
			side.Rule() ? "on" : "off", hosting ? "(host)" : "", side.OwnerCount(), side.Viewing() ? std::format("{:08X}", *side.Viewing()) : "-", side.InFlight(), s.views,
			counts.shown, counts.stashed, s.refusedTakes, s.refusedPuts, s.shortTakes, counts.usedUp, s.closedByHost, s.unmatched, s.listed, s.shorts, counts.listFailed,
			counts.refusedOwn);
	}

	std::string Command(std::string_view a_args)
	{
		std::vector<std::string_view> args;
		for (std::size_t at = 0; at < a_args.size();) {
			const auto end = a_args.find(' ', at);
			const auto word = a_args.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
			if (!word.empty()) {
				args.push_back(word);
			}
			if (end == std::string_view::npos) {
				break;
			}
			at = end + 1;
		}
		if (args.empty() || args[0] == "status") {
			return Describe();
		}
		if (args[0] == "owners") {
			return std::format("{} containers kept in the session", side.OwnerCount());
		}
		if (args[0] == "close") {
			Close();
			return "closed";
		}
		if (args.size() < 2) {
			return "error: storage [status | owners | keeper <ref> | view <ref> | close | count <ref> <item> | take <ref> <item> <count> | put <ref> <item> <count>]";
		}
		const auto id = ParseHex(args[1]);
		const auto ref = Ref(id);
		if (!ref) {
			return std::format("error: no reference {:08X}", id);
		}
		if (args[0] == "keeper") {
			const auto owner = side.OwnerOf(id);
			return std::format("{:08X}: {} (owner {}) storage={} viewFrom={}", id, Name(KeeperOf(id)), owner ? std::to_string(*owner) : "none",
				WorldSync::HolderFactsOf(id).storage, side.ViewFrom(id) ? std::to_string(*side.ViewFrom(id)) : "-");
		}
		if (args[0] == "view") {
			Open(id);
			return side.Viewing() == id ? "viewing" : std::format("not viewed: {}", Name(KeeperOf(id)));
		}
		if (args.size() < 3) {
			return "error: missing item";
		}
		const auto item = ParseHex(args[2]);
		const auto object = Object(item);
		if (!object) {
			return std::format("error: no item {:08X}", item);
		}
		const auto player = Player();
		if (args[0] == "count") {
			return std::format("{:08X} holds {} of {:08X}; our player {}", id, CountIn(ref, object), item, CountIn(player, object));
		}
		std::int32_t count = 1;
		if (args.size() > 3) {
			std::from_chars(args[3].data(), args[3].data() + args[3].size(), count);
		}
		if (count <= 0 || !player) {
			return "error: count";
		}
		// A transfer between our player and the container, as the menu makes one (its event is WorldSync's to report).
		if (args[0] == "take") {
			RE::TESObjectREFR::RemoveItemData data{ object, (std::min)(count, static_cast<std::int32_t>(CountIn(ref, object))) };
			data.otherContainer = player;
			if (data.count > 0) {
				ref->RemoveItem(data);
			}
			return std::format("took {}", data.count);
		}
		if (args[0] == "put") {
			RE::TESObjectREFR::RemoveItemData data{ object, (std::min)(count, static_cast<std::int32_t>(CountIn(player, object))) };
			data.otherContainer = ref;
			if (data.count > 0) {
				player->RemoveItem(data);
			}
			return std::format("put {}", data.count);
		}
		return "error: unknown storage command";
	}
}
