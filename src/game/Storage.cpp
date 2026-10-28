#include "game/Storage.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/LootScope.h"
#include "game/LootSync.h"
#include "game/OpenMenus.h"
#include "game/WorldSync.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
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

		struct Counts
		{
			std::uint32_t refusedOwn = 0;   // inventory changes OwnGear refused (must stay 0)
			std::uint32_t listFailed = 0;   // the host's listings put off (the ledger still asking)
			std::uint32_t unsentSeen = 0;   // our reports the session's numbering couldn't carry (Unsent)
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

		RE::TESObjectREFR* Ref(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			return ref && !ref->IsDeleted() ? ref : nullptr;
		}

		RE::TESObjectREFR* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		// Adds to (a_count > 0) or takes out of (< 0) an inventory OwnGear let us change, as one of Storage's own changes
		// (WorldSync doesn't report it); what was changed (a take takes no more than there is).
		std::int64_t Move(RE::TESObjectREFR* a_ref, std::uint32_t a_item, std::int64_t a_count)
		{
			const auto object = Object(a_item);
			if (!a_ref || !object || a_count == 0) {
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

		// Our copy of a container (never our player: OwnGear refuses).
		std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count)
		{
			const auto ref = Ref(a_container);
			if (!ref || !Equipment::MayChange(ref, OwnGear::Change::kContainer)) {
				++counts.refusedOwn;
				return 0;
			}
			return Move(ref, a_item, a_count);
		}

		// Our player's inventory: only ever for a PlayerChange, which only StorageView::Client makes (one of our own moves
		// put right).
		std::int64_t ChangePlayer(const StorageView::PlayerChange& a_change)
		{
			const auto player = Player();
			if (!player || !Equipment::MayChange(player, OwnGear::Change::kStorageUndo)) {
				++counts.refusedOwn;
				return 0;
			}
			return Move(player, a_change.Item(), a_change.Count());
		}

		std::string_view TextKey(StorageView::Notice a_notice)
		{
			switch (a_notice) {
			case StorageView::Notice::kClosed:
				return "storage.closed";
			case StorageView::Notice::kTakeRefused:
				return "storage.take_refused";
			case StorageView::Notice::kPutRefused:
				return "storage.put_refused";
			case StorageView::Notice::kShort:
				return "storage.short";
			default:
				return "storage.closed";
			}
		}

		class GameEngine final :
			public StorageView::Engine
		{
		public:
			std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_container) override { return WorldSync::SharedIn(a_container); }

			std::set<std::uint32_t> Aliased(std::uint32_t a_container) override { return WorldSync::AliasedIn(a_container); }

			std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count) override
			{
				return Storage::ChangeCopy(a_container, a_item, a_count);
			}

			std::int64_t ChangePlayer(const StorageView::PlayerChange& a_change) override { return Storage::ChangePlayer(a_change); }

			void FlushLoot() override { WorldSync::FlushLoot(); }

			bool Reported(const Protocol::ContainerChange& a_change) override { return WorldSync::Loot().Unreport(a_change); }

			void Send(std::vector<std::uint8_t> a_packet) override { WorldSync::QueueEvent(std::move(a_packet)); }

			void Notify(StorageView::Notice a_notice) override { Hud::Notify(Text::Get(TextKey(a_notice))); }

			void HideMenu() override
			{
				// Not before the UI may be written to: the menu then stays open on our own copy, and closes as the player closes it.
				const auto queue = RE::UIMessageQueue::GetSingleton();
				if (queue && Hud::IsSettled() && OpenMenus::Open(OpenMenus::Menu::kContainer)) {
					queue->AddMessage(RE::ContainerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
				}
			}

			Clock::time_point Now() override { return Clock::now(); }
		};

		GameEngine          engine;
		StorageView::Client client{ engine };  // main thread
		StorageView::Side&  side = client.State();
		std::atomic<bool>   ruleOn{ false };  // for the menu's entry points, which run on any thread
		bool                hosting = false;
		std::optional<std::uint32_t> menuOn;  // the container our menu is open on (main thread)
		std::uint64_t       live = 0;  // the session we are welcomed into (0: none)
		Clock::time_point   scannedAt{};
		std::unordered_map<std::uint32_t, Clock::time_point> notStorage;  // the host's scan: since when
		std::unordered_map<std::uint32_t, Clock::time_point> relistTried;  // listed before, not listable from afar: since when

		// A save's record, read before the game says it has loaded (OnGameLoaded).
		struct Record
		{
			std::uint64_t                          session = 0;
			std::vector<Protocol::ContainerChange> unanswered;
			std::uint32_t                          lacksFrom = 0;
			std::vector<std::uint32_t>             lacks;
		} loaded;

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
					++counts.listFailed;  // the ledger is still asking, or the container's inventory isn't made yet: next time
					continue;
				}
				WorldSync::QueueEvent(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
				side.MarkListed(id);
				++listed;
			}
			// What was listed before and the server holds stale now (our world loaded, or we rejoined) is listed again wherever it
			// is, so friends at a settlement we are not standing in are not shut out of its chests until we walk back. One our game
			// doesn't hold in memory (or whose inventory isn't made) stays stale until we come near, tried again after a while
			// (apart from notStorage: the radius pass lists it as soon as we are near). None while the ledger still asks: nothing
			// can be listed until its catch-up has come, and that is no reason to wait.
			std::erase_if(relistTried, [&](const auto& a_entry) { return now - a_entry.second >= NOT_STORAGE_FOR; });
			const bool asking = WorldSync::Loot().Numbers().Numbers().Asking();
			for (const auto id : asking ? std::vector<std::uint32_t>{} : side.ToRelist()) {
				if (listed >= LIST_PER_SCAN) {
					break;
				}
				if (notStorage.contains(id) || relistTried.contains(id) || std::ranges::find(found, id) != found.end()) {
					continue;
				}
				const auto facts = Ref(id) ? WorldSync::HolderFactsOf(id) : LootScope::HolderFacts{};
				const auto listing = facts.storage && !facts.workshop && !facts.vendorChest ? WorldSync::StorageListing(id) : std::nullopt;
				if (!listing) {
					relistTried[id] = now;
					continue;
				}
				WorldSync::QueueEvent(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
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
			default:
				return "not shared storage";
			}
		}

		template <class T>
		void Put(std::vector<std::uint8_t>& a_out, T a_value)
		{
			const auto at = a_out.size();
			a_out.resize(at + sizeof(T));
			std::memcpy(a_out.data() + at, &a_value, sizeof(T));
		}

		template <class T>
		bool Get(std::span<const std::uint8_t>& a_in, T& a_value)
		{
			if (a_in.size() < sizeof(T)) {
				return false;
			}
			std::memcpy(&a_value, a_in.data(), sizeof(T));
			a_in = a_in.subspan(sizeof(T));
			return true;
		}
	}

	void OnWelcome(bool a_rule, bool a_hosting, std::uint64_t a_session)
	{
		hosting = a_hosting;
		live = a_session;
		client.OnWelcome(a_rule, a_hosting, a_session);
		ruleOn = a_rule;
		notStorage.clear();
		relistTried.clear();
		scannedAt = {};
		REX::INFO("Storage: shared storage {}{} ({} of our moves at a friend's storage still to be answered)", a_rule ? "on" : "off",
			a_rule && a_hosting ? " (our game keeps the true copies)" : "", side.InFlight());
	}

	void OnDisconnected()
	{
		const auto unanswered = side.InFlight();
		client.OnDisconnected();  // nobody's truth now: the session's things leave our copy; what we sent unanswered stays on its way
		ruleOn = false;
		live = 0;
		if (unanswered != 0) {
			REX::INFO("Storage: {} of our moves at a friend's storage had no answer as the connection went: kept, and settled when we rejoin (sent late if the session never numbered them)", unanswered);
		}
	}

	void OnOwners(const Protocol::StorageOwners& a_owners)
	{
		client.OnOwners(a_owners, menuOn, [](std::uint32_t a_id) { return WorldSync::Loot().Ours(a_id); });
	}

	void OnContents(const Protocol::StorageListing& a_contents)
	{
		client.OnContents(a_contents);
	}

	void OnRefused(const Protocol::StorageRefusal& a_refusal)
	{
		client.OnRefused(a_refusal);
	}

	void OnCaughtUp(const Protocol::StorageCaughtUp& a_caughtUp)
	{
		const auto& loot = WorldSync::Loot();
		const auto  settles = side.Stats().settles;
		const auto  lateBefore = side.Stats().late;
		client.OnCaughtUp(a_caughtUp.asked, loot.Asked(), loot.Numbers().Numbers().Next(), loot.Numbers().Numbers().Waiting() != 0);
		if (side.Stats().settles != settles) {
			REX::INFO("Storage: caught up (request {}): settled our moves at a friend's storage ({} our world lacks, {} still unnumbered sent late)", a_caughtUp.asked,
				side.Lacks(), side.Stats().late - lateBefore);
		}
	}

	void BeforeSaveOrLoad()
	{
		client.BeforeSaveOrLoad();
	}

	void OnGameLoaded()
	{
		// Connected, our world is the session's from here (a save from another adopts it): the save's unanswered moves count
		// only when they were made in it. Alone, they wait for a welcome into the save's session.
		auto       record = std::exchange(loaded, {});
		const auto session = live != 0 ? live : record.session;
		if (record.session != session) {
			record.unanswered.clear();
			record.lacks.clear();
			record.lacksFrom = 0;
		}
		client.OnLoad(session, record.unanswered, record.lacks, record.lacksFrom);
		notStorage.clear();
		relistTried.clear();
		if (!record.unanswered.empty() || !record.lacks.empty()) {
			REX::INFO("Storage: the save was made with {} of our moves at a friend's storage unanswered, and its world lacking {} of them numbered; they are settled once the catch-up has come",
				record.unanswered.size(), record.lacks.size());
		}
	}

	void Frame()
	{
		client.Frame();
		// A catch-up that left the ledger waiting settles once the wait is over (a gap given up on asks for nothing more).
		const auto& loot = WorldSync::Loot();
		if (client.RetryCaughtUp(loot.Asked(), loot.Numbers().Numbers().Next(), loot.Numbers().Numbers().Waiting() != 0)) {
			REX::INFO("Storage: settled our moves at a friend's storage once the changes we waited for were given up on ({} our world lacks)", side.Lacks());
		}
		Scan();
	}

	std::vector<std::uint8_t> SaveRecord()
	{
		std::vector<std::uint8_t> out;
		const auto                unanswered = side.Unanswered();
		const auto                lacks = side.Lacking();
		if ((unanswered.empty() && lacks.empty() && side.LacksFrom() == 0) || side.MovesSession() == 0) {
			return out;
		}
		Put(out, side.MovesSession());
		Put(out, static_cast<std::uint32_t>(unanswered.size()));
		for (const auto& change : unanswered) {
			Put(out, change.container);
			Put(out, change.item);
			Put(out, change.count);
		}
		Put(out, side.LacksFrom());
		Put(out, static_cast<std::uint32_t>(lacks.size()));
		for (const auto index : lacks) {
			Put(out, index);
		}
		return out;
	}

	void LoadRecord(std::span<const std::uint8_t> a_bytes)
	{
		loaded = {};
		std::uint64_t session = 0;
		std::uint32_t count = 0;
		if (!Get(a_bytes, session) || !Get(a_bytes, count) || count > StorageView::MAX_IN_FLIGHT || a_bytes.size() < count * 12ull + 8) {
			REX::WARN("Storage: ignoring an unreadable record of our unanswered moves");
			return;
		}
		Record record{ .session = session };
		for (std::uint32_t i = 0; i < count; ++i) {
			Protocol::ContainerChange change{};
			Get(a_bytes, change.container);
			Get(a_bytes, change.item);
			Get(a_bytes, change.count);
			if (Protocol::IsValid(change)) {
				record.unanswered.push_back(change);
			}
		}
		std::uint32_t lacks = 0;
		if (!Get(a_bytes, record.lacksFrom) || !Get(a_bytes, lacks) || lacks > Protocol::MAX_STORAGE_LACKING || a_bytes.size() != lacks * 4ull) {
			REX::WARN("Storage: ignoring an unreadable record of our unanswered moves");
			return;
		}
		for (std::uint32_t i = 0; i < lacks; ++i) {
			std::uint32_t index = 0;
			Get(a_bytes, index);
			record.lacks.push_back(index);
		}
		loaded = std::move(record);
	}

	void RevertRecord()
	{
		loaded = {};
	}

	void Unsent(std::span<const std::uint8_t> a_packet)
	{
		if (a_packet.empty() || a_packet[0] != static_cast<std::uint8_t>(Protocol::MessageType::kReportContainer)) {
			return;
		}
		if (const auto change = Protocol::DecodeContainerChange(a_packet)) {
			++counts.unsentSeen;
			client.Unsent(*change);
		}
	}

	void OnMenuShown(RE::ObjectRefHandle a_container)
	{
		// The container our menu is open on is remembered with the rule off too: it may come on (or the owners arrive) with the menu open.
		MainThread::Post([a_container] {
			if (const auto ref = a_container.get()) {
				const auto id = ref->GetFormID();
				menuOn = id;
				if (ruleOn) {
					client.Open(id, KeeperOf(id) == Keeper::kFriend);
				}
			}
		});
	}

	void OnMenuClosed()
	{
		MainThread::Post([] {
			menuOn.reset();
			if (ruleOn) {  // nothing more to do without the rule (a friend's storage is never viewed)
				client.MenuClosed();
			}
		});
	}

	Keeper KeeperOf(std::uint32_t a_container)
	{
		return side.KeeperOf(a_container, [](std::uint32_t a_id) { return WorldSync::Loot().Ours(a_id); });
	}

	std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container)
	{
		return side.ViewFrom(a_container);
	}

	std::uint32_t KeptSince(std::uint32_t a_container)
	{
		return side.KeptSince(a_container);
	}

	bool Aliased(std::uint32_t a_container, std::uint32_t a_item)
	{
		return side.Aliased(a_container, a_item);
	}

	bool Reporting(const Protocol::ContainerChange& a_change)
	{
		return client.Reporting(a_change);
	}

	void OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking)
	{
		client.OwnStorage(a_change, a_lacking);
	}

	void Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing)
	{
		WorldSync::QueueEvent(Protocol::Encode(Protocol::StorageShort{ a_index, a_missing }));
		side.Unlist(a_change.container);  // listed again, as it is now
		side.NoteShort();
		REX::INFO("Storage: change {} took {} of {:08X} from {:08X}, but this world's copy of it holds {} fewer than that (a world loaded since, or an item this game doesn't share); "
				  "the taker's game is asked to give those back",
			a_index, -a_change.count, a_change.item, a_change.container, a_missing);
	}

	bool Applying()
	{
		return applying;
	}

	std::string Describe()
	{
		const auto& s = side.Stats();
		const auto& c = client.Stats();
		return std::format("storage={}{} owners={} viewing={} inFlight={} settling={} lacks={} setAside={} views={} shown={} refusedTakes={} refusedPuts={} shortTakes={} "
						   "unanswered={} unsent={} overflow={} lacking={} lacksDropped={} settles={} late={} settlingClosed={} corrected={} takenBack={} usedUp={} "
						   "givenBack={} closedByHost={} unmatched={} listed={} shorts={} listPutOff={} ownRefused={}",
			side.Rule() ? "on" : "off", hosting ? "(host)" : "", side.OwnerCount(), side.Viewing() ? std::format("{:08X}", *side.Viewing()) : "-", side.InFlight(),
			side.Settling() ? 1 : 0, side.Lacks(), c.setAside, s.views, c.shown, s.refusedTakes, s.refusedPuts, s.shortTakes, s.unanswered, s.unsent, s.overflow, s.lacking,
			s.lacksDropped, s.settles, s.late, s.settlingClosed, s.corrected, c.takenBack, c.usedUp, c.givenBack, s.closedByHost, s.unmatched, s.listed, s.shorts, counts.listFailed,
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
			client.MenuClosed();
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
			client.Open(id, KeeperOf(id) == Keeper::kFriend);
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
