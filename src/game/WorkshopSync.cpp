#include "game/WorkshopSync.h"

#include "game/BuiltWalk.h"
#include "game/Drops.h"
#include "game/EngineRefs.h"
#include "game/HostRules.h"
#include "game/RemoteEdits.h"
#include "game/SavedScraps.h"
#include "game/SaveStrays.h"
#include "game/SendPacer.h"
#include "game/WorkshopApply.h"
#include "game/WorkshopCells.h"
#include "game/WorkshopInbox.h"
#include "game/WorkshopMove.h"
#include "game/WorkshopPower.h"
#include "game/WorkshopSettle.h"

#include "game/DeferredDelete.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/MenuWatch.h"
#include "game/Papyrus.h"
#include "Text.h"

// A grid's links are kept in sets (BSTSet<PowerUtils::GridConnection>), which CommonLibF4 gives no equality: the same
// connection to the same piece, through the same connector.
template <>
struct std::equal_to<RE::PowerUtils::GridConnection>
{
	bool operator()(const RE::PowerUtils::GridConnection& a_lhs, const RE::PowerUtils::GridConnection& a_rhs) const noexcept
	{
		return a_lhs.connection == a_rhs.connection && a_lhs.connector == a_rhs.connector;
	}
};

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
		// Reports wait here and go a few hundred a second (see SendPacer): a blueprint or a scrap-all of a big
		// settlement, sent at once, went past the server's event budget, and it dropped the excess.
		SendPacer::Queue<Outgoing>                          outgoing{ SendPacer::WORKSHOP_PER_SECOND, SendPacer::WORKSHOP_QUEUED };  // under outLock
		std::uint32_t                                       dropped = 0;  // reports dropped with the queue full (under outLock)
		bool                                                droppedWarned = false;  // since the queue last ran empty (under outLock)
		std::map<Key, Copy>                                 copies;
		WorkshopApply::Twins<Key>                           twins;  // reports another friend's copy stands for (WorkshopApply::TwinOf)
		WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>   inbox;  // other players' reports to place, and those waiting for their cell to load
		std::vector<Copy>                                   graveyard;
		constexpr float                                     ADOPT_RADIUS = 4.0f;  // a copy left by an earlier session, at the same spot
		// How far from its workbench a piece may stand. The largest build area in the game (Spectacle
		// Island) reaches about 9,000 units from its bench; a piece claimed farther away is someone placing
		// things in the open world under a settlement's name.
		constexpr float                                     BUILD_REACH = 12000.0f;

		std::uint32_t reported = 0;
		WorkshopApply::Counts counts;  // what other players' reports did here
		std::uint32_t worldScrapped = 0;  // pieces of the world itself (a settlement's junk) scrapped by someone else
		std::uint32_t anchorRefused = 0;  // named something that is no workbench here as their workbench
		std::uint32_t scrapRefused = 0;   // scraps of the world refused: not junk workshop mode scraps, or outside the build area
		// Scraps waiting for their workbench's build-area volumes to load (the game can't tell whether the
		// piece is inside until they are): tried again as cells attach.
		std::unordered_set<std::uint32_t> scrapsDeferred;
		// Anchors that were no workbench, warned about once each (cleared when full, so it stays small).
		std::unordered_set<std::uint32_t> badAnchorsWarned;
		constexpr std::size_t             MAX_BAD_ANCHORS_WARNED = 64;
		std::uint32_t itemsRescued = 0;   // copies of containers that held what we kept in them, emptied into our inventory first
		std::unordered_set<std::uint32_t> stockedBases;  // container bases refused for what they hold (logged once each)
		std::uint32_t                     unplaceable = 0;  // reports of a base we don't have, or that workshop mode doesn't build
		std::unordered_set<std::uint32_t> unplaceableBases;  // logged once each
		std::int64_t  applyUsMax = 0;     // the longest frame's placing, in microseconds

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
		// destroyed sink (which may run off the game thread), or found scrapped in the save by the walk
		// (SavedScraps::Ledger: each once).
		SavedScraps::Ledger sentScraps;

		// False if it's dropped (too many waiting to be sent).
		bool Push(const Protocol::WorkshopItem& a_item)
		{
			const auto worldScrap = a_item.op == Protocol::WorkshopOp::kScrapped && Protocol::IsShareableRef(a_item.refId) ? a_item.refId : 0;
			std::scoped_lock l{ outLock };
			if (!outgoing.Push({ Protocol::Encode(a_item, Protocol::MessageType::kReportWorkshopItem), worldScrap })) {
				++dropped;
				if (!std::exchange(droppedWarned, true)) {
					REX::WARN("WorkshopSync: {} reports wait to be sent; dropping more until they go (the others won't see those pieces)", outgoing.Size());
				}
				return false;
			}
			++reported;
			return true;
		}

		// WorkshopItemKeyword: what links each of a settlement's workshop items to its workbench.
		constexpr std::uint32_t WORKSHOP_ITEM_KEYWORD = 0x00054BA6;

		// A power wire's shape, from its ExtraBendableSplineParams (whose members CommonLibF4 keeps
		// private): slack, thickness and the half extents from 0x18, whether an end hangs loose at 0x2C (a
		// wire still being strung). None if it has none, or an end is loose.
		std::optional<Protocol::WireShape> WireShapeOf(const RE::TESObjectREFR& a_wire)
		{
			const auto params = a_wire.extraList ? a_wire.extraList->GetByType<RE::ExtraBendableSplineParams>() : nullptr;
			if (!params) {
				return std::nullopt;
			}
			const auto bytes = reinterpret_cast<const std::uint8_t*>(params);
			const auto raw = reinterpret_cast<const float*>(bytes + 0x18);
			if (bytes[0x2C] != 0) {
				return std::nullopt;
			}
			Protocol::WireShape shape;
			shape.slack = raw[0];
			shape.thickness = raw[1];
			shape.halfExtents[0] = raw[2];
			shape.halfExtents[1] = raw[3];
			shape.halfExtents[2] = raw[4];
			return Protocol::Sane(shape) ? std::optional{ shape } : std::nullopt;
		}

		bool IsWire(const RE::TESBoundObject* a_base)
		{
			return a_base && a_base->Is(RE::ENUM_FORM_ID::kBNDS);
		}

		// The workbench the reference is linked to as one of its workshop items, if any.
		RE::TESObjectREFR* BenchOf(const RE::TESObjectREFR& a_ref)
		{
			const auto linked = a_ref.extraList ? a_ref.extraList->GetByType<RE::ExtraLinkedRef>() : nullptr;
			if (!linked) {
				return nullptr;
			}
			for (const auto& link : linked->linkedRefs) {
				if (link.keyword && link.keyword->GetFormID() == WORKSHOP_ITEM_KEYWORD && link.REFR) {
					return link.REFR;
				}
			}
			return nullptr;
		}

		// The wires strung to the reference (its power links that are wires).
		std::vector<RE::NiPointer<RE::TESObjectREFR>> WiresOf(const RE::TESObjectREFR& a_ref)
		{
			std::vector<RE::NiPointer<RE::TESObjectREFR>> wires;
			const auto links = a_ref.extraList ? a_ref.extraList->GetByType<RE::ExtraPowerLinks>() : nullptr;
			if (!links) {
				return wires;
			}
			for (const auto& link : links->powerLinks) {
				RE::NiPointer<RE::TESObjectREFR> other{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(link.formID) };
				if (other && IsWire(other->GetObjectReference())) {
					wires.push_back(other);
				}
			}
			return wires;
		}

		// False if nothing was reported.
		bool ReportRef(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || IsCopy(a_ref) || Drops::IsCopyRef(a_ref->GetFormID())) {
				return false;  // a copy of a friend's piece, or of a friend's drop: theirs, not something we built
			}
			if ((base->GetFormID() >> 24) == 0xFF) {
				return false;  // a base made at runtime exists only here
			}
			if (!WorkshopApply::Shared(true, Equipment::IsPowerArmorFrame(base))) {
				return false;  // a frame moved in workshop mode: RemotePlayers shares it, as parked
			}
			std::optional<Protocol::WireShape> wire;
			if (IsWire(base) && a_op == Protocol::WorkshopOp::kPlaced) {
				wire = WireShapeOf(*a_ref);
				if (!wire) {
					return false;  // still being strung, or a shape no friend's game would take
				}
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
			item.wire = wire;
			return Push(item);
		}

		// Pieces placed or moved, looked at again over the next seconds (WorkshopSettle): reported again where the
		// game ends up putting them. Noted by the event sinks (which may run off the game thread) under the lock, and
		// looked at by Frame.
		std::mutex            settleLock;
		WorkshopSettle::Watch settle;  // under settleLock
		std::uint32_t         settledAgain = 0;
		std::uint32_t         putBack = 0;  // pieces drawn where they stand again (WorkshopSettle::PutBack)

		WorkshopMove::Transform TransformOf(const RE::TESObjectREFR& a_ref)
		{
			return { { a_ref.data.location.x, a_ref.data.location.y, a_ref.data.location.z }, { a_ref.data.angle.x, a_ref.data.angle.y, a_ref.data.angle.z }, a_ref.refScale / 100.0f };
		}

		// A piece of ours just reported placed or moved (no wire: its ends tell where it goes).
		void SettleLater(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || !a_workshop || IsWire(base)) {
				return;
			}
			std::scoped_lock l{ settleLock };
			settle.Reported(a_ref->GetFormID(), a_workshop->GetFormID(), TransformOf(*a_ref), Clock::now());
		}

		struct SettleEngine
		{
			std::optional<WorkshopMove::Transform> TransformOf(std::uint32_t a_ref)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ref);
				if (!ref || ref->IsDisabled() || ref->IsDeleted()) {
					return std::nullopt;
				}
				if (const auto root = ref->Get3D()) {
					const auto base = ref->GetObjectReference();
					const bool loose = ref->As<RE::Actor>() || !base || base->Is(RE::ENUM_FORM_ID::kMISC) || IsWire(base);
					const auto drawn = root->world.translate;
					if (WorkshopSettle::PutBack(drawn.GetDistance(ref->data.location), MenuWatch::IsOpen("WorkshopMenu"sv), loose)) {
						REX::INFO("WorkshopSync: {:08X} was left drawn at {:.0f} {:.0f} {:.0f} (workshop mode closed while it was being moved), but stands at {:.0f} {:.0f} {:.0f}: drawn there again",
							a_ref, drawn.x, drawn.y, drawn.z, ref->data.location.x, ref->data.location.y, ref->data.location.z);
						ref->Update3DPosition(true);
						++putBack;
					}
				}
				return WorkshopSync::TransformOf(*ref);
			}

			bool Report(std::uint32_t a_ref, std::uint32_t a_workshop)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ref);
				const auto workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_workshop);
				if (!ref || !ReportRef(ref, workshop, Protocol::WorkshopOp::kPlaced)) {
					return false;
				}
				REX::DEBUG("WorkshopSync: {:08X} stands somewhere else since it was reported: reported again at {:.0f} {:.0f} {:.0f} rz={:.2f}", a_ref, ref->data.location.x,
					ref->data.location.y, ref->data.location.z, ref->data.angle.z);
				++settledAgain;
				return true;
			}
		};

		void CheckSettled(Clock::time_point a_now)
		{
			SettleEngine     engine;
			std::scoped_lock l{ settleLock };
			settle.Check(engine, a_now);
		}

		// Moving a piece moves the wires strung to it, but the game tells of no move of theirs: they are
		// reported again once the game has redrawn them (a moment after the move). Taken by the event sinks
		// (which may run off the game thread) under the lock, and reported by Frame.
		constexpr auto WIRE_SETTLE = std::chrono::milliseconds(500);

		struct WireCheck
		{
			std::uint32_t     piece;
			std::uint32_t     workshop;
			Clock::time_point due;
		};

		std::mutex             wireChecksLock;
		std::vector<WireCheck> wireChecks;  // under wireChecksLock

		void CheckWiresLater(const RE::TESObjectREFR* a_piece, const RE::TESObjectREFR* a_workshop)
		{
			if (!a_piece || !a_workshop) {
				return;
			}
			std::scoped_lock l{ wireChecksLock };
			if (wireChecks.size() < 256) {
				wireChecks.push_back({ a_piece->GetFormID(), a_workshop->GetFormID(), Clock::now() + WIRE_SETTLE });
			}
		}

		void ReportMovedWires(Clock::time_point a_now)
		{
			std::vector<WireCheck> due;
			{
				std::scoped_lock l{ wireChecksLock };
				if (wireChecks.empty()) {
					return;
				}
				std::erase_if(wireChecks, [&](const WireCheck& a_check) {
					if (a_check.due > a_now) {
						return false;
					}
					due.push_back(a_check);
					return true;
				});
			}
			for (const auto& check : due) {
				const RE::NiPointer<RE::TESObjectREFR> piece{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(check.piece) };
				const auto                             workshop = RE::TESForm::GetFormByID<RE::TESObjectREFR>(check.workshop);
				if (!piece || !workshop || piece->IsDisabled() || piece->IsDeleted()) {
					continue;
				}
				for (const auto& wire : WiresOf(*piece)) {
					if (!wire->IsDisabled() && !wire->IsDeleted()) {
						ReportRef(wire.get(), workshop, Protocol::WorkshopOp::kPlaced);
					}
				}
			}
		}

		void LogEvent(const char* a_what, const RE::TESObjectREFR* a_ref, const RE::TESObjectREFR* a_workshop)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			REX::DEBUG("WorkshopSync: {} {:08X} base {:08X} ({}) at workbench {:08X}", a_what, a_ref ? a_ref->GetFormID() : 0, base ? base->GetFormID() : 0,
				base ? base->GetFormTypeString() : "none", a_workshop ? a_workshop->GetFormID() : 0);
		}

		class PlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				LogEvent("placed", a_event.placedItem.get(), a_event.workshop.get());
				if (ReportRef(a_event.placedItem.get(), a_event.workshop.get(), Protocol::WorkshopOp::kPlaced)) {
					SettleLater(a_event.placedItem.get(), a_event.workshop.get());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class MovedSink : public RE::BSTEventSink<RE::Workshop::ItemMovedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemMovedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemMovedEvent>*) override
			{
				LogEvent("moved", a_event.movedItem.get(), a_event.workshop.get());
				if (ReportRef(a_event.movedItem.get(), a_event.workshop.get(), Protocol::WorkshopOp::kPlaced)) {
					SettleLater(a_event.movedItem.get(), a_event.workshop.get());
				}
				CheckWiresLater(a_event.movedItem.get(), a_event.workshop.get());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class DestroyedSink : public RE::BSTEventSink<RE::Workshop::ItemDestroyedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemDestroyedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemDestroyedEvent>*) override
			{
				const auto ref = a_event.objectDestroyed.get();
				LogEvent("destroyed", ref, a_event.workshop.get());
				const auto id = ref ? ref->GetFormID() : 0;
				// Our copy of a friend's drop, scrapped: the item is our player's take now (they have its components), so it
				// goes for everyone. Not a workshop piece of ours to report.
				const auto bench = a_event.workshop.get();
				if (Drops::OnScrapped(id, bench ? bench->GetFormID() : 0)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The wires strung to it go with it: the game takes them away and tells of no scrap of theirs.
				if (ref && !IsWire(ref->GetObjectReference())) {
					for (const auto& wire : WiresOf(*ref)) {
						ReportRef(wire.get(), a_event.workshop.get(), Protocol::WorkshopOp::kScrapped);
					}
				}
				const bool world = Protocol::IsShareableRef(id);
				if (world && !sentScraps.Note(id)) {
					return RE::BSEventNotifyControl::kContinue;  // reported already
				}
				if (!ReportRef(ref, a_event.workshop.get(), Protocol::WorkshopOp::kScrapped) && world) {
					sentScraps.Forget(id);
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

		// Whether the exterior cell the piece stands in is attached here. CellFor says only that its workbench (a
		// persistent reference, in memory from anywhere in its worldspace) is in ours: from a town away, what stands at
		// the spot can't be seen (AtSpot walks the attached cells), and a copy made there went over the save's copy, or
		// over a piece of ours, unseen. Two at one spot flicker.
		bool SpotAttached(const Protocol::WorkshopItem& a_item)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto grid = tes ? tes->gridCells : nullptr;
			if (!grid) {
				return false;
			}
			const auto x = WorkshopCells::Grid(a_item.position[0]);
			const auto y = WorkshopCells::Grid(a_item.position[1]);
			for (std::uint32_t i = 0; i < grid->dimension; ++i) {
				for (std::uint32_t j = 0; j < grid->dimension; ++j) {
					const auto gridCell = grid->Get(i, j);
					const auto cell = gridCell ? gridCell->cell : nullptr;
					const auto exterior = cell && cell->IsAttached() ? cell->cellData.exterior : nullptr;
					if (exterior && exterior->cellX == x && exterior->cellY == y) {
						return true;
					}
				}
			}
			return false;
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

		// Where the copy stands, for following its piece (WorkshopApply::Follow); none if it's gone, or in
		// no cell.
		std::optional<WorkshopApply::Standing> StandingOf(const Copy& a_copy)
		{
			const auto ref = a_copy.ref.get();
			const auto cell = ref ? ref->GetParentCell() : nullptr;
			if (!cell) {
				return std::nullopt;
			}
			WorkshopApply::Standing now;
			if (const auto exterior = !cell->IsInterior() ? cell->cellData.exterior : nullptr) {
				now.cell.emplace(exterior->cellX, exterior->cellY);
			}
			const auto& location = ref->data.location;
			const auto& angle = ref->data.angle;
			now.at = { { location.x, location.y, location.z }, { angle.x, angle.y, angle.z }, ref->refScale / 100.0f };
			const auto base = ref->GetObjectReference();
			now.base = base ? base->GetFormID() : 0;
			return now;
		}

		// Copies of wires whose shape is set but not drawn yet: the game draws a wire from its shape when its 3D
		// loads, and draws it again only when asked (QueueRebuildBendableSpline), which wants the 3D there.
		struct Unbuilt
		{
			RE::ObjectRefHandle ref;
			Clock::time_point   since;
		};

		std::vector<Unbuilt>   unbuiltWires;
		constexpr auto         UNBUILT_GIVE_UP = std::chrono::seconds(30);
		std::uint32_t          wiresDrawn = 0;

		// Gives the copy of a wire its builder's shape, and has it drawn again.
		void ShapeWire(RE::TESObjectREFR& a_ref, const Protocol::WireShape& a_shape)
		{
			if (!a_ref.extraList) {
				return;
			}
			float       thickness = a_shape.thickness;
			float       slack = a_shape.slack;
			RE::NiPoint3 half{ a_shape.halfExtents[0], a_shape.halfExtents[1], a_shape.halfExtents[2] };
			bool        detached = false;
			a_ref.extraList->SetBendableSplineInfo(&thickness, &slack, &half, &detached);
			unbuiltWires.push_back({ a_ref.GetHandle(), Clock::now() });
		}

		void DrawShapedWires(Clock::time_point a_now)
		{
			const auto tasks = RE::TaskQueueInterface::GetSingleton();
			std::erase_if(unbuiltWires, [&](const Unbuilt& a_wire) {
				const auto ref = a_wire.ref.get();
				if (!ref || !tasks || a_now - a_wire.since > UNBUILT_GIVE_UP) {
					return true;
				}
				if (!ref->Get3D()) {
					return false;
				}
				tasks->QueueRebuildBendableSpline(*ref, false, nullptr);
				++wiresDrawn;
				return true;
			});
		}

		// Moves the copy within its cell to where the report puts it. The reference's place alone moves
		// neither what is drawn nor what collides: the 3D is moved too, and an actor (a turret, a robot)
		// through its character controller. A wire takes its new shape (one of its ends moved).
		void MoveTo(const Copy& a_copy, const Protocol::WorkshopItem& a_item)
		{
			const auto ref = a_copy.ref.get();
			if (!ref) {
				return;
			}
			if (a_item.wire && IsWire(ref->GetObjectReference())) {
				ShapeWire(*ref, *a_item.wire);
			}
			if (const auto scale = RemoteEdits::ScaleToApply(a_item, ref->refScale / 100.0f)) {  // a friend's message can say any size
				ref->SetScale(*scale);
			}
			const RE::NiPoint3 position{ a_item.position[0], a_item.position[1], a_item.position[2] };
			if (const auto actor = ref->As<RE::Actor>()) {
				actor->SetPosition(position, true);
				actor->SetHeading(a_item.rotation[2]);
			} else {
				ref->SetLocationOnReference(position);
				ref->SetAngleOnReference({ a_item.rotation[0], a_item.rotation[1], a_item.rotation[2] });
				ref->Update3DPosition(true);
			}
		}

		bool InGraveyard(const RE::TESObjectREFR* a_ref)
		{
			return std::ranges::any_of(graveyard, [&](const Copy& a_copy) { return a_copy.ref.get().get() == a_ref; });
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

		// ---- Power: no grid names a copy (see WorkshopPower) ----------------------------------------------
		//
		// The game's own functions, found in 1.11.240 (their names from the game's symbols of an earlier version, where
		// the same code sits 0x1000 lower):

		// The default object WorkshopBlockInteractionKeyword's keyword (a BGSKeyword*): workshop mode won't pick a
		// reference that has it (Workshop's pick test, 2195179, which the picker 2195312 asks), so it can't be wired,
		// stored, scrapped or moved there.
		constexpr REL::ID BLOCK_INTERACTION_KEYWORD{ 4797360 };
		// Workshop::ExtraData::RemoveItem(TESObjectREFR&): takes the reference off the workbench's power grids (its
		// links with it), as scrapping it does (Workshop::FinishDestroyItem).
		constexpr REL::ID REMOVE_GRID_ITEM{ 2194997 };
		// SplineUtils::DisconnectSpline(TESObjectREFR&): takes the reference out of the power links (ExtraPowerLinks) of
		// everything it is linked to, and drops its own; for a wire, what scrapping it does.
		constexpr REL::ID DISCONNECT_SPLINE{ 2195074 };
		// Workshop::IsPowered(TESObjectREFR&): whether a workshop item of a workbench is powered (a generator: whether
		// it makes power).
		constexpr REL::ID IS_POWERED{ 2194916 };
		// PowerUtils::ItemGeneratesPower(TESObjectREFR&): its power generated (an actor value) is above 0.
		constexpr REL::ID ITEM_GENERATES_POWER{ 2195079 };
		// TESObjectLIGH::SwitchLight(TESObjectREFR&, bool off): the lights of a reference's 3D on or off (a LIGH's own, or one in a
		// model), as the game's own powering does for every piece (the power handler, 2195089, passes the "off" it fires PowerOffEvent for).
		constexpr REL::ID SWITCH_LIGHT{ 2198253 };

		bool                     blockCopies = true;  // dev (wspower block off): copies can be picked, to try the repair
		std::uint32_t            copiesBlocked = 0;
		WorkshopPower::Counts    untied;              // copies found tied into our power, and what became of them
		std::uint32_t            wiresUntied = 0;     // wires of ours strung to a copy, removed with it
		std::uint32_t            audits = 0;
		std::uint32_t            stuckWarned = 0;
		std::unordered_set<std::uint32_t> retired;   // copies switched off and kept: the game kept them tied in
		std::unordered_set<std::uint32_t> keptTied;  // copies an audit could not take off: the session's audit tries them no more
		std::uint32_t            auditSkips = 0;     // ... skipped by it since
		std::set<std::uint32_t>  powerBenches;       // workbenches seen in loaded cells, and ones a copy was linked to
		std::uint64_t            frameNumber = 0;
		std::uint64_t            gridNamedFrame = ~0ull;
		std::unordered_set<std::uint32_t> gridNamed;  // what the benches' grids name, as of gridNamedFrame

		RE::BGSKeyword* WorkshopItemKeyword()
		{
			return RE::TESForm::GetFormByID<RE::BGSKeyword>(WORKSHOP_ITEM_KEYWORD);
		}

		// Gives a copy the keyword that keeps workshop mode from picking it. Saved with the copy, so one left in a save
		// (a crash) stays out of reach too.
		void BlockInteraction(RE::TESObjectREFR& a_copy)
		{
			static REL::Relocation<RE::BGSKeyword**> keyword{ BLOCK_INTERACTION_KEYWORD };
			if (!blockCopies || !*keyword) {
				return;
			}
			if (!static_cast<const RE::IKeywordFormBase&>(a_copy).HasKeyword(*keyword)) {
				a_copy.AddKeyword(*keyword);
				++copiesBlocked;
			}
		}

		bool Blocked(const RE::TESObjectREFR& a_ref)
		{
			static REL::Relocation<RE::BGSKeyword**> keyword{ BLOCK_INTERACTION_KEYWORD };
			return *keyword && static_cast<const RE::IKeywordFormBase&>(a_ref).HasKeyword(*keyword);
		}

		RE::Workshop::ExtraData* WorkshopDataOf(const RE::TESObjectREFR* a_bench)
		{
			return a_bench && a_bench->extraList ? a_bench->extraList->GetByType<RE::Workshop::ExtraData>() : nullptr;
		}

		std::vector<WorkshopPower::Grid> GridsOf(const RE::TESObjectREFR* a_bench)
		{
			std::vector<WorkshopPower::Grid> grids;
			const auto                       data = WorkshopDataOf(a_bench);
			if (!data) {
				return grids;
			}
			for (const auto grid : data->powerGrid) {
				if (!grid) {
					continue;
				}
				auto& out = grids.emplace_back();
				for (const auto& node : grid->adjacencyMap) {
					out.nodes.push_back(node.first);
					if (node.second) {
						for (const auto& link : *node.second) {
							out.edges.push_back({ node.first, link.connection, link.connector });
						}
					}
				}
				for (const auto id : grid->currentlyPowered) {
					out.powered.push_back(id);
				}
			}
			return grids;
		}

		bool GridsName(const RE::TESObjectREFR* a_bench, std::uint32_t a_id)
		{
			for (const auto& grid : GridsOf(a_bench)) {
				if (std::ranges::binary_search(WorkshopPower::Named(grid), a_id)) {
					return true;
				}
			}
			return false;
		}

		// The workbenches to look at: those seen, and those our copies are linked to (noted for good: one let go of may
		// still name it).
		std::vector<RE::TESObjectREFR*> PowerBenches()
		{
			std::vector<std::uint32_t> copyList;
			{
				std::scoped_lock l{ copyIdsLock };
				copyList.assign(copyIds.begin(), copyIds.end());
			}
			for (const auto id : copyList) {
				const auto copy = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
				if (const auto bench = copy ? BenchOf(*copy) : nullptr) {
					powerBenches.insert(bench->GetFormID());
				}
			}
			std::vector<RE::TESObjectREFR*> benches;
			for (const auto id : powerBenches) {
				if (const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id); WorkshopDataOf(bench)) {
					benches.push_back(bench);
				}
			}
			return benches;
		}

		const std::unordered_set<std::uint32_t>& GridNamed()
		{
			if (gridNamedFrame != frameNumber) {
				gridNamedFrame = frameNumber;
				gridNamed.clear();
				for (const auto bench : PowerBenches()) {
					for (const auto& grid : GridsOf(bench)) {
						const auto named = WorkshopPower::Named(grid);
						gridNamed.insert(named.begin(), named.end());
					}
				}
			}
			return gridNamed;
		}

		bool HasPowerLinks(const RE::TESObjectREFR& a_ref)
		{
			const auto links = a_ref.extraList ? a_ref.extraList->GetByType<RE::ExtraPowerLinks>() : nullptr;
			return links && !links->powerLinks.empty();
		}

		// The game, as WorkshopPower asks it about our copies.
		struct PowerEngine
		{
			WorkshopPower::Links LinksOf(std::uint32_t a_id)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
				if (!ref) {
					return {};
				}
				return { GridNamed().contains(a_id), HasPowerLinks(*ref), BenchOf(*ref) != nullptr };
			}

			// Off its grids (the workbench's own removal), its wires (disconnected; ours removed, and reported scrapped
			// so the friend's copy of the wire goes too), any power link left (a piece snapped on), and its workbench.
			void Detach(std::uint32_t a_id)
			{
				const RE::NiPointer<RE::TESObjectREFR> copy{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) };
				if (!copy) {
					return;
				}
				using remove_t = void (*)(RE::Workshop::ExtraData*, RE::TESObjectREFR&);
				using disconnect_t = void (*)(RE::TESObjectREFR&);
				static REL::Relocation<remove_t>     removeItem{ REMOVE_GRID_ITEM };
				static REL::Relocation<disconnect_t> disconnect{ DISCONNECT_SPLINE };
				const auto                           bench = BenchOf(*copy);
				if (bench) {
					powerBenches.insert(bench->GetFormID());
				}
				for (const auto other : PowerBenches()) {
					if (GridsName(other, a_id)) {
						removeItem(WorkshopDataOf(other), *copy);
					}
				}
				for (const auto& wire : WiresOf(*copy)) {
					disconnect(*wire);
					if (!WorkshopSync::IsCopy(wire.get()) && !wire->IsDeleted()) {
						const auto wireBench = BenchOf(*wire);
						ReportRef(wire.get(), wireBench ? wireBench : bench, Protocol::WorkshopOp::kScrapped);
						if (const auto keyword = WorkshopItemKeyword(); keyword && wireBench) {
							wire->SetLinkedRef(nullptr, keyword);
						}
						wire->Disable();
						wire->SetDelete(true);
						++wiresUntied;
					}
				}
				if (HasPowerLinks(*copy)) {
					disconnect(*copy);
				}
				if (const auto keyword = WorkshopItemKeyword(); keyword && bench) {
					copy->SetLinkedRef(nullptr, keyword);
				}
				gridNamedFrame = ~0ull;  // the grids changed
				REX::WARN("WorkshopSync: copy {:08X} was tied into our power (workbench {:08X}): taken off its grid, wires and workbench", a_id, bench ? bench->GetFormID() : 0);
			}

			void Delete(std::uint32_t a_id)
			{
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id)) {
					ref->Disable();
					ref->SetDelete(true);
				}
			}

			// Switched off and kept, never deleted: the grid that still names it names it, not whatever would be given
			// its ID next.
			void Retire(std::uint32_t a_id)
			{
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id)) {
					ref->Disable();
				}
				retired.insert(a_id);
				REX::WARN("WorkshopSync: copy {:08X} is still tied into our power after taking it off: switched off and kept, not deleted", a_id);
			}

			std::vector<WorkshopPower::Grid> Grids()
			{
				std::vector<WorkshopPower::Grid> all;
				for (const auto bench : PowerBenches()) {
					auto grids = GridsOf(bench);
					all.insert(all.end(), std::make_move_iterator(grids.begin()), std::make_move_iterator(grids.end()));
				}
				return all;
			}

			// Our copies, and those waiting in the graveyard to be deleted (saved too, if a save comes first).
			std::vector<std::uint32_t> Copies()
			{
				std::vector<std::uint32_t> ids;
				{
					std::scoped_lock l{ copyIdsLock };
					ids.assign(copyIds.begin(), copyIds.end());
				}
				for (const auto& copy : graveyard) {
					if (const auto ref = copy.ref.get()) {
						ids.push_back(ref->GetFormID());
					}
				}
				return ids;
			}

			bool IsCopy(std::uint32_t a_id)
			{
				{
					std::scoped_lock l{ copyIdsLock };
					if (copyIds.contains(a_id)) {
						return true;
					}
				}
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
				return ref && InGraveyard(ref);
			}
		};

		// Looks for copies tied into our power, and takes them off (see WorkshopPower::Audit).
		void AuditPower(std::string_view a_when)
		{
			++audits;
			PowerEngine engine;
			const auto  before = untied.found;
			// Every 2 s in a session, a copy the game kept tied in before is not taken off again (its grid and wires
			// re-run each time, to no end); before a save, and by hand, every copy is tried.
			const bool  session = a_when == "session";
			const auto  stuck = WorkshopPower::Audit(engine, untied, [&](std::uint32_t a_id) {
				const bool skip = session && (keptTied.contains(a_id) || retired.contains(a_id));
				auditSkips += skip;
				return skip;
			});
			keptTied.insert(stuck.begin(), stuck.end());
			if (untied.found != before) {
				REX::WARN("WorkshopSync: {}: {} copies were tied into our power; {} still are", a_when, untied.found - before, stuck.size());
			}
			if (!stuck.empty() && stuckWarned++ < 8) {
				REX::WARN("WorkshopSync: {}: copy {:08X} stays tied into our power; it won't be deleted", a_when, stuck.front());
			}
		}

		WorkshopApply::Kind KindOf(RE::TESObjectREFR* a_ref)
		{
			if (a_ref->As<RE::Actor>()) {
				return WorkshopApply::Kind::kActor;
			}
			const auto base = a_ref->GetObjectReference();
			return base && base->Is(RE::ENUM_FORM_ID::kCONT) ? WorkshopApply::Kind::kContainer : WorkshopApply::Kind::kOther;
		}

		// Deletes a copy. What we kept in a copy of a friend's container comes to us first: it would have
		// gone with the copy (their scrap, a move to another cell, or the session ending). Only a
		// container's (WorkshopApply::Rescues): a turret or a robot holds its own gear.
		void Delete(RE::TESObjectREFR* a_ref)
		{
			if (WorkshopApply::Rescues(KindOf(a_ref), HoldsItems(a_ref))) {
				RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems 00000014", a_ref->GetFormID()).c_str());
				REX::INFO("WorkshopSync: copy {:08X} goes; what it held went to the player", a_ref->GetFormID());
				Hud::Notify(Text::Get("workshop.rescued"));
				++itemsRescued;
			}
			// Taken off our power grid first if it is on one; one the game keeps on it is never deleted.
			PowerEngine engine;
			WorkshopPower::Remove(engine, a_ref->GetFormID(), untied);
		}

		void Remove(Copy& a_copy)
		{
			if (const auto ref = a_copy.ref.get()) {
				if (DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_copy.placedAt)) {
					Delete(ref.get());
				} else {
					graveyard.push_back(a_copy);
				}
			}
		}

		// A real workshop object of this game: a piece built here, linked to its workbench as one of its workshop items (a
		// plain copy never is: the power audit unlinks any the game links), or a workbench (the workshop's own extra data,
		// which only a workbench carries: alone, it let our built pieces pass for copies).
		bool IsWorkshopObject(const RE::TESObjectREFR* a_ref)
		{
			return BenchOf(*a_ref) != nullptr || (a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kWorkshop));
		}

		// The first reference made at runtime of the item's base at its spot, still standing (not one deleted already,
		// waiting for the save to drop it), none of our copies, that a_pick takes.
		template <class Pick>
		RE::TESObjectREFR* AtSpot(const Protocol::WorkshopItem& a_item, Pick a_pick, bool a_copies = false)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return nullptr;
			}
			RE::TESObjectREFR* found = nullptr;
			const RE::NiPoint3 at{ a_item.position[0], a_item.position[1], a_item.position[2] };
			tes->ForEachReferenceInRange(at, ADOPT_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->GetFormID() == a_item.base && (a_ref->GetFormID() >> 24) == 0xFF && !a_ref->IsDisabled() && !a_ref->IsDeleted() && IsCopy(a_ref) == a_copies &&
					!InGraveyard(a_ref) && !Drops::IsCopyRef(a_ref->GetFormID()) && a_pick(a_ref)) {
					found = a_ref;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		// A copy left in this save by an earlier session: the same object at the same spot (taken over, a deleted one would
		// be removed twice), and none of ours.
		RE::TESObjectREFR* FindExisting(const Protocol::WorkshopItem& a_item)
		{
			return AtSpot(a_item, [](RE::TESObjectREFR* a_ref) { return !IsWorkshopObject(a_ref); });
		}

		// A piece of ours, the same object, at the friend's piece's spot: their save and ours share a past.
		bool OursAt(const Protocol::WorkshopItem& a_item)
		{
			return AtSpot(a_item, [](RE::TESObjectREFR* a_ref) { return BenchOf(*a_ref) != nullptr; }) != nullptr;
		}

		// One of our copies of a friend's piece, the same object, near the spot: WorkshopApply::TwinOf's quick look, before it
		// asks whose copy it is and whether it stands just so.
		bool CopyAt(const Protocol::WorkshopItem& a_item)
		{
			return AtSpot(a_item, [](RE::TESObjectREFR*) { return true; }, true) != nullptr;
		}

		// What workshop mode can build: furniture, walls, lights, containers, crops, turrets... A
		// message from another player is untrusted, so bases the workshop never places (explosions,
		// hazards, projectiles, unique NPCs) are refused: they would go off, or persist, in our save. Nor a power
		// armor frame (WorkshopApply::Shared): no copy of one, and ours stays where it is when a friend moves theirs.
		bool Buildable(const RE::TESBoundObject* a_base)
		{
			using T = RE::ENUM_FORM_ID;
			switch (a_base->GetFormType()) {
			case T::kSTAT:
			case T::kSCOL:
			case T::kMSTT:
			case T::kACTI:
			case T::kCONT:
			case T::kDOOR:
			case T::kLIGH:
			case T::kTERM:
			case T::kMISC:
			case T::kFLOR:
			case T::kTREE:
				return true;
			case T::kFURN:
				return WorkshopApply::Shared(true, Equipment::IsPowerArmorFrame(a_base));
			case T::kNPC_: {
				// Workshop mode builds turrets (and, with Automatron, robots); any other actor from a workshop
				// message is someone placing creatures or people on us. Those keywords are the race's (a
				// turret's own record has none), so the race's count too.
				const auto npc = static_cast<const RE::TESNPC*>(a_base);
				return !npc->IsUnique() && a_base->GetFormID() != 0x00000007 &&
				       (npc->HasApplicableKeywordString("ActorTypeTurret") || npc->HasApplicableKeywordString("ActorTypeRobot"));
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

		// Whether a copy of the piece may stand here (see WorkshopApply::Placeable).
		WorkshopApply::Placeable CanPlace(const Protocol::WorkshopItem& a_item)
		{
			const auto base = RE::TESForm::GetFormByID<RE::TESBoundObject>(a_item.base);
			const bool wire = IsWire(base) && a_item.wire;  // a wire is drawn from its shape: one without is nothing to draw
			if (base && Equipment::IsPowerArmorFrame(base)) {
				++unplaceable;
				return WorkshopApply::Placeable::kFrame;
			}
			if (!base || (!wire && !Buildable(base))) {
				++unplaceable;
				if (unplaceableBases.size() < 64 && unplaceableBases.insert(a_item.base).second) {
					REX::INFO("WorkshopSync: not placing player {}'s {:08X}: its base {:08X} is {}", a_item.playerId, a_item.refId, a_item.base,
						base ? base->GetFormTypeString() : "not in this game");
				}
				return WorkshopApply::Placeable::kNo;
			}
			// A container the workshop builds starts empty; one whose base holds things of its own (a bureau) would
			// be loot, ours on opening the copy (and again on every rescue from it): its copy is emptied (Empty).
			if (const auto container = base->As<RE::TESObjectCONT>(); container && container->numContainerObjects != 0) {
				if (stockedBases.insert(base->GetFormID()).second) {
					REX::INFO("WorkshopSync: container {:08X} comes with things in it: its copies are emptied", base->GetFormID());
				}
				return WorkshopApply::Placeable::kStocked;
			}
			return WorkshopApply::Placeable::kYes;
		}

		std::optional<Copy> Create(const Protocol::WorkshopItem& a_item)
		{
			const auto cell = CellFor(a_item);
			const auto base = RE::TESForm::GetFormByID<RE::TESBoundObject>(a_item.base);
			if (!cell || !base) {
				return std::nullopt;
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
				return std::nullopt;
			}
			if (const auto scale = RemoteEdits::ScaleToApply(a_item, 1.0f)) {
				ref->SetScale(*scale);
			}
			if (a_item.wire && IsWire(base)) {
				ShapeWire(*ref, *a_item.wire);
			}
			// A turret or robot the game builds is the settlement's, on the players' side, through its
			// workbench's scripts; a copy has none of that and would fire on us. It goes in the player's
			// faction (rank 0: rank 1 marks a stand-in, SaveStrays::StandInFaction).
			if (ref->As<RE::Actor>()) {
				RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} 0", ref->GetFormID(), SaveStrays::STAND_IN_FACTION).c_str());
			}
			return Copy{ handle, Clock::now() };
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

		// The game, as WorkshopApply sees it.
		struct EngineWorld
		{
			using Copy = WorkshopSync::Copy;

			// Its settlement is here, and the cell it stands in attached (SpotAttached): what waits for it is tried again then.
			bool Loaded(const Protocol::WorkshopItem& a_item)
			{
				const auto cell = CellFor(a_item);
				return cell && (cell->IsInterior() || SpotAttached(a_item));
			}
			WorkshopCells::Where     WhereOf(const Protocol::WorkshopItem& a_item) { return WorkshopSync::WhereOf(a_item); }
			WorkshopCells::Where     WhereScrapped(const Protocol::WorkshopItem& a_item) { return WorkshopSync::WhereScrapped(a_item); }
			// Named with a real workbench (else dropped, said once in the log), and within its reach.
			bool InReach(const Protocol::WorkshopItem& a_item)
			{
				if (const auto anchor = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop); !IsWorkbenchAnchor(anchor)) {
					RefuseAnchor(a_item, anchor);
					return false;
				}
				return WorkshopSync::InReach(a_item);
			}
			auto                     StandingOf(const Copy& a_copy) { return WorkshopSync::StandingOf(a_copy); }
			void                     MoveTo(const Copy& a_copy, const Protocol::WorkshopItem& a_item) { WorkshopSync::MoveTo(a_copy, a_item); }
			// A copy made, or found standing from an earlier session: ours to remove, and out of workshop mode's reach.
			void Keep(const Copy& a_copy)
			{
				NoteCopy(a_copy.ref);
				if (const auto ref = a_copy.ref.get()) {
					BlockInteraction(*ref);
				}
			}
			WorkshopApply::Placeable CanPlace(const Protocol::WorkshopItem& a_item) { return WorkshopSync::CanPlace(a_item); }
			std::optional<Copy>      Create(const Protocol::WorkshopItem& a_item) { return WorkshopSync::Create(a_item); }
			// Destroys what the copy holds (its base's things: it was just made), as the workshop's own build does.
			void Empty(const Copy& a_copy)
			{
				if (const auto ref = a_copy.ref.get()) {
					RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", ref->GetFormID()).c_str());
				}
			}
			bool                     ScrapWorldRef(const Protocol::WorkshopItem& a_item) { return WorkshopSync::ScrapWorldRef(a_item); }

			void Discard(const Copy& a_copy)
			{
				ForgetCopy(a_copy.ref);
				auto copy = a_copy;
				Remove(copy);
			}

			bool OwnLoaded(const Protocol::WorkshopItem& a_item) { return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.refId) != nullptr; }

			// Ours of a piece of the world a friend moved, if a friend may move it: one of that workbench's own
			// workshop items here too (what workshop mode lets a player move: no quest's prop, door or loose
			// item is linked to a workbench), the same object, standing.
			std::optional<Copy> OwnPiece(const Protocol::WorkshopItem& a_item)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.refId);
				const auto base = ref ? ref->GetObjectReference() : nullptr;
				const auto bench = ref ? BenchOf(*ref) : nullptr;
				if (!base || base->GetFormID() != a_item.base || !Buildable(base) || ref->IsDisabled() || ref->IsDeleted() || !bench || bench->GetFormID() != a_item.workshop ||
					!IsWorkbenchAnchor(bench)) {
					return std::nullopt;
				}
				return Copy{ ref->GetHandle(), Clock::now() };
			}

			std::optional<Copy> FindExisting(const Protocol::WorkshopItem& a_item)
			{
				const auto existing = WorkshopSync::FindExisting(a_item);
				return existing ? std::optional<Copy>{ Copy{ existing->GetHandle(), Clock::now() } } : std::nullopt;
			}

			// Keep gave it the keyword (since 0.9.0); nothing else of ours, nor the game, gives a frame that one. Not a
			// frame that holds anything: Delete hands back only a container's, and a player may have fitted that one out
			// (or it is their own suit an earlier session took over), so it stays, pieces and all.
			bool MadeByUs(const Copy& a_copy)
			{
				const auto ref = a_copy.ref.get();
				return ref && Blocked(*ref) && !HoldsItems(ref.get());
			}

			bool OursAt(const Protocol::WorkshopItem& a_item) { return WorkshopSync::OursAt(a_item); }
			bool CopyAt(const Protocol::WorkshopItem& a_item) { return WorkshopSync::CopyAt(a_item); }

			// Only a copy of ours (it has the keyword Keep gives): never a loose thing of the same object.
			std::optional<Copy> LeftoverAt(const Protocol::WorkshopItem& a_item)
			{
				const auto leftover = AtSpot(a_item, [](RE::TESObjectREFR* a_ref) { return !IsWorkshopObject(a_ref) && Blocked(*a_ref); });
				return leftover ? std::optional<Copy>{ Copy{ leftover->GetHandle(), Clock::now() } } : std::nullopt;
			}
		};

		// Tries again what waits in, or next to, a cell that just attached (in line with the rest: a cell
		// can bring up hundreds).
		void TryPendingNear(const WorkshopCells::Where& a_cell)
		{
			inbox.Retry(inbox.Pending().Near(a_cell));
		}

		// Tries again the scraps that wait for one of the references of a cell that just attached,
		// whatever cell they were filed under. A scrap tells where its player's piece stood when they
		// scrapped it: if they had moved it first, maybe cells from where ours stands, and a reference
		// that isn't persistent can't be looked up, to find where ours stands, until its cell loads.
		void TryPendingScrapsIn(std::uint32_t a_cell)
		{
			if (!inbox.Pending().WaitsForRefs()) {
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
						const auto waiting = inbox.Pending().For(ref->GetFormID());
						keys.insert(keys.end(), waiting.begin(), waiting.end());
					}
				}
			}
			inbox.Retry(keys);
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
		// The walk, and what it reports once, is SavedScraps::Walk (see there); the game behind it is
		// here: what a reference is to the walk, where a workbench's build area reaches, and the report.
		// Switched off alone isn't scrapped: the game starts plenty of references off (quest props, things
		// an enable marker turns on), and junk a friend scrapped is only switched off here. Nor is
		// everything deleted: quests and scripts delete things too, and what a friend's game takes away
		// stays away from their save for good, so only what the game itself would let a player scrap,
		// inside a workbench's build area, counts (see WhyNotScrapped).
		//
		// The save has no list of the pieces scrapped. The workbench's workshop data has a deletedItems
		// list, but that holds base objects with counts: the workshop's stored items (Workshop::DestroyItem
		// adds the base of a piece stored in workshop mode, Workshop::AddStoredItem adds to it, and
		// building from the store takes one back through UseDeletedItem). It names no reference.

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
					return Papyrus::BOUND_DONE;
				}
				return Papyrus::BOUND_NEXT;  // past a plain ObjectReference object to the scripts after it
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

		// The cell to walk, if it's attached: for a worldspace's persistent references, its persistent
		// cell, while we're in that worldspace.
		RE::TESObjectCELL* CellOf(const SavedScraps::Cell& a_cell)
		{
			if (a_cell.persistent) {
				const auto tes = RE::TES::GetSingleton();
				const auto world = tes ? tes->worldSpace : nullptr;
				return world && world->GetFormID() == a_cell.id ? world->persistentCell : nullptr;
			}
			const auto cell = RE::TESForm::GetFormByID<RE::TESObjectCELL>(a_cell.id);
			return cell && cell->IsAttached() ? cell : nullptr;  // it may have gone since the look
		}

		// The game, as SavedScraps::Walk asks it.
		struct WalkEngine
		{
			// The cell's references (their IDs), taken under its lock: the cell's own list changes as
			// references come and go. Not ones made at runtime: ours, a copy, something dropped.
			std::optional<SavedScraps::Snapshot> Refs(const SavedScraps::Cell& a_cell)
			{
				const auto cell = CellOf(a_cell);
				if (!cell) {
					return std::nullopt;
				}
				SavedScraps::Snapshot snapshot{ a_cell.persistent ? a_cell.id : SpaceOf(cell), {} };
				const RE::BSAutoLock  l{ cell->spinLock };
				snapshot.refs.reserve(cell->references.size());
				for (const auto& ref : cell->references) {
					if (ref && Protocol::IsShareableRef(ref->GetFormID())) {
						snapshot.refs.push_back(ref->GetFormID());
					}
				}
				return snapshot;
			}

			bool Attached(const SavedScraps::Cell& a_cell) { return CellOf(a_cell) != nullptr; }

			// What the reference is to the walk. Only workbenches are looked for among a worldspace's
			// persistent references (a_benchesOnly): no more is asked of one switched off there.
			std::optional<SavedScraps::Seen> Examine(std::uint32_t a_id, bool a_benchesOnly)
			{
				const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) };
				if (!ref) {
					return std::nullopt;
				}
				if (!ref->IsDisabled()) {
					return IsBench(ref.get()) ? SavedScraps::Seen::kBench : SavedScraps::Seen::kStanding;
				}
				if (a_benchesOnly || !ref->IsDeleted()) {
					return SavedScraps::Seen::kOff;
				}
				return WhyNotScrapped(ref.get()) ? SavedScraps::Seen::kDeleted : SavedScraps::Seen::kScrapped;
			}

			bool Loaded(std::uint32_t a_id) { return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) != nullptr; }

			// Whether the piece stands in the workbench's build area: within its reach (the largest in the
			// game, before the game's own test), and inside it (see WorkshopSync::InBuildArea).
			bool InBuildArea(std::uint32_t a_bench, std::uint32_t a_ref)
			{
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_bench);
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ref);
				return bench && ref && bench->data.location.GetDistance(ref->data.location) <= BUILD_REACH && WorkshopSync::InBuildArea(*bench, ref->data.location);
			}

			// Reports the piece scrapped at its workbench, as a scrap made in the session is; false if either
			// is gone, or the report isn't taken.
			bool Report(std::uint32_t a_ref, std::uint32_t a_bench)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ref);
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_bench);
				return ref && bench && ReportRef(ref, bench, Protocol::WorkshopOp::kScrapped);
			}
		};

		SavedScraps::Walk<WalkEngine> walk{ sentScraps };
		Clock::time_point             lastSend{};  // when the walk's reports last had their turn

		// ---- What the host (and, by the host's rule, a guest) built before the session -----------------
		//
		// See BuiltWalk. The pieces are found in each cell as it attaches (and the worldspace's persistent
		// cell), and in the list of workshop items of each workbench found there (which names pieces in cells
		// that aren't loaded, while they are in memory).

		BuiltWalk::Walk builtWalk;
		std::uint32_t   builtWorld = 0;  // the worldspace whose persistent cell was looked over

		BuiltWalk::Piece PieceOf(RE::TESObjectREFR& a_ref)
		{
			const auto base = a_ref.GetObjectReference();
			const auto bench = BenchOf(a_ref);
			return { .ref = a_ref.GetFormID(),
				.bench = bench && IsWorkbenchAnchor(bench) ? bench->GetFormID() : 0,
				.buildable = base && (base->GetFormID() >> 24) != 0xFF && (Buildable(base) || IsWire(base)),
				.copy = IsCopy(&a_ref),
				.off = a_ref.IsDisabled() || a_ref.IsDeleted() };
		}

		void OfferBuilt(std::uint32_t a_id)
		{
			if (const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) }) {
				builtWalk.Offer(PieceOf(*ref));
			}
		}

		// The workshop items the workbench lists.
		void OfferBenchItems(std::uint32_t a_bench)
		{
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_bench);
			const auto children = bench && bench->extraList ? bench->extraList->GetByType<RE::ExtraLinkedRefChildren>() : nullptr;
			if (!children) {
				return;
			}
			std::vector<std::uint32_t> ids;
			for (const auto& child : children->linkedChildren) {
				if (child.keyword && child.keyword->GetFormID() == WORKSHOP_ITEM_KEYWORD) {
					if (const auto ref = child.REFR.get(); ref && !Protocol::IsShareableRef(ref->GetFormID())) {
						ids.push_back(ref->GetFormID());
					}
				}
			}
			for (const auto id : ids) {
				OfferBuilt(id);
			}
		}

		// The cell's pieces made at runtime, and its workbenches' lists (taken under the cell's lock, looked at
		// after).
		void OfferBuiltIn(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return;
			}
			std::vector<std::uint32_t> made, benches;
			{
				const RE::BSAutoLock l{ a_cell->spinLock };
				for (const auto& ref : a_cell->references) {
					if (!ref) {
						continue;
					}
					const auto id = ref->GetFormID();
					if ((id >> 24) == 0xFF) {
						made.push_back(id);
					} else if (ref->extraList && ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kWorkshop)) {
						benches.push_back(id);
					}
				}
			}
			for (const auto id : made) {
				OfferBuilt(id);
			}
			for (const auto id : benches) {
				OfferBenchItems(id);
			}
		}

		struct BuiltEngine
		{
			// Waits while the send queue is half full: the session's own building goes first.
			bool Report(const BuiltWalk::Piece& a_piece)
			{
				{
					std::scoped_lock l{ outLock };
					if (outgoing.Size() >= SendPacer::WORKSHOP_QUEUED / 2) {
						return false;
					}
				}
				const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_piece.ref) };
				const auto                             bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_piece.bench);
				if (ref && bench && !ref->IsDisabled() && !ref->IsDeleted()) {
					ReportRef(ref.get(), bench, Protocol::WorkshopOp::kPlaced);
				}
				return true;  // gone since: nothing to report
			}
		};

		void OfferAndReportBuilt(const std::vector<AttachedCell>& a_fresh)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto world = tes && !tes->interiorCell ? tes->worldSpace : nullptr;
			if (world && world->persistentCell && world->GetFormID() != builtWorld) {
				builtWorld = world->GetFormID();
				OfferBuiltIn(world->persistentCell);
			}
			for (const auto& cell : a_fresh) {
				const auto found = RE::TESForm::GetFormByID<RE::TESObjectCELL>(cell.id);
				if (found && found->IsAttached()) {
					OfferBuiltIn(found);
				}
			}
		}

		void ClearWalk()
		{
			attachedCells.clear();
			walk.Clear();
			builtWalk.Clear();
			builtWorld = 0;
		}

		// Queues the worldspace's persistent references (the workbenches are among them), until they have
		// been walked to the end, and the cells that attached since the last look.
		void QueueWalks(const std::vector<AttachedCell>& a_fresh)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto world = tes && !tes->interiorCell ? tes->worldSpace : nullptr;
			if (world && world->persistentCell) {
				walk.QueueWorld(world->GetFormID());
			}
			for (const auto& cell : a_fresh) {
				walk.QueueCell(cell.id);
			}
		}

		// Walks some of the queued cells (a few hundred references a frame) and reports some of what was
		// found, at the walk's own pace.
		void WalkAndSend(Clock::time_point a_now)
		{
			WalkEngine engine;
			if (const auto round = walk.WalkSome(engine); round && (round->counts.deleted > 0 || round->counts.benches > 0)) {
				REX::INFO("WorkshopSync: walked {} references: {} switched off, {} of them deleted; {} new workbenches; {} scraps to report, {} in no build area seen",
					round->counts.refs, round->counts.disabled, round->counts.deleted, round->counts.benches, round->toSend, round->waiting);
			}
			const auto elapsed = lastSend == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(a_now - lastSend).count();
			lastSend = a_now;
			walk.SendSome(engine, elapsed);
		}

		// Another player's report, its turn come (see WorkshopApply::Apply).
		void ApplyNow(const Key& a_key, const Protocol::WorkshopItem& a_item);

		// ---- Power: which pieces are powered, across games ----------------------------------------------

		WorkshopPower::Reporter reporter;  // our pieces with power
		WorkshopPower::Mirror   mirror;    // what friends' games say of theirs
		Clock::time_point       nextPowerAudit{};
		std::uint32_t           powerSent = 0;     // entries sent
		std::uint32_t           powerReceived = 0;  // entries received
		std::uint32_t           powerShown = 0;     // copies shown powered or not
		std::int64_t            pollUsMax = 0;
		std::uint32_t           benchWorld = 0;     // the worldspace whose persistent cell was looked over for workbenches

		// The workbenches among the cell's references, with their workshop data or not yet: a settlement's workbench gets its data
		// the first time workshop mode is used there (PowerBenches looks again each time).
		void NoteBenchesIn(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return;
			}
			std::vector<std::uint32_t> found;
			{
				const RE::BSAutoLock l{ a_cell->spinLock };
				for (const auto& ref : a_cell->references) {
					if (ref && IsWorkbenchAnchor(ref.get())) {
						found.push_back(ref->GetFormID());
					}
				}
			}
			powerBenches.insert(found.begin(), found.end());
		}

		void NoteBenches(const std::vector<AttachedCell>& a_fresh)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto world = tes && !tes->interiorCell ? tes->worldSpace : nullptr;
			if (world && world->persistentCell && world->GetFormID() != benchWorld) {
				benchWorld = world->GetFormID();
				NoteBenchesIn(world->persistentCell);
			}
			for (const auto& cell : a_fresh) {
				const auto found = RE::TESForm::GetFormByID<RE::TESObjectCELL>(cell.id);
				if (found && found->IsAttached()) {
					NoteBenchesIn(found);
				}
			}
		}

		// Whether the reference takes, makes or carries power.
		bool IsPowerPiece(const RE::TESObjectREFR& a_ref)
		{
			using generates_t = bool (*)(const RE::TESObjectREFR&);
			static REL::Relocation<generates_t> generates{ ITEM_GENERATES_POWER };
			return RE::PowerUtils::ItemIsPowerReceiver(&a_ref) || RE::PowerUtils::ItemIsPowerConnection(&a_ref) || generates(a_ref);
		}

		// A piece of ours a friend's game has a copy of: one we built (made at runtime), standing, one of a workbench's
		// workshop items, with power, and no wire, actor (a turret: its copy isn't shown powered) or copy.
		bool OwnPowerPiece(RE::TESObjectREFR& a_ref)
		{
			const auto base = a_ref.GetObjectReference();
			return Protocol::IsRuntimeRef(a_ref.GetFormID()) && base && !IsWire(base) && !a_ref.As<RE::Actor>() && !a_ref.IsDisabled() && !a_ref.IsDeleted() &&
			       !IsCopy(&a_ref) && BenchOf(a_ref) && IsPowerPiece(a_ref);
		}

		// Our pieces with power, from the workshop items each workbench lists.
		void TrackOwnPower()
		{
			for (const auto bench : PowerBenches()) {
				const auto children = bench->extraList ? bench->extraList->GetByType<RE::ExtraLinkedRefChildren>() : nullptr;
				if (!children) {
					continue;
				}
				std::vector<std::uint32_t> ids;
				for (const auto& child : children->linkedChildren) {
					const auto ref = child.REFR.get();
					if (ref && child.keyword && child.keyword->GetFormID() == WORKSHOP_ITEM_KEYWORD && Protocol::IsRuntimeRef(ref->GetFormID()) && !reporter.Tracks(ref->GetFormID())) {
						ids.push_back(ref->GetFormID());
					}
				}
				for (const auto id : ids) {
					if (const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(id) }; ref && OwnPowerPiece(*ref)) {
						reporter.Track(id);
					}
				}
			}
		}

		struct ReporterEngine
		{
			std::optional<bool> Powered(std::uint32_t a_id)
			{
				using powered_t = bool (*)(RE::TESObjectREFR&);
				static REL::Relocation<powered_t>      isPowered{ IS_POWERED };
				const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) };
				if (!ref || !OwnPowerPiece(*ref)) {
					return std::nullopt;
				}
				return isPowered(*ref);
			}
		};

		void SendPower(Clock::time_point a_now)
		{
			const auto     start = Clock::now();
			ReporterEngine engine;
			const auto     entries = reporter.Poll(engine, a_now);
			pollUsMax = (std::max)(pollUsMax, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
			for (auto& chunk : WorkshopPower::Chunks(entries)) {
				const auto count = chunk.size();
				std::scoped_lock l{ outLock };
				if (outgoing.Push({ Protocol::Encode(Protocol::PowerStates{ 0, std::move(chunk) }, Protocol::MessageType::kReportWorkshopPower), 0 })) {
					powerSent += static_cast<std::uint32_t>(count);
				}
			}
		}

		// A friend's piece shown powered or not, as the game's own powering shows one (Workshop's power handler, 2195089):
		// the lights in its 3D switched (SwitchLight, which the handler calls for every piece, not only a LIGH: a
		// construction light is an activator with a light in its model) and its graph told ("Powered", "Unpowered": a
		// machine's running). Not through the handler itself: that fires the power events, and the scripts on a copy (a
		// workshop object's, a radio beacon's) would act on them as if it were ours. False while it has no 3D.
		bool ShowPowered(RE::TESObjectREFR& a_copy, bool a_on)
		{
			static const RE::BSFixedString        powered{ "Powered" };
			static const RE::BSFixedString        unpowered{ "Unpowered" };
			using switch_t = void (*)(RE::TESObjectREFR&, bool);
			static REL::Relocation<switch_t>      switchLight{ SWITCH_LIGHT };
			if (!a_copy.Get3D()) {
				return false;
			}
			switchLight(a_copy, !a_on);
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
			if (a_copy.GetAnimationGraphManagerImpl(graph) && graph) {
				a_copy.NotifyAnimationGraphImpl(a_on ? powered : unpowered);
			}
			return true;
		}

		struct MirrorEngine
		{
			RE::NiPointer<RE::TESObjectREFR> CopyOf(const WorkshopPower::Key& a_key)
			{
				const auto it = copies.find(a_key);
				auto       ref = it != copies.end() ? it->second.ref.get() : RE::NiPointer<RE::TESObjectREFR>{};
				if (!ref || !IsCopy(ref.get()) || ref->As<RE::Actor>() || ref->IsDisabled()) {
					return {};
				}
				return ref;
			}

			// Not drawn while its 3D isn't in the scene yet (no parent node): nothing of it is seen, and Mirror waits for it
			// without spending a try, however long its cell takes to come in.
			std::uintptr_t Drawn(const WorkshopPower::Key& a_key)
			{
				const auto ref = CopyOf(a_key);
				const auto model = ref ? ref->Get3D() : nullptr;
				return model && model->parent ? reinterpret_cast<std::uintptr_t>(model) : 0;
			}

			bool Show(const WorkshopPower::Key& a_key, bool a_on)
			{
				const auto ref = CopyOf(a_key);
				if (!ref || !ShowPowered(*ref, a_on)) {
					return false;
				}
				++powerShown;
				return true;
			}
		};

		void PowerFrame(Clock::time_point a_now)
		{
			if (a_now >= nextPowerAudit) {
				nextPowerAudit = a_now + WorkshopPower::AUDIT_EVERY;
				AuditPower("session");
				TrackOwnPower();
			}
			SendPower(a_now);
			MirrorEngine engine;
			mirror.Refresh(engine, a_now);
		}

		void ApplyNow(const Key& a_key, const Protocol::WorkshopItem& a_item)
		{
			if (a_item.op == Protocol::WorkshopOp::kScrapped) {
				mirror.Forget(a_key);
			}
			EngineWorld world;
			WorkshopApply::Apply(world, copies, twins, inbox, counts, a_key, a_item);
		}
	}

	bool WorkshopModeScraps(RE::TESObjectREFR& a_ref)
	{
		return CanScrap(a_ref);
	}

	RE::TESObjectREFR* WorkbenchOf(const RE::TESObjectREFR& a_ref)
	{
		return BenchOf(a_ref);
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
		const bool shares = BuiltWalk::Shares(a_host, HostRules::Get());
		++frameNumber;  // what the grids name is read again (GridNamed)
		std::erase_if(graveyard, [&](Copy& a_copy) {
			const auto ref = a_copy.ref.get();
			if (!ref) {
				return true;
			}
			if (!DeferredDelete::Safe(ref->Get3D() != nullptr, now - a_copy.placedAt)) {
				return false;
			}
			Delete(ref.get());
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
			// What our player built before the session: the host's always, a guest's only if the host's rules share it
			// (bShareGuestBuilds).
			if (shares) {
				OfferAndReportBuilt(fresh);
			}
			NoteBenches(fresh);
			for (const auto& cell : fresh) {
				TryPendingNear(cell.where);
				TryPendingScrapsIn(cell.id);
			}
			if (!fresh.empty()) {
				EngineWorld world;
				inbox.Retry(WorkshopApply::FoundSomewhere(world, inbox));  // their workbench may have come with one of them
			}
		}
		const auto applyStart = Clock::now();
		if (inbox.Drain(WorkshopInbox::APPLY_PER_FRAME, WorkshopInbox::APPLY_TIME_PER_FRAME, [] { return Clock::now(); }, ApplyNow) > 0) {
			applyUsMax = (std::max)(applyUsMax, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - applyStart).count());
		}
		if (a_host) {
			WalkAndSend(now);
		}
		if (shares) {
			BuiltEngine engine;
			builtWalk.ReportSome(engine);
		}
		CheckSettled(now);
		ReportMovedWires(now);
		DrawShapedWires(now);
		PowerFrame(now);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending)
	{
		std::vector<Outgoing> taken;
		{
			std::scoped_lock l{ outLock };
			taken = a_sending ? outgoing.TakeDue(Clock::now()) : outgoing.TakeAll();
			if (outgoing.Empty()) {
				droppedWarned = false;
			}
		}
		std::vector<std::vector<std::uint8_t>> packets;
		for (auto& out : taken) {
			if (a_sending) {
				packets.push_back(std::move(out.packet));
			} else if (out.worldScrap != 0) {
				sentScraps.Forget(out.worldScrap);
			}
		}
		return packets;
	}

	void Apply(const Protocol::WorkshopItem& a_item)
	{
		inbox.Arrive({ a_item.playerId, a_item.refId }, a_item);
	}

	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
	{
		ReportRef(a_ref, a_workshop, a_op);
	}

	void Reset()
	{
		EngineWorld world;
		WorkshopApply::RemoveAll(world, copies, twins, inbox);
		{
			std::scoped_lock l{ copyIdsLock };
			copyIds.clear();
		}
		ClearWalk();
		scrapsDeferred.clear();
		unbuiltWires.clear();
		{
			std::scoped_lock w{ wireChecksLock };
			wireChecks.clear();
		}
		{
			std::scoped_lock s{ settleLock };
			settle.Clear();
		}
		sentScraps.Clear();
		reporter.Clear();
		mirror.Clear();
		keptTied.clear();
		powerBenches.clear();
		benchWorld = 0;
		std::scoped_lock l{ outLock };
		outgoing.Clear();
	}

	void OnGameLoaded()
	{
		// The copies' handles name other things now, or nothing (see RemotePlayers::OnGameLoaded): they are
		// let go unread. The copies the save holds are found again, at their spots, when the server sends
		// the session's building again after the load (FindExisting); one it had removed since the save
		// stays. What waits in line, or for its cell, is reports, not handles, and stays.
		REX::INFO("WorkshopSync: load: forgot {} copies, {} in the graveyard", copies.size(), graveyard.size());
		copies.clear();
		twins.clear();  // the session's building sent again twins them again with the copies found
		graveyard.clear();
		unbuiltWires.clear();
		{
			std::scoped_lock l{ copyIdsLock };
			copyIds.clear();
		}
		ClearWalk();
		// Our pieces are sent again (the load may have set them otherwise), and friends' are shown again on the copies
		// found or made after the load. The workbenches are looked for again.
		reporter.SendAll();
		mirror.Invalidate();
		powerBenches.clear();
		benchWorld = 0;
	}

	void OnWorldGone()
	{
		copies.clear();
		twins.clear();
		graveyard.clear();
		unbuiltWires.clear();
		reporter.Clear();
		mirror.Invalidate();
		powerBenches.clear();
		benchWorld = 0;
		std::scoped_lock l{ copyIdsLock };
		copyIds.clear();
	}

	void ApplyPower(const Protocol::PowerStates& a_power)
	{
		for (const auto& entry : a_power.entries) {
			mirror.Set(a_power.playerId, entry);
		}
		powerReceived += static_cast<std::uint32_t>(a_power.entries.size());
	}

	bool SetPowerWantForDev(std::uint32_t a_player, std::uint32_t a_ref, bool a_powered)
	{
		return mirror.Set(a_player, WorkshopPower::Entry{ a_ref, a_powered });
	}

	void OnPlayerJoined()
	{
		reporter.SendAll();
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		mirror.Freeze(a_playerId);
	}

	void BeforeSave()
	{
		AuditPower("save");
		// A copy shown powered is saved running (its graph keeps the state), and a load brings it back running with no
		// session to stop it: shown unpowered for the save, powered again after (AfterSave).
		MirrorEngine engine;
		const auto   hushed = mirror.Hush(engine, Clock::now());
		if (hushed > 0) {
			REX::INFO("WorkshopSync: {} friends' pieces shown unpowered for the save", hushed);
		}
	}

	void AfterSave()
	{
		mirror.SaveDone();
	}

	bool IsCopyMarked(const RE::TESObjectREFR& a_ref)
	{
		return Blocked(a_ref);
	}

	bool HoldsAnything(RE::TESObjectREFR* a_ref)
	{
		return a_ref && HoldsItems(a_ref);
	}

	void SetBlockCopies(bool a_block)
	{
		blockCopies = a_block;
		static REL::Relocation<RE::BGSKeyword**> keyword{ BLOCK_INTERACTION_KEYWORD };
		if (!*keyword) {
			return;
		}
		std::vector<std::uint32_t> ids;
		{
			std::scoped_lock l{ copyIdsLock };
			ids.assign(copyIds.begin(), copyIds.end());
		}
		for (const auto id : ids) {
			if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id)) {
				if (a_block) {
					BlockInteraction(*ref);
				} else if (Blocked(*ref)) {
					ref->RemoveKeyword(*keyword);
				}
			}
		}
	}

	std::string AuditNow()
	{
		const auto before = untied;
		AuditPower("dev");
		return std::format("audit: found={} detached={} retired={}", untied.found - before.found, untied.detached - before.detached, untied.retired - before.retired);
	}

	bool ShowPoweredForDev(RE::TESObjectREFR* a_ref, bool a_on)
	{
		return a_ref && ShowPowered(*a_ref, a_on);
	}

	std::string DescribeCopies()
	{
		std::string out = std::format("{} copies", copies.size());
		for (const auto& [key, copy] : copies) {
			const auto ref = copy.ref.get();
			out += std::format("; {:08X}={}:{:08X}", ref ? ref->GetFormID() : 0, key.first, key.second);
		}
		return out;
	}

	std::string DescribePower()
	{
		return std::format("power: block={} blocked={} audits={} found={} detached={} retired={} wiresRemoved={} benches={} tracked={} sent={} received={} mirrored={} shown={} showCalls={} givenUp={} redrawsCapped={} hushed={} mirrorDropped={} keptTied={} auditSkips={} pollMsMax={:.2f}",
			blockCopies, copiesBlocked, audits, untied.found, untied.detached, untied.retired, wiresUntied, powerBenches.size(), reporter.Size(), powerSent, powerReceived, mirror.Size(),
			mirror.Shown(), powerShown, mirror.GivenUp(), mirror.RedrawsCapped(), mirror.Hushed(), mirror.Dropped(), keptTied.size(), auditSkips, static_cast<double>(pollUsMax) / 1000.0);
	}

	std::string DescribeGrids(RE::TESObjectREFR* a_bench)
	{
		std::vector<RE::TESObjectREFR*> benches;
		if (a_bench) {
			benches.push_back(a_bench);
		} else {
			benches = PowerBenches();
		}
		PowerEngine engine;
		std::string out = std::format("{} workbenches", benches.size());
		std::size_t copiesNamed = 0, gone = 0;
		for (const auto bench : benches) {
			const auto grids = GridsOf(bench);
			if (grids.empty() && !a_bench) {
				continue;
			}
			out += std::format("; {:08X}: {} grids", bench->GetFormID(), grids.size());
			for (const auto& grid : grids) {
				const auto named = WorkshopPower::Named(grid);
				out += std::format(" [nodes={} links={} powered={}", grid.nodes.size(), grid.edges.size(), grid.powered.size());
				if (a_bench) {  // one workbench asked for: its grids' pieces too
					for (std::size_t i = 0; i < grid.nodes.size() && i < 8; ++i) {
						out += std::format(" n{:08X}", grid.nodes[i]);
					}
					for (std::size_t i = 0; i < grid.edges.size() && i < 8; ++i) {
						out += std::format(" e{:08X}>{:08X}/{:08X}", grid.edges[i].node, grid.edges[i].connection, grid.edges[i].connector);
					}
				}
				for (const auto id : named) {
					const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
					if (engine.IsCopy(id) || (ref && Blocked(*ref))) {
						out += std::format(" COPY {:08X}", id);
						++copiesNamed;
					} else if (!ref || ref->IsDeleted()) {
						out += std::format(" GONE {:08X}", id);
						++gone;
					}
				}
				out += "]";
			}
		}
		return out + std::format("; copies named={} gone named={}", copiesNamed, gone);
	}

	std::string Describe()
	{
		std::uint32_t droppedNow = 0;
		std::size_t   unsent = 0;
		{
			std::scoped_lock l{ outLock };
			droppedNow = dropped;
			unsent = outgoing.Size();
		}
		return std::format("workshop: reported={} unsent={} dropped={} copies={} placed={} moved={} recreated={} baseMismatch={} scrapped={} rescued={} worldScrapped={} adopted={} oursThere={} leftovers={} twinned={} untwinned={} twins={} refused={} emptied={} pending={} inbox={} applyMsMax={:.1f}", reported, unsent, droppedNow, copies.size(),
			       counts.placed, counts.moved, counts.recreated, counts.baseMismatch, counts.scrapped, itemsRescued, worldScrapped, counts.adopted, counts.oursThere, counts.leftovers, counts.twinned,
			       counts.untwinned, twins.size(), counts.refused, counts.emptied, inbox.Pending().Size(), inbox.Queued(), static_cast<double>(applyUsMax) / 1000.0) +
		       std::format(" savedScraps={} (walked={} disabled={} deleted={} benches={} waiting={} queued={})", walk.Reported(), walk.SeenCounts().refs, walk.SeenCounts().disabled,
				   walk.SeenCounts().deleted, walk.Benches().size(), walk.Waiting(), walk.ToSend()) +
		       std::format(" anchorRefused={} scrapRefused={} scrapDeferred={} unplaceable={}", anchorRefused, scrapRefused, scrapsDeferred.size(), unplaceable) +
		       std::format(" builtBefore={} (queued={}) wiresDrawn={} movedOwn={} ownRefused={} settledAgain={} putBack={}", builtWalk.Reported(), builtWalk.Queued(), wiresDrawn, counts.movedOwn, counts.ownRefused, settledAgain, putBack) +
		       std::format(" framesCleared={}", counts.framesCleared);
	}

	std::string DescribeRef(RE::TESObjectREFR* a_ref)
	{
		const auto base = a_ref->GetObjectReference();
		const auto cell = a_ref->GetParentCell();
		const auto& at = a_ref->data.location;
		std::string out = std::format("{:08X} base={:08X} type={} copy={} bench={} disabled={} deleted={} cell={:08X} at {:.0f} {:.0f} {:.0f}", a_ref->GetFormID(),
			base ? base->GetFormID() : 0, base ? base->GetFormTypeString() : "none", IsCopy(a_ref), IsBench(a_ref), a_ref->IsDisabled(), a_ref->IsDeleted(),
			cell ? cell->GetFormID() : 0, at.x, at.y, at.z);
		if (const auto list = a_ref->inventoryList) {
			RE::BSAutoReadLock l{ list->rwLock };
			out += std::format(" items={}", list->data.size());
		}
		out += std::format(" blocked={} powerPiece={}", Blocked(*a_ref), IsPowerPiece(*a_ref));
		for (const auto& [key, copy] : copies) {
			if (copy.ref.get().get() == a_ref) {
				const auto wanted = mirror.Wanted(key);
				out += std::format(" builder={} theirRef={:08X} theirPower={}", key.first, key.second, wanted ? (*wanted ? "on" : "off") : "unknown");
				break;
			}
		}
		if (BenchOf(*a_ref)) {
			ReporterEngine engine;
			const auto     powered = engine.Powered(a_ref->GetFormID());
			out += std::format(" powered={}", powered ? (*powered ? "yes" : "no") : "n/a");
		}
		const auto extra = a_ref->extraList.get();
		if (!extra) {
			return out;
		}
		if (const auto linked = extra->GetByType<RE::ExtraLinkedRef>()) {
			for (const auto& link : linked->linkedRefs) {
				out += std::format(" link[{:08X}]={:08X}", link.keyword ? link.keyword->GetFormID() : 0, link.REFR ? link.REFR->GetFormID() : 0);
			}
		}
		if (const auto children = extra->GetByType<RE::ExtraLinkedRefChildren>()) {
			std::size_t shown = 0;
			for (const auto& child : children->linkedChildren) {
				const auto ref = child.REFR.get();
				if (shown++ < 24) {
					out += std::format(" child[{:08X}]={:08X}", child.keyword ? child.keyword->GetFormID() : 0, ref ? ref->GetFormID() : 0);
				}
			}
			out += std::format(" children={}", children->linkedChildren.size());
		}
		if (const auto power = extra->GetByType<RE::ExtraPowerLinks>()) {
			for (const auto& link : power->powerLinks) {
				out += std::format(" power={:08X}/{}", link.formID, link.linkType);
			}
		}
		if (const auto spline = extra->GetByType<RE::ExtraBendableSplineParams>()) {
			const auto raw = reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(spline) + 0x18);
			out += std::format(" spline slack={:.2f} thickness={:.2f} half={:.1f},{:.1f},{:.1f} detached={}", raw[0], raw[1], raw[2], raw[3], raw[4],
				*(reinterpret_cast<const std::uint8_t*>(spline) + 0x2C));
		}
		for (std::int32_t type = 0; type < RE::EXTRA_DATA_TYPE::kTotal; ++type) {
			if (extra->HasType(static_cast<RE::EXTRA_DATA_TYPE>(type))) {
				out += std::format(" x{:02X}", type);
			}
		}
		return out;
	}

	std::string DescribeBenches(RE::TESObjectREFR* a_ref)
	{
		if (a_ref && !IsBench(a_ref)) {
			// What the walk makes of this reference.
			if (const auto why = WhyNotScrapped(a_ref)) {
				return std::format("{:08X}: not reported: {}", a_ref->GetFormID(), why);
			}
			WalkEngine engine;
			const auto bench = walk.BenchFor(engine, a_ref->GetFormID(), SpaceOf(a_ref->GetParentCell()));
			return bench ? std::format("{:08X}: scrapped junk in the build area of workbench {:08X}", a_ref->GetFormID(), bench) :
			               std::format("{:08X}: scrapped junk, but in no build area of a workbench seen", a_ref->GetFormID());
		}
		std::vector<RE::TESObjectREFR*> refs;
		if (a_ref) {
			refs.push_back(a_ref);
		} else {
			for (const auto& bench : walk.Benches()) {
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
