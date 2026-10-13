#include "game/WorkshopSync.h"

#include "game/EngineRefs.h"
#include "game/RemoteEdits.h"
#include "game/WorkshopCells.h"

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

		// A report waiting to be sent, and the piece of the world it reports scrapped, if it does.
		struct Outgoing
		{
			std::vector<std::uint8_t> packet;
			std::uint32_t             worldScrap = 0;
		};

		bool                                                installed = false;
		std::mutex                                          outLock;
		std::vector<Outgoing>                               outgoing;
		std::map<Key, Copy>                                 copies;
		WorkshopCells::Waiting<Key, Protocol::WorkshopItem> pending;  // waiting for their cell to load
		std::vector<Copy>                                   graveyard;
		constexpr std::size_t                               MAX_OUTGOING = 512;
		constexpr float                                     ADOPT_RADIUS = 4.0f;  // a copy left by an earlier session, at the same spot
		// How far from its workbench a piece may stand. The largest build area in the game (Spectacle
		// Island) reaches about 9,000 units from its bench; a piece claimed farther away is someone placing
		// things in the open world under a settlement's name.
		constexpr float                                     BUILD_REACH = 12000.0f;

		std::uint32_t reported = 0;
		std::uint32_t placed = 0;
		std::uint32_t moved = 0;
		std::uint32_t scrapped = 0;
		std::uint32_t adopted = 0;
		std::uint32_t refused = 0;  // outside their workbench's reach
		std::uint32_t worldScrapped = 0;  // pieces of the world itself (a settlement's junk) scrapped by someone else
		std::uint32_t anchorRefused = 0;  // named something that is no workbench here as their workbench
		std::uint32_t scrapRefused = 0;   // scraps of the world refused: not junk workshop mode scraps, or outside the build area
		// Scraps waiting for their workbench's build-area volumes to load (the game can't tell whether the
		// piece is inside until they are): tried again as cells attach.
		std::unordered_set<std::uint32_t> scrapsDeferred;
		// Anchors that were no workbench, warned about once each (cleared when full, so it stays small).
		std::unordered_set<std::uint32_t> badAnchorsWarned;
		constexpr std::size_t             MAX_BAD_ANCHORS_WARNED = 64;

		// Workshop::WorkshopCanScrapRef(TESObjectREFR&, bool): whether workshop mode lets a player scrap
		// the reference. Not one with the keyword that blocks scrapping; one whose base has a scrap
		// recipe, or a misc item with components. The bool is false in the game's own scrap check
		// (Workshop::CanScrapPlacementRef), which lets loose weapons, armor and misc items through.
		constexpr REL::ID CAN_SCRAP_REF{ 2194959 };

		// WorkshopKeyword: on the base of every settlement's workbench.
		constexpr std::uint32_t WORKSHOP_KEYWORD = 0x00054BA7;

		bool CanScrap(RE::TESObjectREFR& a_ref)
		{
			using func_t = bool (*)(RE::TESObjectREFR&, bool);
			static REL::Relocation<func_t> func{ CAN_SCRAP_REF };
			return func(a_ref, false);
		}

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

		// Pieces of the world itself reported scrapped this session, or on their way to be: by the
		// destroyed sink (which may run off the game thread), or found scrapped in the save by the walk.
		// Each is reported once: the server keeps one report, and a second would only be a message for
		// nothing. One that isn't sent after all (not in a session, or gone before its turn) is let go,
		// to be reported when the walk finds it.
		std::mutex                        sentScrapsLock;
		std::unordered_set<std::uint32_t> sentScraps;

		// False if it was reported, or is on its way, already.
		bool NoteWorldScrap(std::uint32_t a_id)
		{
			std::scoped_lock l{ sentScrapsLock };
			return sentScraps.insert(a_id).second;
		}

		void ForgetWorldScrap(std::uint32_t a_id)
		{
			std::scoped_lock l{ sentScrapsLock };
			sentScraps.erase(a_id);
		}

		// False if it's dropped (too many waiting to be sent).
		bool Push(const Protocol::WorkshopItem& a_item)
		{
			const auto worldScrap = a_item.op == Protocol::WorkshopOp::kScrapped && Protocol::IsShareableRef(a_item.refId) ? a_item.refId : 0;
			std::scoped_lock l{ outLock };
			if (outgoing.size() >= MAX_OUTGOING) {
				return false;
			}
			outgoing.push_back({ Protocol::Encode(a_item, Protocol::MessageType::kReportWorkshopItem), worldScrap });
			++reported;
			return true;
		}

		// False if nothing was reported.
		bool ReportRef(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || IsCopy(a_ref)) {
				return false;
			}
			if ((base->GetFormID() >> 24) == 0xFF) {
				return false;  // a base made at runtime exists only here
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
			return Push(item);
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
				const auto id = ref ? ref->GetFormID() : 0;
				const bool world = Protocol::IsShareableRef(id);
				if (world && !NoteWorldScrap(id)) {
					return RE::BSEventNotifyControl::kContinue;  // reported already
				}
				if (!ReportRef(ref, a_event.workshop.get(), Protocol::WorkshopOp::kScrapped) && world) {
					ForgetWorldScrap(id);
				}
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

		// The cell a piece stands in, from its workbench's interior or worldspace and its position: what
		// waits there is tried again when that cell attaches. Nowhere if its workbench isn't in this game.
		WorkshopCells::Where WhereOf(const Protocol::WorkshopItem& a_item)
		{
			const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
			const auto cell = workshop ? workshop->GetParentCell() : nullptr;
			if (!cell) {
				return {};
			}
			if (cell->IsInterior()) {
				return WorkshopCells::Interior(cell->GetFormID());
			}
			return cell->worldSpace ? WorkshopCells::Exterior(cell->worldSpace->GetFormID(), a_item.position[0], a_item.position[1]) : WorkshopCells::Where{};
		}

		// Where a scrapped piece of the world waits: the cell its reference stands in here, while the
		// reference is in memory (a persistent one, or one whose cell is loaded). Its player may have moved
		// it before scrapping it, and the scrap then tells where they moved it to, maybe cells away, while
		// ours stands where it always stood. Otherwise, where the scrap tells; the scrap waits for its
		// reference too, and is tried again when a cell holding it attaches (TryPendingScrapsIn).
		WorkshopCells::Where WhereScrapped(const Protocol::WorkshopItem& a_item)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.refId);
			const auto cell = ref ? ref->GetParentCell() : nullptr;
			if (!cell) {
				return WhereOf(a_item);
			}
			if (cell->IsInterior()) {
				return WorkshopCells::Interior(cell->GetFormID());
			}
			return cell->worldSpace ? WorkshopCells::Exterior(cell->worldSpace->GetFormID(), ref->data.location.x, ref->data.location.y) : WhereOf(a_item);
		}

		void SetTransform(RE::TESObjectREFR* a_ref, const Protocol::WorkshopItem& a_item)
		{
			a_ref->SetLocationOnReference({ a_item.position[0], a_item.position[1], a_item.position[2] });
			a_ref->SetAngleOnReference({ a_item.rotation[0], a_item.rotation[1], a_item.rotation[2] });
			if (const auto scale = RemoteEdits::ScaleToApply(a_item, a_ref->refScale / 100.0f)) {  // a friend's message can say any size
				a_ref->SetScale(*scale);
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

		// A friend named something that is no workbench here as the workbench of their build or scrap: a
		// door, a vendor's chest, a building. Dropped, and the first of each said in the log.
		void RefuseAnchor(const Protocol::WorkshopItem& a_item, const RE::TESObjectREFR* a_anchor)
		{
			++anchorRefused;
			if (badAnchorsWarned.size() >= MAX_BAD_ANCHORS_WARNED) {
				badAnchorsWarned.clear();
			}
			if (badAnchorsWarned.insert(a_item.workshop).second) {
				const auto base = a_anchor ? a_anchor->GetObjectReference() : nullptr;
				REX::WARN("WorkshopSync: player {} named {:08X} (base {:08X}) as a workbench; it is none here, so their {} of {:08X} is dropped", a_item.playerId, a_item.workshop,
					base ? base->GetFormID() : 0, a_item.op == Protocol::WorkshopOp::kScrapped ? "scrap" : "build", a_item.refId);
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
			if (const auto anchor = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop); !IsWorkbenchAnchor(anchor)) {
				RefuseAnchor(a_item, anchor);
				return true;  // dropped, as one out of reach is
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
			if (const auto scale = RemoteEdits::ScaleToApply(a_item, 1.0f)) {
				ref->SetScale(*scale);
			}
			copies[a_key] = { handle, Clock::now() };
			NoteCopy(handle);
			++placed;
			return true;
		}

		// Whether the reference holds anything in our game: its inventory, once our game has made one (the
		// first time anything touches it), is not empty.
		bool HoldsItems(RE::TESObjectREFR* a_ref)
		{
			const auto list = a_ref->inventoryList;
			if (!list) {
				return false;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			return !list->data.empty();
		}

		// Defined with the walk below.
		std::uint32_t          SpaceOf(const RE::TESObjectCELL* a_cell);
		bool                   HasScripts(RE::TESObjectREFR* a_ref);
		RemoteEdits::BuildArea BuildAreaState(const RE::TESObjectREFR& a_bench);

		// A piece of the world itself (the junk a settlement starts with) that its player scrapped: it goes
		// from our world too. False until it's loaded here (or, while its workbench's build-area volumes
		// aren't, until they are). Only what workshop mode could have scrapped: named with a real workbench,
		// in that workbench's cell or worldspace and inside its build area, the same object (base) as theirs,
		// something the game lets a player scrap, and nothing alive, a quest's, switched by an enable parent
		// or with a script on it (the same pieces the host's own walk leaves alone, WhyNotScrapped). What our
		// copy holds goes to our copy of the workbench first, as the game's own scrap does: their scrap gave
		// them its contents, ours would have vanished with it (a dresser we keep our stash in).
		bool TryScrapWorldRef(const Protocol::WorkshopItem& a_item)
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
			const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
			if (!IsWorkbenchAnchor(workshop)) {
				RefuseAnchor(a_item, workshop);
				return true;
			}
			const auto base = ref->GetObjectReference();
			const auto extra = ref->extraList.get();
			const bool quest = extra && extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			const bool switched = extra && extra->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent);
			if (!base || base->GetFormID() != a_item.base || ref->As<RE::Actor>() || !Buildable(base) || quest || switched || !CanScrap(*ref) ||
				SpaceOf(workshop->GetParentCell()) != SpaceOf(ref->GetParentCell()) || HasScripts(ref)) {
				++scrapRefused;
				return true;
			}
			const auto area = BuildAreaState(*workshop);
			const bool inArea = (area == RemoteEdits::BuildArea::kLoaded || area == RemoteEdits::BuildArea::kPartlyLoaded) &&
			                    RE::Workshop::IsLocationWithinBuildableArea(*workshop, ref->data.location);
			switch (RemoteEdits::ScrapReach(area, inArea, workshop->data.location.GetDistance(ref->data.location))) {
			case RemoteEdits::Verdict::kWait:
				scrapsDeferred.insert(a_item.refId);
				return false;
			case RemoteEdits::Verdict::kRefuse:
				++scrapRefused;
				return true;
			default:
				break;
			}
			if (HoldsItems(ref)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems {:08X}", ref->GetFormID(), workshop->GetFormID()).c_str());
				REX::INFO("WorkshopSync: a friend scrapped {:08X}; what it held here went to workbench {:08X}", ref->GetFormID(), workshop->GetFormID());
			}
			ref->Disable();
			++worldScrapped;
			return true;
		}

		bool ScrapWorldRef(const Protocol::WorkshopItem& a_item)
		{
			if (!TryScrapWorldRef(a_item)) {
				return false;
			}
			scrapsDeferred.erase(a_item.refId);
			return true;
		}

		void TryPending(const std::vector<Key>& a_keys)
		{
			for (const auto& key : a_keys) {
				const auto found = pending.Find(key);
				if (!found) {
					continue;  // done with already (a key can come up twice)
				}
				const auto item = *found;
				if (item.op == Protocol::WorkshopOp::kScrapped ? ScrapWorldRef(item) : Place(key, item)) {
					pending.Erase(key);
				}
			}
		}

		// Tries again what waits in, or next to, a cell that just attached.
		void TryPendingNear(const WorkshopCells::Where& a_cell)
		{
			TryPending(pending.Near(a_cell));
		}

		// Tries again the scraps that wait for one of the references of a cell that just attached,
		// whatever cell they were filed under. A scrap tells where its player's piece stood when they
		// scrapped it: if they had moved it first, maybe cells from where ours stands, and a reference
		// that isn't persistent can't be looked up, to find where ours stands, until its cell loads.
		void TryPendingScrapsIn(std::uint32_t a_cell)
		{
			if (!pending.WaitsForRefs()) {
				return;
			}
			const auto cell = RE::TESForm::GetFormByID<RE::TESObjectCELL>(a_cell);
			if (!cell || !cell->IsAttached()) {
				return;
			}
			std::vector<Key> keys;
			{
				const RE::BSAutoLock l{ cell->spinLock };
				for (const auto& ref : cell->references) {
					if (ref && Protocol::IsShareableRef(ref->GetFormID())) {
						const auto waiting = pending.For(ref->GetFormID());
						keys.insert(keys.end(), waiting.begin(), waiting.end());
					}
				}
			}
			TryPending(keys);  // outside the cell's lock: a scrap switches the reference off
		}

		// ---- Cells as they attach -----------------------------------------------------------------

		constexpr auto CELL_LOOK_INTERVAL = std::chrono::seconds(1);

		struct AttachedCell
		{
			std::uint32_t        id;
			WorkshopCells::Where where;
		};

		std::set<std::uint32_t> attachedCells;  // the cells attached at the last look
		Clock::time_point       nextCellLook{};

		// The cells attached since the last look (after a game load, all of them).
		std::vector<AttachedCell> NewlyAttachedCells()
		{
			std::vector<AttachedCell> fresh;
			const auto                tes = RE::TES::GetSingleton();
			if (!tes) {
				return fresh;
			}
			std::set<std::uint32_t> attached;
			const auto note = [&](RE::TESObjectCELL* a_cell) {
				const auto id = a_cell->GetFormID();
				attached.insert(id);
				if (attachedCells.contains(id)) {
					return;
				}
				WorkshopCells::Where where;
				if (a_cell->IsInterior()) {
					where = WorkshopCells::Interior(id);
				} else if (const auto exterior = a_cell->cellData.exterior; exterior && a_cell->worldSpace) {
					where = { a_cell->worldSpace->GetFormID(), exterior->cellX, exterior->cellY };
				}
				fresh.push_back({ id, where });
			};
			if (tes->interiorCell) {
				note(tes->interiorCell);
			} else if (const auto grid = tes->gridCells) {
				for (std::uint32_t x = 0; x < grid->dimension; ++x) {
					for (std::uint32_t y = 0; y < grid->dimension; ++y) {
						const auto gridCell = grid->Get(x, y);
						const auto cell = gridCell ? gridCell->cell : nullptr;
						if (cell && cell->IsAttached()) {
							note(cell);
						}
					}
				}
			}
			attachedCells = std::move(attached);
			return fresh;
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
		// Nor is everything deleted: quests and scripts delete things too, and what a friend's game takes
		// away stays away from their save for good, so only what the game itself would let a player scrap,
		// inside a workbench's build area, counts (see WhyNotScrapped).
		//
		// The save has no list of the pieces scrapped. The workbench's workshop data has a deletedItems
		// list, but that holds base objects with counts: the workshop's stored items (Workshop::DestroyItem
		// adds the base of a piece stored in workshop mode, Workshop::AddStoredItem adds to it, and
		// building from the store takes one back through UseDeletedItem). It names no reference.

		constexpr std::size_t WALK_PER_FRAME = 512;  // references looked at per frame
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

		struct Counts
		{
			std::uint32_t refs = 0, disabled = 0, deleted = 0, benches = 0;
		};

		std::set<std::uint32_t>                             walkedWorlds;  // worldspaces whose persistent references were walked to the end
		std::deque<CellWalk>                                toWalk;
		std::vector<std::uint32_t>                          walkRefs;  // the first queued cell's references, taken when its walk starts
		std::size_t                                         walkAt = 0;  // the next of them to look at
		bool                                                walkStarted = false;
		std::vector<Bench>                                  benches;  // workbenches seen
		std::unordered_map<std::uint32_t, std::uint32_t>    waiting;  // scrapped, with no workbench's build area around it seen yet: reference -> its space
		std::deque<std::pair<std::uint32_t, std::uint32_t>> toSend;   // (reference, its workbench)
		Clock::time_point                                   lastRefill{};
		double                                              sendTokens = 0.0;

		std::uint32_t savedScraps = 0;  // reported from the walk
		Counts        seen;             // by the walk: references of the game's files, switched off, also deleted, workbenches
		Counts        roundStart;       // `seen` when the walk's queue last ran dry, for the log

		void ClearWalk()
		{
			attachedCells.clear();
			walkedWorlds.clear();
			toWalk.clear();
			walkRefs.clear();
			walkAt = 0;
			walkStarted = false;
			benches.clear();
			for (const auto& [ref, space] : waiting) {
				ForgetWorldScrap(ref);
			}
			waiting.clear();
			for (const auto& [ref, bench] : toSend) {
				ForgetWorldScrap(ref);
			}
			toSend.clear();
			roundStart = seen;
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

		// Whether a script is attached to the reference (from its base, or of its own): a quest's or a
		// trigger's object, or a settlement object a script keeps count of. A plain ObjectReference
		// object is only Papyrus holding the reference somewhere (an event about it), not a script on it.
		bool HasScripts(RE::TESObjectREFR* a_ref)
		{
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				return false;
			}
			const auto& handles = vm->GetObjectHandlePolicy();
			const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESObjectREFR>(), a_ref);
			if (handle == handles.EmptyHandle()) {
				return false;
			}
			bool                                                                                      scripted = false;
			const RE::BSTThreadScrapFunction<RE::BSContainer::ForEachResult(RE::BSScript::Object*)> visit{ [&](RE::BSScript::Object* a_object) {
				const auto type = a_object ? a_object->type.get() : nullptr;
				if (type && _stricmp(type->GetName(), "ObjectReference") != 0) {
					scripted = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			} };
			vm->ForEachBoundObject(handle, visit);
			return scripted;
		}

		// Why the reference isn't junk scrapped in workshop mode, or nullptr if it is. Scrapping switches a
		// piece of the world off and marks it deleted; the game must let a player scrap it, and a
		// friend's game take it away (see ScrapWorldRef). What a quest or a script deleted isn't: a
		// reference a quest holds, one an enable parent switches, one with a script attached. Nor loose
		// items: taken or scrapped, we can't tell which, and what players pick up isn't the workshop's
		// business.
		const char* WhyNotScrapped(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref->IsDisabled() || !a_ref->IsDeleted()) {
				return "not switched off and deleted";
			}
			const auto base = a_ref->GetObjectReference();
			if (!base || (base->GetFormID() >> 24) == 0xFF || a_ref->As<RE::Actor>() || base->Is(RE::ENUM_FORM_ID::kMISC) || !Buildable(base)) {
				return "not a piece workshop mode builds or scraps";
			}
			const auto extra = a_ref->extraList.get();
			if (extra && extra->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent)) {
				return "an enable parent switches it";
			}
			if (extra && extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray)) {
				return "a quest holds it";
			}
			if (!CanScrap(*a_ref)) {
				return "the game doesn't let a player scrap it";
			}
			if (HasScripts(a_ref)) {
				return "a script is attached to it";
			}
			return nullptr;
		}

		// What links a build-area volume (a primitive) to its workbench: the form of the default object
		// WorkshopLinkedPrimitiveKeyword, as Workshop::IsLocationWithinBuildableArea reads it.
		constexpr REL::ID BUILD_AREA_KEYWORD{ 4797312 };
		// How far a build area reaches from its workbench while none of its volumes is loaded.
		constexpr float   BUILD_AREA_RADIUS = RemoteEdits::BUILD_AREA_RADIUS;

		// Whether build-area volumes are linked to the workbench, and how many of them are loaded. The game's
		// own test then decides by the loaded ones alone. While none is, it falls back to a radius, but around the
		// settlement the player is at in workshop mode (its location's marker and radius) whichever workbench
		// it is asked about, and around the workbench only outside workshop mode.
		RemoteEdits::BuildArea BuildAreaState(const RE::TESObjectREFR& a_bench)
		{
			static REL::Relocation<RE::BGSKeyword**> keyword{ BUILD_AREA_KEYWORD };
			const auto children = a_bench.extraList ? a_bench.extraList->GetByType<RE::ExtraLinkedRefChildren>() : nullptr;
			if (!children || !*keyword) {
				return RemoteEdits::BuildArea::kNoVolumes;
			}
			std::size_t linked = 0, loaded = 0;
			for (const auto& child : children->linkedChildren) {
				if (child.keyword != *keyword) {
					continue;
				}
				++linked;
				const auto volume = child.REFR.get();
				loaded += volume && volume->extraList && volume->extraList->GetPrimitive();
			}
			if (linked == 0) {
				return RemoteEdits::BuildArea::kNoVolumes;
			}
			return loaded == 0 ? RemoteEdits::BuildArea::kNotLoaded : (loaded < linked ? RemoteEdits::BuildArea::kPartlyLoaded : RemoteEdits::BuildArea::kLoaded);
		}

		bool BuildAreaLoaded(const RE::TESObjectREFR& a_bench)
		{
			const auto area = BuildAreaState(a_bench);
			return area == RemoteEdits::BuildArea::kLoaded || area == RemoteEdits::BuildArea::kPartlyLoaded;
		}

		// Whether the spot is in the workbench's build area: inside one of the build-area volumes linked
		// to it (the game's own test, Workshop::IsLocationWithinBuildableArea), or, while none of them is
		// loaded, within BUILD_AREA_RADIUS of it (the game's fallback outside workshop mode, measured here
		// so the settlement the player is building at doesn't stand in for every workbench).
		bool InBuildArea(const RE::TESObjectREFR& a_bench, const RE::NiPoint3& a_at)
		{
			if (BuildAreaLoaded(a_bench)) {
				return RE::Workshop::IsLocationWithinBuildableArea(a_bench, a_at);
			}
			return a_bench.data.location.GetDistance(a_at) <= BUILD_AREA_RADIUS;
		}

		// The workbench whose build area the piece stands in (0 if none has been seen).
		std::uint32_t BenchFor(const RE::TESObjectREFR* a_ref, std::uint32_t a_space)
		{
			if (a_space == 0) {
				return 0;
			}
			for (const auto& bench : benches) {
				if (bench.space != a_space || bench.at.GetDistance(a_ref->data.location) > BUILD_REACH) {
					continue;
				}
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(bench.id);
				if (ref && InBuildArea(*ref, a_ref->data.location)) {
					return bench.id;
				}
			}
			return 0;
		}

		// Queues a piece already noted (NoteWorldScrap) for reporting if it stands in a workbench's build
		// area; true when it's done with (queued, or gone from the game: let go, and found again when its
		// cell attaches).
		bool QueueScrap(std::uint32_t a_id, std::uint32_t a_space)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref) {
				ForgetWorldScrap(a_id);
				return true;
			}
			const auto bench = BenchFor(ref, a_space);
			if (bench == 0) {
				return false;
			}
			toSend.emplace_back(a_id, bench);
			return true;
		}

		// a_space: the walked cell's (a worldspace's persistent references belong to the worldspace).
		void Examine(RE::TESObjectREFR* a_ref, std::uint32_t a_space, bool a_benchesOnly)
		{
			const auto id = a_ref->GetFormID();
			++seen.refs;
			if (!a_ref->IsDisabled()) {
				if (IsBench(a_ref) && std::ranges::none_of(benches, [&](const Bench& a_bench) { return a_bench.id == id; })) {
					benches.push_back({ id, a_space, a_ref->data.location });
					++seen.benches;
				}
				return;
			}
			if (a_benchesOnly) {
				return;
			}
			++seen.disabled;
			if (!a_ref->IsDeleted()) {
				return;
			}
			++seen.deleted;
			if (WhyNotScrapped(a_ref) || !NoteWorldScrap(id)) {
				return;  // not scrapped junk; or reported already, on its way, or waiting
			}
			if (!QueueScrap(id, a_space)) {
				if (waiting.size() < MAX_WAITING) {
					waiting.emplace(id, a_space);
				} else {
					ForgetWorldScrap(id);
				}
			}
		}

		// Queues the worldspace's persistent references (the workbenches are among them), until they have
		// been walked to the end, and the cells that attached since the last look.
		void QueueWalks(const std::vector<AttachedCell>& a_fresh)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto world = tes && !tes->interiorCell ? tes->worldSpace : nullptr;
			if (world && world->persistentCell && !walkedWorlds.contains(world->GetFormID()) &&
				std::ranges::none_of(toWalk, [&](const CellWalk& a_walk) { return a_walk.persistent && a_walk.id == world->GetFormID(); })) {
				toWalk.push_back({ world->GetFormID(), true });
			}
			for (const auto& cell : a_fresh) {
				toWalk.push_back({ cell.id, false });
			}
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
			// Workbenches seen in this round may have pieces found before them in their build area.
			std::erase_if(waiting, [](const auto& a_entry) { return QueueScrap(a_entry.first, a_entry.second); });
			const Counts round{ seen.refs - roundStart.refs, seen.disabled - roundStart.disabled, seen.deleted - roundStart.deleted, seen.benches - roundStart.benches };
			if (round.deleted > 0 || round.benches > 0) {
				REX::INFO("WorkshopSync: walked {} references: {} switched off, {} of them deleted; {} new workbenches; {} scraps to report, {} in no build area seen",
					round.refs, round.disabled, round.deleted, round.benches, toSend.size(), waiting.size());
			}
			roundStart = seen;
		}

		// Done with the first queued walk: to the end, or given up (its cell went, or the worldspace
		// changed: a cell is queued again when it attaches again, the worldspace when we're back).
		void NextWalk(bool a_finished)
		{
			if (a_finished && toWalk.front().persistent) {
				walkedWorlds.insert(toWalk.front().id);
			}
			toWalk.pop_front();
			walkRefs.clear();
			walkAt = 0;
			walkStarted = false;
			if (toWalk.empty()) {
				FinishRound();
			}
		}

		// Walks up to WALK_PER_FRAME references. A cell's references are taken (their IDs) when its walk
		// starts, under its lock: the cell's own list changes as references come and go, so a place in it
		// means nothing a frame later. Each is looked up again when its turn comes.
		void WalkSome()
		{
			std::size_t budget = WALK_PER_FRAME;
			while (budget > 0 && !toWalk.empty()) {
				const auto walk = toWalk.front();
				const auto cell = CellOf(walk);
				if (!cell) {
					NextWalk(false);
					continue;
				}
				if (!walkStarted) {
					walkStarted = true;
					const RE::BSAutoLock l{ cell->spinLock };
					walkRefs.reserve(cell->references.size());
					for (const auto& ref : cell->references) {
						if (ref && Protocol::IsShareableRef(ref->GetFormID())) {
							walkRefs.push_back(ref->GetFormID());  // not one made at runtime: ours, a copy, something dropped
						}
					}
				}
				const auto space = walk.persistent ? walk.id : SpaceOf(cell);
				for (; budget > 0 && walkAt < walkRefs.size(); --budget) {
					const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(walkRefs[walkAt++]) };
					if (ref) {
						Examine(ref.get(), space, walk.persistent);
					}
				}
				if (walkAt >= walkRefs.size()) {
					NextWalk(true);
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
				if (!ref || !bench || !ReportRef(ref, bench, Protocol::WorkshopOp::kScrapped)) {
					ForgetWorldScrap(refId);  // gone before its turn, or not sent: found again when its cell attaches
					continue;
				}
				++savedScraps;
				sendTokens -= 1.0;
			}
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

	bool IsWorkbenchAnchor(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || a_ref->As<RE::Actor>()) {
			return false;
		}
		if (a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kWorkshop)) {
			return true;
		}
		// A workbench of the game's files never visited in this save may not have its workshop data yet.
		const auto base = a_ref->GetObjectReference();
		const auto keywords = base ? base->As<RE::BGSKeywordForm>() : nullptr;
		return keywords && keywords->HasKeywordID(WORKSHOP_KEYWORD);
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
		if (now >= nextCellLook) {
			nextCellLook = now + CELL_LOOK_INTERVAL;
			const auto fresh = NewlyAttachedCells();
			// The host's world is the session's: a friend's save may have scrapped other things, and those
			// stay theirs.
			if (a_host) {
				QueueWalks(fresh);
			}
			for (const auto& cell : fresh) {
				TryPendingNear(cell.where);
				TryPendingScrapsIn(cell.id);
			}
		}
		if (a_host) {
			WalkSome();
			SendSome(now);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending)
	{
		std::vector<Outgoing> taken;
		{
			std::scoped_lock l{ outLock };
			taken = std::exchange(outgoing, {});
		}
		std::vector<std::vector<std::uint8_t>> packets;
		for (auto& out : taken) {
			if (a_sending) {
				packets.push_back(std::move(out.packet));
			} else if (out.worldScrap != 0) {
				ForgetWorldScrap(out.worldScrap);
			}
		}
		return packets;
	}

	void Apply(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kScrapped) {
			pending.Erase(key);
			if (const auto it = copies.find(key); it != copies.end()) {
				Remove(it->second);
				ForgetCopy(it->second.ref);
				copies.erase(it);
				++scrapped;
			}
			// A piece of the world itself goes from our world too (one its player had moved has a copy
			// here as well, gone above).
			if (Protocol::IsShareableRef(a_item.refId) && !ScrapWorldRef(a_item)) {
				pending.Put(WhereScrapped(a_item), key, a_item, a_item.refId);
			}
			return;
		}
		if (!Place(key, a_item)) {
			pending.Put(WhereOf(a_item), key, a_item);
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
		pending.Clear();
		ClearWalk();
		scrapsDeferred.clear();
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
			       worldScrapped, adopted, refused, pending.Size()) +
		       std::format(" savedScraps={} (walked={} disabled={} deleted={} benches={} waiting={} queued={})", savedScraps, seen.refs, seen.disabled, seen.deleted, benches.size(),
				   waiting.size(), toSend.size()) +
		       std::format(" anchorRefused={} scrapRefused={} scrapDeferred={}", anchorRefused, scrapRefused, scrapsDeferred.size());
	}

	std::string DescribeBenches(RE::TESObjectREFR* a_ref)
	{
		if (a_ref && !IsBench(a_ref)) {
			// What the walk makes of this reference.
			if (const auto why = WhyNotScrapped(a_ref)) {
				return std::format("{:08X}: not reported: {}", a_ref->GetFormID(), why);
			}
			const auto bench = BenchFor(a_ref, SpaceOf(a_ref->GetParentCell()));
			return bench ? std::format("{:08X}: scrapped junk in the build area of workbench {:08X}", a_ref->GetFormID(), bench) :
			               std::format("{:08X}: scrapped junk, but in no build area of a workbench seen", a_ref->GetFormID());
		}
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
		const auto  player = RE::PlayerCharacter::GetSingleton();
		std::string out = std::format("{} workbenches", refs.size());
		for (const auto ref : refs) {
			const auto& at = ref->data.location;
			const bool  inside = player && SpaceOf(player->GetParentCell()) == SpaceOf(ref->GetParentCell()) && InBuildArea(*ref, player->data.location);
			out += std::format("; {:08X} at {:.0f} {:.0f} {:.0f}, {:.0f} away{}", ref->GetFormID(), at.x, at.y, at.z, player ? at.GetDistance(player->data.location) : 0.0f,
				inside ? ", you are in its build area" : "");
		}
		return out;
	}
}
