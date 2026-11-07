#include "game/Storage.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/LootScope.h"
#include "game/LootSync.h"
#include "game/OpenMenus.h"
#include "game/SupplyLines.h"
#include "game/TextureFlush.h"
#include "game/WorkshopSync.h"
#include "game/WorldSync.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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

		// ---- the host's workbench (Storage.h) ----

		// What reads a workbench straight: workshop mode, or a crafting menu.
		enum class BenchMenu : std::uint8_t
		{
			kNone,
			kWorkshop,
			kCrafting,
		};

		// The crafting menus: each a WorkbenchMenuBase, whose gather reads a settlement's workbench. The robot workbench's,
		// Automatron's RobotModMenu, is an ExamineMenu (RTTI: RobotModMenu : ExamineMenu : WorkbenchMenuBase) with the same
		// gather.
		constexpr std::array<OpenMenus::Menu, 4> CRAFTING_MENUS{ OpenMenus::Menu::kExamine, OpenMenus::Menu::kCooking, OpenMenus::Menu::kPowerArmorMod,
			OpenMenus::Menu::kRobotMod };
		// A crafting menu's workbench is looked for this long after it opens (its gather may not have run yet).
		constexpr auto CRAFTING_LOOK_FOR = std::chrono::seconds{ 2 };
		// A view of the host's workbench that couldn't open (our moves not settled yet, the host's copy stale) is tried again
		// after this, doubled each time it fails, up to BENCH_RETRY_MAX.
		constexpr auto BENCH_RETRY = std::chrono::seconds{ 5 };
		constexpr auto BENCH_RETRY_MAX = std::chrono::seconds{ 60 };

		struct Bench
		{
			BenchMenu         menu = BenchMenu::kNone;
			std::uint32_t     id = 0;  // the workbench it reads (0: none found, or none)
			// The workbenches the host's supply lines link to it whose views show the true copy with it (SupplyLines.h), in
			// the host's order, and which of them to view when (a few, one at a time, each backing off when refused).
			std::vector<std::uint32_t> supply;
			SupplyLines::Views         supplyViews;
			Clock::time_point openedAt{};
			Clock::time_point retryAt{};
			Clock::duration   backoff = BENCH_RETRY;
			bool              refresh = false;  // what our copy holds changed: the engine's own container is filled again
			// The host's game: the other kept workbenches a friend changed since the last frame. Those our supply lines link
			// to this one are in its list too, far or near, so it is filled again (SupplyLines::HostListChanged).
			std::vector<std::uint32_t> hostChanged;
		} bench;  // main thread

		std::atomic<bool> hostingOn{ false };  // the host's game, with the rule on (KeepsTrueCopies)
		std::mutex                        benchesLock;
		std::unordered_set<std::uint32_t> benches;  // workbenches kept for the session (WatchesBench)
		std::uint32_t                     listingNow = 0;  // the container our game is listing (SharedBench), main thread

		thread_local int scripted = 0;  // inside a script's AddItem or RemoveItem

		// ---- the host's supply lines (SupplyLines.h) ----

		// The host's game tells the session which workbenches its supply lines link to each it keeps, this often at most.
		constexpr auto        LINKS_INTERVAL = std::chrono::seconds{ 10 };
		// 1.11.240: the calls to Workshop::AddOtherContainers (2195029, +38A140) the engine's gathers make: in
		// Workshop::CollectLinkedWorkshopContainers (workshop mode's list, a build's takes, the settlement's resource counts)
		// and in the crafting menus' gather (2224943, +B28540; CraftingWorkshop.h), each `E8` to it.
		constexpr REL::ID     ADD_OTHER_CONTAINERS{ 2195029 };
		constexpr REL::ID     COLLECT_LINKED{ 2195116 };  // Workshop::CollectLinkedWorkshopContainers (+392130)
		constexpr std::size_t COLLECT_LINKED_CALL = 0x178;
		constexpr REL::ID     CRAFTING_GATHER{ 2224943 };
		constexpr std::size_t CRAFTING_GATHER_CALL = 0x426;
		constexpr REL::ID     FIND_LOCATION_WITH_KEYWORD{ 2199627 };  // BGSLocation::FindLocationWithKeyword(BGSKeyword*) (+49D730)
		constexpr REL::ID     SETTLEMENT_KEYWORD{ 4797293 };          // the keyword the engine finds a workshop's settlement location by (Workshop::FindLocation)
		constexpr std::size_t CALL_STUB = 14;                         // what a write_call<5> takes of the trampoline (both calls share it)

		using ContainerList = RE::BSScrapArray<RE::NiPointer<RE::TESObjectREFR>>;
		using AddOthersFn = void (*)(ContainerList&, RE::BGSLocation*, bool);
		AddOthersFn originalAddOthers = nullptr;  // set once the calls are hooked

		SupplyLines::Reporter reporter;   // the host's game: main thread
		SupplyLines::Heard    heard;      // a friend's game: main thread
		Clock::time_point     linksAt{};  // the host's game's last look at its supply lines

		// What the engine's gather is to find for the settlement our viewed workbench is in: the workbenches the host's supply
		// lines link to it. Set on the main thread, read by the gather on whichever thread runs it.
		std::mutex                 gatherLock;
		RE::BGSLocation*           gatherAt = nullptr;
		std::vector<std::uint32_t> gatherSupply;
		std::atomic<std::uint32_t> gathered{ 0 };  // gathers that found the host's supply lines

		// The settlement location a workbench is in, as the engine finds a workshop's (Workshop::FindLocation: its current
		// location's ancestor with the settlement keyword).
		RE::BGSLocation* SettlementOf(const RE::TESObjectREFR* a_ref)
		{
			static REL::Relocation<RE::BGSKeyword**>                                    keyword{ SETTLEMENT_KEYWORD };
			static REL::Relocation<RE::BGSLocation* (*)(RE::BGSLocation*, RE::BGSKeyword*)> find{ FIND_LOCATION_WITH_KEYWORD };
			const auto                                                                  location = a_ref ? a_ref->GetCurrentLocation() : nullptr;
			return location && *keyword ? find(location, *keyword) : nullptr;
		}

		void SetGather(RE::BGSLocation* a_at, std::vector<std::uint32_t> a_supply)
		{
			std::scoped_lock l{ gatherLock };
			gatherAt = a_supply.empty() ? nullptr : a_at;
			gatherSupply = std::move(a_supply);
		}

		struct LocalMove
		{
			std::uint32_t container = 0;
			std::uint32_t item = 0;
			std::int32_t  count = 0;
		};
		constexpr std::size_t  MAX_LOCAL = 1024;
		std::mutex             localLock;
		std::vector<LocalMove> localInbox;
		std::atomic<bool>      tracing{ false };  // the dev channel's `storage trace on`

		void NoteBench(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (ref && WorkshopSync::IsWorkbenchAnchor(ref)) {
				std::scoped_lock l{ benchesLock };
				benches.insert(a_id);
			}
		}

		void ForgetBenches()
		{
			std::scoped_lock l{ benchesLock };
			benches.clear();
		}

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

		// Whether workshop mode or the crafting menu open reads a_container: the workbench, or one the host's supply lines link
		// to it that we view with it.
		bool ReadByBench(std::uint32_t a_container)
		{
			return bench.menu != BenchMenu::kNone && a_container != 0 && (a_container == bench.id || std::ranges::contains(bench.supply, a_container));
		}

		// Our copy of a container (never our player: OwnGear refuses).
		std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count)
		{
			const auto ref = Ref(a_container);
			if (!ref || !Equipment::MayChange(ref, OwnGear::Change::kContainer)) {
				++counts.refusedOwn;
				return 0;
			}
			const auto done = Move(ref, a_item, a_count);
			if (done != 0 && ReadByBench(a_container)) {
				bench.refresh = true;
			}
			return done;
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
			case StorageView::Notice::kSettleCut:
				return "storage.settle_cut";
			default:
				return "storage.closed";
			}
		}

		void DrainLocal();
		void EndClosedBenchView();
		bool BenchShown();

		class GameEngine final :
			public StorageView::Engine
		{
		public:
			std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_container) override { return WorldSync::SharedIn(a_container); }

			std::set<std::uint32_t> Aliased(std::uint32_t a_container) override { return WorldSync::AliasedIn(a_container); }

			bool SharedItem(std::uint32_t a_container, std::uint32_t a_item) override { return WorldSync::Shares(a_container, a_item); }

			bool PaysFromStash(std::uint32_t a_container) override { return ReadByBench(a_container); }

			std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count) override
			{
				return Storage::ChangeCopy(a_container, a_item, a_count);
			}

			std::int64_t ChangePlayer(const StorageView::PlayerChange& a_change) override { return Storage::ChangePlayer(a_change); }

			void FlushLoot() override
			{
				WorldSync::FlushLoot();
				DrainLocal();  // our scripts' changes at a viewed workbench go to what was set aside, before what the view does next
			}

			bool Reported(const Protocol::ContainerChange& a_change) override { return WorldSync::Loot().Unreport(a_change); }

			void Send(std::vector<std::uint8_t> a_packet) override { WorldSync::QueueEvent(std::move(a_packet)); }

			void Notify(StorageView::Notice a_notice) override
			{
				const bool atBench = a_notice == StorageView::Notice::kClosed && bench.menu != BenchMenu::kNone;
				if (atBench && BenchShown()) {
					return;  // a supply line's workbench's view closed, not the workbench's: it is tried again quietly (OpenSupply)
				}
				Hud::Notify(Text::Get(atBench ? "storage.bench_closed" : TextKey(a_notice)));
			}

			void HideMenu() override
			{
				// Workshop mode and a crafting menu are never closed for it (only the workshop's own exit ends workshop mode:
				// MenuMessages.h): the view ends instead, and they read our copy's own things again.
				if (bench.menu != BenchMenu::kNone) {
					MainThread::Post([] { EndClosedBenchView(); });
					return;
				}
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

		// ---- the host's workbench ----

		// The workbenches our own world's supply lines reach from a_ref's settlement, as the engine's gather finds them: its
		// own AddOtherContainers, called past our hook, the player left out.
		std::vector<std::uint32_t> SupplyOf(RE::TESObjectREFR* a_ref)
		{
			static REL::Relocation<AddOthersFn> addOthers{ ADD_OTHER_CONTAINERS };
			std::vector<std::uint32_t>          out;
			const auto                          at = SettlementOf(a_ref);
			if (!at) {
				return out;
			}
			ContainerList found;
			addOthers(found, at, true);
			for (const auto& container : found) {
				if (container && container.get() != a_ref) {
					out.push_back(container->GetFormID());
				}
			}
			return out;
		}

		// Workshop::AddOtherContainers, as the engine's gathers call it: for the settlement of the host's workbench we view, the
		// workbenches the host's supply lines link to it come first, in the host's order, then our own world's others, then
		// (unless the caller left it out) our player, as the engine's last (SupplyLines::Gathered). Anywhere else, the
		// engine's own.
		void GatherHook(ContainerList& a_list, RE::BGSLocation* a_location, bool a_noPlayer)
		{
			std::vector<std::uint32_t> keepers;
			{
				std::scoped_lock l{ gatherLock };
				if (a_location && a_location == gatherAt) {
					keepers = gatherSupply;
				}
			}
			if (keepers.empty()) {
				originalAddOthers(a_list, a_location, a_noPlayer);
				return;
			}
			std::vector<std::uint32_t> have;
			for (const auto& container : a_list) {
				have.push_back(container ? container->GetFormID() : 0);
			}
			const auto before = a_list.size();
			originalAddOthers(a_list, a_location, true);
			std::vector<RE::NiPointer<RE::TESObjectREFR>> added;
			std::vector<std::uint32_t>                    own;
			for (auto i = before; i < a_list.size(); ++i) {
				added.push_back(a_list[i]);
				own.push_back(a_list[i] ? a_list[i]->GetFormID() : 0);
			}
			a_list.erase(a_list.begin() + before, a_list.end());
			for (const auto id : SupplyLines::Gathered(have, own, keepers)) {
				const auto it = std::ranges::find_if(added, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) { return a_ref && a_ref->GetFormID() == id; });
				RE::NiPointer<RE::TESObjectREFR> ref{ it != added.end() ? it->get() : Ref(id) };
				if (ref) {
					a_list.push_back(ref);
				}
			}
			if (!a_noPlayer) {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					a_list.push_back(RE::NiPointer<RE::TESObjectREFR>{ player });
				}
			}
			++gathered;
		}

		// Both of the engine's calls to Workshop::AddOtherContainers go through GatherHook: workshop mode's list and a
		// build's takes (Workshop::CollectLinkedWorkshopContainers), and the crafting menus' gather. One 14-byte stub serves
		// both (the trampoline keeps one per destination).
		void InstallGather()
		{
			auto& trampoline = REL::GetTrampoline();
			if (trampoline.free_size() < CALL_STUB) {
				REX::WARN("Storage: no room for the supply lines' hook ({} bytes free); a friend's builds at the host's settlements use our own world's supply lines", trampoline.free_size());
				return;
			}
			const std::array<std::pair<std::uintptr_t, const char*>, 2> sites{ { { COLLECT_LINKED.address() + COLLECT_LINKED_CALL, "workshop mode" },
				{ CRAFTING_GATHER.address() + CRAFTING_GATHER_CALL, "the crafting menus" } } };
			originalAddOthers = reinterpret_cast<AddOthersFn>(ADD_OTHER_CONTAINERS.address());
			for (const auto& [site, what] : sites) {
				std::array<std::uint8_t, 5> bytes{};
				std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
				if (TextureFlush::CallTarget(bytes, site) != ADD_OTHER_CONTAINERS.address()) {
					REX::WARN("Storage: {}' gather doesn't call Workshop::AddOtherContainers where 1.11.240 does (another plugin's patch?); a friend's builds there use our own "
							  "world's supply lines",
						what);
					continue;
				}
				trampoline.write_call<5>(site, &GatherHook);
				REX::INFO("Storage: {}' gather hooked ({:X}): at the host's workbench it finds the workbenches the host's supply lines link", what, site);
			}
		}

		void DrainLocal()
		{
			std::vector<LocalMove> moves;
			{
				std::scoped_lock l{ localLock };
				moves.swap(localInbox);
			}
			for (const auto& move : moves) {
				client.Local(move.container, move.item, move.count);  // nothing unless our copy of it shows the true copy
			}
		}

		// The engine's container of what workshop mode may build with, filled again from the workbench and the rest
		// (nothing outside workshop mode: it has no such container then).
		void RefillWorkshop()
		{
			static REL::Relocation<void (*)()> refill{ REL::ID(2194977) };  // Workshop::RefillSharedWorkshopContainer (+385C50)
			refill();
		}

		// a_fn with the open crafting menu, looked up as MenuInput looks one up: on the main thread between frames, with the UI
		// settled, only a menu OpenMenus says is up, and the reference let go before it returns (OpenMenusTests: no menu is
		// held where the engine's deadlock could take its chance).
		template <class Fn>
		void WithCraftingMenu(Fn&& a_fn)
		{
			MainThread::Assert("Storage::WithCraftingMenu");
			const auto ui = RE::UI::GetSingleton();
			const auto it = std::ranges::find_if(CRAFTING_MENUS, [](OpenMenus::Menu a_menu) { return OpenMenus::Open(a_menu); });
			if (!ui || !Hud::IsSettled() || it == CRAFTING_MENUS.end()) {
				return;
			}
			const auto name = OpenMenus::NAMES[static_cast<std::size_t>(*it)];
			const auto menu = ui->GetMenu(RE::BSFixedString{ std::string{ name }.c_str() });
			if (const auto workbench = menu && menu->OnStack() ? RE::fallout_cast<RE::WorkbenchMenuBase*>(menu.get()) : nullptr) {
				a_fn(*workbench);
			}
		}

		// The settlement workbench an open crafting menu reads (among its gather's containers); 0: none (a station outside
		// a settlement, an inspection from the Pip-Boy).
		std::uint32_t CraftingBench()
		{
			std::uint32_t found = 0;
			WithCraftingMenu([&](RE::WorkbenchMenuBase& a_menu) {
				for (const auto& container : a_menu.sharedContainers) {
					if (container && WorkshopSync::IsWorkbenchAnchor(container.get())) {
						found = container->GetFormID();
						return;
					}
				}
			});
			return found;
		}

		// What the menu reading our workbench shows is gathered again from it (the engine's own gather, as after a build).
		void RefreshBenchMenu()
		{
			if (bench.menu == BenchMenu::kWorkshop) {
				RefillWorkshop();
			} else if (bench.menu == BenchMenu::kCrafting) {
				WithCraftingMenu([](RE::WorkbenchMenuBase& a_menu) { a_menu.UpdateOptimizedAutoBuildInv(); });
			}
		}

		// The views of the workbenches the host's supply lines link to the one we view (SupplyLines.h), once its own view is
		// open: those no longer linked end, and the new ones open a few at a time (SupplyLines::Views: the server sends only
		// so many views' contents). The ones shown, and what the engine's gather is to find for its settlement.
		void OpenSupply()
		{
			if (bench.id == 0 || !side.Shows(bench.id)) {
				return;
			}
			const auto linked = heard.With(bench.id, [](std::uint32_t a_id) { return Ref(a_id) && KeeperOf(a_id) == Keeper::kFriend; });
			auto       viewed = side.Viewed();
			std::erase(viewed, bench.id);
			const auto step = bench.supplyViews.Next(linked, viewed, Clock::now());
			bool       changed = false;
			for (const auto link : step.close) {
				client.CloseOne(link);
				changed = true;
			}
			if (step.open && client.OpenAlso(*step.open)) {
				changed = true;
			}
			std::vector<std::uint32_t> supply;
			for (const auto link : step.wanted) {
				if (side.Shows(link)) {
					supply.push_back(link);
				}
			}
			if (supply != bench.supply) {
				if (supply.size() > bench.supply.size() && supply.size() == step.wanted.size()) {
					REX::INFO("Storage: the host's supply lines link {} workbench{} to {:08X}{}: our builds there use them too", supply.size(), supply.size() == 1 ? "" : "es",
						bench.id, linked.size() > step.wanted.size() ? std::format(" (of {}; the rest stay our own)", linked.size()) : "");
				}
				bench.supply = std::move(supply);
				SetGather(SettlementOf(Ref(bench.id)), bench.supply);
				changed = true;
			}
			if (changed) {
				bench.refresh = true;
			}
		}

		// Our view of the host's workbench our menu reads shows its true copy.
		bool BenchShown()
		{
			return bench.id != 0 && side.Shows(bench.id);
		}

		// A view of the workbench our menu reads, when a friend's game keeps it and our moves are settled.
		void OpenBench()
		{
			const auto now = Clock::now();
			if (side.Views(bench.id)) {
				OpenSupply();
				return;
			}
			if (!ruleOn || bench.id == 0 || side.Viewing() || now < bench.retryAt || KeeperOf(bench.id) != Keeper::kFriend) {
				return;
			}
			if (side.Settling()) {
				return;  // the server refuses the view until our moves are settled: tried again then, and that wait is no failure
			}
			// A try: the next waits BENCH_RETRY, doubled each time one fails, up to BENCH_RETRY_MAX (reset as its contents come).
			bench.retryAt = now + bench.backoff;
			bench.backoff = (std::min<Clock::duration>)(bench.backoff * 2, BENCH_RETRY_MAX);
			client.Open(bench.id, true);
			if (side.Views(bench.id)) {
				REX::INFO("Storage: viewing the host's workbench {:08X} ({}): its junk, ammo and aid are the one pile", bench.id,
					bench.menu == BenchMenu::kWorkshop ? "workshop mode" : "a crafting menu");
			}
		}

		void CloseBench()
		{
			if (bench.id != 0 && side.Views(bench.id)) {
				client.MenuClosed();  // its supply lines' workbenches' views too
			}
			bench = {};
			SetGather(nullptr, {});
		}

		// The server closed our view of the host's workbench (its keeper away, or their world reloaded): the view ends at once,
		// so workshop mode and the menu read our copy's own things again; it opens again later (OpenBench). One of a workbench
		// its supply lines link to ends alone, and is tried again later (OpenSupply).
		void EndClosedBenchView()
		{
			if (bench.id != 0 && side.Views(bench.id) && !side.Shows(bench.id)) {
				client.MenuClosed();
				bench.supply.clear();
				SetGather(nullptr, {});
				bench.refresh = true;
				return;
			}
			for (const auto link : side.Viewed()) {
				if (link != bench.id && !side.Shows(link)) {
					client.CloseOne(link);
					bench.supplyViews.Closed(link, Clock::now());  // tried again after its backoff (OpenSupply)
					bench.refresh = true;
				}
			}
		}

		// Every frame: a crafting menu's workbench found, a view opened that couldn't be yet, and the menu filled again.
		void BenchFrame()
		{
			if (bench.menu == BenchMenu::kCrafting && bench.id == 0 && Clock::now() - bench.openedAt < CRAFTING_LOOK_FOR) {
				bench.id = CraftingBench();
			}
			if (bench.menu != BenchMenu::kNone) {
				DrainLocal();
				OpenBench();
			}
			if (const auto changed = std::exchange(bench.hostChanged, {});
				bench.menu != BenchMenu::kNone && SupplyLines::HostListChanged(bench.id, changed, [] { return SupplyOf(Ref(bench.id)); })) {
				bench.refresh = true;
			}
			if (std::exchange(bench.refresh, false)) {
				RefreshBenchMenu();
			}
		}

		// A settlement's workbench the host's game lists: only one of a settlement the host owns (a workbench whose
		// settlement is a friend's in their save, not the host's, stays theirs). WorkshopScript.SetOwnedByPlayer(true), the
		// way a settlement becomes the player's, makes the player's NPC the workbench's owner (SetActorOwner), and losing it
		// clears that (SetActorOwner(none)). On a save from normal play the workbenches' owner matched the script's own
		// OwnedByPlayer on all 23 (the rig, 2026-11-04). PlayerFaction counts too, as for the other storage the player owns.
		bool OwnBench(RE::TESObjectREFR* a_ref)
		{
			const auto owner = a_ref ? a_ref->GetOwner() : nullptr;
			return owner && (owner->GetFormID() == 0x00000007 || owner->GetFormID() == 0x0001C21C);  // the player's NPC, PlayerFaction
		}

		// Our listing of a container, the workbench among them (SharedBench says it is storage while it is made). Our player's
		// moves and the engine's changes our game reports go out first: the listing holds them already (our world has them),
		// and the server counts none of ours numbered before it on top (StorageBook::List), so one sent after would count twice.
		std::optional<Protocol::StorageListing> Listing(std::uint32_t a_id)
		{
			WorldSync::FlushLoot();
			listingNow = a_id;
			auto listing = WorldSync::StorageListing(a_id);
			listingNow = 0;
			return listing;
		}

		// Whether the scan lists a container: storage, not a vendor's, and a workbench only of a settlement the host owns.
		bool Listable(std::uint32_t a_id, const LootScope::HolderFacts& a_facts)
		{
			return a_facts.storage && !a_facts.vendorChest && (!a_facts.workshop || OwnBench(Ref(a_id)));
		}

		void MarkListed(std::uint32_t a_id)
		{
			side.MarkListed(a_id);
			NoteBench(a_id);
		}

		// The host's game: which of its kept workbenches its supply lines link to each it keeps, told to the session when that
		// changes (SupplyLines::Reporter). A linked workbench of a settlement it owns that isn't listed yet is listed first
		// (from afar: a workbench is a persistent reference); one of a settlement it doesn't own isn't linked (its own
		// stash, the host's game builds with it, but it isn't the session's).
		void ReportSupply()
		{
			const auto now = Clock::now();
			if (!hosting || !side.Rule() || now - linksAt < LINKS_INTERVAL || !WorldSync::InWorld()) {
				return;
			}
			linksAt = now;
			std::vector<std::uint32_t> kept;
			{
				std::scoped_lock l{ benchesLock };
				kept.assign(benches.begin(), benches.end());
			}
			std::ranges::sort(kept);
			int listed = 0;
			for (const auto id : kept) {
				const auto ref = Ref(id);
				if (!ref || KeeperOf(id) != Keeper::kUs) {
					continue;
				}
				std::vector<std::uint32_t> links;
				for (const auto link : SupplyOf(ref)) {
					const auto linkRef = Ref(link);
					if (!Protocol::IsShareableRef(link) || !linkRef || !WorkshopSync::IsWorkbenchAnchor(linkRef) || !OwnBench(linkRef)) {
						continue;
					}
					if (!side.Listed(link)) {
						if (listed >= LIST_PER_SCAN) {
							continue;  // the next look
						}
						const auto listing = Listable(link, WorldSync::HolderFactsOf(link)) ? Listing(link) : std::nullopt;
						if (!listing) {
							continue;
						}
						WorldSync::QueueEvent(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
						MarkListed(link);
						++listed;
					}
					links.push_back(link);
				}
				if (auto report = reporter.Update(id, std::move(links))) {
					REX::INFO("Storage: our supply lines link {} workbench{} to {:08X}: a friend building there uses them too", report->links.size(),
						report->links.size() == 1 ? "" : "es", id);
					WorldSync::QueueEvent(Protocol::Encode(*report, Protocol::MessageType::kReportBenchLinks));
				}
			}
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
				if (!Listable(id, facts)) {
					notStorage[id] = now;
					continue;
				}
				const auto listing = Listing(id);
				if (!listing) {
					++counts.listFailed;  // the ledger is still asking, or the container's inventory isn't made yet: next time
					continue;
				}
				WorldSync::QueueEvent(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
				MarkListed(id);
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
				const auto listing = Listable(id, facts) ? Listing(id) : std::nullopt;
				if (!listing) {
					relistTried[id] = now;
					continue;
				}
				WorldSync::QueueEvent(Protocol::Encode(*listing, Protocol::MessageType::kReportStorage));
				MarkListed(id);
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
		hostingOn = a_rule && a_hosting;
		ForgetBenches();  // the owner table comes again
		heard.Clear();    // so do the supply lines
		reporter.Forget();
		linksAt = {};
		bench.supply.clear();
		bench.supplyViews.Clear();
		SetGather(nullptr, {});
		bench.retryAt = {};
		bench.backoff = BENCH_RETRY;
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
		// Workshop mode or a crafting menu still open reads our own stash again now: no Frame() runs out of a session.
		if (std::exchange(bench.refresh, false)) {
			RefreshBenchMenu();
		}
		ruleOn = false;
		hostingOn = false;
		ForgetBenches();
		heard.Clear();
		bench.supply.clear();
		bench.supplyViews.Clear();
		SetGather(nullptr, {});
		live = 0;
		if (unanswered != 0) {
			REX::INFO("Storage: {} of our moves at a friend's storage had no answer as the connection went: kept, and settled when we rejoin (sent late if the session never numbered them)", unanswered);
		}
	}

	void OnOwners(const Protocol::StorageOwners& a_owners)
	{
		client.OnOwners(a_owners, menuOn, [](std::uint32_t a_id) { return WorldSync::Loot().Ours(a_id); });
		for (const auto& owner : a_owners.entries) {
			NoteBench(owner.container);  // a workbench kept: its changes are watched (WatchesBench)
		}
		bench.retryAt = {};  // a workbench our menu reads may be kept now: its view opens (BenchFrame)
		bench.supplyViews.Clear();  // a linked workbench may be kept now: tried at once
	}

	void OnBenchLinks(const Protocol::BenchLinks& a_links)
	{
		heard.Take(a_links);  // views open or end with the next frame (OpenSupply)
	}

	void OnContents(const Protocol::StorageListing& a_contents)
	{
		if (a_contents.container == bench.id && side.Shows(bench.id)) {
			bench.backoff = BENCH_RETRY;
		}
		if (bench.id != 0 && a_contents.container != bench.id && side.Shows(a_contents.container)) {
			bench.supplyViews.Shown(a_contents.container);
		}
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
		DrainLocal();
		client.BeforeSaveOrLoad();
		// A save in workshop mode: its view opens again after it (BenchFrame), and the engine's container is filled again.
		bench.retryAt = {};
		bench.refresh = true;
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
		bench = {};  // a load ends workshop mode and the menus
		SetGather(nullptr, {});
		reporter.Forget();  // our world's supply lines may differ
		linksAt = {};
		{
			std::scoped_lock l{ localLock };
			localInbox.clear();
		}
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
		ReportSupply();
		BenchFrame();
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

	namespace
	{
		class WorkshopModeWatcher final :
			public RE::BSTEventSink<RE::Workshop::WorkshopModeEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::WorkshopModeEvent& a_event, RE::BSTEventSource<RE::Workshop::WorkshopModeEvent>*) override
			{
				OnWorkshopMode(a_event.workshop ? a_event.workshop->GetFormID() : 0, a_event.start);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		WorkshopModeWatcher workshopModeWatcher;

		// The game's Papyrus AddItem and RemoveItem, run as delay functors on the main thread (GameScript's AddItemFunctor and
		// RemoveItemFunctor; their operator(), vtable slot 1, returns a BSScript::Variable through a hidden pointer).
		using RunFn = void* (*)(void*, void*);
		RunFn originalAddRun = nullptr;
		RunFn originalRemoveRun = nullptr;

		void* AddRun(void* a_this, void* a_result)
		{
			++scripted;
			const auto result = originalAddRun(a_this, a_result);
			--scripted;
			return result;
		}

		void* RemoveRun(void* a_this, void* a_result)
		{
			++scripted;
			const auto result = originalRemoveRun(a_this, a_result);
			--scripted;
			return result;
		}

		bool HookRun(std::uintptr_t a_vtable, REL::ID a_run, RunFn a_hook, RunFn& a_original, std::string_view a_name)
		{
			const auto slot = a_vtable + sizeof(void*);
			const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
			if (current != a_run.address()) {
				REX::WARN("Storage: the scripts' {} isn't where 1.11.240 has it (another plugin's patch?): a script's {} at the host's workbench counts as a move of ours", a_name,
					a_name);
				return false;
			}
			a_original = reinterpret_cast<RunFn>(current);
			REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(a_hook));
			return true;
		}
	}

	void Install()
	{
		static bool installed = false;
		if (std::exchange(installed, true)) {
			return;
		}
		RE::Workshop::RegisterForWorkshopModeEvent(&workshopModeWatcher);
		const bool add = HookRun(RE::VTABLE::GameScript____AddItemFunctor[0].address(), REL::ID(2253314), &AddRun, originalAddRun, "AddItem");
		const bool remove = HookRun(RE::VTABLE::GameScript____RemoveItemFunctor[0].address(), REL::ID(2253317), &RemoveRun, originalRemoveRun, "RemoveItem");
		REX::INFO("Storage: watching workshop mode; the scripts' AddItem {} and RemoveItem {}", add ? "watched" : "not watched", remove ? "watched" : "not watched");
		InstallGather();
	}

	void OnWorkshopMode(std::uint32_t a_workshop, bool a_start)
	{
		MainThread::Post([a_workshop, a_start] {
			if (!a_start) {
				if (bench.menu == BenchMenu::kWorkshop) {
					CloseBench();
				}
				return;
			}
			CloseBench();
			// The settlement's workbench is its workshop reference itself, a container (Workshop::GetWorkshopContainer takes the
			// workshop when its base is a CONT, as every settlement's is).
			const auto ref = Ref(a_workshop);
			const auto base = ref ? ref->GetObjectReference() : nullptr;
			bench.menu = BenchMenu::kWorkshop;
			bench.id = base && base->Is(RE::ENUM_FORM_ID::kCONT) && WorkshopSync::IsWorkbenchAnchor(ref) ? a_workshop : 0;
			bench.openedAt = Clock::now();
			OpenBench();
		});
	}

	void OnWorkbenchMenu(bool a_opening)
	{
		MainThread::Post([a_opening] {
			if (!a_opening) {
				if (bench.menu == BenchMenu::kCrafting) {
					CloseBench();
				}
				return;
			}
			if (bench.menu == BenchMenu::kWorkshop) {
				return;  // a menu over workshop mode reads the same workbench
			}
			CloseBench();
			bench.menu = BenchMenu::kCrafting;
			bench.openedAt = Clock::now();
			bench.id = CraftingBench();  // or found in the next frames (BenchFrame)
			OpenBench();
		});
	}

	bool SharedBench(std::uint32_t a_container)
	{
		return side.KeepsBench(a_container, listingNow == a_container);
	}

	bool WatchesBench(std::uint32_t a_container)
	{
		if (!ruleOn) {
			return false;
		}
		std::scoped_lock l{ benchesLock };
		return benches.contains(a_container);
	}

	bool KeepsTrueCopies()
	{
		return hostingOn;
	}

	bool Scripted()
	{
		return scripted > 0;
	}

	void LocalChange(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count)
	{
		std::scoped_lock l{ localLock };
		if (localInbox.size() < MAX_LOCAL) {
			localInbox.push_back({ a_container, a_item, a_count });
		}
	}

	bool ApplyAtBench(const Protocol::ContainerChange& a_change)
	{
		if (!WatchesBench(a_change.container)) {
			return false;
		}
		const auto done = Move(Ref(a_change.container), a_change.item, a_change.count);
		if (ReadByBench(a_change.container)) {
			bench.refresh = true;
		} else if (hosting && done != 0 && bench.menu != BenchMenu::kNone && bench.id != 0 &&
				   std::ranges::find(bench.hostChanged, a_change.container) == bench.hostChanged.end()) {
			bench.hostChanged.push_back(a_change.container);  // one our supply lines link to the workbench we're at? (BenchFrame)
		}
		if (tracing) {
			REX::INFO("Storage: trace a friend's change at {:08X}: {} of {:08X} (made {})", a_change.container, a_change.count, a_change.item, done);
		}
		return true;
	}

	bool Tracing()
	{
		return tracing;
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
						   "givenBack={} closedByHost={} unmatched={} listed={} shorts={} listPutOff={} ownRefused={} bench={}:{:08X} benches={} localKept={} localUnpaid={} stashPaid={} "
						   "supply={} linkedHeard={} linkedSent={} linkReports={} supplyGathers={}",
			side.Rule() ? "on" : "off", hosting ? "(host)" : "", side.OwnerCount(), side.Viewing() ? std::format("{:08X}", *side.Viewing()) : "-", side.InFlight(),
			side.Settling() ? 1 : 0, side.Lacks(), c.setAside, s.views, c.shown, s.refusedTakes, s.refusedPuts, s.shortTakes, s.unanswered, s.unsent, s.overflow, s.lacking,
			s.lacksDropped, s.settles, s.late, s.settlingClosed, s.corrected, c.takenBack, c.usedUp, c.givenBack, s.closedByHost, s.unmatched, s.listed, s.shorts, counts.listFailed,
			counts.refusedOwn, bench.menu == BenchMenu::kWorkshop ? "workshop" : bench.menu == BenchMenu::kCrafting ? "crafting" : "none", bench.id,
			[] {
				std::scoped_lock l{ benchesLock };
				return benches.size();
			}(),
			c.localKept, c.localUnpaid, c.stashPaid, bench.supply.size(), heard.Benches(), reporter.Benches(), reporter.Reports(), gathered.load());
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
		// The workbench our workshop mode or crafting menu reads, and what each of the engine's lists holds of an item: our
		// copy of the workbench, workshop mode's own container (Workshop::RefillSharedWorkshopContainer's), and the open
		// crafting menu's (WorkbenchMenuBase::sharedContainerRef, its gather's).
		if (args[0] == "bench") {
			std::string out = std::format("bench={} {:08X} viewing={} shows={} keeper={}", bench.menu == BenchMenu::kWorkshop ? "workshop" : bench.menu == BenchMenu::kCrafting ? "crafting" : "none",
				bench.id, side.Viewing() == bench.id && bench.id != 0, bench.id != 0 && side.Shows(bench.id), Name(KeeperOf(bench.id)));
			for (const auto link : bench.supply) {
				out += std::format(" supply={:08X}:{}", link, side.Shows(link) ? "shown" : "closed");
			}
			if (hosting && bench.id != 0) {
				for (const auto link : SupplyOf(Ref(bench.id))) {
					out += std::format(" ourLine={:08X}", link);
				}
			}
			const auto object = args.size() > 1 ? Object(ParseHex(args[1])) : nullptr;
			if (!object) {
				return out;
			}
			static REL::Relocation<std::uint32_t*> sharedHandle{ REL::ID(4797256) };  // the workshop's shared container (+30F76EC)
			RE::ObjectRefHandle handle;
			std::memcpy(&handle, sharedHandle.get(), sizeof(handle));
			const auto workshopList = handle ? handle.get() : RE::NiPointer<RE::TESObjectREFR>{};
			std::int64_t craftingList = -1;
			WithCraftingMenu([&](RE::WorkbenchMenuBase& a_menu) { craftingList = CountIn(a_menu.sharedContainerRef.get(), object); });
			out += std::format(" item={:08X} copy={} workshopList={} craftingList={} player={}", object->GetFormID(), CountIn(Ref(bench.id), object),
				workshopList ? CountIn(workshopList.get(), object) : -1, craftingList, CountIn(Player(), object));
			for (const auto link : bench.supply) {
				out += std::format(" at{:08X}={}", link, CountIn(Ref(link), object));
			}
			return out;
		}
		// What a build does with each of its components, in workshop mode: Workshop::RemoveItemFromWorkshopContainers (the
		// workbench, then the supply lines' benches, then the player), then the build list is filled again, as
		// Workshop::PlaceCurrentReference does once the piece is placed (no piece is placed here).
		if (args[0] == "trace") {
			tracing = args.size() < 2 || args[1] != "off";
			return tracing ? "tracing the watched workbenches' changes (the plugin log)" : "trace off";
		}
		if (args[0] == "build") {
			if (!OpenMenus::Open(OpenMenus::Menu::kWorkshop) || args.size() < 3) {
				return "error: storage build <item> <count>, in workshop mode";
			}
			const auto object = Object(ParseHex(args[1]));
			std::uint32_t count = 0;
			std::from_chars(args[2].data(), args[2].data() + args[2].size(), count);
			if (!object || count == 0) {
				return "error: item or count";
			}
			using RemoveFn = void (*)(RE::TESBoundObject*, std::uint32_t);
			static REL::Relocation<RemoveFn> remove{ REL::ID(2195117) };  // (+392340)
			const auto before = CountIn(Ref(bench.id), object);
			const auto carried = CountIn(Player(), object);
			remove(object, count);
			RefillWorkshop();
			return std::format("built with {} of {:08X}: workbench {} -> {}, player {} -> {}", count, object->GetFormID(), before, CountIn(Ref(bench.id), object), carried,
				CountIn(Player(), object));
		}
		if (args.size() < 2) {
			return "error: storage [status | owners | keeper <ref> | view <ref> | close | bench [item] | build <item> <count> | count <ref> <item> | take <ref> <item> <count> | put <ref> <item> <count>]";
		}
		const auto id = ParseHex(args[1]);
		const auto ref = Ref(id);
		if (!ref) {
			return std::format("error: no reference {:08X}", id);
		}
		if (args[0] == "keeper") {
			const auto owner = side.OwnerOf(id);
			const auto refOwner = ref->GetOwner();
			return std::format("{:08X}: {} (owner {}) storage={} viewFrom={} refOwner={:08X} ownBench={}", id, Name(KeeperOf(id)), owner ? std::to_string(*owner) : "none",
				WorldSync::HolderFactsOf(id).storage, side.ViewFrom(id) ? std::to_string(*side.ViewFrom(id)) : "-", refOwner ? refOwner->GetFormID() : 0, OwnBench(ref));
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
