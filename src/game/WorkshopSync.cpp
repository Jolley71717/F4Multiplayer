#include "game/WorkshopSync.h"

#include "game/EngineRefs.h"

#include "game/DeferredDelete.h"

namespace WorkshopSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Key = std::pair<std::uint32_t, std::uint32_t>;  // (player, their reference ID)

		struct Copy
		{
			RE::ObjectRefHandle ref;
			Clock::time_point   placedAt;
		};

		bool                                   installed = false;
		std::mutex                             outLock;
		std::vector<std::vector<std::uint8_t>> outgoing;
		std::map<Key, Copy>                    copies;
		std::map<Key, Protocol::WorkshopItem>  pending;  // waiting for their settlement's cell to load
		std::vector<Copy>                      graveyard;
		Clock::time_point                      nextPendingTry{};
		constexpr auto                         PENDING_INTERVAL = std::chrono::seconds(2);
		constexpr std::size_t                  MAX_OUTGOING = 512;
		constexpr float                        ADOPT_RADIUS = 4.0f;  // a copy left by an earlier session, at the same spot
		// How far from its workbench a piece may stand. The largest build area in the game (Spectacle
		// Island) reaches about 9,000 units from its bench; a piece claimed farther away is someone placing
		// things in the open world under a settlement's name.
		constexpr float                        BUILD_REACH = 12000.0f;

		std::uint32_t reported = 0;
		std::uint32_t placed = 0;
		std::uint32_t moved = 0;
		std::uint32_t scrapped = 0;
		std::uint32_t adopted = 0;
		std::uint32_t refused = 0;  // outside their workbench's reach
		std::uint32_t worldScrapped = 0;  // pieces of the world itself (a settlement's junk) scrapped by someone else

		// The form IDs of our copies, for the workshop event sinks (which may run off the game thread while
		// the game thread changes `copies`): kept in step with `copies` under its own lock.
		std::mutex                        copyIdsLock;
		std::unordered_set<std::uint32_t> copyIds;

		void NoteCopy(const RE::ObjectRefHandle& a_handle)
		{
			if (const auto ref = a_handle.get()) {
				std::scoped_lock l{ copyIdsLock };
				copyIds.insert(ref->GetFormID());
			}
		}

		void ForgetCopy(const RE::ObjectRefHandle& a_handle)
		{
			if (const auto ref = a_handle.get()) {
				std::scoped_lock l{ copyIdsLock };
				copyIds.erase(ref->GetFormID());
			}
		}

		bool IsCopy(const RE::TESObjectREFR* a_ref)
		{
			std::scoped_lock l{ copyIdsLock };
			return copyIds.contains(a_ref->GetFormID());
		}

		// Pieces of the world itself reported scrapped this session, by the destroyed sink (which may run
		// off the game thread) or found scrapped in the save: each is reported once. The server keeps one
		// report, and a second would only be a message for nothing.
		std::mutex                        sentScrapsLock;
		std::unordered_set<std::uint32_t> sentScraps;

		// False if it was already reported.
		bool NoteWorldScrap(std::uint32_t a_id)
		{
			std::scoped_lock l{ sentScrapsLock };
			return sentScraps.insert(a_id).second;
		}

		bool WorldScrapSent(std::uint32_t a_id)
		{
			std::scoped_lock l{ sentScrapsLock };
			return sentScraps.contains(a_id);
		}

		void ForgetWorldScrap(std::uint32_t a_id)
		{
			std::scoped_lock l{ sentScrapsLock };
			sentScraps.erase(a_id);
		}

		void Push(const Protocol::WorkshopItem& a_item)
		{
			std::scoped_lock l{ outLock };
			if (outgoing.size() < MAX_OUTGOING) {
				outgoing.push_back(Protocol::Encode(a_item, Protocol::MessageType::kReportWorkshopItem));
				++reported;
			}
		}

		void ReportRef(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || IsCopy(a_ref)) {
				return;
			}
			if ((base->GetFormID() >> 24) == 0xFF) {
				return;  // a base made at runtime exists only here
			}
			Protocol::WorkshopItem item;
			item.refId = a_ref->GetFormID();
			item.op = a_op;
			item.base = base->GetFormID();
			item.workshop = a_workshop ? a_workshop->GetFormID() : 0;
			item.position[0] = a_ref->data.location.x;
			item.position[1] = a_ref->data.location.y;
			item.position[2] = a_ref->data.location.z;
			item.rotation[0] = a_ref->data.angle.x;
			item.rotation[1] = a_ref->data.angle.y;
			item.rotation[2] = a_ref->data.angle.z;
			item.scale = a_ref->refScale / 100.0f;
			Push(item);
		}

		class PlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				ReportRef(a_event.placedItem.get(), a_event.workshop.get(), Protocol::WorkshopOp::kPlaced);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class MovedSink : public RE::BSTEventSink<RE::Workshop::ItemMovedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemMovedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemMovedEvent>*) override
			{
				ReportRef(a_event.movedItem.get(), a_event.workshop.get(), Protocol::WorkshopOp::kPlaced);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class DestroyedSink : public RE::BSTEventSink<RE::Workshop::ItemDestroyedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemDestroyedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemDestroyedEvent>*) override
			{
				const auto ref = a_event.objectDestroyed.get();
				if (ref && Protocol::IsShareableRef(ref->GetFormID()) && !NoteWorldScrap(ref->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				ReportRef(ref, a_event.workshop.get(), Protocol::WorkshopOp::kScrapped);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		PlacedSink    placedSink;
		MovedSink     movedSink;
		DestroyedSink destroyedSink;

		// The cell the copy belongs in, if it's loaded: the settlement's workbench tells us where.
		RE::TESObjectCELL* CellFor(const Protocol::WorkshopItem& a_item)
		{
			const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto here = player ? player->GetParentCell() : nullptr;
			if (!here) {
				return nullptr;
			}
			const auto cell = workshop ? workshop->GetParentCell() : nullptr;
			if (!cell) {
				return nullptr;  // its workbench isn't loaded here: the piece waits (it used to land in whatever cell we were in)
			}
			if (cell->IsInterior()) {
				return cell == here ? cell : nullptr;
			}
			return cell->worldSpace == here->worldSpace ? here : nullptr;
		}

		void SetTransform(RE::TESObjectREFR* a_ref, const Protocol::WorkshopItem& a_item)
		{
			a_ref->SetLocationOnReference({ a_item.position[0], a_item.position[1], a_item.position[2] });
			a_ref->SetAngleOnReference({ a_item.rotation[0], a_item.rotation[1], a_item.rotation[2] });
			if (std::abs(a_ref->refScale / 100.0f - a_item.scale) > 0.01f) {
				a_ref->SetScale(a_item.scale);
			}
		}

		bool InGraveyard(const RE::TESObjectREFR* a_ref)
		{
			return std::ranges::any_of(graveyard, [&](const Copy& a_copy) { return a_copy.ref.get().get() == a_ref; });
		}

		// A real workshop object of this game (ours, or one we scrapped/moved ourselves): it carries the
		// workshop's own extra data, which a plain copy never has.
		bool IsWorkshopObject(const RE::TESObjectREFR* a_ref)
		{
			return a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kWorkshop);
		}

		// A copy left in this save by an earlier session: the same object at the same spot.
		RE::TESObjectREFR* FindExisting(const Protocol::WorkshopItem& a_item)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return nullptr;
			}
			RE::TESObjectREFR* found = nullptr;
			const RE::NiPoint3 at{ a_item.position[0], a_item.position[1], a_item.position[2] };
			tes->ForEachReferenceInRange(at, ADOPT_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->GetFormID() == a_item.base && (a_ref->GetFormID() >> 24) == 0xFF && !IsCopy(a_ref) && !InGraveyard(a_ref) && !IsWorkshopObject(a_ref)) {
					found = a_ref;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		// What workshop mode can build: furniture, walls, lights, containers, crops, turrets... A
		// message from another player is untrusted, so bases the workshop never places (explosions,
		// hazards, projectiles, unique NPCs) are refused: they would go off, or persist, in our save.
		bool Buildable(const RE::TESBoundObject* a_base)
		{
			using T = RE::ENUM_FORM_ID;
			switch (a_base->GetFormType()) {
			case T::kSTAT:
			case T::kSCOL:
			case T::kMSTT:
			case T::kACTI:
			case T::kFURN:
			case T::kCONT:
			case T::kDOOR:
			case T::kLIGH:
			case T::kTERM:
			case T::kMISC:
			case T::kFLOR:
			case T::kTREE:
				return true;
			case T::kNPC_: {
				// Workshop mode builds turrets (and, with Automatron, robots); any other actor from a workshop
				// message is someone placing creatures or people on us.
				const auto npc = static_cast<const RE::TESNPC*>(a_base);
				return !npc->IsUnique() && a_base->GetFormID() != 0x00000007 &&
				       (npc->HasKeywordString("ActorTypeTurret") || npc->HasKeywordString("ActorTypeRobot"));
			}
			default:
				return false;
			}
		}

		// Whether the piece stands within its workbench's reach (the bench is known to be loaded here).
		bool InReach(const Protocol::WorkshopItem& a_item)
		{
			const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
			if (!workshop) {
				return false;
			}
			const RE::NiPoint3 at{ a_item.position[0], a_item.position[1], a_item.position[2] };
			return workshop->data.location.GetDistance(at) <= BUILD_REACH;
		}

		// Places (or moves) the copy; false if its settlement isn't loaded yet.
		bool Place(const Key& a_key, const Protocol::WorkshopItem& a_item)
		{
			const auto cell = CellFor(a_item);
			if (!cell) {
				return false;
			}
			if (!InReach(a_item)) {
				++refused;
				return true;  // dropped, and a copy already standing is left where it was
			}
			if (const auto it = copies.find(a_key); it != copies.end()) {
				if (const auto ref = it->second.ref.get()) {
					SetTransform(ref.get(), a_item);
					++moved;
					return true;
				}
				ForgetCopy(it->second.ref);
				copies.erase(it);  // the game dropped it
			}
			const auto base = RE::TESForm::GetFormByID<RE::TESBoundObject>(a_item.base);
			if (!base || !Buildable(base)) {
				return true;  // nothing we can show (a mod we don't have), or nothing we would
			}
			if (const auto existing = FindExisting(a_item)) {
				SetTransform(existing, a_item);
				copies[a_key] = { existing->GetHandle(), Clock::now() };
				NoteCopy(copies[a_key].ref);
				++adopted;
				return true;
			}
			RE::NEW_REFR_DATA data;
			data.location = { a_item.position[0], a_item.position[1], a_item.position[2] };
			data.direction = { a_item.rotation[0], a_item.rotation[1], a_item.rotation[2] };
			data.object = base;
			data.interior = cell->IsInterior() ? cell : nullptr;
			data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
			data.clearStillLoadingFlag = true;
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref) {
				return true;
			}
			if (std::abs(a_item.scale - 1.0f) > 0.01f) {
				ref->SetScale(a_item.scale);
			}
			copies[a_key] = { handle, Clock::now() };
			NoteCopy(handle);
			++placed;
			return true;
		}

		// A piece of the world itself (the junk a settlement starts with) that its player scrapped: it goes
		// from our world too. False until it's loaded here. Only what workshop mode could have scrapped:
		// the same object (base) as theirs, within its workbench's reach, and nothing alive or a quest's.
		bool ScrapWorldRef(const Protocol::WorkshopItem& a_item)
		{
			if (!CellFor(a_item)) {
				return false;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.refId);
			if (!ref) {
				return false;  // its cell isn't loaded yet
			}
			if (ref->IsDisabled() || ref->IsDeleted()) {
				return true;
			}
			const auto base = ref->GetObjectReference();
			const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
			const bool quest = ref->extraList && ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			if (!base || base->GetFormID() != a_item.base || ref->As<RE::Actor>() || !Buildable(base) || quest || !workshop ||
				workshop->data.location.GetDistance(ref->data.location) > BUILD_REACH) {
				++refused;
				return true;
			}
			ref->Disable();
			++worldScrapped;
			return true;
		}

		// ---- Settlement junk the host scrapped before the session ------------------------------------
		//
		// The workshop's events only tell of scraps made while we watch. What the host scrapped earlier is
		// in their save, and their friends would still see that junk. Scrapping a piece of the world itself
		// switches it off and marks it deleted (a reference from the game's files can't really be deleted:
		// the save keeps both flags). A switched-off reference sends no load event, so the host walks the
		// references of the cells that attach, a few hundred a frame, and reports each piece it finds
		// scrapped once, the way a scrap made in the session is reported; the server keeps it for whoever
		// joins later. Switched off alone isn't scrapped: the game starts plenty of references off (quest
		// props, things an enable marker turns on), and junk a friend scrapped is only switched off here.

		constexpr std::size_t WALK_PER_FRAME = 512;  // references looked at per frame
		constexpr auto        CELL_LOOK_INTERVAL = std::chrono::seconds(1);
		constexpr double      SAVED_SCRAPS_PER_SECOND = 100.0;  // reports, well inside the server's event budget
		constexpr double      SAVED_SCRAPS_BURST = 20.0;
		constexpr std::size_t MAX_WAITING = 4096;

		struct Bench
		{
			std::uint32_t id;
			std::uint32_t space;  // its interior cell, or its worldspace
			RE::NiPoint3  at;
		};

		struct CellWalk
		{
			std::uint32_t id;          // the cell; for a worldspace's persistent references, the worldspace
			bool          persistent;  // only workbenches are looked for there (it holds the whole worldspace's)
		};

		std::set<std::uint32_t>                             attachedCells;  // the cells attached at the last look
		std::set<std::uint32_t>                             walkedWorlds;   // worldspaces whose persistent references were walked
		std::deque<CellWalk>                                toWalk;
		std::uint32_t                                       walkFrom = 0;  // where in the first cell's references
		std::vector<Bench>                                  benches;       // workbenches seen
		std::unordered_set<std::uint32_t>                   waiting;       // scrapped, with no workbench in reach seen yet
		std::deque<std::pair<std::uint32_t, std::uint32_t>> toSend;        // (reference, its workbench)
		Clock::time_point                                   nextCellLook{};
		Clock::time_point                                   lastRefill{};
		double                                              sendTokens = 0.0;

		std::uint32_t savedScraps = 0;  // reported from the walk
		std::uint32_t walkedRefs = 0;
		std::uint32_t seenDisabled = 0;  // references of the game's files found switched off...
		std::uint32_t seenDeleted = 0;   // ... and marked deleted too
		struct Round
		{
			std::uint32_t refs = 0, disabled = 0, deleted = 0, benches = 0;
		} walkRound;  // since the walk's queue last ran dry, for the log

		void ClearWalk()
		{
			attachedCells.clear();
			walkedWorlds.clear();
			toWalk.clear();
			walkFrom = 0;
			benches.clear();
			waiting.clear();
			for (const auto& [ref, bench] : toSend) {
				ForgetWorldScrap(ref);
			}
			toSend.clear();
			walkRound = {};
		}

		// The interior cell, or the worldspace: workbench and junk must share it.
		std::uint32_t SpaceOf(const RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return 0;
			}
			if (a_cell->IsInterior()) {
				return a_cell->GetFormID();
			}
			return a_cell->worldSpace ? a_cell->worldSpace->GetFormID() : 0;
		}

		// A settlement's workbench: it holds the workshop's own data (power grids and the like).
		bool IsBench(RE::TESObjectREFR* a_ref)
		{
			return !a_ref->IsDisabled() && !a_ref->As<RE::Actor>() && a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kWorkshop);
		}

		// Junk scrapped in workshop mode: switched off and deleted, and something a friend's game would
		// take away (see ScrapWorldRef). Not loose items: taken or scrapped, we can't tell which, and
		// what players pick up isn't the workshop's business. Not one an enable parent switches: its
		// parent's state decides it.
		bool LooksScrapped(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetObjectReference();
			const auto extra = a_ref->extraList.get();
			return base && (base->GetFormID() >> 24) != 0xFF && !base->Is(RE::ENUM_FORM_ID::kMISC) && Buildable(base) && !a_ref->As<RE::Actor>() &&
			       !(extra && (extra->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent) || extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray)));
		}

		// The nearest workbench the piece is within reach of, if one has been seen.
		const Bench* BenchFor(const RE::TESObjectREFR* a_ref)
		{
			const auto   space = SpaceOf(a_ref->GetParentCell());
			const Bench* nearest = nullptr;
			float        best = BUILD_REACH;
			for (const auto& bench : benches) {
				if (space != 0 && bench.space == space) {
					if (const float distance = bench.at.GetDistance(a_ref->data.location); distance <= best) {
						nearest = &bench;
						best = distance;
					}
				}
			}
			return nearest;
		}

		// Queues the piece for reporting if a workbench is in reach; true when it's done with (queued,
		// or gone from the game: it's found again when its cell attaches).
		bool QueueScrap(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref) {
				return true;
			}
			const auto bench = BenchFor(ref);
			if (!bench) {
				return false;
			}
			if (NoteWorldScrap(a_id)) {
				toSend.emplace_back(a_id, bench->id);
			}
			return true;
		}

		// a_space: the walked cell's (a worldspace's persistent references belong to the worldspace).
		void Examine(RE::TESObjectREFR* a_ref, std::uint32_t a_space, bool a_benchesOnly)
		{
			const auto id = a_ref->GetFormID();
			if (!Protocol::IsShareableRef(id)) {
				return;  // made at runtime: ours, a copy, something dropped
			}
			++walkedRefs;
			++walkRound.refs;
			if (!a_ref->IsDisabled()) {
				if (IsBench(a_ref) && std::ranges::none_of(benches, [&](const Bench& a_bench) { return a_bench.id == id; })) {
					benches.push_back({ id, a_space, a_ref->data.location });
					++walkRound.benches;
				}
				return;
			}
			if (a_benchesOnly) {
				return;
			}
			++seenDisabled;
			++walkRound.disabled;
			if (!a_ref->IsDeleted()) {
				return;
			}
			++seenDeleted;
			++walkRound.deleted;
			if (!LooksScrapped(a_ref) || WorldScrapSent(id) || waiting.contains(id)) {
				return;
			}
			if (!QueueScrap(id) && waiting.size() < MAX_WAITING) {
				waiting.insert(id);
			}
		}

		// Queues the cells that attached since the last look (and, outdoors, the worldspace's persistent
		// references once: the workbenches are among them).
		void LookForNewCells()
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			std::set<std::uint32_t> attached;
			if (tes->interiorCell) {
				attached.insert(tes->interiorCell->GetFormID());
			} else if (const auto grid = tes->gridCells) {
				for (std::uint32_t x = 0; x < grid->dimension; ++x) {
					for (std::uint32_t y = 0; y < grid->dimension; ++y) {
						const auto gridCell = grid->Get(x, y);
						const auto cell = gridCell ? gridCell->cell : nullptr;
						if (cell && cell->IsAttached()) {
							attached.insert(cell->GetFormID());
						}
					}
				}
				if (const auto world = tes->worldSpace; world && world->persistentCell && walkedWorlds.insert(world->GetFormID()).second) {
					toWalk.push_back({ world->GetFormID(), true });
				}
			}
			for (const auto id : attached) {
				if (!attachedCells.contains(id)) {
					toWalk.push_back({ id, false });
				}
			}
			attachedCells = std::move(attached);
		}

		RE::TESObjectCELL* CellOf(const CellWalk& a_walk)
		{
			if (a_walk.persistent) {
				const auto tes = RE::TES::GetSingleton();
				const auto world = tes ? tes->worldSpace : nullptr;
				return world && world->GetFormID() == a_walk.id ? world->persistentCell : nullptr;
			}
			const auto cell = RE::TESForm::GetFormByID<RE::TESObjectCELL>(a_walk.id);
			return cell && cell->IsAttached() ? cell : nullptr;  // it may have gone since the look
		}

		void FinishRound()
		{
			// Workbenches seen in this round may be in reach of pieces found before them.
			std::erase_if(waiting, [](std::uint32_t a_id) { return QueueScrap(a_id); });
			if (walkRound.deleted > 0 || walkRound.benches > 0) {
				REX::INFO("WorkshopSync: walked {} references: {} switched off, {} of them deleted; {} new workbenches; {} scraps to report, {} with no workbench in reach",
					walkRound.refs, walkRound.disabled, walkRound.deleted, walkRound.benches, toSend.size(), waiting.size());
			}
			walkRound = {};
		}

		// Walks up to WALK_PER_FRAME references. The cell's lock is held only to take its references: they
		// are looked at after it's let go (each one held, so it can't go away meanwhile).
		void WalkSome()
		{
			std::vector<RE::NiPointer<RE::TESObjectREFR>> refs;
			std::size_t                                   budget = WALK_PER_FRAME;
			while (budget > 0 && !toWalk.empty()) {
				const auto    walk = toWalk.front();
				bool          done = true;
				std::uint32_t space = 0;
				refs.clear();
				if (const auto cell = CellOf(walk)) {
					space = walk.persistent ? walk.id : SpaceOf(cell);
					const RE::BSAutoLock l{ cell->spinLock };
					const std::size_t    size = cell->references.size();
					const std::size_t    end = (std::min)(size, walkFrom + budget);
					for (std::size_t i = walkFrom; i < end; ++i) {
						refs.push_back(cell->references[static_cast<std::uint32_t>(i)]);
					}
					done = end >= size;
					walkFrom = static_cast<std::uint32_t>(end);
				}
				budget -= (std::min)(budget, refs.size());
				for (const auto& ref : refs) {
					if (ref) {
						Examine(ref.get(), space, walk.persistent);
					}
				}
				if (done) {
					toWalk.pop_front();
					walkFrom = 0;
					if (toWalk.empty()) {
						FinishRound();
					}
				}
			}
		}

		void SendSome(Clock::time_point a_now)
		{
			if (lastRefill != Clock::time_point{}) {
				sendTokens = (std::min)(SAVED_SCRAPS_BURST, sendTokens + std::chrono::duration<double>(a_now - lastRefill).count() * SAVED_SCRAPS_PER_SECOND);
			}
			lastRefill = a_now;
			while (sendTokens >= 1.0 && !toSend.empty()) {
				const auto [refId, benchId] = toSend.front();
				toSend.pop_front();
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(refId);
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(benchId);
				if (!ref || !bench) {
					ForgetWorldScrap(refId);  // gone before its turn: found again when its cell attaches
					continue;
				}
				ReportRef(ref, bench, Protocol::WorkshopOp::kScrapped);
				++savedScraps;
				sendTokens -= 1.0;
			}
		}

		void FindSavedScraps(Clock::time_point a_now)
		{
			if (a_now >= nextCellLook) {
				nextCellLook = a_now + CELL_LOOK_INTERVAL;
				LookForNewCells();
			}
			WalkSome();
			SendSome(a_now);
		}

		void Remove(Copy& a_copy)
		{
			if (const auto ref = a_copy.ref.get()) {
				if (DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_copy.placedAt)) {
					ref->Disable();
					ref->SetDelete(true);
				} else {
					graveyard.push_back(a_copy);
				}
			}
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		installed = true;
		RE::Workshop::RegisterForItemPlaced(&placedSink);
		RE::Workshop::RegisterForItemMoved(&movedSink);
		RE::Workshop::RegisterForItemDestroyed(&destroyedSink);
		REX::INFO("WorkshopSync: watching workshop placements");
	}

	void Frame(bool a_host)
	{
		const auto now = Clock::now();
		std::erase_if(graveyard, [&](Copy& a_copy) {
			const auto ref = a_copy.ref.get();
			if (!ref) {
				return true;
			}
			if (!DeferredDelete::Safe(ref->Get3D() != nullptr, now - a_copy.placedAt)) {
				return false;
			}
			ref->Disable();
			ref->SetDelete(true);
			return true;
		});
		// The host's world is the session's: a friend's save may have scrapped other things, and those
		// stay theirs.
		if (a_host) {
			FindSavedScraps(now);
		}
		if (pending.empty() || now < nextPendingTry) {
			return;
		}
		nextPendingTry = now + PENDING_INTERVAL;
		std::erase_if(pending, [&](const auto& a_entry) {
			return a_entry.second.op == Protocol::WorkshopOp::kScrapped ? ScrapWorldRef(a_entry.second) : Place(a_entry.first, a_entry.second);
		});
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		std::scoped_lock l{ outLock };
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kScrapped) {
			pending.erase(key);
			if (const auto it = copies.find(key); it != copies.end()) {
				Remove(it->second);
				ForgetCopy(it->second.ref);
				copies.erase(it);
				++scrapped;
			} else if (Protocol::IsShareableRef(a_item.refId) && !ScrapWorldRef(a_item)) {
				pending[key] = a_item;
			}
			return;
		}
		if (!Place(key, a_item)) {
			pending[key] = a_item;
		}
	}

	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
	{
		ReportRef(a_ref, a_workshop, a_op);
	}

	void Reset()
	{
		for (auto& [key, copy] : copies) {
			Remove(copy);
		}
		copies.clear();
		{
			std::scoped_lock l{ copyIdsLock };
			copyIds.clear();
		}
		pending.clear();
		ClearWalk();
		{
			std::scoped_lock l{ sentScrapsLock };
			sentScraps.clear();
		}
		std::scoped_lock l{ outLock };
		outgoing.clear();
	}

	void OnGameLoaded()
	{
		ClearWalk();
	}

	std::string Describe()
	{
		return std::format("workshop: reported={} copies={} placed={} moved={} scrapped={} worldScrapped={} adopted={} refused={} pending={}", reported, copies.size(), placed, moved, scrapped,
			       worldScrapped, adopted, refused, pending.size()) +
		       std::format(" savedScraps={} (walked={} disabled={} deleted={} benches={} waiting={} queued={})", savedScraps, walkedRefs, seenDisabled, seenDeleted, benches.size(),
				   waiting.size(), toSend.size());
	}

	std::string DescribeBenches(RE::TESObjectREFR* a_ref)
	{
		std::vector<RE::TESObjectREFR*> refs;
		if (a_ref) {
			refs.push_back(a_ref);
		} else {
			for (const auto& bench : benches) {
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(bench.id)) {
					refs.push_back(ref);
				}
			}
		}
		std::string out = std::format("{} workbenches", refs.size());
		for (const auto ref : refs) {
			const auto  base = ref->GetObjectReference();
			const auto& at = ref->data.location;
			out += std::format("; {:08X} base={:08X} at {:.0f} {:.0f} {:.0f}", ref->GetFormID(), base ? base->GetFormID() : 0, at.x, at.y, at.z);
			const auto data = ref->extraList ? ref->extraList->GetByType<RE::Workshop::ExtraData>() : nullptr;
			if (!data) {
				out += " (no workshop data)";
				continue;
			}
			// What the list names isn't known yet (references, or base objects with a count): the first
			// few entries, with what each turns out to be here.
			const auto& items = data->deletedItems;
			out += std::format(" deletedItems={}", items.size());
			for (std::uint32_t i = 0; i < items.size() && i < 8; ++i) {
				const auto info = items[i];
				if (!info) {
					continue;
				}
				const auto form = RE::TESForm::GetFormByID(info->formID);
				const auto item = form ? form->As<RE::TESObjectREFR>() : nullptr;
				std::string what = "not loaded";
				if (item) {
					what = std::format("reference disabled={} deleted={}", item->IsDisabled(), item->IsDeleted());
				} else if (form) {
					what = form->GetFormTypeString();
				}
				out += std::format(" [{:08X} x{} {}]", info->formID, info->count, what);
			}
		}
		return out;
	}
}
