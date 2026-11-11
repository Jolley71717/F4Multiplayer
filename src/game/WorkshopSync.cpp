#include "game/WorkshopSync.h"

#include "game/BuiltWalk.h"
#include "game/ClickWatch.h"
#include "game/CraftingWorkshop.h"
#include "game/Drops.h"
#include "game/EngineRefs.h"
#include "game/HostRules.h"
#include "game/RemoteEdits.h"
#include "game/SavedScraps.h"
#include "game/SaveStrays.h"
#include "game/SendPacer.h"
#include "game/SharedWorkshop.h"
#include "game/WorkshopApply.h"
#include "game/WorkshopAudit.h"
#include "game/WorkshopCells.h"
#include "game/WorkshopInbox.h"
#include "game/WorkshopMove.h"
#include "game/WorkshopPower.h"
#include "game/WorkshopResources.h"
#include "game/WorkshopSettle.h"

#include "game/DeferredDelete.h"
#include "game/Equipment.h"
#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/MenuWatch.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/Puppets.h"
#include "game/Story.h"
#include "game/TextureFlush.h"
#include "MainThread.h"
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

		// A copy as we keep it: its handle, and its form ID and base, which find it again when the handle names nothing (its
		// cell unloaded and loaded again: see WorkshopApply::SameCopy). Read through Live, never through the handle alone.
		struct Copy
		{
			mutable RE::ObjectRefHandle ref;  // taken anew by Live
			Clock::time_point           placedAt;
			std::uint32_t               id = 0;
			std::uint32_t               base = 0;
			mutable Clock::time_point   missedAt{};  // Live's last look under its ID that found nothing (its cell unloaded)
		};

		constexpr auto LOOK_AGAIN_AFTER = std::chrono::seconds(1);  // how often Live looks for a copy whose cell isn't loaded

		Copy MakeCopy(const RE::ObjectRefHandle& a_handle, Clock::time_point a_at)
		{
			Copy copy{ a_handle, a_at };
			if (const auto ref = a_handle.get()) {
				const auto base = ref->GetObjectReference();
				copy.id = ref->GetFormID();
				copy.base = base ? base->GetFormID() : 0;
			}
			return copy;
		}

		std::uint32_t refound = 0;        // copies found again under their ID, their handle naming nothing (Live)
		std::uint32_t refoundLogged = 0;  // ... said in the log (the first few of a session)

		bool IsWorkshopObject(const RE::TESObjectREFR* a_ref);
		void ForgetGraphTold(std::uint32_t a_copy);
		// The shared workshop (defined with it, below): a piece our workshop mode placed taken as a request of the host, and
		// whether a piece of ours waits for the host's answer (its moves and scraps are not reported meanwhile).
		bool HandOver(RE::TESObjectREFR* a_item, RE::TESObjectREFR* a_bench);
		bool IsPendingPiece(std::uint32_t a_id);
		const char* PendingNoticeOf(std::uint32_t a_id);  // the notice for a key on a copy something is asked of already (a scrap or a store)
		void AuditStrays(std::string_view a_when);
		void SharedBeforeSave();
		// The last reference a destroyed event named, on the main thread: what the host's store for a friend reads after its one
		// engine call (the engine's store deletes a created piece deferred, so the reference may be neither disabled nor deleted yet).
		std::uint32_t destroyedHere = 0;

		// The copy's reference: through its handle, or, once that names nothing (its cell unloaded), the reference under its
		// ID while that is the copy (WorkshopApply::SameCopy) and no workshop object of ours (an ID something else freed
		// and our own build took since), whose handle is kept from then on. None while its cell isn't loaded. Its graph is
		// made anew with its 3D: what we told the old one is forgotten.
		RE::NiPointer<RE::TESObjectREFR> Live(const Copy& a_copy)
		{
			if (auto ref = a_copy.ref.get()) {
				return ref;
			}
			const auto now = Clock::now();
			if (a_copy.id == 0 || now - a_copy.missedAt < LOOK_AGAIN_AFTER) {
				return {};  // (the Mirror asks four times a second for each copy)
			}
			RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_copy.id) };
			const auto                       base = ref ? ref->GetObjectReference() : nullptr;
			if (!ref || !WorkshopApply::SameCopy(a_copy.id, base ? base->GetFormID() : 0, a_copy.base, ref->IsDeleted()) || IsWorkshopObject(ref.get())) {
				a_copy.missedAt = now;
				return {};
			}
			a_copy.ref = ref->GetHandle();
			ForgetGraphTold(a_copy.id);
			++refound;
			if (refoundLogged < 16) {
				++refoundLogged;
				REX::INFO("WorkshopSync: copy {:08X} (base {:08X}) found again under its ID: its cell was unloaded and loaded again", a_copy.id, a_copy.base);
			}
			return ref;
		}

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
		// The workbench each copy's report named, for the crafting menus' gather (CraftingWorkshop.h), which runs on the
		// menu's thread. Under copyIdsLock, in step with copyIds.
		CraftingWorkshop::Benches copyBenches;
		// Whose piece each copy is (the builder's player id), for the shared workshop's picks (which run on whatever thread
		// workshop mode's pick does). Under copyIdsLock, in step with copyIds.
		std::unordered_map<std::uint32_t, std::uint32_t> copyBuilders;

		// Our copies of friends' lights (LIGH) shown powered, by form ID: their light is made as the game makes a powered
		// workshop light's (LightPoweredHook). Under its own lock: the game makes lights on its loading threads too.
		std::mutex                        litLock;
		std::unordered_set<std::uint32_t> litCopies;

		void NoteCopy(const Copy& a_copy, std::uint32_t a_bench, std::uint32_t a_builder)
		{
			if (a_copy.id != 0) {
				std::scoped_lock l{ copyIdsLock };
				copyIds.insert(a_copy.id);
				copyBenches.Note(a_copy.id, a_bench);
				copyBuilders[a_copy.id] = a_builder;
			}
		}

		// By its ID: one whose cell is unloaded (its handle naming nothing) is forgotten too.
		void ForgetCopy(const Copy& a_copy)
		{
			if (a_copy.id != 0) {
				{
					std::scoped_lock l{ copyIdsLock };
					copyIds.erase(a_copy.id);
					copyBenches.Forget(a_copy.id);
					copyBuilders.erase(a_copy.id);
				}
				std::scoped_lock l{ litLock };
				litCopies.erase(a_copy.id);
			}
		}

		void ForgetAllCopies()
		{
			{
				std::scoped_lock l{ copyIdsLock };
				copyIds.clear();
				copyBenches.Clear();
				copyBuilders.clear();
			}
			std::scoped_lock l{ litLock };
			litCopies.clear();
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

		// What the session keeps as ours, looked for in our world (WorkshopAudit): what it lacks is reported gone. Under
		// its own lock: Push (from the event sinks, maybe off the game thread) takes out what we report.
		std::mutex           auditLock;
		WorkshopAudit::Audit audit;  // under auditLock
		// How long each attached cell has been (CellAges), and the audit's time: frames while the world is settled (AuditFrame).
		WorkshopAudit::CellAges cellAges;      // under auditLock
		WorkshopAudit::Duration auditClock{};  // under auditLock
		std::optional<std::chrono::steady_clock::time_point> lastAuditFrame;  // under auditLock

		// Forgets the list and the cells' ages (a load, the world gone, the session left).
		void ClearAudit()
		{
			std::scoped_lock a{ auditLock };
			audit.Clear();
			cellAges.Clear();
			lastAuditFrame.reset();
		}

		// False if it's dropped (too many waiting to be sent).
		bool Push(const Protocol::WorkshopItem& a_item)
		{
			const auto worldScrap = a_item.op == Protocol::WorkshopOp::kScrapped && Protocol::IsShareableRef(a_item.refId) ? a_item.refId : 0;
			{
				std::scoped_lock l{ outLock };
				if (!outgoing.Push({ Protocol::Encode(a_item, Protocol::MessageType::kReportWorkshopItem), worldScrap })) {
					++dropped;
					if (!std::exchange(droppedWarned, true)) {
						REX::WARN("WorkshopSync: {} reports wait to be sent; dropping more until they go (the others won't see those pieces)", outgoing.Size());
					}
					return false;
				}
				++reported;
			}
			std::scoped_lock a{ auditLock };
			audit.Reported(a_item.refId);
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
			if (a_ref == a_workshop || IsWorkbenchAnchor(a_ref)) {
				// A settlement's workbench is never one of its pieces (a console report of the wrong reference): kept as built,
				// it would be sent at every join, and scrapped, every friend's game would scrap its own workbench.
				return false;
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
				if (HandOver(a_event.placedItem.get(), a_event.workshop.get())) {
					return RE::BSEventNotifyControl::kContinue;  // asked of the host (or taken off): not ours to report
				}
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
				if (a_event.movedItem && IsPendingPiece(a_event.movedItem->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;  // asked of the host where it was placed: the host's piece is where it stands
				}
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
				destroyedHere = id;
				// Our copy of a friend's drop, scrapped: the item is our player's take now (they have its components), so it
				// goes for everyone. Not a workshop piece of ours to report.
				const auto bench = a_event.workshop.get();
				if (Drops::OnScrapped(id, bench ? bench->GetFormID() : 0)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (IsPendingPiece(id)) {
					return RE::BSEventNotifyControl::kContinue;  // never reported placed: nothing to report scrapped (the host's piece, if made, stands)
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
			const auto ref = Live(a_copy);
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
			const auto ref = Live(a_copy);
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
			const auto id = a_ref->GetFormID();
			return std::ranges::any_of(graveyard, [&](const Copy& a_copy) { return a_copy.id == id; });
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
		// What it does is decided by whether the reference has its light (ExtraLight) when it is called, and done later, by a
		// task (0x7A) the game runs in a frame after ours: with "off" and a light, the light is removed (+5003E0); otherwise, with
		// 3D and no light, one is made (+500260, through the light maker), "off" or not. In the game's own powering that make
		// is harmless, as the light maker asks Workshop::IsPowered first; for our copies it asks litCopies (LightPoweredHook).
		constexpr REL::ID SWITCH_LIGHT{ 2198253 };
		// A reference's conveyor belt (BGSConveyorBelt::ConveyorBelt, kind 2 of its init actions, ExtraInitActions), as
		// Papyrus's conveyor natives use it (ObjectReference.ConveyorBeltOn, IsConveyorBeltOn: 1.11.240's script functions
		// jump straight to these). The find (+259210) gives the belt or null; running (+259170) whether it has one that runs;
		// set (+259120) sets it running or stopped (through +259D90: its flag, and its surface velocity on the 3D's belt
		// node), true if it has one. Nothing but Papyrus calls the set: the power handler (2195089) never touches a belt.
		constexpr REL::ID FIND_CONVEYOR_BELT{ 2189355 };
		constexpr REL::ID CONVEYOR_BELT_RUNS{ 2189353 };
		constexpr REL::ID SET_CONVEYOR_BELT{ 2189352 };

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
					if (copy.id != 0) {
						ids.push_back(copy.id);
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
			AuditStrays(a_when);  // the shared workshop's: nothing in-session of ours the host took over stays on a grid
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

		// Deletes the copy now, or once it is safe to; one whose cell isn't loaded (no reference under its ID now) waits in the
		// graveyard until it is, and goes then (Frame in a session, Sweep out of one): leaving the session, or a friend's
		// scrap, deletes it even if we were elsewhere.
		void Remove(Copy& a_copy)
		{
			const bool known = static_cast<bool>(a_copy.ref.get());
			const auto ref = Live(a_copy);
			if (!ref) {
				if (a_copy.id != 0) {
					graveyard.push_back(a_copy);
				}
				return;
			}
			if (!known) {
				a_copy.placedAt = Clock::now();  // made again with its cell just now: its 3D may still be loading (DeferredDelete)
			}
			if (DeferredDelete::Safe(ref->Get3D() != nullptr, Clock::now() - a_copy.placedAt)) {
				Delete(ref.get());
			} else {
				graveyard.push_back(a_copy);
			}
		}

		// Deletes what waits in the graveyard once it may be: its 3D in, or long enough after it was placed, or made again
		// with its cell. One whose cell isn't loaded waits for it (until a load, which lets the graveyard go).
		Clock::time_point nextGraveyardLook{};

		void SweepGraveyard(Clock::time_point a_now)
		{
			if (graveyard.empty() || a_now < nextGraveyardLook) {
				return;
			}
			nextGraveyardLook = a_now + std::chrono::milliseconds(250);  // those whose cell isn't loaded are looked up by ID
			std::erase_if(graveyard, [&](Copy& a_copy) {
				if (a_copy.id == 0) {
					return true;
				}
				const bool known = static_cast<bool>(a_copy.ref.get());
				const auto ref = Live(a_copy);
				if (!ref) {
					return false;
				}
				if (!known) {
					a_copy.placedAt = a_now;  // its cell just loaded again: waited for like a copy just placed
				}
				if (IsCopy(ref.get())) {
					return true;  // its ID is a copy we keep now (given out again after all): not this one any more
				}
				if (!DeferredDelete::Safe(ref->Get3D() != nullptr, a_now - a_copy.placedAt)) {
					return false;
				}
				Delete(ref.get());
				return true;
			});
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
			return MakeCopy(handle, Clock::now());
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
			// A copy made, or found standing from an earlier session: ours to remove, and out of workshop mode's reach. Noted with
			// the workbench its report names, which a crafting station's copy reads as its settlement (CraftingWorkshop.h).
			void Keep(const Copy& a_copy, const Protocol::WorkshopItem& a_item)
			{
				NoteCopy(a_copy, a_item.workshop, a_item.playerId);
				if (const auto ref = Live(a_copy)) {
					BlockInteraction(*ref);
				}
			}
			WorkshopApply::Placeable CanPlace(const Protocol::WorkshopItem& a_item) { return WorkshopSync::CanPlace(a_item); }
			std::optional<Copy>      Create(const Protocol::WorkshopItem& a_item) { return WorkshopSync::Create(a_item); }
			// Destroys what the copy holds (its base's things: it was just made), as the workshop's own build does.
			void Empty(const Copy& a_copy)
			{
				if (const auto ref = Live(a_copy)) {
					RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", ref->GetFormID()).c_str());
				}
			}
			bool                     ScrapWorldRef(const Protocol::WorkshopItem& a_item) { return WorkshopSync::ScrapWorldRef(a_item); }

			void Discard(const Copy& a_copy)
			{
				ForgetCopy(a_copy);
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
				return MakeCopy(ref->GetHandle(), Clock::now());
			}

			std::optional<Copy> FindExisting(const Protocol::WorkshopItem& a_item)
			{
				const auto existing = WorkshopSync::FindExisting(a_item);
				return existing ? std::optional<Copy>{ MakeCopy(existing->GetHandle(), Clock::now()) } : std::nullopt;
			}

			// Keep gave it the keyword (since 0.9.0); nothing else of ours, nor the game, gives a frame that one. Not a
			// frame that holds anything: Delete hands back only a container's, and a player may have fitted that one out
			// (or it is their own suit an earlier session took over), so it stays, pieces and all.
			bool MadeByUs(const Copy& a_copy)
			{
				const auto ref = Live(a_copy);
				return ref && Blocked(*ref) && !HoldsItems(ref.get());
			}

			bool OursAt(const Protocol::WorkshopItem& a_item) { return WorkshopSync::OursAt(a_item); }
			bool CopyAt(const Protocol::WorkshopItem& a_item) { return WorkshopSync::CopyAt(a_item); }

			// Only a copy of ours (it has the keyword Keep gives): never a loose thing of the same object.
			std::optional<Copy> LeftoverAt(const Protocol::WorkshopItem& a_item)
			{
				const auto leftover = AtSpot(a_item, [](RE::TESObjectREFR* a_ref) { return !IsWorkshopObject(a_ref) && Blocked(*a_ref); });
				return leftover ? std::optional<Copy>{ MakeCopy(leftover->GetHandle(), Clock::now()) } : std::nullopt;
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

		// The cells attached since the last look (after a game load, all of them), and (a_all) every cell attached now.
		std::vector<AttachedCell> NewlyAttachedCells(std::vector<WorkshopCells::Where>* a_all = nullptr)
		{
			std::vector<AttachedCell> fresh;
			const auto                tes = RE::TES::GetSingleton();
			if (!tes) {
				return fresh;
			}
			std::set<std::uint32_t> attached;
			const auto note = [&](RE::TESObjectCELL* a_cell) {
				const auto           id = a_cell->GetFormID();
				WorkshopCells::Where where;
				if (a_cell->IsInterior()) {
					where = WorkshopCells::Interior(id);
				} else if (const auto exterior = a_cell->cellData.exterior; exterior && a_cell->worldSpace) {
					where = { a_cell->worldSpace->GetFormID(), exterior->cellX, exterior->cellY };
				}
				if (a_all && where.space != 0) {
					a_all->push_back(where);
				}
				attached.insert(id);
				if (!attachedCells.contains(id)) {
					fresh.push_back({ id, where });
				}
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

		// ---- Which of the pieces the session keeps as ours our world lacks (WorkshopAudit) ----------------------------

		// The world is settled enough to say what it lacks: the player in a cell, no loading screen or load under way, no
		// menu stopping the game, not the opening (a new game's world isn't the session's yet). As the session ends
		// (OnWorldGone, Reset) the list is gone already.
		bool AuditSettled()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() && !Hud::LoadingScreenUp() && Hud::IsSettled() && !GameStop::Now() && !Story::InOpening();
		}

		// Under auditLock (cellAges, auditClock).
		struct AuditEngine
		{
			WorkshopAudit::Seen Look(const Protocol::WorkshopItem& a_item)
			{
				using WorkshopAudit::Seen;
				if (const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.refId) }) {
					const auto base = ref->GetObjectReference();
					if (ref->IsDeleted() || !base || base->GetFormID() != a_item.base || IsCopy(ref.get()) || Drops::IsCopyRef(ref->GetFormID())) {
						return Seen::kGone;  // the ID is something else here now
					}
					return Seen::kStands;
				}
				// A piece that isn't persistent is in memory only while the cell it was built in is loaded, and one moved
				// since keeps that cell: missing means gone only with every cell around its spot loaded long enough to have
				// made its references, while we stand in its settlement's build area (the cells around us are loaded).
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto here = player ? player->GetParentCell() : nullptr;
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_item.workshop);
				const auto benchCell = bench ? bench->GetParentCell() : nullptr;
				if (!here || !benchCell) {
					return Seen::kUnseen;  // its workbench isn't loaded here (or a plugin's we lack)
				}
				if (benchCell->IsInterior()) {
					if (benchCell != here || !cellAges.Settled(WorkshopCells::Interior(here->GetFormID()), auditClock)) {
						return Seen::kUnseen;
					}
				} else {
					if (here->IsInterior() || benchCell->worldSpace != here->worldSpace || !here->worldSpace) {
						return Seen::kUnseen;
					}
					const auto space = here->worldSpace->GetFormID();
					for (const auto& [x, y] : WorkshopAudit::Spots(a_item)) {
						if (!cellAges.SettledAround(space, x, y, auditClock)) {
							return Seen::kUnseen;
						}
					}
				}
				return InBuildArea(*bench, player->data.location) ? Seen::kMissing : Seen::kUnseen;
			}
		};

		// A frame's part of the audit: its time goes on while the world is settled, and a round of looks may run. What is
		// gone is reported, in line behind our other reports; a lost piece found standing is reported placed again.
		void AuditFrame(Clock::time_point a_now)
		{
			// A frame longer than this (a hitch, a save) counts as this: the audit's time is the world's, seen running.
			constexpr auto LONGEST_FRAME = std::chrono::milliseconds(250);
			const bool     settled = AuditSettled();
			WorkshopAudit::Round round;
			{
				std::scoped_lock l{ auditLock };
				const auto       last = std::exchange(lastAuditFrame, a_now);
				if (!settled) {
					return;
				}
				if (last) {
					auditClock += (std::min)(std::chrono::duration_cast<WorkshopAudit::Duration>(a_now - *last), std::chrono::duration_cast<WorkshopAudit::Duration>(LONGEST_FRAME));
				}
				AuditEngine engine;
				round = audit.Look(engine, auditClock);
			}
			for (const auto& item : round.found) {
				const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(item.refId) };
				const auto                             bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(item.workshop);
				if (ref && bench && ReportRef(ref.get(), bench, Protocol::WorkshopOp::kPlaced)) {
					REX::INFO("WorkshopSync: our world has {:08X} (base {:08X}) again, which it had lacked: reported placed, back for everyone", item.refId, item.base);
				}
			}
			if (round.gone.empty()) {
				return;
			}
			const auto& gone = round.gone;
			for (const auto& item : gone) {
				REX::INFO("WorkshopSync: our world has no {:08X} (base {:08X}) the session keeps as ours (a save from before it was built): reported gone", item.refId, item.base);
			}
			std::scoped_lock l{ outLock };
			for (std::size_t i = 0; i < gone.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
				const auto end = (std::min)(gone.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
				if (!outgoing.Push({ Protocol::Encode(std::vector<Protocol::WorkshopItem>(gone.begin() + i, gone.begin() + end), Protocol::MessageType::kReportWorkshopGone), 0 })) {
					REX::WARN("WorkshopSync: {} reports wait to be sent; {} pieces our world lacks stay for the others until our next load", outgoing.Size(), gone.size() - i);
					break;
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
		WorkshopPower::Strays   strays;    // friends' copies our save holds that no session keeps, shown unpowered
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

		std::uint32_t graphRefused = 0;  // shows a copy's loaded graph didn't take and wasn't showing already (a try spent)
		std::uint32_t graphAlready = 0;  // shows a copy's loaded graph refused because it showed that already (WorkshopPower::RefusalShows)
		std::uint32_t graphWaits = 0;    // looks at a copy whose graph wasn't loaded yet (waited for, no try spent)
		std::uint32_t showsLogged = 0;   // shows written to the log (the first SHOWS_LOGGED of a session)
		constexpr std::uint32_t SHOWS_LOGGED = 32;

		bool IsLightBase(const RE::TESObjectREFR& a_ref)
		{
			const auto base = a_ref.GetObjectReference();
			return base && base->GetFormType() == RE::ENUM_FORM_ID::kLIGH;
		}

		// Whether the copy's model names an animation graph (a NIF's BSBehaviorGraphExtraData, "BGED"): the game loads
		// that graph after the 3D, on its own.
		bool NamesGraph(const RE::NiAVObject& a_model)
		{
			static const RE::BSFixedString bged{ "BGED" };
			return a_model.HasExtraData(bged);
		}

		// The copy's loaded graph (its manager's first), or 0 while there is none.
		std::uintptr_t LoadedGraph(const RE::TESObjectREFR& a_ref)
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!a_ref.GetAnimationGraphManagerImpl(manager) || !manager || manager->graph.size() == 0) {
				return 0;
			}
			return reinterpret_cast<std::uintptr_t>(manager->graph[0].get());
		}

		// A friend's piece shown powered or not, as the game's own powering shows one (Workshop's power handler, 2195089),
		// without the handler itself: that fires the power events, and the scripts on a copy (a workshop object's, a radio
		// beacon's) would act on them as if it were ours.
		//
		// A light (LIGH: the light bulb, the industrial light) is lit by switching its light (SwitchLight, which the handler
		// calls and which does nothing for any other form). Every other piece, the activator lights among them (the
		// construction light, the light box, the high-tech lights, the strobe), shows its power through its animation graph,
		// told "Powered" or "Unpowered". False while it has no 3D, or when its graph is loaded and doesn't take the event
		// and isn't showing it already (Mirror tries again); a piece with no graph and no light has nothing more to show. A
		// graph refuses an event it has no move for from where it stands: one that refuses "Unpowered" is dark already, and
		// one that refuses "Powered" lit already, unless we told it the other way (it may still be turning): see
		// WorkshopPower::RefusalShows.
		//
		// A light's base may make its light only while Workshop::IsPowered says the reference is powered (the light bulb's
		// does): no copy is ever, so its light is made only because it is in litCopies (LightPoweredHook), which it is put in
		// before its light is switched on, and taken out of before it is switched off. The switch is asked for only when the
		// light isn't as wanted (SwitchLight with "off" makes a light where there is none) and is done a frame later (a task):
		// a light shown on while the removal asked for a moment before is still to come goes dark after all, which the
		// Mirror sees (Holds) and shows again.
		//
		// For a save (a_forSave), a light's light is let be and only its graph is shown unpowered: the light (ExtraLight) is
		// the game's at runtime and no save keeps it, and a load makes it again only for one in litCopies, which the load
		// empties (BeforeLoad). Switching it off and on around the save is what left the rig's bulb dark at every other save:
		// the off's removal ran after the on, which found the light still there.
		//
		// A conveyor belt (Contraptions Workshop's belts: the engine's conveyor on the reference, made running with its 3D
		// when the model asks for one, +50A47B, or by a script's ApplyConveyorBelt) runs or stops only as Papyrus says
		// (ConveyorBeltOn: nothing native calls it, and the power handler doesn't touch it), so by the belt's own script on
		// the power events, which a copy never gets. A copy's belt stayed as its script left it, whatever its builder's game
		// said (a 0.11.1 report: conveyors built and powered, not running on the friend's screen). Now it is set as
		// ConveyorBeltOn sets it (its native, at once, no events), for a save's hush too (unlike a light's light: nothing is
		// left to run after it). Its script may set it again after our show (a copy is never powered for it): Holds sees
		// that and the Mirror shows it again.
		using Took = WorkshopPower::Took;

		// What we last told each copy's graph. Emptied with the copies (Reset, a load, the world gone); a copy's entry goes
		// with it.
		WorkshopPower::GraphsTold graphsTold;

		void ForgetGraphTold(std::uint32_t a_copy) { graphsTold.Forget(a_copy); }

		void SetLit(std::uint32_t a_copy, bool a_lit)
		{
			std::scoped_lock l{ litLock };
			if (a_lit) {
				litCopies.insert(a_copy);
			} else {
				litCopies.erase(a_copy);
			}
		}

		bool HasLight(const RE::TESObjectREFR& a_ref)
		{
			return a_ref.extraList && a_ref.extraList->HasType(RE::EXTRA_DATA_TYPE::kLight);
		}

		std::uint32_t beltsSwitched = 0;  // copies' conveyor belts set running or stopped (ShowPoweredHow)

		// Whether the reference's conveyor belt runs, or none if it has none (no init actions, or none a conveyor). Its
		// extra data's lock is taken by the engine's find, as Papyrus's ConveyorBeltOn and IsConveyorBeltOn take it.
		std::optional<bool> BeltRuns(RE::TESObjectREFR& a_ref)
		{
			using find_t = void* (*)(RE::TESObjectREFR&);
			using runs_t = bool (*)(RE::TESObjectREFR&);
			static REL::Relocation<find_t> find{ FIND_CONVEYOR_BELT };
			static REL::Relocation<runs_t> runs{ CONVEYOR_BELT_RUNS };
			if (!a_ref.extraList || !a_ref.extraList->HasType(RE::EXTRA_DATA_TYPE::kInitActions) || !find(a_ref)) {
				return std::nullopt;
			}
			return runs(a_ref);
		}

		// Sets the reference's conveyor belt running or stopped, if it has one: false if it has none.
		bool SetBelt(RE::TESObjectREFR& a_ref, bool a_on)
		{
			using set_t = bool (*)(RE::TESObjectREFR&, bool);
			static REL::Relocation<set_t> set{ SET_CONVEYOR_BELT };
			const auto runs = BeltRuns(a_ref);
			if (!runs) {
				return false;
			}
			if (*runs != a_on && set(a_ref, a_on)) {
				++beltsSwitched;
			}
			return true;
		}

		Took ShowPoweredHow(RE::TESObjectREFR& a_copy, bool a_on, bool a_forSave = false)
		{
			static const RE::BSFixedString        powered{ "Powered" };
			static const RE::BSFixedString        unpowered{ "Unpowered" };
			using switch_t = void (*)(RE::TESObjectREFR&, bool);
			static REL::Relocation<switch_t>      switchLight{ SWITCH_LIGHT };
			WorkshopPower::ShowParts parts;
			parts.model = a_copy.Get3D() != nullptr;
			if (!parts.model) {
				return WorkshopPower::Outcome(parts, a_on);
			}
			const auto id = a_copy.GetFormID();
			parts.light = IsLightBase(a_copy);
			if (parts.light && !a_forSave) {
				if (IsCopy(&a_copy)) {
					SetLit(id, a_on);  // first: switching it on makes its light, which asks LightPoweredHook
				}
				if (HasLight(a_copy) != a_on) {
					switchLight(a_copy, !a_on);
				}
			}
			parts.belt = !parts.light && SetBelt(a_copy, a_on);
			const auto graph = LoadedGraph(a_copy);
			parts.graph = graph != 0;
			if (parts.graph) {
				parts.graphTook = a_copy.NotifyAnimationGraphImpl(a_on ? powered : unpowered);
				if (parts.graphTook) {
					graphsTold.Took(id, graph, a_on);
				} else if (graphsTold.Refused(id, graph, a_on)) {
					parts.alreadyShows = true;
					++graphAlready;
				}
			}
			const auto took = WorkshopPower::Outcome(parts, a_on);
			if (took == Took::kRefused) {
				++graphRefused;
			}
			return took;
		}

		bool ShowPowered(RE::TESObjectREFR& a_copy, bool a_on)
		{
			return WorkshopPower::ShowCounts(ShowPoweredHow(a_copy, a_on));
		}

		// A copy's 3D as shows of its power go by it (WorkshopPower::Drawing): see MirrorEngine::Drawn.
		std::uintptr_t DrawingOf(RE::TESObjectREFR& a_ref)
		{
			const auto model = a_ref.Get3D();
			if (!model) {
				return 0;
			}
			const bool namesGraph = !IsLightBase(a_ref) && NamesGraph(*model);
			const auto graph = LoadedGraph(a_ref);
			if (model->parent && namesGraph && graph == 0) {
				++graphWaits;
			}
			return WorkshopPower::Drawing(reinterpret_cast<std::uintptr_t>(model), model->parent != nullptr, namesGraph, graph);
		}

		struct MirrorEngine
		{
			bool forSave = false;  // shows for a save's hush (ShowPoweredHow: a light's light is let be)

			RE::NiPointer<RE::TESObjectREFR> CopyOf(const WorkshopPower::Key& a_key)
			{
				const auto it = copies.find(a_key);
				auto       ref = it != copies.end() ? Live(it->second) : RE::NiPointer<RE::TESObjectREFR>{};
				if (!ref || !IsCopy(ref.get()) || ref->As<RE::Actor>() || ref->IsDisabled()) {
					return {};
				}
				return ref;
			}

			// Not drawn while its 3D isn't in the scene yet (no parent node), nor while the graph its model names isn't
			// loaded yet (a light's isn't waited for: its light is what's switched): nothing it is shown would hold, and
			// Mirror waits for it without spending a try, however long its cell or its graph takes to come in. Drawn anew
			// when its graph is made anew, too: a new graph starts unpowered (WorkshopPower::Drawing).
			std::uintptr_t Drawn(const WorkshopPower::Key& a_key)
			{
				const auto ref = CopyOf(a_key);
				return ref ? DrawingOf(*ref) : 0;
			}

			// A light's light there while it is shown on, and gone while shown off; a conveyor belt running while shown on,
			// and stopped while shown off (its own script may have set it since); anything else holds what it was shown (its
			// graph keeps it; one drawn anew is Drawn's).
			bool Holds(const WorkshopPower::Key& a_key, bool a_on)
			{
				const auto ref = CopyOf(a_key);
				if (!ref || !ref->Get3D()) {
					return true;
				}
				if (IsLightBase(*ref)) {
					return WorkshopPower::StillShows(HasLight(*ref), std::nullopt, a_on);
				}
				return WorkshopPower::StillShows(std::nullopt, BeltRuns(*ref), a_on);
			}

			bool Show(const WorkshopPower::Key& a_key, bool a_on)
			{
				const auto ref = CopyOf(a_key);
				if (!ref) {
					return false;
				}
				const auto took = ShowPoweredHow(*ref, a_on, forSave);
				if (showsLogged < SHOWS_LOGGED && took != Took::kNoModel) {
					++showsLogged;
					static constexpr std::array<const char*, 8> how{ "no 3D", "light switched", "graph took", "graph refused (tried again)", "no light, belt or graph",
						"graph dark already (never lit)", "graph lit already", "belt set" };
					REX::INFO("WorkshopSync: friend's piece {:08X} (player {} piece {:08X}) shown {}: {}", ref->GetFormID(), a_key.first, a_key.second, a_on ? "on" : "off",
						how[static_cast<std::size_t>(took)]);
				}
				if (!WorkshopPower::ShowCounts(took)) {
					return false;
				}
				++powerShown;
				return true;
			}
		};

		// A friend's copy our save holds that no session keeps (WorkshopPower::Strays): marked as every copy is (the block
		// keyword, saved with it), made at runtime, no copy we keep or wait to delete, no piece of ours (linked to a workbench,
		// or a workbench), no actor (a turret) and no drop's copy, and one with power to show: a generator, a light, a pylon.
		// Within STRAY_RANGE of the player: a sound is heard from no further.
		constexpr float STRAY_RANGE = 8192.0f;

		struct StrayEngine
		{
			std::vector<std::uint32_t> Found()
			{
				std::vector<std::uint32_t> found;
				const auto                 tes = RE::TES::GetSingleton();
				const auto                 player = RE::PlayerCharacter::GetSingleton();
				if (!tes || !player) {
					return found;
				}
				tes->ForEachReferenceInRange(player->data.location, STRAY_RANGE, [&](RE::TESObjectREFR* a_ref) {
					if (a_ref && Protocol::IsRuntimeRef(a_ref->GetFormID()) && !a_ref->IsDisabled() && !a_ref->IsDeleted() && !a_ref->As<RE::Actor>() && !IsCopy(a_ref) &&
						!InGraveyard(a_ref) && !Drops::IsCopyRef(a_ref->GetFormID()) && Blocked(*a_ref) && !IsWorkshopObject(a_ref) && IsPowerPiece(*a_ref)) {
						found.push_back(a_ref->GetFormID());
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				std::ranges::sort(found);
				return found;
			}

			std::uintptr_t Drawn(std::uint32_t a_id)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
				return ref ? DrawingOf(*ref) : 0;
			}

			bool Show(std::uint32_t a_id)
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
				if (!ref) {
					return false;
				}
				const auto took = ShowPoweredHow(*ref, false);
				if (!WorkshopPower::ShowCounts(took)) {
					return false;
				}
				const auto base = ref->GetObjectReference();
				REX::INFO("WorkshopSync: stray copy {:08X} (base {:08X}) that no session keeps shown unpowered (an older save's, or a piece its builder no longer has): {}", a_id,
					base ? base->GetFormID() : 0, took == WorkshopPower::Took::kDark ? "dark already" : "took it");
				return true;
			}
		};

		// In a session and out of one: a stray is the save's, whoever is here.
		void HushStrays(Clock::time_point a_now)
		{
			if (!AuditSettled()) {
				return;
			}
			StrayEngine engine;
			strays.Sweep(engine, a_now);
		}

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
			HushStrays(a_now);
		}

		// ---- Resources: what pieces count for in a settlement, across games (ResourceMirror.h) ------------

		ResourceMirror::Reporter resources;      // our pieces that count for something
		ResourceMirror::Tally    theirResources;  // what friends' games say theirs count for
		ResourceMirror::Applier  onBenches;       // what of theirs is on our workbenches
		Clock::time_point        nextResourceTrack{};
		Clock::time_point        nextResourceApply{};
		std::uint32_t            mainMenusSeen = 0;      // OpenMenus::MainMenus at the last frame
		std::uint32_t            benchMenusSeen = 0;     // OpenMenus::MainMenus at the last look for our workbenches (SeeMainMenu)
		std::uint32_t            resourcesSent = 0;      // entries sent
		std::uint32_t            resourcesReceived = 0;  // entries received
		std::uint32_t            resourceChanges = 0;    // values changed on our workbenches
		std::uint32_t            resourcesHushed = 0;    // values taken off our workbenches for saves
		std::uint32_t            resourcesLeftover = 0;  // values a loaded save held of an earlier session's, taken off
		std::int64_t             resourcePollUsMax = 0;
		// What each save holds of friends' pieces on our workbenches (nothing, unless its hush failed or never ran): its co-save
		// record, written on the save's thread (ResourceSaveRecord). And what a loaded save's record said, taken off our
		// workbenches once the load is over (OnGameLoaded).
		using BenchValues = std::vector<std::pair<std::uint32_t, ResourceMirror::Values>>;
		std::mutex                 resourceRecordLock;
		ResourceMirror::SaveRecord resourceRecord;   // under resourceRecordLock
		BenchValues                resourcesLoaded;  // under resourceRecordLock

		// What is on our workbenches now, for a save whose hush doesn't run (ResourceMirror::SaveRecord).
		void NoteResourcesOn()
		{
			std::scoped_lock l{ resourceRecordLock };
			resourceRecord.Live(onBenches.On());
		}

		// A piece of ours a friend's game may have a copy of, and whose worth in its settlement it is told: one we built (made
		// at runtime), standing, one of a workbench's workshop items, and no copy (the settlers, workshop items too, count
		// for none of the values sent).
		bool OwnResourcePiece(RE::TESObjectREFR& a_ref)
		{
			return Protocol::IsRuntimeRef(a_ref.GetFormID()) && !a_ref.IsDisabled() && !a_ref.IsDeleted() && !IsCopy(&a_ref) && BenchOf(a_ref);
		}

		// Our pieces that count for anything, from the workshop items of each workbench seen.
		void TrackOwnResources()
		{
			for (const auto benchId : powerBenches) {
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(benchId);
				const auto children = bench && bench->extraList ? bench->extraList->GetByType<RE::ExtraLinkedRefChildren>() : nullptr;
				if (!children) {
					continue;
				}
				std::vector<std::uint32_t> ids;
				for (const auto& child : children->linkedChildren) {
					const auto ref = child.REFR.get();
					if (ref && child.keyword && child.keyword->GetFormID() == WORKSHOP_ITEM_KEYWORD && Protocol::IsRuntimeRef(ref->GetFormID()) &&
						!resources.Tracks(ref->GetFormID())) {
						ids.push_back(ref->GetFormID());
					}
				}
				for (const auto id : ids) {
					if (const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(id) };
						ref && OwnResourcePiece(*ref) && WorkshopResources::CountsForAnything(*ref)) {
						resources.Track(id);
					}
				}
			}
		}

		struct ResourceReporterEngine
		{
			ResourceMirror::Look Look(std::uint32_t a_id)
			{
				const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) };
				if (!ref) {
					return { ResourceMirror::Look::State::kAway, {} };  // its cell unloaded with it: it counts as it did
				}
				if (!OwnResourcePiece(*ref)) {
					return {};  // scrapped, stored, or the ID is another thing's now
				}
				return { ResourceMirror::Look::State::kHere, WorkshopResources::Counts(*ref) };
			}
		};

		void SendResources(Clock::time_point a_now)
		{
			const auto             start = Clock::now();
			ResourceReporterEngine engine;
			const auto             entries = resources.Poll(engine, a_now);
			resourcePollUsMax = (std::max)(resourcePollUsMax, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
			// The queue's packets go reliably; one the full queue refuses goes again at the next poll (Reporter::Unsent), so a
			// gone piece's one word that it counts for nothing is never lost.
			for (const auto& chunk : ResourceMirror::Chunks(entries)) {
				std::scoped_lock l{ outLock };
				if (outgoing.Push({ Protocol::Encode(Protocol::ResourceStates{ 0, chunk }, Protocol::MessageType::kReportWorkshopResources), 0 })) {
					resourcesSent += static_cast<std::uint32_t>(chunk.size());
				} else {
					resources.Unsent(chunk);
				}
			}
		}

		// Where each friend's piece counts here: the workbench our copy of it was reported at, if we have a copy (none: it
		// isn't placed yet, one of our own pieces stands at its spot, or another friend's copy stands for it).
		struct TallyEngine
		{
			std::uint32_t BenchOf(const ResourceMirror::Key& a_key)
			{
				const auto it = copies.find(a_key);
				if (it == copies.end() || it->second.id == 0) {
					return 0;
				}
				std::scoped_lock l{ copyIdsLock };
				return copyBenches.Of(it->second.id);
			}
		};

		struct BenchEngine
		{
			bool Add(std::uint32_t a_bench, ResourceMirror::Kind a_kind, float a_delta) { return WorkshopResources::AddToBench(a_bench, a_kind, a_delta); }
			std::optional<float> Temporary(std::uint32_t a_bench, ResourceMirror::Kind a_kind) { return WorkshopResources::TemporaryOf(a_bench, a_kind); }
		};

		// The main menu, for what of friends' is on our workbenches (Applier::MainMenu): looked at before anything is put on,
		// taken off for a save or a load, or taken off as the session ends, so none of those works on the world before's
		// workbenches as if they were this world's. One opened and closed since the last look is seen by its count.
		void SeeMainMenu()
		{
			const bool  open = OpenMenus::MainMenuOpen();
			const auto  menus = OpenMenus::MainMenus();
			const bool  since = std::exchange(benchMenusSeen, menus) != menus;
			const auto  benchesBefore = onBenches.On().size();
			BenchEngine benches;
			const auto  off = onBenches.MainMenu(benches, open, since);
			if (benchesBefore > 0 && onBenches.On().empty()) {
				REX::INFO("WorkshopSync: the main menu: friends' pieces' worth on {} workbenches of the world before forgotten; {} values still on them taken off", benchesBefore, off);
				NoteResourcesOn();
			}
		}

		void PutResourcesOn(Clock::time_point a_now)
		{
			nextResourceApply = a_now + ResourceMirror::APPLY_EVERY;
			SeeMainMenu();
			TallyEngine tally;
			BenchEngine benches;
			if (const auto changed = onBenches.Apply(benches, theirResources.Totals(tally), a_now); changed > 0) {
				resourceChanges += static_cast<std::uint32_t>(changed);
				NoteResourcesOn();
			}
		}

		// Our world is gone: what our tracked pieces counted for at friends' is sent as nothing, and those of the next world
		// are found again (Reporter::WorldGone).
		void ForgetOwnPieces(std::string_view a_why)
		{
			if (const auto gone = resources.WorldGone(); gone > 0) {
				REX::INFO("WorkshopSync: {}: {} of our pieces counting in friends' settlements were the world before's; sent as counting for nothing", a_why, gone);
			}
		}

		void ResourceFrame(Clock::time_point a_now)
		{
			// The main menu: the world played until it opened is gone, whatever comes next, and a coc from it sends no message
			// (no load, no new game). While it is open, no piece of ours is looked for (the old world's may still be in memory);
			// one opened and closed between two frames is seen by its count.
			const bool atMainMenu = OpenMenus::MainMenuOpen();
			if (const auto menus = OpenMenus::MainMenus(); atMainMenu || menus != mainMenusSeen) {
				mainMenusSeen = menus;
				ForgetOwnPieces("the main menu");
			}
			if (!atMainMenu && a_now >= nextResourceTrack) {
				nextResourceTrack = a_now + WorkshopPower::AUDIT_EVERY;
				TrackOwnResources();
			}
			SendResources(a_now);
			if (a_now >= nextResourceApply) {
				PutResourcesOn(a_now);
			}
		}

		// The new world's clear of what we put on the old one's workbenches (Applier::ForgetWorld), and what a loaded save's
		// record says it held taken off (OnGameLoaded says which, and why).
		void ClearResourcesForNewWorld(bool a_newGame)
		{
			BenchEngine benches;
			const auto  unreached = onBenches.On().size();
			if (const auto off = onBenches.ForgetWorld(benches, a_newGame); off > 0) {
				REX::WARN("WorkshopSync: the new game's workbenches still held {} values of friends' pieces; taken off", off);
			} else if (!a_newGame && unreached > 0) {
				REX::WARN("WorkshopSync: {} workbenches of the world before couldn't be reached before the load; forgotten", unreached);
			}
			BenchValues leftover;
			{
				std::scoped_lock l{ resourceRecordLock };
				leftover.swap(resourcesLoaded);
			}
			if (a_newGame) {
				leftover.clear();  // a new game has no save: a record read for an earlier load is no record of its
			}
			if (const auto off = ResourceMirror::TakeOffRecord(benches, leftover); !leftover.empty()) {
				resourcesLeftover += static_cast<std::uint32_t>(off);
				REX::WARN("WorkshopSync: the save held friends' pieces' worth on {} workbenches; {} values taken off (only what each still holds)", leftover.size(), off);
			}
			NoteResourcesOn();
		}

		// Friends' pieces off our workbenches (the session ended).
		void TakeResourcesOff(std::string_view a_why)
		{
			SeeMainMenu();  // a session ended at the main menu: only what the workbenches still hold comes off
			BenchEngine benches;
			if (const auto off = onBenches.TakeAllOff(benches); off > 0) {
				REX::INFO("WorkshopSync: {}: {} values of friends' pieces taken off our workbenches", a_why, off);
			}
			NoteResourcesOn();
		}

		void ApplyNow(const Key& a_key, const Protocol::WorkshopItem& a_item)
		{
			if (a_item.op == Protocol::WorkshopOp::kScrapped) {
				mirror.Forget(a_key);
				if (const auto it = copies.find(a_key); it != copies.end()) {
					graphsTold.Forget(it->second.id);
				}
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

	namespace
	{
		// The crafting menus' gather of what their recipes may use, and the settlement lookup it calls (CraftingWorkshop.h).
		constexpr REL::ID     WORKBENCH_GATHER{ 2224943 };  // (+B28540)
		constexpr REL::ID     GET_WORKSHOP{ 2194973 };      // Workshop::GetWorkshop(TESObjectREFR&) (+385A80)
		constexpr std::size_t CALL_STUB = 14;               // what a write_call<5> takes of the trampoline

		using GetWorkshopFn = RE::TESObjectREFR* (*)(RE::TESObjectREFR&);
		GetWorkshopFn              originalGetWorkshop = nullptr;
		std::atomic<std::uint32_t> craftingAtCopies{ 0 };   // gathers at a copy of a friend's station, read from our workbench
		std::atomic<std::uint32_t> craftingUnbenched{ 0 };  // ... at one whose workbench is no workbench here: the player's alone
		constexpr std::uint32_t    MAX_CRAFTING_LOGGED = 20;

		// The gather asks which settlement the menu's workbench reads. The engine's answer (the station's link) stands; a
		// station with none that is our copy of a friend's piece reads the workbench their report named, as theirs does in
		// their game. On the menu's thread: the copies' workbenches are read under their lock, and the log is the main
		// thread's.
		RE::TESObjectREFR* CraftingWorkshopHook(RE::TESObjectREFR& a_workbench)
		{
			const auto linked = originalGetWorkshop(a_workbench);
			if (linked) {
				return linked;
			}
			const auto    station = a_workbench.GetFormID();
			std::uint32_t named = 0;
			{
				std::scoped_lock l{ copyIdsLock };
				named = copyBenches.Of(station);
			}
			if (named == 0) {
				return nullptr;  // a station of the world's or ours with no settlement: vanilla reads the player's things alone
			}
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(named);
			if (CraftingWorkshop::Pick(0, named, IsWorkbenchAnchor(bench)) == 0) {
				if (craftingUnbenched.fetch_add(1) < MAX_CRAFTING_LOGGED) {
					MainThread::Post([station, named] {
						REX::WARN("WorkshopSync: crafting at {:08X}, our copy of a friend's station, reads only our player's things: its workbench {:08X} is no workbench here",
							station, named);
					});
				}
				return nullptr;
			}
			if (craftingAtCopies.fetch_add(1) < MAX_CRAFTING_LOGGED) {
				MainThread::Post([station, named] {
					REX::INFO("WorkshopSync: crafting at {:08X}, our copy of a friend's station, reads workbench {:08X}'s junk and supply lines, as a station of ours would",
						station, named);
				});
			}
			return bench;
		}

		// TESObjectLIGH's light maker (+454880 in 1.11.240): makes a light reference's light (its ExtraLight), called by the
		// light's 3D load and by SwitchLight's task (+500260) when a light is switched on. For a base flagged to wait for power
		// (OBJ_LIGH flag 0x20: the light bulb 000560DF has 0x21) or a reference flagged so, it first asks Workshop::IsPowered
		// (2194916) and makes no light if that says no: a workshop item off our power grids (a bench link, a powered grid)
		// isn't powered, and our copy of a friend's bulb is on none. So the bulb's graph took "Powered" and its mesh glowed,
		// but it never had a light (the rig: no ExtraLight on the copy, which the builder's powered bulb has).
		constexpr REL::ID     GENERATE_LIGHT{ 2198256 };
		constexpr std::size_t GENERATE_LIGHT_IS_POWERED_CALL = 0x1A9;  // E8 to Workshop::IsPowered (+382530)

		using IsPoweredFn = bool (*)(RE::TESObjectREFR&);
		IsPoweredFn                originalLightPowered = nullptr;
		std::atomic<std::uint32_t> copyLightsMade{ 0 };  // lights made for copies shown powered

		// The light maker's power check: the game's answer, or, for one of our copies shown powered (litCopies), yes. On the
		// thread that makes the light (the main thread or a loader's): the copies are read under their lock.
		bool LightPoweredHook(RE::TESObjectREFR& a_ref)
		{
			if (originalLightPowered(a_ref)) {
				return true;
			}
			{
				std::scoped_lock l{ litLock };
				if (!litCopies.contains(a_ref.GetFormID())) {
					return false;
				}
			}
			copyLightsMade.fetch_add(1);
			return true;
		}
	}

	namespace
	{
		// ---- The shared workshop (SharedWorkshop.h) ------------------------------------------------------
		//
		// At a settlement the host owns, a friend's build or wire is a numbered request the host's game performs on its own
		// pieces (docs/DESIGN.md "Shared workshop"). The friend's side: its placed event becomes a request, and the answer
		// adopts its piece as a copy of the host's (Adopt) or leaves it the friend's own; a wire it strung between two copies
		// is taken off in the same frame (DetachWire) and asked of the host. The host's side: the request is checked, the
		// piece made where the friend's stood and finished with the engine's own placement steps (FinishEngine), then
		// answered. The hooks (InstallPicks) let workshop mode pick a copy to wire it, refuse to grab it, and turn its Scrap and
		// its Store into requests of the host (when the host's bSharedWorkshopEdit allows them; refused with a notice if not).
		//
		// The game's own functions (1.11.240), by their PDB names:

		// The two picks of Workshop::UpdateForegroundReferenceJob that ask Workshop::IsInteractionBlocked (2195179: the
		// reference has WorkshopBlockInteractionKeyword) at +0x4FA (a 5-byte call, the reference in rcx): the general pick
		// (+3ABF60; what E, Store, Scrap and the wire's first end act on) and the wire pick (+3AB130; the wire's second end).
		// The third, the snap magnet (2195313), is left as it is: a copy is never snapped to.
		constexpr REL::ID     PICK_GENERAL{ 2195314 };
		constexpr REL::ID     PICK_WIRE{ 2195312 };
		constexpr REL::ID     IS_INTERACTION_BLOCKED{ 2195179 };
		constexpr std::size_t PICK_BLOCKED_CALL = 0x4FA;
		// Workshop::CanStoreRef(TESObjectREFR&) (+39F0A0), asked by Workshop::CanStorePlacementItem (+384BD0, which the menu's
		// button hints ask every frame, and the Cancel key once) at +0x80 and by Workshop::RequestStorePlacementItem (+3853F0) at
		// +0x8E. Both refuse a copy silently: the key's refusal is counted and said where the menu asks (MENU_STORE_CALL).
		constexpr REL::ID     CAN_STORE_PLACEMENT_ITEM{ 2194963 };
		constexpr REL::ID     REQUEST_STORE_PLACEMENT_ITEM{ 4477212 };
		constexpr REL::ID     CAN_STORE_REF{ 2195195 };
		constexpr std::size_t CAN_STORE_CALL = 0x80;
		constexpr std::size_t REQUEST_STORE_CALL = 0x8E;
		// Workshop::WorkshopCanScrapRef(TESObjectREFR&, bool) (CAN_SCRAP_REF above), asked by Workshop::CanScrapPlacementRef(bool)
		// (+39E830, the Scrap button: the hints ask it every frame) at +0x188, refused silently, and by
		// Workshop::RequestScrapPlacementItem (+385310; only the menu's XButton calls it) at +0x85, refused, counted and said.
		// Its other callers (the general filter, the navmesh) are left alone: a copy must pass the general filter.
		constexpr REL::ID     CAN_SCRAP_PLACEMENT_REF{ 2195192 };
		constexpr REL::ID     REQUEST_SCRAP_PLACEMENT_ITEM{ 2194966 };
		constexpr std::size_t CAN_SCRAP_CALL = 0x188;
		constexpr std::size_t REQUEST_SCRAP_CALL = 0x85;
		// The engine's scrap confirmation (ExamineConfirmMenu, opened by Workshop::RequestScrapRef with the rewards worked out
		// from the piece's recipe) runs Workshop::RequestScrapCallback::OnAccept (+3C19C0) on Yes, which calls
		// Workshop::ScrapReference(ContextData&, NiPointer<TESObjectREFR>&, BSTArray<BSTTuple<TESBoundObject*, uint>>* rewards)
		// (+393F30) at +0x107 (a 5-byte call). For a copy of the host's piece that call becomes a request of the host
		// (ScrapAcceptHook); the rest of OnAccept (the menu's "ItemScrapped", the sound, back to the selected recipe) runs as the
		// engine wrote it, harmless. The host's own game scraps for a friend with the same function, a null rewards pointer
		// (the engine then works the yield out itself; an empty array would give nothing), inside the bench and budget scopes.
		constexpr REL::ID     SCRAP_ACCEPT{ 2195603 };
		constexpr REL::ID     SCRAP_REFERENCE{ 2195125 };
		constexpr std::size_t SCRAP_ACCEPT_CALL = 0x107;
		// The engine's store confirmation (a plain yes/no message box, MessageMenuManager::CreateYesNoMessage, opened by
		// Workshop::RequestStorePlacementItem once CanStoreRef passed the piece) runs Workshop::RequestStoreCallback's Run (+3B8180,
		// the callback's vtable[1], the button in dl: only 0, Yes, goes on) which calls Workshop::DestroyItem(ContextData&,
		// NiPointer<TESObjectREFR>&, bool store, bool onlyDisconnect, bool quiet) (+393220) at +0xDE (a 5-byte call: rcx the
		// callback's ContextData copy at +0x18, rdx a local NiPointer of the piece, r8b 1, r9d 0, [rsp+20h] 0; nothing of rax read
		// after). For a copy of the host's piece that call becomes a request of the host (StoreAcceptHook); the rest of Run (the
		// placement handle cleared, back to the selected recipe) runs as the engine wrote it, harmless. The host's own game stores
		// for a friend with the same function (store, not only disconnecting, quiet) inside the bench and budget scopes.
		constexpr REL::ID     STORE_ACCEPT{ 2195501 };
		constexpr REL::ID     DESTROY_ITEM{ 2195122 };
		constexpr std::size_t STORE_ACCEPT_CALL = 0xDE;
		// Workshop::FindAndRemoveHolotapeFromTerminal(TESObjectREFR&) (+39E560), void: nothing unless the reference's base is a
		// terminal; then the holotape it holds (the first item of its inventory list, else a holotape in its base's container)
		// goes to the player (TESObjectREFR::AddInventoryItem on g_PlayerCharacter, one of it). The store callback's Run calls
		// it on the piece (rcx the raw reference) just before its DestroyItem call, and Workshop::ScrapReference calls it
		// itself: so the host's store for a friend calls it first too (StorePiece), or the tape would go with the terminal.
		constexpr REL::ID FIND_AND_REMOVE_HOLOTAPE{ 2195190 };
		// Workshop::ExtraData::UseDeletedItem(TESObjectREFR&) (+388950): takes one of the reference's base off the workbench's stored
		// list (the entry removed at 0), as placing a piece from the store does; true when it found one. The dev stand-in for
		// placing a stored piece back (wsstored <bench> use <ref>), nothing else.
		constexpr REL::ID USE_DELETED_ITEM{ 2195009 };
		// The grab. Workshop::SetSelectedEditItem (2195119) is not it: the general pick calls it every frame for whatever the
		// crosshair is on (the highlight; workshop mode's placement item, PLACEMENT_ITEM, is that piece at idle), and a copy must
		// be highlighted to be wired. The menu's user event handler (+B2C170) grabs on Accept: with no new piece and nothing held,
		// it asks Workshop::WorkshopCanSelectRef(TESObjectREFR&) (+384880; a hash list of bases the player may never move,
		// which a copy passes) of the highlighted piece at +0x148D, and only then repairs it (RequestRepairItem) or picks it up
		// (UpdateActiveEdit, the ItemGrabbedEvent). Refused there, counted and said. Workshop::CanSelectPlacementRef (+384CD0, the
		// Move button's hint, every frame) asks it at +0x80: refused silently.
		constexpr REL::ID     WORKSHOP_MENU_EVENT{ 2224981 };
		constexpr REL::ID     WORKSHOP_CAN_SELECT_REF{ 2194958 };
		constexpr REL::ID     CAN_SELECT_PLACEMENT_REF{ 2194964 };
		constexpr std::size_t MENU_SELECT_CALL = 0x148D;
		constexpr std::size_t CAN_SELECT_CALL = 0x80;
		// The menu's Cancel with nothing held and not placing a new piece asks Workshop::CanStorePlacementItem() at +0xA5B, and
		// stores the highlighted piece when it may (else workshop mode's own Cancel): the Store key's one call, where its
		// refusal of a copy is counted and said.
		constexpr std::size_t MENU_STORE_CALL = 0xA5B;
		// The general pick, read whole (1.11.240): Workshop::UpdateForegroundReferenceJob (2194909, +3820F0), run each frame by a
		// job (JobListManager, +BD5980) with a Workshop::ContextData made from the player (+3897F0: the ray is the player's
		// GetEyeVector(origin, direction, false), not the camera), calls it at +0x46 while no new piece is being placed and none
		// is held. It casts a 0.1-unit shape 600 units along that ray through the cell's havok world (hknpWorld::castShape, every
		// layer but the actor zone and the navcut), sorts the hits near to far (the collector's sort, 2256236, at +0x3ED), and
		// for each: a trigger body is skipped; one with no reference (TESHavokUtilities::FindBodyIdRef, 2208693, at +0x469) that
		// is static or terrain ends the search; the player (the PlayerCharacter singleton, by address), a marker or a primitive
		// is skipped; a door is taken as its linked reference; then the pick test (IsInteractionBlocked, +0x4FA, hooked above)
		// and the general filter (2195585, +3BF6F0, at +0x511: a workshop item, or one the player could scrap, build, store or
		// carry, inside the open workbench's build area, never a used furniture marker). The first reference to reach the test
		// ends the search: highlighted (+3B7D30, Workshop::SetSelectedEditItem's caller) if it passes both, nothing highlighted
		// if either refuses it (the "general" flag, r13b, sets the stop at +0x5CD). So whatever stands in front, refused, hides
		// what is behind it. A friend's stand-in is no player to the engine: standing between the eye and a piece (or on the
		// player's own spot, where the cast starts inside it) it was refused by the filter and ended every search (the rig's
		// run at 98d80fd3: "00000014:skipped 00000014:skipped body3413:no-ref FF00103A:filter-no", FF00103A the host's
		// stand-in, whichever way the VM looked). The hits' call names a stand-in the player, so the engine passes it by as it
		// passes the player, in the general pick and the wire pick (2195312, the same loop: its FindBodyIdRef call is at +0x469
		// too). The filter's own call admits a copy the test admitted (a copy is no workshop item of ours, and the filter's other
		// tests were never written for one), when the copy belongs to the open workbench (the filter's build-area test, kept).
		constexpr REL::ID     FOREGROUND_JOB{ 2194909 };
		constexpr std::size_t FOREGROUND_PICK_CALL = 0x46;
		constexpr REL::ID     FIND_BODY_REF{ 2208693 };
		constexpr std::size_t PICK_HIT_CALL = 0x469;
		constexpr REL::ID     GENERAL_FILTER{ 2195585 };
		constexpr std::size_t PICK_FILTER_CALL = 0x511;
		// The wire pick's run (1.11.240): the job calls it at +0xCC (the same lambda and context, the flag 0) every frame the
		// placement item is a wire (TESObjectREFR::IsSpline): one held, and also one only highlighted at idle, when the general
		// pick has run first in the same frame (the job runs the general pick only with nothing selected or its byte 430f6ee8
		// 0, at idle; a held wire runs the wire pick alone). Its loop is the general pick's, with its own filter at +0x511:
		// Workshop's wire filter (2195584, +3BF4B0) passes a reference with the workshopPowerConnection actor value or the
		// power keyword (4797192, read below; both the base's, so a copy of a piece that has one has it too), passes the
		// wire's first end, and refuses a reference the bench's grids already connect to the first end. A piece with neither
		// (a light bulb, 000560DF) is no wire end in any game: the filter refuses it, copy or not.
		// It asks nothing of owners, workbenches or the build area. What it passes is the wire's target (SetWireTarget,
		// 2195498, at +0x5C1: its handle in WIRE_TARGET), and the placement update's spline status (2195059) says whether the
		// wire may go there: CAN_PLACE below is that status (SharedWorkshop::WireStatusName). Watched only: a copy needs nothing
		// at the wire filter that the pick test (hooked above) doesn't already give it.
		constexpr std::size_t FOREGROUND_WIRE_CALL = 0xCC;
		constexpr REL::ID     WIRE_FILTER{ 2195584 };
		constexpr REL::ID     WIRE_FIRST_END{ 4797240 };  // an ObjectRefHandle: the end the wire was started at
		constexpr REL::ID     WIRE_TARGET{ 4797239 };     // an ObjectRefHandle: the second end the wire pick found (none: 0)
		constexpr REL::ID     POWER_KEYWORD{ 4797192 };   // a BGSKeyword*: the wire filter's other way to pass
		// Workshop::RequestScrapPlacementItem asks its scrap test only while this byte is 0 (1.11.240: the placement job,
		// 2195216, sets it to 1 when the menu's Accept on a new piece asks it to, 2195215; the placement, 2195151, 2195152 and
		// PlaceCurrentReference put it back to 0). A 1 left there turns every Scrap key away before the test, silently.
		constexpr REL::ID SCRAP_GATE{ 4797148 };
		// The keyword WorkshopCanScrapRef refuses a reference for. Short of it, for a reference that is no actor and no
		// inventory item (a wire), the test reads only its base: inGameFormFlags bit 0, or a MISC with components.
		constexpr REL::ID SCRAP_BLOCK_KEYWORD{ 4797212 };
		// TESContainer::ContainerCanHoldForm(TESBoundObject*) (+30E570): WorkshopCanScrapRef's "a thing one carries" test.
		constexpr REL::ID CONTAINER_CAN_HOLD_FORM{ 2193008 };
		// The cast's hits, read for the pick watch once sorted: the collector (hknpAllHitsCollector) holds them at +0x20 and
		// their count at +0x28; each is a hknpCollisionResult of 0x60 bytes: the hit's position in havok units at +0x00, the
		// fraction of the cast at +0x20, the body id at +0x40 and its shape's collision filter info at +0x4C (the layer in its
		// low seven bits), as the pick's loop reads them.
		constexpr REL::ID     PICK_SORT{ 2256236 };
		constexpr std::size_t PICK_SORT_CALL = 0x3ED;
		constexpr std::size_t CAST_HITS = 0x20;
		constexpr std::size_t CAST_COUNT = 0x28;
		constexpr std::size_t CAST_STRIDE = 0x60;
		constexpr std::size_t CAST_FRACTION = 0x20;
		constexpr float       CAST_REACH = 600.0f;  // the cast's length (the constant at +2483E54): a fraction of it is a distance
		constexpr std::size_t CAST_BODY = 0x40;
		constexpr std::size_t CAST_FILTER = 0x4C;

		// The engine's own finish of a placed piece (SharedWorkshop::FINISH_STEPS), each a plain function:
		constexpr REL::ID SET_IS_WORKSHOP_ITEM{ 2194972 };     // Workshop::SetIsWorkshopItem(TESObjectREFR&, bool, TESObjectREFR* bench)
		constexpr REL::ID BUDGET_ADD_OBJECT{ 2195107 };        // Workshop::Budget::AddObject(TESObjectREFR&)
		constexpr REL::ID BUDGET_REMOVE_OBJECT{ 2195108 };     // Workshop::Budget::RemoveObject(TESObjectREFR&) (an adopted piece off our budget)
		// The budget's running totals are workshop mode's globals, loaded from a workbench's actor values when workshop mode
		// is entered there (Workshop::Budget::Initialize(TESObjectREFR& bench), 2195106, +390330: the maximum triangles and draws
		// into 4797248 and 4797252, the current ones into 4797162 and 4797163, each a 32-bit integer; read in 1.11.240). AddObject
		// and RemoveObject add to the current totals and, with the current workbench set, write them to that workbench. So
		// outside workshop mode at the bench the totals are loaded first and put back after (BudgetScope), or a stale figure
		// (another settlement's, or 0 after a load) would be written onto the bench, into the host's save.
		constexpr REL::ID BUDGET_INITIALIZE{ 2195106 };
		constexpr REL::ID BUDGET_TRIANGLES{ 4797162 };
		constexpr REL::ID BUDGET_DRAWS{ 4797163 };
		constexpr REL::ID BUDGET_MAX_TRIANGLES{ 4797248 };
		constexpr REL::ID BUDGET_MAX_DRAWS{ 4797252 };
		constexpr REL::ID MAKE_REFERENCE_STATIC{ 2195055 };    // Workshop::MakeReferenceStatic(TESObjectREFR&)
		constexpr REL::ID SET_FURNITURE_USED{ 2195158 };       // Workshop::SetFurnitureUsed(TESObjectREFR&, bool)
		constexpr REL::ID CHECK_SNAPPED_POWER{ 2195213 };      // Workshop::CheckSnappedPowerConnections(ContextData&, TESObjectREFR&, PlacementItemData&)
		constexpr REL::ID PLACEMENT_DATA_CLEAR{ 2195555 };     // Workshop::PlacementItemData::Clear()
		constexpr REL::ID EXTRA_ADD_ITEM{ 2194996 };           // Workshop::ExtraData::AddItem(TESObjectREFR&)
		constexpr REL::ID ESTABLISH_TERMINAL_LINKS{ 2195102 }; // TerminalUtils::EstablishTerminalLinks(TESObjectREFR&)
		constexpr REL::ID UPDATE_DYNAMIC_NAVMESH{ 2201206 };   // TESObjectREFR::UpdateDynamicNavmesh(bool)
		constexpr REL::ID CALCULATE_POWER_RATING{ 2194985 };   // Workshop::CalculatePowerRating(TESObjectREFR& bench)
		constexpr REL::ID RECALCULATE_RESOURCES{ 2194984 };    // Workshop::RecalculateResources(TESObjectREFR& bench)
		constexpr REL::ID PLACED_EVENT_SOURCE{ 2195289 };      // Workshop::QSource<Workshop::ItemPlacedEvent>()
		constexpr REL::ID ADD_CONNECTION{ 2194998 };           // Workshop::ExtraData::AddConnection(TESObjectREFR&, TESObjectREFR&, TESObjectREFR* wire)
		// Workshop mode's current workbench (an ObjectRefHandle; null outside workshop mode): the budget, MakeReferenceStatic and
		// the snapped-power check read it. Set to the piece's workbench for the finish, and put back after (BenchScope).
		constexpr REL::ID CURRENT_WORKSHOP{ 4797241 };
		// Workshop mode's "new item mode" byte (+2EDEF38; Workshop::QNewItemMode is "byte == 0"): 0 only while a new piece is
		// being placed, 1 otherwise, at idle and after a load too (the rig read it so). MakeReferenceStatic reads it twice
		// (+0x133, +0x162): with 0 the piece gets InitScripts, the loaded event and its power's initial state (a new piece);
		// with 1 it is put back as a moved one. So the finish sets it to 0 for its steps and puts it back (NewItemScope); it
		// says nothing about what the host holds.
		constexpr REL::ID MOVING_ITEM{ 2661518 };
		// Workshop mode's placement item (an ObjectRefHandle; Workshop::SetSelectedEditItem's): the new piece's ghost while one is
		// placed, the grabbed piece while one is held, and otherwise the piece merely highlighted under the crosshair. Held in
		// the player's hands only with the active-edit byte set (Workshop::HasActiveEditItem: the handle and +30F6EE8 != 0);
		// Workshop::CanPlaceItem is the handle and +30F6EE7 == 1 (the ghost green; while a wire is held, the update copies the
		// spline status there, whose other values say why not). A pending piece of ours held is not adopted until it is put down.
		constexpr REL::ID PLACEMENT_ITEM{ 4797235 };
		constexpr REL::ID ACTIVE_EDIT{ 4797147 };
		constexpr REL::ID CAN_PLACE{ 4797146 };
		// Workshop::QSource<Workshop::ItemDestroyedEvent>(): what the game's scrap notifies, which Papyrus answers by undoing
		// what it did for the piece (a settler's assignment to a bed or a crop, the workshop's counts); an adopted piece gets
		// it, so our save keeps no assignment to a reference that is the host's now.
		constexpr REL::ID DESTROYED_EVENT_SOURCE{ 2195286 };
		// The workshop's power wire (what the wire tool strings).
		constexpr std::uint32_t WIRE_BASE = 0x0001D971;

		using IsBlockedFn = bool (*)(RE::TESObjectREFR&);
		using CanStoreFn = bool (*)(RE::TESObjectREFR&);
		using CanScrapFn = bool (*)(RE::TESObjectREFR&, bool);
		using CanSelectFn = bool (*)(RE::TESObjectREFR&);
		using CanStoreItemFn = bool (*)();
		using ScrapFn = void (*)(RE::Workshop::ContextData&, RE::NiPointer<RE::TESObjectREFR>&, RE::BSTArray<RE::BSTTuple<RE::TESBoundObject*, std::uint32_t>>*);
		using DestroyFn = void (*)(RE::Workshop::ContextData&, RE::NiPointer<RE::TESObjectREFR>&, bool, bool, bool);  // store, only disconnect, quiet
		using UseDeletedFn = bool (*)(RE::Workshop::ExtraData*, RE::TESObjectREFR&);
		using GeneralPickFn = void (*)(void*, RE::Workshop::ContextData&, bool);  // the pick's lambda, the context, "general"
		using FindBodyRefFn = RE::TESObjectREFR* (*)(void*, std::uint32_t);      // hknpBSWorld&, hknpBodyId (four bytes, by value)
		using GeneralFilterFn = bool (*)(void*, RE::TESObjectREFR&);
		using PickSortFn = void (*)(void*);                                       // the cast's collector
		IsBlockedFn     originalIsBlocked = nullptr;
		CanStoreFn      originalCanStore = nullptr;
		CanScrapFn      originalCanScrap = nullptr;
		CanSelectFn     originalCanSelect = nullptr;
		CanStoreItemFn  originalCanStoreItem = nullptr;
		ScrapFn         originalScrapReference = nullptr;
		DestroyFn       originalDestroyItem = nullptr;
		GeneralPickFn   originalGeneralPick = nullptr;
		FindBodyRefFn   originalFindBodyRef = nullptr;
		GeneralFilterFn originalGeneralFilter = nullptr;
		GeneralPickFn   originalWirePick = nullptr;
		GeneralFilterFn originalWireFilter = nullptr;
		PickSortFn      originalPickSort = nullptr;
		bool            picksInstalled = false;

		// ---- The pick watch: what the general pick's ray hit, frame by frame (on whatever thread the job runs) -------------
		enum class HitVerdict : std::uint8_t
		{
			kNoRef,        // a body no reference owns (the search ends there if it is static or terrain)
			kSkipped,      // never asked the pick test: the player, a marker, a primitive, a trigger, or the search had ended
			kStandIn,      // a friend's stand-in, named the player to the engine: passed by as the player is
			kBlocked,      // the pick test refused it (the keyword; not a copy we admit): the search ends here
			kTested,       // the pick test let it through, then the filter wasn't asked (should not happen)
			kAdmitted,     // a copy the pick test let through
			kFilterNo,     // the general filter refused it: the search ends here
			kOtherBench,   // ... a copy the test admitted, of another workbench than the open one: not admitted, the search ends
			kPicked,       // the general filter took it: the highlight
			kPickedCopy,   // ... a copy the filter refused, admitted at the filter's call
		};
		struct PickHit
		{
			std::uint32_t ref = 0;
			std::uint32_t body = 0;
			bool          blockedKeyword = false;
			HitVerdict    verdict = HitVerdict::kSkipped;
		};
		// One of the cast's hits, as the collector holds them (sorted near to far), whether or not the search reached it.
		struct CastHit
		{
			std::uint32_t body = 0;
			std::uint32_t ref = 0;   // FindBodyIdRef's answer, asked by the watch (0: none, or not asked: a trigger)
			std::uint8_t  layer = 0;
			float         distance = 0.0f;  // from the ray's start, game units
		};
		struct PickRun
		{
			RE::NiPoint3                 from{};
			RE::NiPoint3                 dir{};
			std::uint32_t                cell = 0;
			void*                        world = nullptr;  // the cell's hknpBSWorld (only while a piece is watched)
			std::array<PickHit, 8>       hits{};
			std::uint32_t                count = 0;  // hits the search reached (FindBodyIdRef's calls; only the first hits.size() kept)
			std::array<CastHit, 12>      cast{};
			std::int32_t                 castCount = -1;  // the cast's hits, all of them (-1: not read)
			std::uint32_t                picked = 0;
			std::chrono::steady_clock::time_point at{};
		};
		thread_local PickRun*      currentPick = nullptr;  // set by GeneralPickHook (or WirePickHook) for the call's length
		// Each pick's last run, and its watch's last line (under pickLock).
		struct PickRecord
		{
			PickRun                               last;
			std::string                           logged;
			std::chrono::steady_clock::time_point loggedAt{};
		};
		std::mutex                 pickLock;
		PickRecord                 generalPick;            // under pickLock
		PickRecord                 wirePick;               // under pickLock
		std::atomic<std::uint32_t> wirePickRuns{ 0 };      // the wire pick's calls (each frame the placement item is a wire)
		std::atomic<std::uint32_t> wireTargets{ 0 };       // ... that found a second end the wire filter passed
		std::atomic<std::uint32_t> wireFilterCopyNo{ 0 };   // copies the wire filter refused (the first few logged, with why)
		std::atomic<std::uint32_t> wireFilterCopyYes{ 0 };  // ... and admitted, each frame one is the target (the first few logged)
		std::atomic<std::uint32_t> scrapKeys{ 0 };         // the Scrap key's scrap test asked (the gate before it open)
		std::atomic<std::uint32_t> scrapKeyNo{ 0 };        // ... and the engine's test said no
		std::atomic<std::uint32_t> pickWatch{ 0 };     // wsmode pick <ref>: that reference watched, its runs logged
		std::atomic<std::uint32_t> pickRuns{ 0 };      // the general pick's calls (each frame workshop mode is up and idle)
		std::atomic<std::uint32_t> pickCopyHits{ 0 };  // ray hits on a copy (a body of one, whatever came of it)
		std::atomic<std::uint32_t> pickCopyRefused{ 0 };  // ... that the pick test refused (not admitted: why is logged)
		std::atomic<std::uint32_t> filterAdmitted{ 0 };   // admitted copies the general filter refused, admitted at its call
		std::atomic<std::uint32_t> filterOtherBench{ 0 };  // ... not admitted there: another workbench's copy than the open one
		std::atomic<std::uint32_t> standInsPassed{ 0 };   // hits on a friend's stand-in, passed by as the player (either pick)

		std::optional<bool>        sharedDev;             // wsmode: the rule forced on or off
		bool                       hosting = false;       // Frame's a_host
		std::atomic<bool>          admitsCopies{ false };  // the hooks' gate: the rule on, in a session, not the host (set each frame)
		std::atomic<std::uint32_t> picksAdmitted{ 0 };  // the pick test let a copy through (every frame the crosshair is on one)
		std::atomic<std::uint32_t> grabsRefused{ 0 };   // key presses only: Accept on a copy or a pending piece of ours,
		std::atomic<std::uint32_t> storesRefused{ 0 };  // Cancel (the Store key) with a copy highlighted,
		std::atomic<std::uint32_t> scrapsRefused{ 0 };  // and XButton (Scrap) on a copy (the host's setting off, or its scrap asked already)
		std::atomic<std::uint32_t> scrapsAsked{ 0 };    // the scrap confirmation's Yes on a copy: asked of the host, nothing scrapped here
		std::atomic<std::uint32_t> storesAsked{ 0 };    // the store box's Yes on a copy: asked of the host, nothing stored here
		std::set<std::uint32_t>    notOwnedBenches;  // Benches::IsNotOwned, for the hooks (under copyIdsLock)

		SharedWorkshop::Requests requests;  // ours, waiting for the host's answer
		SharedWorkshop::Benches  taught;    // what the answers taught
		std::set<std::uint32_t>  madeHere;  // our pieces adopted as copies, and our wires taken off: no grid of ours may name them
		std::uint32_t            requestsSent = 0;
		std::uint32_t            wiresAsked = 0;
		std::uint32_t            wiresMixed = 0;  // wires with one end ours and one a copy: taken off, refused
		std::uint32_t            wiresDetached = 0;
		std::uint32_t            answersApplied = 0;
		std::uint32_t            answersRefused = 0;
		std::uint32_t            answersLate = 0;     // no answer in ANSWER_WAIT: taken as refused
		std::uint32_t            answersUnknown = 0;  // answers to no request of ours
		std::uint32_t            adoptedHere = 0;     // our pieces that became copies of the host's
		std::uint32_t            adoptMissing = 0;    // ...that were gone by the time the answer came
		std::uint32_t            straysFound = 0;     // in-session pieces of ours a grid still named (AuditStrays)
		// The host's side.
		struct Making
		{
			Protocol::WorkshopRequest request;
			RE::ObjectRefHandle       ref;  // a build: the piece made, waiting for its 3D (and for workshop mode to close)
			Clock::time_point         since;
		};
		std::vector<Making> making;
		std::uint32_t       requestsServed = 0;
		std::uint32_t       requestsRefused = 0;
		std::uint32_t       scrapsServed = 0;  // our pieces scrapped for friends with the engine's own scrap
		std::uint32_t       storesServed = 0;  // our pieces stored for friends with the engine's own store (into our stored list)
		std::uint32_t       finishedHere = 0;
		std::uint32_t       wiresMadeHere = 0;
		std::uint32_t       finishSteps = 0;
		bool                answersHeld = false;  // wsmode hold (dev): requests taken, not made or answered until wsmode serve
		// The host's own game's player id, from the welcome (0: none, a dedicated server: nothing is handed over). The copies a
		// friend may wire are the host's alone, and a wire's ends are the host's references: another friend's copies, or two
		// builders' pieces whose runtime ids collide, are never asked.
		std::atomic<std::uint32_t> hostId{ 0 };
		bool                       hostGone = false;  // the server said no host's game answers (kHostAway, kOff): nothing more is asked this session
		// Answers to builds whose piece the player was holding in workshop mode when they came (the placement item): adopted
		// once it is put down (the engine's drop links a held piece to the bench and adds it again).
		std::vector<std::pair<SharedWorkshop::Pending, Key>> adoptLater;
		std::uint32_t                                        adoptsDeferred = 0;
		// The host's wires made for friends, counted in the budget once drawn (MakeWire, CountWiresLater).
		struct WireToCount
		{
			RE::ObjectRefHandle wire;
			std::uint32_t       bench;
			Clock::time_point   since;
		};
		std::vector<WireToCount> wiresToCount;
		std::uint32_t            wiresCounted = 0;
		std::uint32_t            wiresUncounted = 0;  // drawn, but the budget's AddObject added nothing for them by WIRE_COUNT_GIVE_UP
		constexpr auto           WIRE_COUNT_GIVE_UP = std::chrono::seconds(10);
		std::set<std::uint32_t>  unopenedTold;  // workbenches the host was told to open once (a notice each)

		bool RuleOn()
		{
			return sharedDev.value_or(HostRules::Get().sharedWorkshop);
		}

		bool HandsOver()
		{
			return SharedWorkshop::HandsOver(RuleOn(), HostRules::InSession(), hosting) && hostId.load() != 0 && !hostGone;
		}

		// Whether friends may scrap the host's pieces at its settlements: the host's bSharedWorkshopEdit (in the welcome's rules
		// for a friend; the host's own game reads its own). Any thread.
		bool EditAllowed()
		{
			return HostRules::Get().sharedWorkshopEdit;
		}

		// Whether our copy with this ID is of one of the host's pieces (under copyIdsLock).
		bool HostsCopy(std::uint32_t a_id)
		{
			const auto it = copyBuilders.find(a_id);
			return it != copyBuilders.end() && it->second == hostId.load();
		}

		// Workshop mode's placement item and its state (PLACEMENT_ITEM): what the crosshair highlights, a new piece's ghost, or
		// the piece held.
		struct Placement
		{
			std::uint32_t handle = 0;
			bool          newItem = false;  // a new piece's ghost (Workshop::QNewItemMode)
			bool          editing = false;  // a piece held (Workshop::HasActiveEditItem)
			bool          canPlace = false;  // Workshop::CanPlaceItem: the ghost or the held piece may be put down where it is
			std::uint8_t  status = 0;        // the byte CanPlaceItem reads (a held wire: SharedWorkshop::WireStatusName)

			[[nodiscard]] bool Holds() const { return handle != 0 && (newItem || editing); }
		};

		Placement PlacementNow()
		{
			static REL::Relocation<std::uint32_t*> placement{ PLACEMENT_ITEM };
			static REL::Relocation<std::uint8_t*>  moving{ MOVING_ITEM };
			static REL::Relocation<std::uint8_t*>  active{ ACTIVE_EDIT };
			static REL::Relocation<std::uint8_t*>  canPlace{ CAN_PLACE };
			Placement p;
			p.handle = *placement;
			p.newItem = *moving == 0;
			p.editing = p.handle != 0 && *active != 0;
			p.status = *canPlace;
			p.canPlace = p.handle != 0 && p.status == 1;
			return p;
		}

		// The piece workshop mode highlights or holds, if any.
		RE::NiPointer<RE::TESObjectREFR> PlacementRef()
		{
			static REL::Relocation<RE::ObjectRefHandle*> placement{ PLACEMENT_ITEM };
			return placement->get();
		}

		// Whether the player holds this piece of ours in workshop mode (grabbed: the placement item is it, with an active edit).
		// Merely highlighted, it is not held: the crosshair on it is no reason to wait.
		bool Held(RE::TESObjectREFR& a_ref)
		{
			const auto p = PlacementNow();
			return p.editing && p.handle == a_ref.GetHandle().get_handle();
		}

		bool Serves()
		{
			return SharedWorkshop::Serves(RuleOn(), HostRules::InSession(), hosting);
		}

		// Whether workshop mode may pick the reference though it is a copy: the rule on here, our copy of a friend's piece, at a
		// workbench the host's game hasn't refused as not its own. On whatever thread the pick runs: only the locked sets.
		bool AdmitsCopy(const RE::TESObjectREFR& a_ref)
		{
			if (!admitsCopies.load()) {
				return false;
			}
			std::scoped_lock l{ copyIdsLock };
			const auto       id = a_ref.GetFormID();
			return copyIds.contains(id) && HostsCopy(id) && !notOwnedBenches.contains(copyBenches.Of(id));
		}

		// The workbench our copy belongs to (0: none known). Any thread.
		std::uint32_t BenchOfCopy(const RE::TESObjectREFR& a_ref)
		{
			std::scoped_lock l{ copyIdsLock };
			return copyBenches.Of(a_ref.GetFormID());
		}

		// The workbench workshop mode is open at (CURRENT_WORKSHOP, the handle the general filter reads too), or 0. Any thread.
		std::uint32_t OpenBench()
		{
			static REL::Relocation<RE::ObjectRefHandle*> bench{ CURRENT_WORKSHOP };
			const auto                                   ref = bench->get();
			return ref ? ref->GetFormID() : 0;
		}

		// A remote player's stand-in (not a mirrored NPC). Any thread: Puppets' locked registry.
		bool IsStandIn(RE::TESObjectREFR& a_ref)
		{
			const auto actor = a_ref.As<RE::Actor>();
			return actor && Puppets::IsPuppet(actor);
		}

		// Why AdmitsCopy says no, for the log: each of its tests.
		std::string WhyNotAdmitted(const RE::TESObjectREFR& a_ref)
		{
			std::scoped_lock l{ copyIdsLock };
			const auto       id = a_ref.GetFormID();
			const auto       builder = copyBuilders.find(id);
			return std::format("rule={} copy={} builder={} host={} bench={:08X} notOwned={}", admitsCopies.load(), copyIds.contains(id),
				builder != copyBuilders.end() ? builder->second : 0, hostId.load(), copyBenches.Of(id), notOwnedBenches.contains(copyBenches.Of(id)));
		}

		// The pick watch's record of this call (GeneralPickHook), the latest hit of that reference.
		PickHit* PickHitOf(std::uint32_t a_ref)
		{
			if (!currentPick) {
				return nullptr;
			}
			const auto kept = (std::min)(currentPick->count, static_cast<std::uint32_t>(currentPick->hits.size()));
			for (auto i = kept; i > 0; --i) {
				if (currentPick->hits[i - 1].ref == a_ref) {
					return &currentPick->hits[i - 1];
				}
			}
			// A door's linked reference stands in for it: noted as its own hit while there is room. Past the kept hits a test or
			// filter call on one of them is no new hit, so nothing is counted for it (the count is FindBodyIdRef's calls).
			if (currentPick->count < currentPick->hits.size()) {
				auto& hit = currentPick->hits[currentPick->count++];
				hit.ref = a_ref;
				return &hit;
			}
			return nullptr;
		}

		bool PickBlockedHook(RE::TESObjectREFR& a_ref)
		{
			const bool blocked = originalIsBlocked(a_ref);
			const bool admit = blocked && AdmitsCopy(a_ref);
			if (const auto hit = PickHitOf(a_ref.GetFormID())) {
				hit->verdict = !blocked ? HitVerdict::kTested : admit ? HitVerdict::kAdmitted : HitVerdict::kBlocked;
			}
			if (!blocked) {
				return false;
			}
			if (!admit) {
				if (currentPick && IsCopy(&a_ref) && pickCopyRefused.fetch_add(1) < 3) {
					REX::INFO("WorkshopSync: workshop mode's pick reached copy {:08X} and its test refused it: {}", a_ref.GetFormID(), WhyNotAdmitted(a_ref));
				}
				return true;
			}
			picksAdmitted.fetch_add(1);
			return false;
		}

		// Each body the pick's cast hit, as the engine names its reference (the general pick's and the wire pick's calls). A
		// friend's stand-in is named the player: the engine passes the player by (by address), and would otherwise refuse the
		// stand-in at its filter and end the general pick's search there, hiding every piece behind it (or every piece at all,
		// with the stand-in on the player's own spot).
		RE::TESObjectREFR* PickHitHook(void* a_world, std::uint32_t a_body)
		{
			auto       ref = originalFindBodyRef(a_world, a_body);
			const bool standIn = ref && IsStandIn(*ref);
			if (currentPick) {
				if (currentPick->count < currentPick->hits.size()) {
					auto& hit = currentPick->hits[currentPick->count];
					hit.ref = ref ? ref->GetFormID() : 0;
					hit.body = a_body;
					hit.blockedKeyword = ref && Blocked(*ref);
					hit.verdict = standIn ? HitVerdict::kStandIn : ref ? HitVerdict::kSkipped : HitVerdict::kNoRef;
				}
				++currentPick->count;
				if (ref && Blocked(*ref) && IsCopy(ref)) {
					pickCopyHits.fetch_add(1);
				}
			}
			if (standIn) {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					if (standInsPassed.fetch_add(1) < 3) {
						REX::INFO("WorkshopSync: workshop mode's pick passed by a friend's stand-in ({:08X}, body {}) as it passes the player", ref->GetFormID(), a_body);
					}
					ref = player;
				}
			}
			return ref;
		}

		// The general filter, at the pick's call: a copy the pick test admitted is admitted here too, when it belongs to the
		// workbench workshop mode is open at (the filter's build-area test, which a copy of another of the host's settlements
		// in the ray's reach would fail).
		bool PickFilterHook(void* a_lambda, RE::TESObjectREFR& a_ref)
		{
			bool       pass = originalGeneralFilter(a_lambda, a_ref);
			const auto hit = PickHitOf(a_ref.GetFormID());
			if (pass) {
				if (hit) {
					hit->verdict = HitVerdict::kPicked;
				}
			} else if (AdmitsCopy(a_ref)) {
				const auto bench = BenchOfCopy(a_ref);
				const auto open = OpenBench();
				if (bench != 0 && bench == open) {
					pass = true;
					if (hit) {
						hit->verdict = HitVerdict::kPickedCopy;
					}
					if (filterAdmitted.fetch_add(1) < 3) {
						REX::INFO("WorkshopSync: workshop mode's general filter refused copy {:08X} (no workshop item of ours); admitted at its call", a_ref.GetFormID());
					}
				} else {
					if (hit) {
						hit->verdict = HitVerdict::kOtherBench;
					}
					if (filterOtherBench.fetch_add(1) < 3) {
						REX::INFO("WorkshopSync: workshop mode's general filter refused copy {:08X} of workbench {:08X}, and so do we: workshop mode is open at {:08X}", a_ref.GetFormID(), bench, open);
					}
				}
			} else if (hit) {
				hit->verdict = HitVerdict::kFilterNo;
			}
			if (pass && currentPick) {
				currentPick->picked = a_ref.GetFormID();
			}
			return pass;
		}

		const char* VerdictName(HitVerdict a_verdict)
		{
			switch (a_verdict) {
			case HitVerdict::kNoRef:
				return "no-ref";
			case HitVerdict::kSkipped:
				return "skipped";
			case HitVerdict::kStandIn:
				return "stand-in";
			case HitVerdict::kOtherBench:
				return "other-bench";
			case HitVerdict::kBlocked:
				return "blocked";
			case HitVerdict::kTested:
				return "tested";
			case HitVerdict::kAdmitted:
				return "admitted";
			case HitVerdict::kFilterNo:
				return "filter-no";
			case HitVerdict::kPicked:
				return "picked";
			case HitVerdict::kPickedCopy:
				return "picked-copy";
			}
			return "?";
		}

		// "N hits: ref:verdict ..." (a body with no reference by its id), near to far as the cast sorted them.
		std::string DescribeHits(const PickRun& a_run)
		{
			std::string out = std::format("{} hits:", a_run.count);
			const auto  kept = (std::min)(a_run.count, static_cast<std::uint32_t>(a_run.hits.size()));
			for (std::uint32_t i = 0; i < kept; ++i) {
				const auto& hit = a_run.hits[i];
				if (hit.ref) {
					out += std::format(" {:08X}{}:{}", hit.ref, hit.blockedKeyword ? "(blocked)" : "", VerdictName(hit.verdict));
				} else {
					out += std::format(" body{}:{}", hit.body, VerdictName(hit.verdict));
				}
			}
			return out;
		}

		// "cast N hits: b<body>[=<ref>]:L<layer>@<distance> ...": every hit of the cast, near to far, whether the search reached
		// it or not (a reference only while a piece is watched; a trigger's body never asked).
		std::string DescribeCast(const PickRun& a_run)
		{
			if (a_run.castCount < 0) {
				return "cast: no hits (or its sort unhooked)";
			}
			std::string out = std::format("cast {} hits:", a_run.castCount);
			const auto  kept = (std::min)(static_cast<std::size_t>(a_run.castCount), a_run.cast.size());
			for (std::size_t i = 0; i < kept; ++i) {
				const auto& hit = a_run.cast[i];
				out += hit.ref ? std::format(" b{}={:08X}:L{}@{:.0f}", hit.body, hit.ref, hit.layer, hit.distance) : std::format(" b{}:L{}@{:.0f}", hit.body, hit.layer, hit.distance);
			}
			if (static_cast<std::size_t>(a_run.castCount) > kept) {
				out += " ...";
			}
			return out;
		}

		// The cast's collector, sorted (the pick's call at +0x3ED, right before its search): the watch reads every hit.
		void PickSortHook(void* a_collector)
		{
			originalPickSort(a_collector);
			const auto run = currentPick;
			if (!run || !a_collector) {
				return;
			}
			const auto         collector = static_cast<const std::byte*>(a_collector);
			const std::byte*   hits = nullptr;
			std::int32_t       count = 0;
			std::memcpy(&hits, collector + CAST_HITS, sizeof(hits));
			std::memcpy(&count, collector + CAST_COUNT, sizeof(count));
			run->castCount = (std::max)(count, 0);
			if (!hits || count <= 0) {
				return;
			}
			const auto kept = (std::min)(static_cast<std::size_t>(count), run->cast.size());
			for (std::size_t i = 0; i < kept; ++i) {
				const auto    hit = hits + i * CAST_STRIDE;
				float         fraction = 0.0f;
				std::uint32_t filter = 0;
				auto&         c = run->cast[i];
				std::memcpy(&fraction, hit + CAST_FRACTION, sizeof(fraction));
				std::memcpy(&c.body, hit + CAST_BODY, sizeof(c.body));
				std::memcpy(&filter, hit + CAST_FILTER, sizeof(filter));
				c.layer = static_cast<std::uint8_t>(filter & 0x7F);
				c.distance = fraction * CAST_REACH;
				// The search's own lookup, after the cast let go of the world as the search's calls are (a trigger never asked).
				if (run->world && originalFindBodyRef && c.layer != 12) {
					const auto ref = originalFindBodyRef(run->world, c.body);
					c.ref = ref ? ref->GetFormID() : 0;
				}
			}
		}

		// One run of a pick (the general pick, or the wire pick): its ray and hits recorded in a_record; with a reference watched,
		// the run logged when what the search reached changes, and again every 2 s while it doesn't (at most four lines a second).
		void WatchPick(GeneralPickFn a_original, void* a_lambda, RE::Workshop::ContextData& a_context, bool a_general, PickRecord& a_record, const char* a_what)
		{
			const auto watched = pickWatch.load();
			PickRun    run;
			run.from = a_context.lookPos;
			run.dir = a_context.lookDir;
			run.cell = a_context.parentCell ? a_context.parentCell->GetFormID() : 0;
			if (watched && a_context.parentCell) {
				if (const auto world = a_context.parentCell->GetbhkWorld()) {
					run.world = world->m_worldNP.get();  // what the pick casts through ([bhkWorld+0x60], as it reads it)
				}
			}
			run.at = std::chrono::steady_clock::now();
			currentPick = &run;
			a_original(a_lambda, a_context, a_general);
			currentPick = nullptr;
			run.world = nullptr;
			std::scoped_lock l{ pickLock };
			a_record.last = run;
			if (watched == 0) {
				return;
			}
			auto hits = DescribeHits(run);
			if ((hits == a_record.logged && run.at - a_record.loggedAt < std::chrono::seconds(2)) || run.at - a_record.loggedAt < std::chrono::milliseconds(250)) {
				return;
			}
			REX::INFO("WorkshopSync: {} watch {:08X}: ray from ({:.0f}, {:.0f}, {:.0f}) along ({:.3f}, {:.3f}, {:.3f}) in cell {:08X}; {}; picked {:08X}; {}", a_what, watched, run.from.x,
				run.from.y, run.from.z, run.dir.x, run.dir.y, run.dir.z, run.cell, hits, run.picked, DescribeCast(run));
			a_record.logged = std::move(hits);
			a_record.loggedAt = run.at;
		}

		// The general pick (each frame workshop mode is up and idle).
		void GeneralPickHook(void* a_lambda, RE::Workshop::ContextData& a_context, bool a_general)
		{
			WatchPick(originalGeneralPick, a_lambda, a_context, a_general, generalPick, "pick");
			pickRuns.fetch_add(1);
		}

		// The wire pick (each frame the placement item is a wire: one held, or one highlighted at idle): what it reached for the
		// wire's second end, and what it took (picked: the wire filter passed it, the target).
		void WirePickHook(void* a_lambda, RE::Workshop::ContextData& a_context, bool a_general)
		{
			WatchPick(originalWirePick, a_lambda, a_context, a_general, wirePick, "wire pick");
			wirePickRuns.fetch_add(1);
		}

		// The wire filter's tests of this reference as the engine reads them, and so why it refused it: no wire end for anyone
		// (neither test passes: the base can't take a wire, in any game, copy or not), or one the bench's grids already connect
		// to the wire's first end.
		std::string WireEndParts(const RE::TESObjectREFR& a_ref, bool a_refused)
		{
			static REL::Relocation<RE::BGSKeyword**>     keyword{ POWER_KEYWORD };
			static REL::Relocation<RE::ObjectRefHandle*> firstEnd{ WIRE_FIRST_END };
			const auto                                   values = RE::ActorValue::GetSingleton();
			const auto                                   info = values ? values->workshopPowerConnection : nullptr;
			const auto                                   from = firstEnd->get();
			const auto                                   base = a_ref.GetObjectReference();
			const float                                  connection = info ? a_ref.GetActorValue(*info) : 0.0f;
			const bool                                   powerKeyword = *keyword && static_cast<const RE::IKeywordFormBase&>(a_ref).HasKeyword(*keyword);
			const char* why = !a_refused                     ? "a wire end"
			                  : connection == 0 && !powerKeyword ? "no wire end in any game: its base has neither the workshopPowerConnection value nor the power keyword"
			                                                     : "the bench's grids connect it to the first end already";
			return std::format("{} (base={:08X} powerConnection={} powerKeyword={} firstEnd={:08X})", why, base ? base->GetFormID() : 0, info ? std::format("{:.0f}", connection) : "?",
				powerKeyword, from ? from.get()->GetFormID() : 0);
		}

		// The wire filter, at the wire pick's call: watched (its verdict on each hit, the target), never changed. A copy is
		// judged by the engine's own rule, as the host's own piece would be: one that can take a wire passes (counted, the
		// first few logged as admitted), one that can't is refused (counted, the first few logged with why).
		bool WireFilterHook(void* a_lambda, RE::TESObjectREFR& a_ref)
		{
			const bool pass = originalWireFilter(a_lambda, a_ref);
			if (const auto hit = PickHitOf(a_ref.GetFormID())) {
				hit->verdict = pass ? HitVerdict::kPicked : HitVerdict::kFilterNo;
			}
			if (pass) {
				if (currentPick) {
					currentPick->picked = a_ref.GetFormID();
				}
				wireTargets.fetch_add(1);
				if (IsCopy(&a_ref) && wireFilterCopyYes.fetch_add(1) < 5) {
					REX::INFO("WorkshopSync: workshop mode's wire filter admitted copy {:08X} as the wire's second end: {}", a_ref.GetFormID(), WireEndParts(a_ref, false));
				}
			} else if (IsCopy(&a_ref) && wireFilterCopyNo.fetch_add(1) < 5) {
				REX::INFO("WorkshopSync: workshop mode's wire filter refused copy {:08X} as the wire's second end: {}", a_ref.GetFormID(), WireEndParts(a_ref, true));
			}
			return pass;
		}

		// The Store hint on a copy (every frame), and the store request's own test before its yes/no box: the engine's own test
		// when the host allows a friend's store, as for any piece (a copy's base is its builder's); refused silently when the
		// host's setting is off, and on a copy of the host's wire (not served, as a scrap of one isn't). The key itself (the
		// menu's Cancel, StoreKeyHook) says why.
		bool CanStoreHook(RE::TESObjectREFR& a_ref)
		{
			if (AdmitsCopy(a_ref) && (!EditAllowed() || IsWire(a_ref.GetObjectReference()))) {
				return false;
			}
			return originalCanStore(a_ref);
		}

		// The engine's own store test, Workshop::CanStoreRef (the function itself, not the patched calls of it): what the host's
		// game asks of its piece before storing it for a friend.
		bool EngineCanStore(RE::TESObjectREFR& a_ref)
		{
			static REL::Relocation<CanStoreFn> canStore{ CAN_STORE_REF };
			return canStore(a_ref);
		}

		// The Scrap hint on a copy: shown when the host allows a friend's scrap, by the engine's own test, as for any piece
		// (a copy's base is its builder's: a piece a recipe makes passes); hidden when the host's setting is off, and on a
		// copy of the host's wire (the host's game doesn't scrap a wire for a friend: ScrapKeyHook says so when pressed).
		bool CanScrapHook(RE::TESObjectREFR& a_ref, bool a_flag)
		{
			if (AdmitsCopy(a_ref) && (!EditAllowed() || IsWire(a_ref.GetObjectReference()))) {
				return false;
			}
			return originalCanScrap(a_ref, a_flag);
		}

		bool CanSelectHook(RE::TESObjectREFR& a_ref)
		{
			return !AdmitsCopy(a_ref) && originalCanSelect(a_ref);
		}

		// A key on a copy, refused: counted, the first few logged, and (a_say) said on the HUD (one press, one notice).
		void RefusedKey(std::atomic<std::uint32_t>& a_counter, const char* a_what, const RE::TESObjectREFR& a_ref, bool a_say = true)
		{
			if (a_counter.fetch_add(1) < 3) {
				REX::INFO("WorkshopSync: workshop mode's {} of copy {:08X} refused: the host's piece", a_what, a_ref.GetFormID());
			}
			if (a_say) {
				Hud::Notify(Text::Get("workshop.shared.no_grab"));
			}
		}

		// The menu's Accept on the highlighted piece (the grab, or a repair): on the main thread (the menu's user event handler).
		bool SelectKeyHook(RE::TESObjectREFR& a_ref)
		{
			if (AdmitsCopy(a_ref)) {
				RefusedKey(grabsRefused, "grab", a_ref);
				return false;
			}
			// A piece of ours still waiting for the host's answer: picked up, it would be put down as a moved piece (linked to
			// our bench again, snapped again) just as it becomes the host's. On the main thread, where the requests live.
			if (HandsOver() && IsPendingPiece(a_ref.GetFormID())) {
				grabsRefused.fetch_add(1);
				Hud::Notify(Text::Get("workshop.shared.pending"));
				return false;
			}
			return originalCanSelect(a_ref);
		}

		// Whether something asked of the host about this reference still waits for its answer, for the Scrap and Store keys
		// (SharedWorkshop::OnScrapKey's a_pending): a copy's scrap or store asked already, or a piece of ours whose build is
		// asked (while pieces are handed over, as the grab asks it). On the main thread, where the requests live.
		bool PendingFor(bool a_copy, std::uint32_t a_id)
		{
			return (a_copy || HandsOver()) && IsPendingPiece(a_id);
		}

		// The Scrap or Store key on a piece of ours whose build still waits for the host's answer: refused. Scrapped or stored
		// here now, it would pay us back (or enter our stored list) while the host's game builds its own piece, which comes
		// back as a copy. Counted, the first few logged, said on the HUD.
		void RefusedOwnPending(std::atomic<std::uint32_t>& a_counter, const char* a_what, std::uint32_t a_id)
		{
			if (a_counter.fetch_add(1) < 3) {
				REX::INFO("WorkshopSync: workshop mode's {} of our {:08X} refused: its build still waits for the host's answer", a_what, a_id);
			}
			Hud::Notify(Text::Get("workshop.shared.pending"));
		}

		// The menu's Store key (Cancel with nothing held, on the highlighted piece): on the host's copy, refused with a notice when
		// the host's setting is off or something is asked of that copy already (SharedWorkshop::OnStoreKey); else the engine's
		// own test (CanStoreHook inside it) and, when it says yes, the engine's yes/no box, whose Yes asks the host
		// (StoreAcceptHook). A copy the engine's own test refuses is counted, not said: Cancel on a piece that can't be stored is
		// workshop mode's own Cancel, as before, with no notice. A piece of ours whose build still waits for the host's answer
		// is refused with a notice too (RefusedOwnPending).
		bool StoreKeyHook()
		{
			const auto ref = PlacementRef();
			const bool copy = ref && AdmitsCopy(*ref);
			if (ref) {
				const auto id = ref->GetFormID();
				switch (SharedWorkshop::OnStoreKey(copy, EditAllowed(), IsWire(ref->GetObjectReference()), PendingFor(copy, id))) {
				case SharedWorkshop::ScrapKey::kRefuseEditOff:
					if (storesRefused.fetch_add(1) < 3) {
						REX::INFO("WorkshopSync: workshop mode's store of copy {:08X} refused: the host's bSharedWorkshopEdit is off", id);
					}
					Hud::Notify(Text::Get("workshop.shared.edit_off"));
					return false;
				case SharedWorkshop::ScrapKey::kRefuseWire:
					if (storesRefused.fetch_add(1) < 3) {
						REX::INFO("WorkshopSync: workshop mode's store of copy {:08X} refused: a wire of the host's (not served)", id);
					}
					Hud::Notify(Text::Get("workshop.shared.wire_no_store"));
					return false;
				case SharedWorkshop::ScrapKey::kRefusePending:
					storesRefused.fetch_add(1);
					Hud::Notify(Text::Get(PendingNoticeOf(id)));
					return false;
				case SharedWorkshop::ScrapKey::kRefuseOwnPending:
					RefusedOwnPending(storesRefused, "store", id);
					return false;
				case SharedWorkshop::ScrapKey::kAsk:  // the engine's test and its box as for any piece; its Yes asks the host
				case SharedWorkshop::ScrapKey::kEngine:
					break;
				}
			}
			const bool may = originalCanStoreItem();
			if (!may && copy) {
				RefusedKey(storesRefused, "store", *ref, false);  // the engine's own test refused the copy, as it would its builder's piece
			}
			return may;
		}

		// WorkshopCanScrapRef's parts for this reference, as the engine reads them (CAN_SCRAP_REF, SCRAP_BLOCK_KEYWORD,
		// CONTAINER_CAN_HOLD_FORM), and the test that refused it (SharedWorkshop::ScrapRefusal, its order the engine's): for
		// a_flag as the caller asks it (the Scrap key and the Scrap button ask false). A wire fails at recipe=false in any
		// game: no recipe makes BlackWireSpline01, so the engine never marks its base (inGameFormFlags bit 0).
		std::string ScrapParts(const RE::TESObjectREFR& a_ref, bool a_flag = false)
		{
			static REL::Relocation<RE::BGSKeyword**> keyword{ SCRAP_BLOCK_KEYWORD };
			static REL::Relocation<std::uint8_t*>    gate{ SCRAP_GATE };
			using hold_t = bool (*)(RE::TESBoundObject*);
			static REL::Relocation<hold_t> canHold{ CONTAINER_CAN_HOLD_FORM };
			const auto                     base = a_ref.GetObjectReference();
			SharedWorkshop::ScrapFacts     facts;
			facts.base = base != nullptr;
			facts.blockKeyword = *keyword && static_cast<const RE::IKeywordFormBase&>(a_ref).HasKeyword(*keyword);
			if (base) {
				const auto type = base->GetFormType();
				facts.inventoryType = type == RE::ENUM_FORM_ID::kARMO || type == RE::ENUM_FORM_ID::kMISC || type == RE::ENUM_FORM_ID::kWEAP;
				facts.loose = a_ref.IsProjectile() == nullptr && canHold(base);
				facts.recipe = (base->inGameFormFlags & 1) != 0;  // an actor's is its actor base's: close enough for a log
				facts.miscComponents = type == RE::ENUM_FORM_ID::kMISC && static_cast<const RE::TESObjectMISC*>(base)->componentData != nullptr;
			}
			facts.objectInstance = a_ref.extraList && a_ref.extraList->HasType(RE::EXTRA_DATA_TYPE::kObjectInstance);
			const auto fail = SharedWorkshop::ScrapRefusal(facts, a_flag);
			return std::format("failed={} (base={:08X} baseType={:02X} blockKeyword={} inventoryType={} loose={} recipe={} miscComponents={} objectInstance={} actor={} gate={})",
				SharedWorkshop::ScrapFailName(fail), base ? base->GetFormID() : 0, base ? std::to_underlying(base->GetFormType()) : 0, facts.blockKeyword, facts.inventoryType, facts.loose,
				facts.recipe, facts.miscComponents, facts.objectInstance, a_ref.GetFormType() == RE::ENUM_FORM_ID::kACHR, *gate);
		}

		// The Scrap key (Workshop::RequestScrapPlacementItem, only the menu's XButton calls it, and only past its gate,
		// SCRAP_GATE). Each press logged with the engine's answer: a press that never comes here never got past the gate (or
		// the menu); one that does and is refused, the engine's scrap test refused the piece (its parts said). A piece of ours
		// whose build still waits for the host's answer is refused before the engine is asked (RefusedOwnPending).
		bool ScrapKeyHook(RE::TESObjectREFR& a_ref, bool a_flag)
		{
			const bool copy = AdmitsCopy(a_ref);
			switch (SharedWorkshop::OnScrapKey(copy, EditAllowed(), IsWire(a_ref.GetObjectReference()), PendingFor(copy, a_ref.GetFormID()))) {
			case SharedWorkshop::ScrapKey::kRefuseEditOff:
				if (scrapsRefused.fetch_add(1) < 3) {
					REX::INFO("WorkshopSync: workshop mode's scrap of copy {:08X} refused: the host's bSharedWorkshopEdit is off", a_ref.GetFormID());
				}
				Hud::Notify(Text::Get("workshop.shared.edit_off"));
				return false;
			case SharedWorkshop::ScrapKey::kRefuseWire:
				if (scrapsRefused.fetch_add(1) < 3) {
					REX::INFO("WorkshopSync: workshop mode's scrap of copy {:08X} refused: a wire of the host's (not served)", a_ref.GetFormID());
				}
				Hud::Notify(Text::Get("workshop.shared.wire_no_scrap"));
				return false;
			case SharedWorkshop::ScrapKey::kRefusePending:
				scrapsRefused.fetch_add(1);
				Hud::Notify(Text::Get(PendingNoticeOf(a_ref.GetFormID())));
				return false;
			case SharedWorkshop::ScrapKey::kRefuseOwnPending:
				RefusedOwnPending(scrapsRefused, "scrap", a_ref.GetFormID());
				return false;
			case SharedWorkshop::ScrapKey::kAsk:  // the engine's test and its confirmation as for any piece; its Yes asks the host (ScrapAcceptHook)
			case SharedWorkshop::ScrapKey::kEngine:
				break;
			}
			const bool may = originalCanScrap(a_ref, a_flag);
			scrapKeys.fetch_add(1);
			if (!may) {
				scrapKeyNo.fetch_add(1);
			}
			REX::INFO("WorkshopSync: workshop mode's Scrap key on {:08X}: the engine's scrap test said {} ({})", a_ref.GetFormID(), may ? "yes (its confirmation opens)" : "no",
				ScrapParts(a_ref, a_flag));
			return may;
		}

		void NoteNotOwned(std::uint32_t a_bench)
		{
			taught.NotOwned(a_bench);
			std::scoped_lock l{ copyIdsLock };
			notOwnedBenches.insert(a_bench);
		}

		void NoteOwned(std::uint32_t a_bench)
		{
			taught.Owned(a_bench);
			std::scoped_lock l{ copyIdsLock };
			notOwnedBenches.erase(a_bench);
		}

		// A settlement the host's game owns: the way a settlement becomes the player's (WorkshopScript.SetOwnedByPlayer) makes the
		// player's NPC the workbench's owner, and losing it clears that; PlayerFaction counts too (Storage.cpp's OwnBench).
		bool OwnBench(RE::TESObjectREFR* a_bench)
		{
			const auto owner = a_bench ? a_bench->GetOwner() : nullptr;
			return owner && (owner->GetFormID() == 0x00000007 || owner->GetFormID() == 0x0001C21C);
		}

		void PushShared(std::vector<std::uint8_t> a_packet)
		{
			std::scoped_lock l{ outLock };
			if (!outgoing.Push({ std::move(a_packet), 0 })) {
				++dropped;
			}
		}

		// The ends of a wire: the references its power links name that are no wire.
		std::vector<RE::NiPointer<RE::TESObjectREFR>> EndsOf(const RE::TESObjectREFR& a_wire)
		{
			std::vector<RE::NiPointer<RE::TESObjectREFR>> ends;
			const auto                                    links = a_wire.extraList ? a_wire.extraList->GetByType<RE::ExtraPowerLinks>() : nullptr;
			if (!links) {
				return ends;
			}
			for (const auto& link : links->powerLinks) {
				RE::NiPointer<RE::TESObjectREFR> end{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(link.formID) };
				if (end && !IsWire(end->GetObjectReference())) {
					ends.push_back(end);
				}
			}
			return ends;
		}

		// The key of our copy with this ID, if it is one.
		std::optional<Key> KeyOfCopy(std::uint32_t a_id)
		{
			for (const auto& [key, copy] : copies) {
				if (copy.id == a_id) {
					return key;
				}
			}
			return std::nullopt;
		}

		// Workshop mode's current workbench set to a_bench while this lives, and put back after (the engine's finish reads it).
		// How many of our BenchScopes are open, and what the engine itself held as current before the outermost one set the
		// piece's bench there: the budget's "live" decision must read the engine's own value, never ours (a finish runs inside
		// a BenchScope, so the static always names the piece's bench then).
		std::uint32_t benchScopes = 0;
		std::uint32_t engineBench = 0;

		struct BenchScope
		{
			std::uint32_t* current;
			std::uint32_t  before;

			explicit BenchScope(RE::TESObjectREFR& a_bench)
			{
				static REL::Relocation<std::uint32_t*> handle{ CURRENT_WORKSHOP };
				current = handle.get();
				before = *current;
				if (benchScopes++ == 0) {
					engineBench = before;
				}
				*current = a_bench.GetHandle().get_handle();
			}

			~BenchScope()
			{
				*current = before;
				--benchScopes;
			}

			BenchScope(const BenchScope&) = delete;
			BenchScope& operator=(const BenchScope&) = delete;
		};

		// The workbench the engine itself holds as current: the static, unless one of our scopes has set it (then what it was before).
		std::uint32_t EngineBench()
		{
			static REL::Relocation<std::uint32_t*> current{ CURRENT_WORKSHOP };
			return benchScopes > 0 ? engineBench : *current;
		}

		// The budget's totals made the workbench's for an AddObject or RemoveObject on one of its pieces, and put back after:
		// live as they are when workshop mode is open at this very workbench (the engine loaded them there); otherwise the
		// four globals saved, loaded from the workbench (Budget::Initialize, as workshop mode's entry does), and restored,
		// with the current workbench set meanwhile so the result is written to the bench.
		struct BudgetScope
		{
			bool                                  live;
			std::array<std::uint32_t*, 4>         globals{};
			std::array<std::uint32_t, 4>          saved{};
			std::optional<BenchScope>             bench;

			explicit BudgetScope(RE::TESObjectREFR& a_bench)
			{
				using init_t = void (*)(RE::TESObjectREFR&);
				static REL::Relocation<std::uint32_t*> current{ CURRENT_WORKSHOP };
				static REL::Relocation<std::uint32_t*> triangles{ BUDGET_TRIANGLES };
				static REL::Relocation<std::uint32_t*> draws{ BUDGET_DRAWS };
				static REL::Relocation<std::uint32_t*> maxTriangles{ BUDGET_MAX_TRIANGLES };
				static REL::Relocation<std::uint32_t*> maxDraws{ BUDGET_MAX_DRAWS };
				static REL::Relocation<init_t>         initialize{ BUDGET_INITIALIZE };
				// The engine's own current workbench (what it had before any scope of ours, which a finish runs inside), and the menu
				// open: then the totals are this bench's, live (SharedWorkshop::BudgetLive).
				live = SharedWorkshop::BudgetLive(EngineBench(), a_bench.GetHandle().get_handle(), MenuWatch::IsOpen("WorkshopMenu"sv));
				if (live) {
					return;
				}
				globals = { triangles.get(), draws.get(), maxTriangles.get(), maxDraws.get() };
				for (std::size_t i = 0; i < globals.size(); ++i) {
					saved[i] = *globals[i];
				}
				bench.emplace(a_bench);
				initialize(a_bench);
			}

			~BudgetScope()
			{
				if (!live) {
					for (std::size_t i = 0; i < globals.size(); ++i) {
						*globals[i] = saved[i];
					}
				}
			}

			BudgetScope(const BudgetScope&) = delete;
			BudgetScope& operator=(const BudgetScope&) = delete;
		};

		// Workshop mode's "new item mode" byte set to 0 (a new piece being placed) for the finish of a piece of ours, as the
		// engine has it while a ghost is confirmed, and put back after: MakeReferenceStatic then starts the piece's scripts,
		// sends its loaded event and sets its power's initial state, as for any new piece.
		struct NewItemScope
		{
			std::uint8_t* byte;
			std::uint8_t  before;

			NewItemScope()
			{
				static REL::Relocation<std::uint8_t*> moving{ MOVING_ITEM };
				byte = moving.get();
				before = *byte;
				*byte = 0;
			}

			~NewItemScope() { *byte = before; }

			NewItemScope(const NewItemScope&) = delete;
			NewItemScope& operator=(const NewItemScope&) = delete;
		};

		// Takes a piece off our workshop: off every grid that names it, its power links, its workbench and, a_budgeted, its
		// budget. For a piece of ours adopted as the host's copy, and for a wire of ours taken off the moment it was strung
		// (both counted in the budget by the engine's placement: taken out again), and for a copy the engine's wiring tied in
		// (never counted: a wire's placement adds only the wire, and links the ends; the budget is left alone). Not a word of
		// the power audit's warnings: this is the rule's own doing.
		void Unbuild(RE::TESObjectREFR& a_ref, RE::TESObjectREFR* a_bench, bool a_budgeted)
		{
			using remove_t = void (*)(RE::Workshop::ExtraData*, RE::TESObjectREFR&);
			using disconnect_t = void (*)(RE::TESObjectREFR&);
			using budget_t = void (*)(RE::TESObjectREFR&);
			static REL::Relocation<remove_t>     removeItem{ REMOVE_GRID_ITEM };
			static REL::Relocation<disconnect_t> disconnect{ DISCONNECT_SPLINE };
			static REL::Relocation<budget_t>     budgetRemove{ BUDGET_REMOVE_OBJECT };
			const auto                           id = a_ref.GetFormID();
			if (a_bench) {
				powerBenches.insert(a_bench->GetFormID());
			}
			if (HasPowerLinks(a_ref)) {
				disconnect(a_ref);  // a wire: out of its ends' links; a piece: its wires' and snapped neighbours' links dropped
			}
			for (const auto other : PowerBenches()) {
				if (GridsName(other, id)) {
					removeItem(WorkshopDataOf(other), a_ref);
				}
			}
			if (a_budgeted && a_bench && a_ref.Get3D()) {
				BudgetScope scope{ *a_bench };  // the bench's own totals, less this piece, written to the bench
				budgetRemove(a_ref);
			}
			if (const auto keyword = WorkshopItemKeyword(); keyword && BenchOf(a_ref)) {
				a_ref.SetLinkedRef(nullptr, keyword);
			}
			gridNamedFrame = ~0ull;  // the grids changed
		}

		void RecountBench(RE::TESObjectREFR& a_bench)
		{
			using bench_t = void (*)(RE::TESObjectREFR&);
			static REL::Relocation<bench_t> powerRating{ CALCULATE_POWER_RATING };
			static REL::Relocation<bench_t> recount{ RECALCULATE_RESOURCES };
			powerRating(a_bench);
			recount(a_bench);
		}

		// A wire our workshop mode just strung to (or between) copies: off our grid, its ends' links and our workbench, and
		// gone, in this same frame, so no save can hold it (the audit nets what this misses). The ends that are copies are
		// taken off whatever the engine's placement tied them into (the grid, a bench link).
		void DetachWire(RE::TESObjectREFR& a_wire, const std::vector<RE::NiPointer<RE::TESObjectREFR>>& a_ends, RE::TESObjectREFR* a_bench)
		{
			Unbuild(a_wire, a_bench, true);  // the engine's placement counted the wire in the budget
			for (const auto& end : a_ends) {
				if (IsCopy(end.get())) {
					Unbuild(*end, a_bench, false);  // ...and only linked the ends: never counted, so never taken out
				}
			}
			if (const auto keyword = WorkshopItemKeyword(); keyword && BenchOf(a_wire)) {
				a_wire.SetLinkedRef(nullptr, keyword);
			}
			a_wire.Disable();
			a_wire.SetDelete(true);
			// Our bench's power rating and resources once more without it (as Adopt does): the engine's placement counted the
			// copy ends as powered through the wire, which our grid no longer holds.
			if (a_bench) {
				RecountBench(*a_bench);
			}
			madeHere.insert(a_wire.GetFormID());
			++wiresDetached;
		}

		// Our piece becomes our copy of the host's (the answer named it): off our workshop, blocked from workshop mode and kept
		// under the host's key, as any copy. The host's own report of it (its placed event) finds the copy kept, or found
		// ours standing at the spot already and made none.
		// The destroyed event for a piece of ours that stops being ours (adopted as the host's, or deleted before a save or on
		// leaving): what Workshop::FinishDestroyItem notifies for a scrap, which the workshop scripts answer with
		// OnWorkshopObjectDestroyed, undoing what the placed event did for the piece as ours (a settler's assignment to a bed
		// or a crop, the workshop's counts). Without it a save keeps the assignment for a reference that is the host's, or
		// gone. Mods listening for WorkshopObjectDestroyed hear it as a scrap (docs/DESIGN.md).
		void NotifyDestroyed(RE::TESObjectREFR* a_bench, RE::TESObjectREFR& a_ref)
		{
			if (!a_bench) {
				return;
			}
			using source_t = RE::BSTEventSource<RE::Workshop::ItemDestroyedEvent>* (*)();
			static REL::Relocation<source_t> source{ DESTROYED_EVENT_SOURCE };
			if (const auto events = source()) {
				RE::Workshop::ItemDestroyedEvent event{ RE::NiPointer<RE::TESObjectREFR>{ a_bench }, RE::NiPointer<RE::TESObjectREFR>{ &a_ref } };
				events->Notify(event);
			}
		}

		// False if it has to wait: the player holds the piece in workshop mode (the engine's drop would link it to our bench and
		// count it again), so it is adopted once put down (adoptLater, SharedFrame).
		bool Adopt(const SharedWorkshop::Pending& a_pending, const Key& a_key)
		{
			const auto                             a_piece = a_pending.piece;
			const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_piece) };
			// (A dropped build's piece is gone, and its runtime id may name another reference by now: never adopted.)
			if (a_pending.dropped || !ref || ref->IsDeleted() || IsCopy(ref.get())) {
				++adoptMissing;
				REX::INFO("WorkshopSync: the host built our {:08X} as its {:08X}, but ours is gone ({}): the host's copy comes on its own", a_piece, a_key.second,
					a_pending.dropped ? "dropped before a save, while it waited" : "or a copy already");
				return true;
			}
			if (Held(*ref)) {
				return false;
			}
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_pending.bench);
			Unbuild(*ref, bench, true);  // the engine's placement counted it in the budget
			if (bench) {
				RecountBench(*bench);
			}
			reporter.Forget(a_piece);
			if (const auto it = copies.find(a_key); it != copies.end()) {
				// The host's report came first and made a copy of its own at the spot (ours stood there, so it should not have:
				// WorkshopApply's OursAt), or an earlier session's: ours stands in for it, and that one goes.
				EngineWorld world;
				world.Discard(it->second);
				copies.erase(it);
			}
			auto copy = MakeCopy(ref->GetHandle(), Clock::now());
			copies.emplace(a_key, copy);
			NoteCopy(copy, a_pending.bench, a_key.first);
			BlockInteraction(*ref);
			madeHere.insert(a_piece);
			++adoptedHere;
			++counts.adopted;
			// The engine's own placed event already ran Papyrus for the piece as ours, and a save would keep that for a reference
			// that is the host's now: undone as a scrap undoes it. Our own sink sees a copy by then and reports nothing.
			NotifyDestroyed(bench, *ref);
			REX::INFO("WorkshopSync: our {:08X} (base {:08X}) is the host's piece now: adopted as our copy of player {}'s {:08X} at workbench {:08X}", a_piece, copy.base, a_key.first,
				a_key.second, a_pending.bench);
			return true;
		}

		// A piece of ours still waiting for an answer, or a build the answer refused, is ours as before the rule: reported.
		void KeepOwn(const SharedWorkshop::Pending& a_pending)
		{
			if (a_pending.dropped) {
				return;  // gone before the save (and its runtime id may name another reference by now)
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_pending.piece);
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_pending.bench);
			if (ref && bench && !ref->IsDeleted() && !ref->IsDisabled() && ReportRef(ref, bench, Protocol::WorkshopOp::kPlaced)) {
				SettleLater(ref, bench);
			}
		}

		void Answered(std::uint32_t a_seq, std::uint8_t a_op, Protocol::WorkshopAnswer::Why a_why, const SharedWorkshop::Pending& a_pending, const Key& a_made)
		{
			const auto verdict = SharedWorkshop::Judge(a_op, a_why);
			if (verdict.benchNotOwned) {
				NoteNotOwned(a_pending.bench);
			} else if (a_why == Protocol::WorkshopAnswer::Why::kApplied) {
				NoteOwned(a_pending.bench);
			}
			++(a_why == Protocol::WorkshopAnswer::Why::kApplied ? answersApplied : answersRefused);
			REX::INFO("WorkshopSync: request {} ({}) at workbench {:08X}: {}{}", a_seq, Protocol::OpName(a_op), a_pending.bench, Protocol::Name(a_why),
				a_made.second != 0 ? std::format(", the host's {:08X}", a_made.second) : std::string{});
			if (verdict.notice) {
				Hud::Notify(Text::Get(verdict.notice));
			}
			if (verdict.sound && MenuWatch::IsOpen("WorkshopMenu"sv)) {
				// The store's sound, which the engine's own store plays at the box's Yes and ours skipped (AskStore): now that the
				// host's game stored the piece. Not at a press, so counted (a request is answered once: one sound a store).
				ClickWatch::Note(ClickRates::Source::kStoreSound);
				RE::UIUtils::PlayMenuSound(verdict.sound);
			}
			switch (verdict.outcome) {
			case SharedWorkshop::Outcome::kAdopt:
				if (!Adopt(a_pending, a_made)) {
					adoptLater.emplace_back(a_pending, a_made);  // held in the player's hands: once put down
					++adoptsDeferred;
				}
				break;
			case SharedWorkshop::Outcome::kKeepOwn:
				KeepOwn(a_pending);
				break;
			case SharedWorkshop::Outcome::kNothing:
				break;
			}
		}

		// A piece (or a wire) our workshop mode just placed, under the rule: true if it was taken as a request (or taken off),
		// and so is not reported as ours.
		bool HandOver(RE::TESObjectREFR* a_item, RE::TESObjectREFR* a_bench)
		{
			if (!HandsOver() || !a_item || !a_bench || IsCopy(a_item) || !IsWorkbenchAnchor(a_bench)) {
				return false;
			}
			const auto base = a_item->GetObjectReference();
			if (!base || (base->GetFormID() >> 24) == 0xFF || !WorkshopApply::Shared(true, Equipment::IsPowerArmorFrame(base)) || base->As<RE::TESNPC>()) {
				return false;  // (a turret or a robot stays its builder's own, quietly: not in phase 1)
			}
			MainThread::Assert("WorkshopSync::HandOver");
			const auto benchId = a_bench->GetFormID();
			if (IsWire(base)) {
				const auto ends = EndsOf(*a_item);
				const auto from = ends.size() == 2 ? KeyOfCopy(ends[0]->GetFormID()) : std::nullopt;
				const auto to = ends.size() == 2 ? KeyOfCopy(ends[1]->GetFormID()) : std::nullopt;
				if (ends.size() != 2) {
					// Its ends can't be read (one of them gone already?): not a wire of ours to report, nor one to ask for.
					DetachWire(*a_item, ends, a_bench);
					Hud::Notify(Text::Get("workshop.shared.wire_refused"));
					REX::INFO("WorkshopSync: wire {:08X} taken off: {} ends found", a_item->GetFormID(), ends.size());
					return true;
				}
				// Both ends the host's pieces (the copies a friend may wire: AdmitsCopy): another friend's copies are never asked,
				// nor two builders' pieces whose runtime ids happen to agree.
				switch (SharedWorkshop::KindOfWire(from.has_value(), to.has_value(), from && to && from->first == to->first && from->first == hostId.load())) {
				case SharedWorkshop::WireKind::kOwn:
					return false;
				case SharedWorkshop::WireKind::kMixed:
					DetachWire(*a_item, ends, a_bench);
					++wiresMixed;
					Hud::Notify(Text::Get("workshop.shared.wire_mixed"));
					REX::INFO("WorkshopSync: wire {:08X} from {:08X} to {:08X} taken off: one end is ours and one a friend's copy", a_item->GetFormID(), ends[0]->GetFormID(), ends[1]->GetFormID());
					return true;
				case SharedWorkshop::WireKind::kRequest:
					break;
				}
				DetachWire(*a_item, ends, a_bench);
				if (taught.IsNotOwned(benchId)) {
					Hud::Notify(Text::Get("workshop.shared.wire_refused"));
					return true;
				}
				const auto seq = requests.Open(Protocol::WorkshopRequestOp::kWire, 0, benchId, Clock::now());
				if (!seq) {
					Hud::Notify(Text::Get("workshop.shared.wire_refused"));
					return true;
				}
				Protocol::WorkshopRequest request;
				request.seq = *seq;
				request.op = Protocol::WorkshopRequestOp::kWire;
				request.workshop = benchId;
				request.wireFrom = from->second;
				request.wireTo = to->second;
				PushShared(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest));
				++wiresAsked;
				REX::INFO("WorkshopSync: wire {:08X} between copies of player {}'s {:08X} and {:08X} taken off and asked of the host as request {}", a_item->GetFormID(), from->first,
					from->second, to->second, *seq);
				return true;
			}
			if (taught.IsNotOwned(benchId)) {
				return false;  // the host's game said this settlement isn't its own: ours, as before
			}
			if (IsPendingPiece(a_item->GetFormID())) {
				return true;  // asked already (its placed event after wsreport's hand-over): one request, not two
			}
			const auto seq = requests.Open(Protocol::WorkshopRequestOp::kBuild, a_item->GetFormID(), benchId, Clock::now());
			if (!seq) {
				return false;
			}
			Protocol::WorkshopRequest request;
			request.seq = *seq;
			request.op = Protocol::WorkshopRequestOp::kBuild;
			request.workshop = benchId;
			request.base = base->GetFormID();
			request.position = { a_item->data.location.x, a_item->data.location.y, a_item->data.location.z };
			request.rotation = { a_item->data.angle.x, a_item->data.angle.y, a_item->data.angle.z };
			request.scale = a_item->refScale / 100.0f;
			if (!Protocol::IsValid(request)) {
				return false;
			}
			PushShared(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest));
			++requestsSent;
			REX::INFO("WorkshopSync: our {:08X} (base {:08X}) at workbench {:08X} asked of the host as request {}", a_item->GetFormID(), base->GetFormID(), benchId, *seq);
			return true;
		}

		// A copy of the host's piece the player confirmed to scrap (the engine's own confirmation said Yes): asked of the host as
		// a numbered request, never scrapped here (the host's scrap of its piece comes back as its scrapped report, which takes
		// the copy away). On the main thread (the confirmation's callback). A copy that can't be asked for stands as it was.
		void AskScrap(RE::TESObjectREFR& a_copy)
		{
			MainThread::Assert("WorkshopSync::AskScrap");
			const auto id = a_copy.GetFormID();
			const auto key = KeyOfCopy(id);
			const auto benchId = BenchOfCopy(a_copy);
			if (IsPendingPiece(id)) {
				Hud::Notify(Text::Get("workshop.shared.scrap_pending"));
				return;
			}
			if (!HandsOver() || !EditAllowed() || !key || benchId == 0 || taught.IsNotOwned(benchId)) {
				Hud::Notify(Text::Get("workshop.shared.scrap_refused"));
				REX::INFO("WorkshopSync: the scrap of our copy {:08X} not asked ({})", id, WhyNotAdmitted(a_copy));
				return;
			}
			const auto seq = requests.Open(Protocol::WorkshopRequestOp::kScrap, id, benchId, Clock::now());
			if (!seq) {
				Hud::Notify(Text::Get("workshop.shared.scrap_refused"));
				return;
			}
			Protocol::WorkshopRequest request;
			request.seq = *seq;
			request.op = Protocol::WorkshopRequestOp::kScrap;
			request.workshop = benchId;
			request.target = key->second;
			PushShared(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest));
			scrapsAsked.fetch_add(1);
			REX::INFO("WorkshopSync: the scrap of our copy {:08X} of player {}'s {:08X} at workbench {:08X} asked of the host as request {} (nothing scrapped here)", id, key->first,
				key->second, benchId, *seq);
		}

		// Workshop::RequestScrapCallback::OnAccept's call of Workshop::ScrapReference (SCRAP_ACCEPT_CALL), in both games. The
		// confirmation is modal and may have stood open a long while since the key: the reference is judged as it is now
		// (SharedWorkshop::OnScrapAccept). One disabled or deleted meanwhile (the host's: a friend's request scrapped it first;
		// the friend's copy: the host's scrapped report took it, which forgets the copy but leaves the handle resolving) is never
		// given to the engine's scrap (the host would yield twice; the friend would put the rewards into its bench from nothing
		// and delete a copy outside the plugin's bookkeeping). A copy, or a reference that was one (the block-interaction
		// keyword every copy keeps, the copy sets, the graveyard), is asked of the host while it is still admitted, else refused
		// with a notice. Anything else is the player's own piece: scrapped as ever. The main thread (the menu's callback).
		void ScrapAcceptHook(RE::Workshop::ContextData& a_context, RE::NiPointer<RE::TESObjectREFR>& a_ref, RE::BSTArray<RE::BSTTuple<RE::TESBoundObject*, std::uint32_t>>* a_rewards)
		{
			if (a_ref) {
				MainThread::Assert("WorkshopSync::ScrapAcceptHook");
				const auto id = a_ref->GetFormID();
				const bool gone = a_ref->IsDisabled() || a_ref->IsDeleted();
				const bool copy = Blocked(*a_ref) || IsCopy(a_ref.get()) || InGraveyard(a_ref.get());
				switch (SharedWorkshop::OnScrapAccept(gone, copy, AdmitsCopy(*a_ref))) {
				case SharedWorkshop::ScrapAccept::kRefuseGone:
					scrapsRefused.fetch_add(1);
					REX::INFO("WorkshopSync: the scrap confirmation's Yes on {:08X} refused: {} already (copy={}); nothing scrapped", id, a_ref->IsDeleted() ? "deleted" : "disabled", copy);
					Hud::Notify(Text::Get("workshop.shared.scrap_gone"));
					return;
				case SharedWorkshop::ScrapAccept::kRefuseCopy:
					scrapsRefused.fetch_add(1);
					REX::INFO("WorkshopSync: the scrap confirmation's Yes on copy {:08X} refused: no longer admitted ({}, hostGone={}); nothing scrapped", id, WhyNotAdmitted(*a_ref), hostGone);
					Hud::Notify(Text::Get("workshop.shared.scrap_refused"));
					return;
				case SharedWorkshop::ScrapAccept::kAsk:
					AskScrap(*a_ref);
					return;
				case SharedWorkshop::ScrapAccept::kEngine:
					break;
				}
			}
			originalScrapReference(a_context, a_ref, a_rewards);
		}

		// A copy of the host's piece the player confirmed to store (the engine's own yes/no box said Yes): asked of the host as a
		// numbered request, never stored here (nothing enters our own stored list; the host's store of its piece comes back as
		// its destroyed report, which takes the copy away). On the main thread (the box's callback). The store's sound is the
		// engine's, played inside the call we skip: it is played when the host's answer says the piece was stored (Answered),
		// not here, so a store the host refuses makes no sound. A copy that can't be asked for stands as it was.
		void AskStore(RE::TESObjectREFR& a_copy)
		{
			MainThread::Assert("WorkshopSync::AskStore");
			const auto id = a_copy.GetFormID();
			const auto key = KeyOfCopy(id);
			const auto benchId = BenchOfCopy(a_copy);
			if (IsPendingPiece(id)) {
				Hud::Notify(Text::Get(PendingNoticeOf(id)));
				return;
			}
			if (!HandsOver() || !EditAllowed() || !key || benchId == 0 || taught.IsNotOwned(benchId)) {
				Hud::Notify(Text::Get("workshop.shared.store_refused"));
				REX::INFO("WorkshopSync: the store of our copy {:08X} not asked ({})", id, WhyNotAdmitted(a_copy));
				return;
			}
			const auto seq = requests.Open(Protocol::WorkshopRequestOp::kStore, id, benchId, Clock::now());
			if (!seq) {
				Hud::Notify(Text::Get("workshop.shared.store_refused"));
				return;
			}
			Protocol::WorkshopRequest request;
			request.seq = *seq;
			request.op = Protocol::WorkshopRequestOp::kStore;
			request.workshop = benchId;
			request.target = key->second;
			PushShared(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest));
			storesAsked.fetch_add(1);
			REX::INFO("WorkshopSync: the store of our copy {:08X} of player {}'s {:08X} at workbench {:08X} asked of the host as request {} (nothing stored here)", id, key->first,
				key->second, benchId, *seq);
		}

		// Workshop::RequestStoreCallback's Run's call of Workshop::DestroyItem (STORE_ACCEPT_CALL), in both games, judged as the
		// scrap confirmation's Yes is (SharedWorkshop::OnScrapAccept): the box is modal and may have stood open a while, so a
		// reference gone meanwhile (the host's report took the copy: disabled, its handle still resolving; the host's own piece a
		// friend's request took first) is never given to the engine (it would store a piece from nothing into our own list,
		// delete it outside WorkshopPower::Remove and tell Papyrus of a reference it never registered); a copy, or a reference
		// that was one, is asked of the host while admitted, else refused with a notice; anything else is the player's own
		// piece, stored as ever. The main thread (the box's callback).
		void StoreAcceptHook(RE::Workshop::ContextData& a_context, RE::NiPointer<RE::TESObjectREFR>& a_ref, bool a_store, bool a_onlyDisconnect, bool a_quiet)
		{
			if (a_ref) {
				MainThread::Assert("WorkshopSync::StoreAcceptHook");
				const auto id = a_ref->GetFormID();
				const bool gone = a_ref->IsDisabled() || a_ref->IsDeleted();
				const bool copy = Blocked(*a_ref) || IsCopy(a_ref.get()) || InGraveyard(a_ref.get());
				switch (SharedWorkshop::OnScrapAccept(gone, copy, AdmitsCopy(*a_ref))) {
				case SharedWorkshop::ScrapAccept::kRefuseGone:
					storesRefused.fetch_add(1);
					REX::INFO("WorkshopSync: the store box's Yes on {:08X} refused: {} already (copy={}); nothing stored", id, a_ref->IsDeleted() ? "deleted" : "disabled", copy);
					Hud::Notify(Text::Get("workshop.shared.store_gone"));
					return;
				case SharedWorkshop::ScrapAccept::kRefuseCopy:
					storesRefused.fetch_add(1);
					REX::INFO("WorkshopSync: the store box's Yes on copy {:08X} refused: no longer admitted ({}, hostGone={}); nothing stored", id, WhyNotAdmitted(*a_ref), hostGone);
					Hud::Notify(Text::Get("workshop.shared.store_refused"));
					return;
				case SharedWorkshop::ScrapAccept::kAsk:
					AskStore(*a_ref);
					return;
				case SharedWorkshop::ScrapAccept::kEngine:
					break;
				}
			}
			originalDestroyItem(a_context, a_ref, a_store, a_onlyDisconnect, a_quiet);
		}

		bool IsPendingPiece(std::uint32_t a_id)
		{
			if (requests.IsPending(a_id)) {
				return true;
			}
			return std::ranges::any_of(adoptLater, [&](const auto& a_later) { return a_later.first.piece == a_id; });  // answered, not yet ours to adopt: still not ours to report
		}

		const char* PendingNoticeOf(std::uint32_t a_id)
		{
			return SharedWorkshop::PendingNotice(requests.PendingOp(a_id).value_or(Protocol::WorkshopRequestOp::kScrap));
		}

		// ---- The host's side ------------------------------------------------------------------------------

		void Answer(const Protocol::WorkshopRequest& a_request, Protocol::WorkshopAnswer::Why a_why, std::uint32_t a_made)
		{
			++(a_why == Protocol::WorkshopAnswer::Why::kApplied ? requestsServed : requestsRefused);
			REX::INFO("WorkshopSync: player {}'s request {} ({}) at workbench {:08X}: {}{}", a_request.playerId, a_request.seq,
				Protocol::OpName(a_request.op), a_request.workshop, Protocol::Name(a_why),
				a_made != 0 ? std::format(", {} {:08X}", a_request.op == Protocol::WorkshopRequestOp::kScrap ? "scrapped" : a_request.op == Protocol::WorkshopRequestOp::kStore ? "stored" : "made", a_made) : std::string{});
			PushShared(Protocol::Encode(Protocol::WorkshopAnswer{ a_request.seq, a_request.playerId, a_request.op, a_why, a_request.workshop, a_made }, Protocol::MessageType::kReportWorkshopAnswer));
		}

		// The engine's placement steps on a piece of ours, as SharedWorkshop::Finish drives them.
		struct FinishEngine
		{
			RE::TESObjectREFR& ref;
			RE::TESObjectREFR& bench;
			std::string        log;

			BenchScope Bench() { return BenchScope{ bench }; }

			void Do(SharedWorkshop::Step a_step)
			{
				using ref_t = void (*)(RE::TESObjectREFR&);
				using link_t = void (*)(RE::TESObjectREFR&, bool, RE::TESObjectREFR*);
				using furniture_t = void (*)(RE::TESObjectREFR&, bool);
				using snapped_t = void (*)(RE::Workshop::ContextData&, RE::TESObjectREFR&, RE::Workshop::PlacementItemData&);
				using clear_t = void (*)(RE::Workshop::PlacementItemData*);
				using extra_t = void (*)(RE::Workshop::ExtraData*, RE::TESObjectREFR&);
				using navmesh_t = void (*)(RE::TESObjectREFR*, bool);
				using source_t = RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>* (*)();
				static REL::Relocation<link_t>      link{ SET_IS_WORKSHOP_ITEM };
				static REL::Relocation<ref_t>       budget{ BUDGET_ADD_OBJECT };
				static REL::Relocation<ref_t>       makeStatic{ MAKE_REFERENCE_STATIC };
				static REL::Relocation<furniture_t> furniture{ SET_FURNITURE_USED };
				static REL::Relocation<snapped_t>   snapped{ CHECK_SNAPPED_POWER };
				static REL::Relocation<clear_t>     clear{ PLACEMENT_DATA_CLEAR };
				static REL::Relocation<extra_t>     addItem{ EXTRA_ADD_ITEM };
				static REL::Relocation<ref_t>       terminals{ ESTABLISH_TERMINAL_LINKS };
				static REL::Relocation<navmesh_t>   navmesh{ UPDATE_DYNAMIC_NAVMESH };
				static REL::Relocation<ref_t>       powerRating{ CALCULATE_POWER_RATING };
				static REL::Relocation<ref_t>       recount{ RECALCULATE_RESOURCES };
				static REL::Relocation<source_t>    source{ PLACED_EVENT_SOURCE };
				log += std::format("{}{}", log.empty() ? "" : " ", SharedWorkshop::Name(a_step));
				switch (a_step) {
				case SharedWorkshop::Step::kLink:
					link(ref, true, &bench);
					break;
				case SharedWorkshop::Step::kBudget: {
					BudgetScope totals{ bench };  // the bench's own totals (not another settlement's, nor 0 after a load), plus this piece
					budget(ref);
					break;
				}
				case SharedWorkshop::Step::kStatic:
					makeStatic(ref);
					break;
				case SharedWorkshop::Step::kFurniture:
					furniture(ref, false);
					break;
				case SharedWorkshop::Step::kSnap: {
					// A placement data of our own for the piece's connect points (PlacementItemData::Set reads them off the 3D):
					// the engine's memory, never run through a C++ destructor, cleared by the engine's own Clear (which frees
					// what Set made).
					const auto player = RE::PlayerCharacter::GetSingleton();
					if (!player || !player->parentCell) {
						log += "(no player cell)";
						break;
					}
					const auto memory = RE::aligned_alloc(alignof(RE::Workshop::PlacementItemData), sizeof(RE::Workshop::PlacementItemData));
					if (!memory) {
						log += "(no memory)";
						break;
					}
					std::memset(memory, 0, sizeof(RE::Workshop::PlacementItemData));
					const auto data = static_cast<RE::Workshop::PlacementItemData*>(memory);
					RE::Workshop::ContextData context{ player };
					data->Set(ref);
					snapped(context, ref, *data);
					clear(data);
					RE::aligned_free(memory);
					break;
				}
				case SharedWorkshop::Step::kAddItem:
					if (const auto data = WorkshopDataOf(&bench)) {
						addItem(data, ref);
					} else {
						log += "(no workshop data)";
					}
					break;
				case SharedWorkshop::Step::kTerminals:
					terminals(ref);
					break;
				case SharedWorkshop::Step::kNavmesh:
					navmesh(&ref, true);
					break;
				case SharedWorkshop::Step::kPower:
					powerRating(bench);
					break;
				case SharedWorkshop::Step::kResources:
					recount(bench);
					break;
				case SharedWorkshop::Step::kEvent:
					if (const auto events = source()) {
						RE::Workshop::ItemPlacedEvent event{ RE::NiPointer<RE::TESObjectREFR>{ &bench }, RE::NiPointer<RE::TESObjectREFR>{ &ref } };
						events->Notify(event);
					} else {
						log += "(no event source)";
					}
					break;
				}
			}
		};

		// Finishes a piece of ours at the workbench with the engine's steps; what ran.
		std::string FinishPiece(RE::TESObjectREFR& a_ref, RE::TESObjectREFR& a_bench)
		{
			// With our own workshop mode open, what it holds before and after the finish (said once per finish): the host's ghost
			// must come out of a friend's build as it went in (the same piece, still new, still placeable where it stood).
			const bool menu = MenuWatch::IsOpen("WorkshopMenu"sv);
			const auto before = PlacementNow();
			std::size_t done = 0;
			FinishEngine engine{ a_ref, a_bench };
			{
				NewItemScope newItem;  // a new piece: its scripts started, its loaded event sent, its power's initial state set
				done = SharedWorkshop::Finish(engine);
			}
			finishSteps += static_cast<std::uint32_t>(done);
			++finishedHere;
			if (menu) {
				const auto after = PlacementNow();
				const bool same = after.handle == before.handle && after.newItem == before.newItem && after.editing == before.editing && after.canPlace == before.canPlace;
				REX::INFO("WorkshopSync: {:08X} finished while our workshop mode is open: its placement item {} (handle {:08X} -> {:08X}, newItem {} -> {}, editing {} -> {}, canPlace {} -> {})",
					a_ref.GetFormID(), same ? "unchanged" : "CHANGED", before.handle, after.handle, before.newItem, after.newItem, before.editing, after.editing, before.canPlace,
					after.canPlace);
			}
			return engine.log;
		}

		// A wire between two of our pieces, strung with SharedWorkshop::StringWire's steps: as wswire strings one (SplineUtils::
		// ConnectSpline, Workshop::ExtraData::AddConnection, the bench link, the spline rebuilt), plus the wire static, the bench's
		// totals and the placed event. Nothing after the connection decides an end's power again (SharedWorkshop.h, WireStep).
		// The engine's own wiring (Workshop::PlaceCurrentReference) does run PowerUtils::UpdateMovingWirelessItem on both ends
		// after AddConnection, but inside workshop mode; this path runs outside it, where nothing re-evaluates the light
		// afterwards. The first version made that call here, and on the rig it unpowered the light the friend wired (out of the
		// grid's powered set, PowerOff fired, the wired edge kept): don't add it back for parity. Its shape is reported once
		// drawn (the moved-wire check), as a moved piece's wires are.
		struct WireEngine
		{
			RE::TESObjectREFR&       from;
			RE::TESObjectREFR&       to;
			RE::TESObjectREFR&       bench;
			RE::TESObjectREFR&       wire;
			RE::Workshop::ExtraData& data;
			std::string              log;

			BenchScope Bench() { return BenchScope{ bench }; }

			void Do(SharedWorkshop::WireStep a_step)
			{
				using add_t = void (*)(RE::Workshop::ExtraData*, RE::TESObjectREFR&, RE::TESObjectREFR&, RE::TESObjectREFR*);
				using ref_t = void (*)(RE::TESObjectREFR&);
				using link_t = void (*)(RE::TESObjectREFR&, bool, RE::TESObjectREFR*);
				using source_t = RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>* (*)();
				static REL::Relocation<add_t>    addConnection{ ADD_CONNECTION };
				static REL::Relocation<ref_t>    makeStatic{ MAKE_REFERENCE_STATIC };
				static REL::Relocation<link_t>   link{ SET_IS_WORKSHOP_ITEM };
				static REL::Relocation<source_t> source{ PLACED_EVENT_SOURCE };
				log += std::format("{}{}", log.empty() ? "" : " ", SharedWorkshop::Name(a_step));
				switch (a_step) {
				case SharedWorkshop::WireStep::kLink:
					link(wire, true, &bench);  // as the engine's wiring links a wire: the bench link and the location's special ref
					break;
				case SharedWorkshop::WireStep::kSpline:
					RE::SplineUtils::ConnectSpline(&from, 0, &to, 0, &wire);
					break;
				case SharedWorkshop::WireStep::kConnect:
					addConnection(&data, from, to, &wire);  // the grids merged and evaluated: the ends' power decided here
					break;
				case SharedWorkshop::WireStep::kCount:
					// Counted in the budget once its spline is drawn (CountWiresLater): the engine counts a placed wire's drawn 3D, and
					// ours has none yet (a count of 0 now, the real count off at a later scrap, would drift the budget down).
					wiresToCount.push_back({ wire.GetHandle(), bench.GetFormID(), Clock::now() });
					break;
				case SharedWorkshop::WireStep::kRebuild:
					if (const auto tasks = RE::TaskQueueInterface::GetSingleton()) {
						tasks->QueueRebuildBendableSpline(wire, false, nullptr);
					}
					break;
				case SharedWorkshop::WireStep::kStatic:
					makeStatic(wire);
					break;
				case SharedWorkshop::WireStep::kResources:
					RecountBench(bench);
					break;
				case SharedWorkshop::WireStep::kEvent:
					if (const auto events = source()) {
						RE::Workshop::ItemPlacedEvent event{ RE::NiPointer<RE::TESObjectREFR>{ &bench }, RE::NiPointer<RE::TESObjectREFR>{ &wire } };
						events->Notify(event);
					} else {
						log += "(no event source)";
					}
					break;
				case SharedWorkshop::WireStep::kEndWireless:
					break;  // never in WIRE_STEPS: it would undo the power the connection gave the ends
				}
			}
		};

		RE::TESObjectREFR* MakeWire(RE::TESObjectREFR& a_from, RE::TESObjectREFR& a_to, RE::TESObjectREFR& a_bench)
		{
			const auto data = WorkshopDataOf(&a_bench);
			const auto spline = RE::TESForm::GetFormByID<RE::TESBoundObject>(WIRE_BASE);
			const auto cell = a_from.GetParentCell();
			if (!data || !spline || !cell) {
				return nullptr;
			}
			RE::NEW_REFR_DATA place;
			place.location = a_from.data.location;
			place.direction = a_from.data.angle;
			place.object = spline;
			place.interior = cell->IsInterior() ? cell : nullptr;
			place.world = cell->IsInterior() ? nullptr : cell->worldSpace;
			place.clearStillLoadingFlag = true;
			const auto handle = EngineRefs::CreateReferenceAtLocation(place);
			const auto wire = handle.get();
			if (!wire) {
				return nullptr;
			}
			{
				NewItemScope newItem;  // a new piece for MakeReferenceStatic
				WireEngine   engine{ a_from, a_to, a_bench, *wire, *data, {} };
				SharedWorkshop::StringWire(engine);  // the bench set as workshop mode's current one for every step
			}
			CheckWiresLater(&a_from, &a_bench);  // reported again with its shape once drawn
			++wiresMadeHere;
			return wire.get();
		}

		// Whether a wire can end on this piece, read as the engine's wire filter reads it (SharedWorkshop::TakesWire).
		bool TakesWire(const RE::TESObjectREFR& a_ref)
		{
			static REL::Relocation<RE::BGSKeyword**> keyword{ POWER_KEYWORD };
			const auto                               values = RE::ActorValue::GetSingleton();
			const auto                               info = values ? values->workshopPowerConnection : nullptr;
			const float                              connection = info ? a_ref.GetActorValue(*info) : 0.0f;
			return SharedWorkshop::TakesWire(connection, *keyword && static_cast<const RE::IKeywordFormBase&>(a_ref).HasKeyword(*keyword));
		}

		// What the host's game finds of a request (SharedWorkshop::Refusal looks in this order).
		SharedWorkshop::Facts FactsOf(const Protocol::WorkshopRequest& a_request)
		{
			SharedWorkshop::Facts facts;
			const auto            bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.workshop);
			facts.benchKnown = IsWorkbenchAnchor(bench);
			facts.hostOwned = facts.benchKnown && OwnBench(bench);
			facts.benchOpened = facts.benchKnown && WorkshopDataOf(bench) != nullptr;  // made by the engine when the workshop is first opened; saved with the bench
			facts.inWorkshopMode = MenuWatch::IsOpen("WorkshopMenu"sv);
			Protocol::WorkshopItem spot;
			spot.workshop = a_request.workshop;
			if (a_request.op == Protocol::WorkshopRequestOp::kWire) {
				const auto from = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.wireFrom);
				const auto to = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.wireTo);
				facts.endsKnown = from && to && !from->IsDeleted() && !to->IsDeleted() && !from->IsDisabled() && !to->IsDisabled();
				facts.endsAtBench = facts.endsKnown && bench && BenchOf(*from) == bench && BenchOf(*to) == bench && !IsWire(from->GetObjectReference()) &&
				                    !IsWire(to->GetObjectReference()) && !IsCopy(from) && !IsCopy(to) && from != to;
				// The vanilla rules workshop mode's own wiring keeps (its wire filter): both ends take a wire, and no wire joins them
				// yet. Read again when the wire is made: two requests for one wire both pass here before either is strung.
				facts.endsTakeWire = facts.endsAtBench && TakesWire(*from) && TakesWire(*to);
				facts.alreadyWired = facts.endsAtBench && SharedWorkshop::Joined(GridsOf(bench), from->GetFormID(), to->GetFormID());
				if (facts.endsKnown) {
					spot.position[0] = from->data.location.x;
					spot.position[1] = from->data.location.y;
					spot.position[2] = from->data.location.z;
				}
			} else if (a_request.op == Protocol::WorkshopRequestOp::kScrap) {
				const auto target = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.target);
				facts.editAllowed = EditAllowed();
				// Workshop mode open here at another workbench: the engine's scrap would fill its build list from this one (Refusal).
				facts.otherBenchOpen = facts.inWorkshopMode && OpenBench() != 0 && OpenBench() != a_request.workshop;
				facts.targetKnown = target && !target->IsDeleted() && !target->IsDisabled();
				// One of this workbench's own pieces: linked to it, not a copy of a friend's (the friend's game scraps those), not the
				// workbench itself (never a piece: ReportRef), and not a wire (ScrapReference's spline branches are untried from here;
				// the friend's Scrap key refuses a copy of a wire with a notice before it asks).
				facts.targetAtBench = facts.targetKnown && bench && target != bench && BenchOf(*target) == bench && !IsCopy(target) && !IsWorkbenchAnchor(target) &&
				                      !IsWire(target->GetObjectReference());
				// The engine's own scrap test, as the friend's key ran it on the copy (WorkshopCanScrapRef with false, the Scrap key's
				// flag): a settler or a companion assigned here is linked to the bench like a piece, and only a crafted or mistranslated
				// request names one (copies have keys, actors have none), so no actor passes, whatever the test says.
				facts.targetScrappable = facts.targetAtBench && !target->As<RE::Actor>() && CanScrap(*target);
				facts.targetLoaded = facts.targetKnown && target->Get3D() != nullptr;
				facts.targetHeld = facts.targetKnown && Held(*target);
				if (facts.targetKnown) {
					spot.position[0] = target->data.location.x;
					spot.position[1] = target->data.location.y;
					spot.position[2] = target->data.location.z;
				}
			} else if (a_request.op == Protocol::WorkshopRequestOp::kStore) {
				const auto target = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.target);
				facts.editAllowed = EditAllowed();
				// As for a scrap: workshop mode open here at another workbench, the engine's store would fill its build list from this one.
				facts.otherBenchOpen = facts.inWorkshopMode && OpenBench() != 0 && OpenBench() != a_request.workshop;
				facts.targetKnown = target && !target->IsDeleted() && !target->IsDisabled();
				facts.targetAtBench = facts.targetKnown && bench && target != bench && BenchOf(*target) == bench && !IsCopy(target) && !Blocked(*target) && !IsWorkbenchAnchor(target) &&
				                      !IsWire(target->GetObjectReference());
				// What the engine's own Store refuses (its test on the piece, Workshop::CanStoreRef: a settler or a companion linked to the
				// bench, a pre-placed piece), a request never does to it; and no living actor, whatever the test says.
				facts.targetStorable = facts.targetAtBench && !target->As<RE::Actor>() && EngineCanStore(*target);
				facts.targetLoaded = facts.targetKnown && target->Get3D() != nullptr;  // FinishDestroyItem takes a piece off the grids only with its 3D in
				facts.targetHeld = facts.targetKnown && Held(*target);
				if (facts.targetKnown) {
					spot.position[0] = target->data.location.x;
					spot.position[1] = target->data.location.y;
					spot.position[2] = target->data.location.z;
				}
			} else {
				const auto base = RE::TESForm::GetFormByID<RE::TESBoundObject>(a_request.base);
				facts.objectKnown = base != nullptr;
				facts.buildable = base && Buildable(base) && WorkshopApply::Shared(true, Equipment::IsPowerArmorFrame(base)) && !base->As<RE::TESNPC>();
				std::copy(a_request.position.begin(), a_request.position.end(), spot.position);
				facts.inReach = InReach(spot);
			}
			EngineWorld world;
			facts.benchLoaded = facts.benchKnown && world.Loaded(spot);
			return facts;
		}

		// The engine's own scrap of our piece (Workshop::ScrapReference, 1.11.240: it disables the piece, takes it off the
		// workbench's grids and destroys the wires strung to it, clears its snaps and stacked children, puts the yield in the
		// workbench, unlinks it, recounts the bench, takes it out of the budget, notifies ItemDestroyedEvent and deletes it), as
		// SharedWorkshop::SCRAP_STEPS has it: inside the bench scope (the yield's container, the grids and the recount are
		// workshop mode's current workbench's) and the budget scope (Budget::RemoveObject writes the running totals to the
		// bench), with a null rewards pointer (the engine works the yield out from the piece's recipe, as workshop mode's own
		// scrap does: half its components, floored, more with Scrapper, from this game's perks). Its destroyed events run our
		// DestroyedSink inside the call: the piece and its wires are reported scrapped, so the friends' copies go. What ran.
		std::string ScrapPiece(RE::TESObjectREFR& a_ref, RE::TESObjectREFR& a_bench)
		{
			static REL::Relocation<ScrapFn> scrap{ SCRAP_REFERENCE };
			const auto                      player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->parentCell) {
				return "(no player cell)";
			}
			std::string log;
			{
				BenchScope bench{ a_bench };
				log += SharedWorkshop::Name(SharedWorkshop::ScrapStep::kBench);
				BudgetScope totals{ a_bench };
				log += std::format(" {}", SharedWorkshop::Name(SharedWorkshop::ScrapStep::kBudget));
				RE::Workshop::ContextData        context{ player };
				RE::NiPointer<RE::TESObjectREFR> target{ &a_ref };
				scrap(context, target, nullptr);
				log += std::format(" {}", SharedWorkshop::Name(SharedWorkshop::ScrapStep::kScrap));
			}
			gridNamedFrame = ~0ull;  // the grids changed
			++scrapsServed;
			return log;
		}

		// A friend's scrap of one of our pieces: checked again now (the piece may have gone, or be in the host's hands since the
		// request came), then the engine's own scrap of it, and answered with the piece's id (what the friend's copy is of).
		void ServeScrap(const Protocol::WorkshopRequest& a_request)
		{
			const auto facts = FactsOf(a_request);
			if (const auto why = SharedWorkshop::Refusal(a_request.op, facts)) {
				Answer(a_request, *why, 0);
				return;
			}
			const auto target = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.target);
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.workshop);
			if (!target || !bench) {
				Answer(a_request, Protocol::WorkshopAnswer::Why::kNoSuchPiece, 0);
				return;
			}
			const auto base = target->GetObjectReference();
			const auto steps = ScrapPiece(*target, *bench);
			const bool gone = target->IsDisabled() || target->IsDeleted();
			REX::INFO("WorkshopSync: our {:08X} (base {:08X}) at workbench {:08X} scrapped for player {}'s request {}: {}{}", a_request.target, base ? base->GetFormID() : 0, a_request.workshop,
				a_request.playerId, a_request.seq, steps, gone ? "" : " (the engine left it standing)");
			Answer(a_request, gone ? Protocol::WorkshopAnswer::Why::kApplied : Protocol::WorkshopAnswer::Why::kFailed, gone ? a_request.target : 0);
		}

		// How many of this base the workbench's stored list holds (what the build menu shows as stored, placed again free).
		std::uint32_t StoredCount(const RE::TESObjectREFR& a_bench, std::uint32_t a_base)
		{
			const auto data = WorkshopDataOf(&a_bench);
			if (!data || a_base == 0) {
				return 0;
			}
			std::uint32_t count = 0;
			for (const auto entry : data->deletedItems) {
				if (entry && entry->formID == a_base) {
					count += entry->count;
				}
			}
			return count;
		}

		// The engine's own store of our piece (Workshop::DestroyItem with its store flag, 1.11.240: it adds the piece's base to the
		// current workbench's stored list, takes the piece off the grids and destroys the wires strung to it, puts a container's
		// contents in the workbench, unlinks it, recounts the bench, notifies ItemDestroyedEvent, takes it out of the budget and
		// deletes it), as SharedWorkshop::STORE_STEPS has it: inside the bench scope (the stored list, the grids and the recount
		// are workshop mode's current workbench's) and the budget scope, quiet (the sound is the friend's). Its destroyed events
		// run our DestroyedSink inside the call: the piece and its wires are reported scrapped, so the friends' copies go. Before
		// it, as the engine's own store does before the same call: a terminal's holotape is taken out and given to our player
		// (FIND_AND_REMOVE_HOLOTAPE; nothing for any other piece), or it would be deleted with the terminal. What ran.
		std::string StorePiece(RE::TESObjectREFR& a_ref, RE::TESObjectREFR& a_bench)
		{
			using EjectFn = void (*)(RE::TESObjectREFR&);
			static REL::Relocation<DestroyFn> destroy{ DESTROY_ITEM };
			static REL::Relocation<EjectFn>   ejectHolotape{ FIND_AND_REMOVE_HOLOTAPE };
			const auto                        player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->parentCell) {
				return "(no player cell)";
			}
			std::string log;
			{
				BenchScope bench{ a_bench };
				log += SharedWorkshop::Name(SharedWorkshop::StoreStep::kBench);
				BudgetScope totals{ a_bench };
				log += std::format(" {}", SharedWorkshop::Name(SharedWorkshop::StoreStep::kBudget));
				RE::Workshop::ContextData        context{ player };
				RE::NiPointer<RE::TESObjectREFR> target{ &a_ref };  // held across both calls
				ejectHolotape(a_ref);
				log += std::format(" {}", SharedWorkshop::Name(SharedWorkshop::StoreStep::kHolotape));
				destroy(context, target, true, false, true);
				log += std::format(" {}", SharedWorkshop::Name(SharedWorkshop::StoreStep::kStore));
			}
			gridNamedFrame = ~0ull;  // the grids changed
			++storesServed;
			return log;
		}

		// A friend's store of one of our pieces: checked again now (the piece may have gone, or be in the host's hands since the
		// request came; the engine's own store test asked of it), then the engine's own store of it, and answered with the piece's
		// id (what the friend's copy is of). Done when the destroyed event named the piece (the engine deletes a created piece
		// deferred, so disabled or deleted alone would miss it); the stored list's count of its base is logged before and after.
		void ServeStore(const Protocol::WorkshopRequest& a_request)
		{
			const auto facts = FactsOf(a_request);
			if (const auto why = SharedWorkshop::Refusal(a_request.op, facts)) {
				Answer(a_request, *why, 0);
				return;
			}
			const auto target = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.target);
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_request.workshop);
			if (!target || !bench) {
				Answer(a_request, Protocol::WorkshopAnswer::Why::kNoSuchPiece, 0);
				return;
			}
			const auto base = target->GetObjectReference();
			const auto baseId = base ? base->GetFormID() : 0;
			const auto before = StoredCount(*bench, baseId);
			destroyedHere = 0;
			const auto steps = StorePiece(*target, *bench);
			const auto after = StoredCount(*bench, baseId);
			const bool gone = destroyedHere == a_request.target || target->IsDisabled() || target->IsDeleted();
			REX::INFO("WorkshopSync: our {:08X} (base {:08X}) at workbench {:08X} stored for player {}'s request {}: {} | stored {} -> {}{}", a_request.target, baseId, a_request.workshop,
				a_request.playerId, a_request.seq, steps, before, after, gone ? "" : " (the engine left it standing)");
			Answer(a_request, gone ? Protocol::WorkshopAnswer::Why::kApplied : Protocol::WorkshopAnswer::Why::kFailed, gone ? a_request.target : 0);
		}

		// A build or a wire waiting to be made (workshop mode open here, or the piece's 3D not there yet), or a scrap or a store
		// waiting to be done: made and answered when it may be, refused when it waited too long.
		void MakeRequests(Clock::time_point a_now)
		{
			if (making.empty() || answersHeld) {  // wsmode hold: taken, not made or answered yet
				return;
			}
			// The host building in workshop mode is the point: nothing of its workshop mode holds a finish off (the finish sets
			// the engine's new-item byte itself: NewItemScope).
			std::erase_if(making, [&](Making& a_making) {
				const auto& request = a_making.request;
				if (request.op == Protocol::WorkshopRequestOp::kScrap) {
					ServeScrap(request);  // one engine call, now (out of the request's handler)
					return true;
				}
				if (request.op == Protocol::WorkshopRequestOp::kStore) {
					ServeStore(request);
					return true;
				}
				const bool  wire = request.op == Protocol::WorkshopRequestOp::kWire;
				const auto  ref = wire ? RE::NiPointer<RE::TESObjectREFR>{} : a_making.ref.get();
				if (!wire && !ref) {
					Answer(request, Protocol::WorkshopAnswer::Why::kFailed, 0);
					return true;
				}
				switch (SharedWorkshop::Ready(wire || ref->Get3D() != nullptr, a_now - a_making.since)) {
				case SharedWorkshop::Readiness::kWait:
					return false;
				case SharedWorkshop::Readiness::kNoModel:
					if (ref) {
						ref->Disable();
						ref->SetDelete(true);
					}
					Answer(request, Protocol::WorkshopAnswer::Why::kFailed, 0);
					return true;
				case SharedWorkshop::Readiness::kReady:
					break;
				}
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(request.workshop);
				if (!bench) {
					if (ref) {
						ref->Disable();
						ref->SetDelete(true);
					}
					Answer(request, Protocol::WorkshopAnswer::Why::kBenchNotLoaded, 0);
					return true;
				}
				if (wire) {
					const auto facts = FactsOf(request);
					if (const auto why = SharedWorkshop::Refusal(request.op, facts)) {
						Answer(request, *why, 0);
						return true;
					}
					const auto made = MakeWire(*RE::TESForm::GetFormByID<RE::TESObjectREFR>(request.wireFrom), *RE::TESForm::GetFormByID<RE::TESObjectREFR>(request.wireTo), *bench);
					Answer(request, made ? Protocol::WorkshopAnswer::Why::kApplied : Protocol::WorkshopAnswer::Why::kFailed, made ? made->GetFormID() : 0);
					return true;
				}
				const auto steps = FinishPiece(*ref, *bench);
				REX::INFO("WorkshopSync: {:08X} (base {:08X}) built for player {} at workbench {:08X}: {}", ref->GetFormID(), request.base, request.playerId, request.workshop, steps);
				Answer(request, Protocol::WorkshopAnswer::Why::kApplied, ref->GetFormID());
				return true;
			});
		}

		// What a 3D holds, for the wire count's log: its root's name and type, its geometries, and how many of those (and of the
		// nodes) are culled (NiAVObject's app-culled flag, which Budget::AddObject's walk skips).
		std::string Describe3D(RE::NiAVObject* a_root)
		{
			if (!a_root) {
				return "no 3D";
			}
			std::size_t nodes = 0, geometries = 0, culledNodes = 0, culledGeometries = 0;
			std::vector<RE::NiAVObject*> todo{ a_root };
			while (!todo.empty() && nodes + geometries < 4096) {
				const auto object = todo.back();
				todo.pop_back();
				if (object->IsGeometry()) {
					++geometries;
					culledGeometries += object->GetAppCulled();
				} else {
					++nodes;
					culledNodes += object->GetAppCulled();
				}
				if (const auto node = object->IsNode()) {
					for (const auto& child : node->children) {  // the array runs to its capacity: empty slots are null
						if (child) {
							todo.push_back(child.get());
						}
					}
				}
			}
			return std::format("root '{}' ({}) culled={}: {} nodes ({} culled), {} geometries ({} culled), bound radius {:.0f}", a_root->name.c_str() ? a_root->name.c_str() : "",
				a_root->GetRTTI() && a_root->GetRTTI()->GetName() ? a_root->GetRTTI()->GetName() : "?", a_root->GetAppCulled(), nodes, culledNodes, geometries, culledGeometries,
				a_root->worldBound.fRadius);
		}

		// The host's wires made for friends, counted in the budget once their spline is drawn (WIRE_SETTLE after they were
		// made, as the moved-wire check waits). Counted only when the budget's AddObject added something (its walk counts the
		// 3D's drawn geometry: a spline not built yet adds 0), else tried again each frame until WIRE_COUNT_GIVE_UP, then let
		// be with a warning that says what the 3D held (wiresUncounted).
		void CountWiresLater(Clock::time_point a_now)
		{
			if (wiresToCount.empty()) {
				return;
			}
			using ref_t = void (*)(RE::TESObjectREFR&);
			static REL::Relocation<ref_t>          budget{ BUDGET_ADD_OBJECT };
			static REL::Relocation<std::uint32_t*> triangles{ BUDGET_TRIANGLES };
			static REL::Relocation<std::uint32_t*> draws{ BUDGET_DRAWS };
			std::erase_if(wiresToCount, [&](const WireToCount& a_wire) {
				if (a_now - a_wire.since < WIRE_SETTLE) {
					return false;
				}
				const auto wire = a_wire.wire.get();
				const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_wire.bench);
				if (!wire || wire->IsDeleted() || !bench) {
					return true;
				}
				const bool giveUp = a_now - a_wire.since >= WIRE_COUNT_GIVE_UP;
				const auto root = wire->Get3D();
				if (!root) {
					if (giveUp) {
						++wiresUncounted;
						REX::WARN("WorkshopSync: our wire {:08X} for a friend has no 3D after {} s: not counted in workbench {:08X}'s budget", wire->GetFormID(),
							std::chrono::duration_cast<std::chrono::seconds>(WIRE_COUNT_GIVE_UP).count(), a_wire.bench);
					}
					return giveUp;
				}
				std::int64_t addedTriangles = 0, addedDraws = 0;
				{
					BudgetScope totals{ *bench };
					const auto  t0 = *triangles;
					const auto  d0 = *draws;
					budget(*wire);
					addedTriangles = static_cast<std::int64_t>(*triangles) - t0;
					addedDraws = static_cast<std::int64_t>(*draws) - d0;
				}
				if (addedTriangles == 0 && addedDraws == 0) {
					if (giveUp) {
						++wiresUncounted;
						REX::WARN("WorkshopSync: our wire {:08X} for a friend added nothing to workbench {:08X}'s budget in {} s (the budget's walk found no drawn geometry): {}",
							wire->GetFormID(), a_wire.bench, std::chrono::duration_cast<std::chrono::seconds>(WIRE_COUNT_GIVE_UP).count(), Describe3D(root));
					}
					return giveUp;
				}
				RecountBench(*bench);  // the power rating once more with the wire drawn (the rig saw the light powered late)
				++wiresCounted;
				REX::INFO("WorkshopSync: our wire {:08X} for a friend counted in workbench {:08X}'s budget after {} ms: triangles {:+}, draws {:+} ({})", wire->GetFormID(), a_wire.bench,
					std::chrono::duration_cast<std::chrono::milliseconds>(a_now - a_wire.since).count(), addedTriangles, addedDraws, Describe3D(root));
				return true;
			});
		}

		// Every frame, in a session: the host's requests made, ours expired.
		void SharedFrame(Clock::time_point a_now, bool a_host)
		{
			hosting = a_host;
			admitsCopies.store(HandsOver());
			if (Serves()) {
				MakeRequests(a_now);
				CountWiresLater(a_now);
			} else if (!making.empty()) {
				for (auto& item : making) {
					if (const auto ref = item.ref.get()) {
						ref->Disable();
						ref->SetDelete(true);
					}
				}
				making.clear();
			}
			for (const auto& [seq, pending] : requests.Expire(a_now)) {
				++answersLate;
				REX::WARN("WorkshopSync: request {} at workbench {:08X} got no answer in {} s (dropped on the way, or the host's game stuck): taken as refused", seq, pending.bench,
					std::chrono::duration_cast<std::chrono::seconds>(SharedWorkshop::ANSWER_WAIT).count());
				Answered(seq, pending.op, Protocol::WorkshopAnswer::Why::kNoAnswer, pending, {});
			}
			// Builds answered while the player held the piece: adopted once it is put down (or gone).
			std::erase_if(adoptLater, [&](const auto& a_later) { return Adopt(a_later.first, a_later.second); });
		}

		// No grid of ours may name an in-session piece of ours that became the host's (adopted) or went (a wire taken off): one
		// that does is taken off again, and said. With the copy audit, before every save and every few seconds.
		void AuditStrays(std::string_view a_when)
		{
			if (madeHere.empty()) {
				return;
			}
			PowerEngine engine;
			const auto  named = SharedWorkshop::Strays(engine.Grids(), madeHere);
			for (const auto id : named) {
				++straysFound;
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
				REX::WARN("WorkshopSync: {}: a grid of ours still names {:08X}, our piece the host's game took over (or our wire taken off): taken off again{}", a_when, id,
					ref ? "" : " (no reference under its ID now)");
				if (ref) {
					Unbuild(*ref, BenchOf(*ref), false);  // its budget was settled when it was adopted or taken off
				}
			}
		}

		// A piece of ours still waiting for the host's answer goes (a save, or leaving): told to its workbench's scripts as a
		// scrap (its settler freed), off our workshop and budget, and deleted.
		void DropPending(const SharedWorkshop::Pending& a_pending, const char* a_when)
		{
			const RE::NiPointer<RE::TESObjectREFR> ref{ RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_pending.piece) };
			if (!ref || ref->IsDeleted() || IsCopy(ref.get())) {
				return;
			}
			const auto bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_pending.bench);
			NotifyDestroyed(bench ? bench : BenchOf(*ref), *ref);
			Unbuild(*ref, bench ? bench : BenchOf(*ref), true);
			ref->Disable();
			ref->SetDelete(true);
			REX::INFO("WorkshopSync: {}: our {:08X}, still waiting for the host's answer, goes (the host's copy comes if it was built)", a_when, a_pending.piece);
		}

		// Leaving the session: our pieces still waiting for an answer go (nothing we built at the host's settlements stays in
		// our game), and the host's unfinished pieces for friends go too.
		void ResetShared()
		{
			for (const auto& build : requests.Builds()) {
				DropPending(build, "leaving");
			}
			for (const auto& later : adoptLater) {
				DropPending(later.first, "leaving");
			}
			adoptLater.clear();
			wiresToCount.clear();
			requests.Clear();
			taught.Clear();
			{
				std::scoped_lock l{ copyIdsLock };
				notOwnedBenches.clear();
			}
			madeHere.clear();
			for (auto& item : making) {
				if (const auto ref = item.ref.get()) {
					ref->Disable();
					ref->SetDelete(true);
				}
			}
			making.clear();
			unopenedTold.clear();
			// The dev hold (wsmode hold) ends with the session: a run aborted while holding must not leave this game taking
			// every friend's request in the next session and letting it time out as refused.
			if (answersHeld) {
				REX::INFO("WorkshopSync: leaving the session: the held answers (wsmode hold) are served again");
				answersHeld = false;
			}
			hostGone = false;
			hostId.store(0);
			admitsCopies.store(false);
		}

		// A save was loaded, or the world is gone: what waited was the old world's.
		void ForgetShared()
		{
			requests.Clear();
			adoptLater.clear();
			making.clear();
			madeHere.clear();
		}

		// A save is about to be written (kPreSaveGame, the main thread): nothing half-made may go into it. The host's pieces
		// made for friends and not finished yet are finished now if their 3D is there, else deleted and refused; a friend's
		// pieces still waiting for the host's answer go (nothing a friend built at the host's settlement stays in their save;
		// an answer "applied" after this finds ours gone and lets the host's copy come on its own, a refusal finds nothing to
		// keep). Both windows are short (a 3D is there within a frame or two, an answer within a round trip), so this is
		// rarely more than nothing.
		void SharedBeforeSave()
		{
			std::erase_if(making, [&](Making& a_making) {
				const auto& request = a_making.request;
				if (request.op == Protocol::WorkshopRequestOp::kScrap) {
					// Nothing half done to finish: the piece stands as it did. The engine's scrap (a Papyrus event, a deferred delete,
					// havok) is not run inside the pre-save hook; the friend presses again.
					REX::INFO("WorkshopSync: save: player {}'s request {} (scrap of {:08X}) refused as busy rather than scrapped inside the save", request.playerId, request.seq, request.target);
					Answer(request, Protocol::WorkshopAnswer::Why::kHostBusy, 0);
					return true;
				}
				if (request.op == Protocol::WorkshopRequestOp::kStore) {
					// Not the engine's store (a Papyrus event, a deferred delete, havok) inside the save hook: a pending store has no
					// half state, so it is refused and the friend presses again.
					Answer(request, Protocol::WorkshopAnswer::Why::kHostBusy, 0);
					return true;
				}
				const auto  ref = a_making.ref.get();
				const auto  bench = RE::TESForm::GetFormByID<RE::TESObjectREFR>(request.workshop);
				if (request.op == Protocol::WorkshopRequestOp::kBuild && ref && bench && ref->Get3D()) {
					const auto steps = FinishPiece(*ref, *bench);
					REX::INFO("WorkshopSync: save: {:08X} built for player {} finished before it: {}", ref->GetFormID(), request.playerId, steps);
					Answer(request, Protocol::WorkshopAnswer::Why::kApplied, ref->GetFormID());
					return true;
				}
				if (ref) {
					ref->Disable();
					ref->SetDelete(true);
				}
				Answer(request, Protocol::WorkshopAnswer::Why::kFailed, 0);
				return true;
			});
			// Marked dropped, not taken out: the host may have built them already, and its answer must still be matched (the
			// host's copy then comes on its own). They no longer count as waiting (wsmode pending=; dropped= until answered).
			for (const auto& build : requests.DropBuilds()) {
				DropPending(build, "save");
			}
			std::erase_if(adoptLater, [&](const auto& a_later) { return Adopt(a_later.first, a_later.second); });
			for (const auto& later : adoptLater) {
				DropPending(later.first, "save");  // still held: gone rather than saved half ours
			}
			adoptLater.clear();
		}
	}

	void InstallPicks()
	{
		if (picksInstalled) {
			return;
		}
		struct Site
		{
			REL::ID       function;
			std::size_t   offset;
			REL::ID       target;
			void*         hook;
			void**        original;  // where the call's own target goes (the first site of each hook)
			const char*   what;
		};
		const auto at = [](auto& a_original) { return reinterpret_cast<void**>(&a_original); };
		const std::array<Site, 11> sites{ {
			{ PICK_GENERAL, PICK_BLOCKED_CALL, IS_INTERACTION_BLOCKED, reinterpret_cast<void*>(&PickBlockedHook), at(originalIsBlocked), "the general pick's test" },
			{ PICK_WIRE, PICK_BLOCKED_CALL, IS_INTERACTION_BLOCKED, reinterpret_cast<void*>(&PickBlockedHook), at(originalIsBlocked), "the wire pick's test" },
			{ CAN_STORE_PLACEMENT_ITEM, CAN_STORE_CALL, CAN_STORE_REF, reinterpret_cast<void*>(&CanStoreHook), at(originalCanStore), "the store test" },
			{ REQUEST_STORE_PLACEMENT_ITEM, REQUEST_STORE_CALL, CAN_STORE_REF, reinterpret_cast<void*>(&CanStoreHook), at(originalCanStore), "the store request's test" },
			{ CAN_SCRAP_PLACEMENT_REF, CAN_SCRAP_CALL, CAN_SCRAP_REF, reinterpret_cast<void*>(&CanScrapHook), at(originalCanScrap), "the scrap button" },
			{ REQUEST_SCRAP_PLACEMENT_ITEM, REQUEST_SCRAP_CALL, CAN_SCRAP_REF, reinterpret_cast<void*>(&ScrapKeyHook), at(originalCanScrap), "the scrap key" },
			{ CAN_SELECT_PLACEMENT_REF, CAN_SELECT_CALL, WORKSHOP_CAN_SELECT_REF, reinterpret_cast<void*>(&CanSelectHook), at(originalCanSelect), "the move button" },
			{ WORKSHOP_MENU_EVENT, MENU_SELECT_CALL, WORKSHOP_CAN_SELECT_REF, reinterpret_cast<void*>(&SelectKeyHook), at(originalCanSelect), "the menu's Accept (the grab)" },
			{ WORKSHOP_MENU_EVENT, MENU_STORE_CALL, CAN_STORE_PLACEMENT_ITEM, reinterpret_cast<void*>(&StoreKeyHook), at(originalCanStoreItem), "the menu's Store key" },
			{ SCRAP_ACCEPT, SCRAP_ACCEPT_CALL, SCRAP_REFERENCE, reinterpret_cast<void*>(&ScrapAcceptHook), at(originalScrapReference), "the scrap confirmation's Yes" },
			{ STORE_ACCEPT, STORE_ACCEPT_CALL, DESTROY_ITEM, reinterpret_cast<void*>(&StoreAcceptHook), at(originalDestroyItem), "the store box's Yes" },
		} };
		// Every site as 1.11.240 has it (a call to the function the plugin expects), or none is patched: the hooks only make
		// sense together (a copy picked but not refused at the gates could be scrapped for free).
		for (const auto& site : sites) {
			const auto                  where = site.function.address() + site.offset;
			std::array<std::uint8_t, 5> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(where), bytes.size());
			if (TextureFlush::CallTarget(bytes, where) != site.target.address()) {
				REX::WARN("WorkshopSync: {} ({:X}+{:X}) doesn't call what 1.11.240 calls there (another plugin's patch?); the shared workshop's picks are not patched, so the host's pieces can't be wired by friends",
					site.what, site.function.address(), site.offset);
				return;
			}
		}
		// The picks' own calls: each pick's run (the pick watch's ray) and its cast's sorted hits (the watch's view of the whole
		// cast), each hit's reference in both picks (a friend's stand-in named the player: passed by), the general filter, which
		// admits the copies the test above admitted, and the wire filter (watched: its verdicts and the wire's target). Each
		// patched only if 1.11.240's call is there: without them the picks still work, unwatched, a stand-in in front hides what
		// is behind it, and a copy the general filter refuses stays unpicked.
		const std::array<Site, 8> watch{ {
			{ FOREGROUND_JOB, FOREGROUND_PICK_CALL, PICK_GENERAL, reinterpret_cast<void*>(&GeneralPickHook), at(originalGeneralPick), "the general pick's run" },
			{ PICK_GENERAL, PICK_SORT_CALL, PICK_SORT, reinterpret_cast<void*>(&PickSortHook), at(originalPickSort), "the general pick's cast" },
			{ PICK_GENERAL, PICK_HIT_CALL, FIND_BODY_REF, reinterpret_cast<void*>(&PickHitHook), at(originalFindBodyRef), "the general pick's hits" },
			{ PICK_WIRE, PICK_HIT_CALL, FIND_BODY_REF, reinterpret_cast<void*>(&PickHitHook), at(originalFindBodyRef), "the wire pick's hits" },
			{ PICK_GENERAL, PICK_FILTER_CALL, GENERAL_FILTER, reinterpret_cast<void*>(&PickFilterHook), at(originalGeneralFilter), "the general pick's filter" },
			{ FOREGROUND_JOB, FOREGROUND_WIRE_CALL, PICK_WIRE, reinterpret_cast<void*>(&WirePickHook), at(originalWirePick), "the wire pick's run" },
			{ PICK_WIRE, PICK_SORT_CALL, PICK_SORT, reinterpret_cast<void*>(&PickSortHook), at(originalPickSort), "the wire pick's cast" },
			{ PICK_WIRE, PICK_FILTER_CALL, WIRE_FILTER, reinterpret_cast<void*>(&WireFilterHook), at(originalWireFilter), "the wire filter" },
		} };
		std::array<bool, 8> watchable{};
		for (std::size_t i = 0; i < watch.size(); ++i) {
			const auto&                 site = watch[i];
			const auto                  where = site.function.address() + site.offset;
			std::array<std::uint8_t, 5> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(where), bytes.size());
			watchable[i] = TextureFlush::CallTarget(bytes, where) == site.target.address();
			if (!watchable[i]) {
				REX::WARN("WorkshopSync: {} ({:X}+{:X}) doesn't call what 1.11.240 calls there (another plugin's patch?); not watched", site.what, site.function.address(), site.offset);
			}
		}
		// One call stub per distinct hook: nine, and the watch's six (both picks' casts share one, and both picks' hits).
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < 15 * CALL_STUB) {
			REX::WARN("WorkshopSync: no room for the shared workshop's hooks ({} bytes free); the host's pieces can't be wired by friends", trampoline.free_size());
			return;
		}
		for (const auto& site : sites) {
			const auto original = trampoline.write_call<5>(site.function.address() + site.offset, site.hook);
			if (!*site.original) {
				*site.original = reinterpret_cast<void*>(original);
			}
		}
		std::size_t watched = 0;
		for (std::size_t i = 0; i < watch.size(); ++i) {
			if (watchable[i]) {
				const auto original = trampoline.write_call<5>(watch[i].function.address() + watch[i].offset, watch[i].hook);
				if (!*watch[i].original) {
					*watch[i].original = reinterpret_cast<void*>(original);
				}
				++watched;
			}
		}
		picksInstalled = true;
		REX::INFO("WorkshopSync: workshop mode's picks hooked ({} sites, and {} of the picks' own calls): at a settlement the host owns, a friend can highlight and wire the host's pieces, never grab them, and their scrap or store of one is asked of the host",
			sites.size(), watched);
	}

	void OnRequest(const Protocol::WorkshopRequest& a_request)
	{
		if (!Serves()) {
			Answer(a_request, hosting ? Protocol::WorkshopAnswer::Why::kOff : Protocol::WorkshopAnswer::Why::kHostAway, 0);
			return;
		}
		const auto facts = FactsOf(a_request);
		if (const auto why = SharedWorkshop::Refusal(a_request.op, facts)) {
			if (*why == Protocol::WorkshopAnswer::Why::kBenchUnopened && unopenedTold.insert(a_request.workshop).second) {
				// The engine makes a workbench's workshop data (its grids, its stored items) when its workshop is first opened,
				// and saves it with the bench; a settlement the host took by console (or never built at) has none. Made by
				// us it would be a struct of the engine's we never saw made: the host opens the workshop once instead.
				Hud::Notify(Text::Get("workshop.shared.host_open_bench"));
			}
			Answer(a_request, *why, 0);
			return;
		}
		if (making.size() >= SharedWorkshop::MAX_PENDING) {
			Answer(a_request, Protocol::WorkshopAnswer::Why::kHostBusy, 0);
			return;
		}
		if (a_request.op == Protocol::WorkshopRequestOp::kWire || Protocol::WorkshopRequestOp::IsEdit(a_request.op)) {
			making.push_back({ a_request, {}, Clock::now() });  // made (or scrapped, or stored) at the next frame, out of the request's handler
			return;
		}
		// The piece, where the friend's workshop mode put theirs; finished once its 3D is there (MakeRequests).
		Protocol::WorkshopItem item;
		item.workshop = a_request.workshop;
		item.base = a_request.base;
		std::copy(a_request.position.begin(), a_request.position.end(), item.position);
		std::copy(a_request.rotation.begin(), a_request.rotation.end(), item.rotation);
		item.scale = a_request.scale;
		const auto made = Create(item);
		if (!made || !made->ref.get()) {
			Answer(a_request, Protocol::WorkshopAnswer::Why::kFailed, 0);
			return;
		}
		making.push_back({ a_request, made->ref, Clock::now() });
	}

	void OnAnswer(const Protocol::WorkshopAnswer& a_answer)
	{
		const auto pending = requests.Take(a_answer.seq);
		if (!pending) {
			++answersUnknown;
			return;
		}
		if (a_answer.why == Protocol::WorkshopAnswer::Why::kOff) {
			// The rule is off at the server: nothing more is asked this session, so no build waits a round trip for the same
			// answer. (A "no host's game" is not held against the session: the server also says it in the moments the host's
			// game isn't welcomed, and the welcome's host id already keeps a dedicated server from being asked at all.)
			if (!std::exchange(hostGone, true)) {
				REX::INFO("WorkshopSync: the server answered '{}': nothing more is asked of the host this session", Protocol::Name(a_answer.why));
			}
		}
		Answered(a_answer.seq, pending->op, a_answer.why, *pending, Key{ a_answer.playerId, a_answer.refId });
	}

	void OnWelcome(std::uint32_t a_hostId)
	{
		hostId.store(a_hostId);
		hostGone = false;
		if (a_hostId == 0) {
			REX::INFO("WorkshopSync: no host's game in the session (a dedicated server): the shared workshop asks nothing");
		}
	}

	std::string ScrapForDev(RE::TESObjectREFR* a_ref, bool a_engine)
	{
		if (!a_ref || a_ref->IsDeleted()) {
			return "error: no such reference, or deleted already";
		}
		if (IsCopy(a_ref)) {
			return "error: a copy of a friend's piece: its builder's game scraps it";
		}
		const auto bench = BenchOf(*a_ref);
		const auto id = a_ref->GetFormID();
		if (!bench) {
			// Runtime ids are each game's own: on another machine the same id names something else (an NPC, a stand-in, a
			// dropped item), which must not be reported scrapped and deleted. Only a piece linked to a workbench is a piece.
			REX::INFO("WorkshopSync: wsscrap {:08X} refused: not a workshop piece linked to a workbench", id);
			return std::format("error: {:08X} is no workshop piece linked to a workbench: not scrapped", id);
		}
		if (a_engine) {
			// The engine's own scrap, as a friend's request runs it (the yield into the workbench): the request path's probe.
			if (Held(*a_ref)) {
				return std::format("error: {:08X} is held in workshop mode: put it down first", id);
			}
			const auto steps = ScrapPiece(*a_ref, *bench);
			return std::format("scrapped {:08X} (workbench {:08X}) with the engine's scrap: {} | disabled={} deleted={}", id, bench->GetFormID(), steps, a_ref->IsDisabled(), a_ref->IsDeleted());
		}
		// What the engine's scrap does, short of the yield: told to the workbench's scripts, off the grids and the links,
		// out of the budget, unlinked, reported scrapped for the friends' copies, disabled and deleted.
		NotifyDestroyed(bench, *a_ref);
		Unbuild(*a_ref, bench, true);
		ReportRef(a_ref, bench, Protocol::WorkshopOp::kScrapped);
		a_ref->Disable();
		a_ref->SetDelete(true);
		gridNamedFrame = ~0ull;
		return std::format("scrapped {:08X} (workbench {:08X}): off the grids, the budget and the workbench, reported, deleted", id, bench ? bench->GetFormID() : 0);
	}

	std::string StoreForDev(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled()) {
			return "error: no such reference, or deleted or disabled already";
		}
		if (IsCopy(a_ref) || Blocked(*a_ref)) {
			return "error: a copy of a friend's piece: its builder's game stores it";
		}
		const auto bench = BenchOf(*a_ref);
		const auto id = a_ref->GetFormID();
		if (!bench) {
			return std::format("error: {:08X} is no workshop piece linked to a workbench: not stored", id);
		}
		if (!a_ref->Get3D()) {
			return std::format("error: {:08X} has no 3D yet", id);
		}
		if (Held(*a_ref)) {
			return std::format("error: {:08X} is held in workshop mode: put it down first", id);
		}
		if (a_ref->As<RE::Actor>() || !EngineCanStore(*a_ref)) {
			return std::format("error: the engine's own store test refuses {:08X} (an actor, a pre-placed piece, a wire)", id);
		}
		const auto base = a_ref->GetObjectReference();
		const auto baseId = base ? base->GetFormID() : 0;
		const auto before = StoredCount(*bench, baseId);
		destroyedHere = 0;
		const auto steps = StorePiece(*a_ref, *bench);
		return std::format("stored {:08X} (workbench {:08X}) with the engine's store: {} | base {:08X} stored {} -> {} | destroyed={} disabled={} deleted={}", id, bench->GetFormID(), steps, baseId,
			before, StoredCount(*bench, baseId), destroyedHere == id, a_ref->IsDisabled(), a_ref->IsDeleted());
	}

	std::string StoredForDev(RE::TESObjectREFR* a_bench, RE::TESObjectREFR* a_use)
	{
		if (!IsWorkbenchAnchor(a_bench)) {
			return "error: not a workbench";
		}
		const auto data = WorkshopDataOf(a_bench);
		if (!data) {
			return "error: the workbench has no workshop data yet (open its workshop once)";
		}
		if (a_use) {
			if (a_use->IsDeleted() || IsCopy(a_use)) {
				return "error: a deleted reference, or a copy of a friend's piece";
			}
			static REL::Relocation<UseDeletedFn> use{ USE_DELETED_ITEM };
			const auto                           base = a_use->GetObjectReference();
			const auto                           baseId = base ? base->GetFormID() : 0;
			const auto                           before = StoredCount(*a_bench, baseId);
			const bool                           taken = use(data, *a_use);
			return std::format("used {:08X} (base {:08X}) from the stored list of workbench {:08X}: taken={} {} -> {}", a_use->GetFormID(), baseId, a_bench->GetFormID(), taken, before,
				StoredCount(*a_bench, baseId));
		}
		std::string out = std::format("stored list of workbench {:08X}: {} entries", a_bench->GetFormID(), data->deletedItems.size());
		for (const auto entry : data->deletedItems) {
			if (entry) {
				out += std::format(" {:08X}x{}", entry->formID, entry->count);
			}
		}
		return out;
	}

	std::string FinishForDev(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_bench)
	{
		if (!a_ref || !a_bench) {
			return "error: no such reference";
		}
		if (IsCopy(a_ref)) {
			return "error: a copy of a friend's piece is never finished as ours";
		}
		if (!a_ref->Get3D()) {
			return "error: the reference has no 3D yet";
		}
		const auto steps = FinishPiece(*a_ref, *a_bench);
		return std::format("finished {:08X} at {:08X}: {} | {}", a_ref->GetFormID(), a_bench->GetFormID(), steps, DescribeRef(a_ref));
	}

	void SetSharedModeForDev(std::optional<bool> a_on)
	{
		sharedDev = a_on;
		admitsCopies.store(HandsOver());
	}

	void HoldAnswersForDev(bool a_hold)
	{
		answersHeld = a_hold;
	}

	std::string HeldForDev()
	{
		static REL::Relocation<RE::ObjectRefHandle*> firstEnd{ WIRE_FIRST_END };
		static REL::Relocation<RE::ObjectRefHandle*> target{ WIRE_TARGET };
		static REL::Relocation<std::uint8_t*>        gate{ SCRAP_GATE };
		const auto                                   p = PlacementNow();
		const auto                                   ref = PlacementRef();
		const auto                                   base = ref ? ref->GetObjectReference() : nullptr;
		const auto                                   id = ref ? ref->GetFormID() : 0;
		const auto                                   from = firstEnd->get();
		const auto                                   to = target->get();
		// The wire's own state while one is held (its first end set): the spline status by name, its ends.
		const auto wire = from ? std::format(" wireStatus={} wireFrom={:08X} wireTarget={:08X}", SharedWorkshop::WireStatusName(p.status), from->GetFormID(), to ? to->GetFormID() : 0) :
		                         std::string{};
		return std::format("held={:08X} heldBase={:08X} newItem={} selected={:08X} editing={} canPlace={} placeStatus={}{} scrapGate={}", p.Holds() ? id : 0,
			p.Holds() && base ? base->GetFormID() : 0, p.newItem, id, p.editing, p.canPlace, p.status, wire, *gate);
	}

	std::string ScrapTestForDev(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "error: no such reference";
		}
		// The engine's own test, as the Scrap key asks it (CAN_SCRAP_REF itself is not hooked: its callers are).
		return std::format("scraptest {:08X}: engine={} {} copy={} admitted={}", a_ref->GetFormID(), CanScrap(*a_ref), ScrapParts(*a_ref), IsCopy(a_ref), AdmitsCopy(*a_ref));
	}

	namespace
	{
		// The watched piece's bodies (each collision object under its 3D: the body id, its layer, whether it is in the cell's
		// havok world), for matching against a pick's cast.
		struct Bodies
		{
			std::vector<std::uint32_t> ids;
			std::string                text;
		};

		Bodies BodiesOf(RE::TESObjectREFR& a_ref, RE::NiAVObject* a_root)
		{
			const auto                   refCell = a_ref.GetParentCell();
			const auto                   world = refCell ? refCell->GetbhkWorld() : nullptr;
			Bodies                       out;
			std::vector<RE::NiAVObject*> stack{ a_root };
			std::size_t                  walked = 0;
			while (!stack.empty() && walked++ < 4096) {
				const auto node = stack.back();
				stack.pop_back();
				const auto object = node->collisionObject.get();
				if (const auto collision = object ? object->IsbhkNPCollisionObject() : nullptr) {
					RE::hknpBodyId id{};
					collision->GetBodyId(id);
					std::uint32_t raw = 0;
					std::memcpy(&raw, &id, sizeof(raw));
					const auto body = collision->GetBody();
					const auto layer = body ? (body->m_collisionFilterInfo & 0x7F) : 0xFFu;
					const bool added = world && raw != 0x7FFFFFFF && world->IsBodyAdded(id);
					out.ids.push_back(raw);
					if (out.ids.size() <= 6) {
						out.text += std::format("{}b{}:L{}{}", out.text.empty() ? "" : ",", raw, layer, added ? "" : "(not in world)");
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
			return out;
		}

		// Whether the run's cast hit one of these bodies at all, its reference or not.
		bool InCast(const PickRun& a_run, const Bodies& a_bodies)
		{
			const auto kept = (std::min)(static_cast<std::size_t>((std::max)(a_run.castCount, 0)), a_run.cast.size());
			for (std::size_t i = 0; i < kept; ++i) {
				if (std::ranges::find(a_bodies.ids, a_run.cast[i].body) != a_bodies.ids.end()) {
					return true;
				}
			}
			return false;
		}

		// Where a point stands from the run's ray: how far along it, and how far off it.
		std::pair<float, float> AlongAndMiss(const PickRun& a_run, const RE::NiPoint3& a_point)
		{
			const auto  to = RE::NiPoint3{ a_point.x - a_run.from.x, a_point.y - a_run.from.y, a_point.z - a_run.from.z };
			const float length = std::sqrt(a_run.dir.x * a_run.dir.x + a_run.dir.y * a_run.dir.y + a_run.dir.z * a_run.dir.z);
			const auto  unit = length > 0.0f ? RE::NiPoint3{ a_run.dir.x / length, a_run.dir.y / length, a_run.dir.z / length } : RE::NiPoint3{};
			const float along = to.x * unit.x + to.y * unit.y + to.z * unit.z;
			const auto  off = RE::NiPoint3{ to.x - along * unit.x, to.y - along * unit.y, to.z - along * unit.z };
			return { along, std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z) };
		}

		// Whether the run's search reached the reference (its hits as the search met them).
		bool SearchReached(const PickRun& a_run, std::uint32_t a_ref)
		{
			const auto kept = (std::min)(a_run.count, static_cast<std::uint32_t>(a_run.hits.size()));
			for (std::uint32_t i = 0; i < kept; ++i) {
				if (a_run.hits[i].ref == a_ref) {
					return true;
				}
			}
			return false;
		}
	}

	std::string PickForDev(std::optional<RE::TESObjectREFR*> a_watch)
	{
		// The piece described below: the one watched now, or with "off" the one that was (the last runs are of it).
		auto watched = pickWatch.load();
		if (a_watch) {
			const auto was = pickWatch.exchange(*a_watch ? (*a_watch)->GetFormID() : 0);
			watched = *a_watch ? (*a_watch)->GetFormID() : was;
		}
		PickRun run;
		PickRun wire;
		{
			std::scoped_lock l{ pickLock };
			run = generalPick.last;
			wire = wirePick.last;
		}
		const auto now = std::chrono::steady_clock::now();
		auto       out = std::format("pick watch={:08X} runs={} copyHits={} copyRefused={} picksAdmitted={} filterAdmitted={} filterOtherBench={} standInsPassed={} openBench={:08X}",
				  pickWatch.load(), pickRuns.load(), pickCopyHits.load(), pickCopyRefused.load(), picksAdmitted.load(), filterAdmitted.load(), filterOtherBench.load(), standInsPassed.load(),
				  OpenBench());
		const bool ran = run.at != std::chrono::steady_clock::time_point{};
		if (ran) {
			const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(now - run.at).count();
			out += std::format(" | last run {} ms ago: ray from ({:.0f}, {:.0f}, {:.0f}) along ({:.3f}, {:.3f}, {:.3f}) cell={:08X}; {}; picked={:08X}; {}", ago, run.from.x, run.from.y,
				run.from.z, run.dir.x, run.dir.y, run.dir.z, run.cell, DescribeHits(run), run.picked, DescribeCast(run));
		} else {
			out += " | no run yet (workshop mode not up, idle and unhooked?)";
		}
		// The wire pick's last run (it runs each frame the placement item is a wire): picked is the wire's target.
		const bool wireRan = wire.at != std::chrono::steady_clock::time_point{};
		out += std::format(" | wire pick runs={} targets={} copyYes={} copyNo={}", wirePickRuns.load(), wireTargets.load(), wireFilterCopyYes.load(), wireFilterCopyNo.load());
		if (wireRan) {
			const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(now - wire.at).count();
			out += std::format(": wire run {} ms ago: ray from ({:.0f}, {:.0f}, {:.0f}) along ({:.3f}, {:.3f}, {:.3f}); {}; target={:08X}; {}", ago, wire.from.x, wire.from.y, wire.from.z,
				wire.dir.x, wire.dir.y, wire.dir.z, DescribeHits(wire), wire.picked, DescribeCast(wire));
		}
		// Where the watched piece stands from each ray: how far along it, how far off it (its bound's center), against its radius.
		const auto ref = watched ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(watched) : nullptr;
		const auto root = ref ? ref->Get3D() : nullptr;
		if (!root) {
			return watched ? out + std::format(" | {:08X}: no 3D", watched) : out;
		}
		const auto bodies = BodiesOf(*ref, root);
		const auto center = root->worldBound.center;
		const auto cell = ref->GetParentCell();
		if (ran) {
			const auto [along, miss] = AlongAndMiss(run, center);
			out += std::format(" | {:08X}: bodies {} [{}] inCast={}", watched, bodies.ids.size(), bodies.ids.empty() ? "none: no collision" : bodies.text, InCast(run, bodies));
			out += std::format(" | {:08X}: center ({:.0f}, {:.0f}, {:.0f}) radius={:.0f} along={:.0f} miss={:.0f} cell={:08X} copy={} blocked={} hit={}", watched, center.x, center.y, center.z,
				root->worldBound.fRadius, along, miss, cell ? cell->GetFormID() : 0, IsCopy(ref), Blocked(*ref), SearchReached(run, watched));
		}
		if (wireRan) {
			const auto [along, miss] = AlongAndMiss(wire, center);
			out += std::format(" | wire {:08X}: radius={:.0f} along={:.0f} miss={:.0f} inCast={} reached={} target={}", watched, root->worldBound.fRadius, along, miss, InCast(wire, bodies),
				SearchReached(wire, watched), wire.picked == watched);
		}
		return out;
	}

	std::string DescribeShared()
	{
		return std::format("shared workshop: rule={} edit={} dev={} hostId={} hostGone={} handsOver={} serves={} hooks={} pickRuns={} pickCopyHits={} pickCopyRefused={} filterAdmitted={} filterOtherBench={} standInsPassed={} picksAdmitted={} wirePickRuns={} wireTargets={} wireFilterCopyYes={} wireFilterCopyNo={} grabsRefused={} storesRefused={} scrapsRefused={} scrapsAsked={} storesAsked={} scrapKeys={} scrapKeyNo={} requestsSent={} wiresAsked={} wiresMixed={} wiresDetached={} pending={} dropped={} answersApplied={} answersRefused={} answersLate={} answersUnknown={} adopted={} adoptsDeferred={} adoptMissing={} benchesTaught={} made={} strays={} | host: served={} refused={} making={} held={} finished={} steps={} wiresMade={} wiresCounted={} wiresUncounted={} scrapsServed={} storesServed={}",
			HostRules::Get().sharedWorkshop, EditAllowed(), sharedDev ? (*sharedDev ? "on" : "off") : "host", hostId.load(), hostGone, HandsOver(), Serves(), picksInstalled, pickRuns.load(), pickCopyHits.load(), pickCopyRefused.load(), filterAdmitted.load(), filterOtherBench.load(), standInsPassed.load(), picksAdmitted.load(), wirePickRuns.load(), wireTargets.load(), wireFilterCopyYes.load(), wireFilterCopyNo.load(), grabsRefused.load(),
			storesRefused.load(), scrapsRefused.load(), scrapsAsked.load(), storesAsked.load(), scrapKeys.load(), scrapKeyNo.load(), requestsSent, wiresAsked, wiresMixed, wiresDetached, requests.Waiting(), requests.Dropped(), answersApplied, answersRefused, answersLate, answersUnknown, adoptedHere,
			adoptsDeferred, adoptMissing, taught.Size(), madeHere.size(), straysFound, requestsServed, requestsRefused, making.size(), answersHeld, finishedHere, finishSteps, wiresMadeHere, wiresCounted, wiresUncounted,
			scrapsServed, storesServed);
	}

	void InstallLights()
	{
		if (originalLightPowered) {
			return;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("WorkshopSync: no room for the light maker's hook ({} bytes free); a friend's light bulb glows but casts no light here", trampoline.free_size());
			return;
		}
		const auto                  site = GENERATE_LIGHT.address() + GENERATE_LIGHT_IS_POWERED_CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != IS_POWERED.address()) {
			REX::WARN("WorkshopSync: the light maker doesn't ask Workshop::IsPowered where 1.11.240 does (another plugin's patch?); a friend's light bulb glows but casts no light here");
			return;
		}
		originalLightPowered = reinterpret_cast<IsPoweredFn>(trampoline.write_call<5>(site, &LightPoweredHook));
		REX::INFO("WorkshopSync: light maker hooked ({:X}+{:X}): a friend's light shown powered here casts its light", GENERATE_LIGHT.address(), GENERATE_LIGHT_IS_POWERED_CALL);
	}

	void InstallCrafting()
	{
		if (originalGetWorkshop) {
			return;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("WorkshopSync: no room for the crafting menus' hook ({} bytes free); a friend's crafting station reads only our player's things", trampoline.free_size());
			return;
		}
		const auto                  site = WORKBENCH_GATHER.address() + CraftingWorkshop::GET_WORKSHOP_CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != GET_WORKSHOP.address()) {
			REX::WARN("WorkshopSync: the crafting menus' gather doesn't ask Workshop::GetWorkshop where 1.11.240 does (another plugin's patch?); a friend's crafting station reads only "
					  "our player's things");
			return;
		}
		originalGetWorkshop = reinterpret_cast<GetWorkshopFn>(trampoline.write_call<5>(site, &CraftingWorkshopHook));
		REX::INFO("WorkshopSync: crafting menus hooked (gather {:X}+{:X}): a friend's station copied here reads our workbench", WORKBENCH_GATHER.address(),
			CraftingWorkshop::GET_WORKSHOP_CALL);
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
		SweepGraveyard(now);
		if (now >= nextCellLook) {
			nextCellLook = now + CELL_LOOK_INTERVAL;
			std::vector<WorkshopCells::Where> attachedNow;
			const auto                        fresh = NewlyAttachedCells(&attachedNow);
			{
				std::scoped_lock a{ auditLock };
				cellAges.Update(attachedNow, auditClock);  // how long each has been, for the audit (AuditEngine)
			}
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
		SharedFrame(now, a_host);
		PowerFrame(now);
		ResourceFrame(now);
		AuditFrame(now);
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

	std::vector<std::vector<std::uint8_t>> TakeFromWorldBefore(bool a_sameWorld)
	{
		SendPacer::Queue<Outgoing>::Fenced taken;
		std::size_t                        after = 0;
		{
			std::scoped_lock l{ outLock };
			if (a_sameWorld) {
				// The loaded world is the one the server tags our reports with already: after the request they get the same
				// character, so they wait their turn, none dropped.
				if (outgoing.BeforeFence() != 0) {
					REX::INFO("WorkshopSync: load: {} reports from before it wait their turn (the same world)", outgoing.BeforeFence());
				}
				outgoing.Unfence();
				return {};
			}
			taken = outgoing.TakeFenced(Clock::now());
			after = outgoing.Size();
			if (outgoing.Empty()) {
				droppedWarned = false;
			}
		}
		std::vector<std::vector<std::uint8_t>> packets;
		for (auto& out : taken.due) {
			packets.push_back(std::move(out.packet));
		}
		for (const auto& out : taken.late) {
			if (out.worldScrap != 0) {
				sentScraps.Forget(out.worldScrap);  // found again by the walk, if the loaded world has it scrapped too
			}
		}
		if (!taken.due.empty() || !taken.late.empty()) {
			REX::INFO("WorkshopSync: load: {} reports from the world before sent ahead of the request for the session's changes, {} dropped (more than may go at once); {} "
					  "of the loaded world's wait",
				taken.due.size(), taken.late.size(), after);
		}
		if (!taken.late.empty()) {
			std::scoped_lock l{ outLock };
			dropped += static_cast<std::uint32_t>(taken.late.size());
		}
		return packets;
	}

	void Apply(const Protocol::WorkshopItem& a_item)
	{
		inbox.Arrive({ a_item.playerId, a_item.refId }, a_item);
	}

	void AuditOwn(const std::vector<Protocol::WorkshopItem>& a_items)
	{
		std::scoped_lock l{ auditLock };
		audit.Add(a_items);
		REX::DEBUG("WorkshopSync: the session keeps {} more pieces as ours; {} to look for in our world", a_items.size(), audit.Waiting());
	}

	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op)
	{
		// Under the shared workshop's rule a placed piece goes the way workshop mode's own goes: asked of the host (wsreport
		// place is the rig's stand-in for a build).
		if (a_op == Protocol::WorkshopOp::kPlaced && HandOver(a_ref, a_workshop)) {
			return;
		}
		ReportRef(a_ref, a_workshop, a_op);
	}

	std::optional<std::string> RequestWireForDev(RE::TESObjectREFR* a_from, RE::TESObjectREFR* a_to)
	{
		if (!HandsOver() || !a_from || !a_to) {
			return std::nullopt;
		}
		const auto from = KeyOfCopy(a_from->GetFormID());
		const auto to = KeyOfCopy(a_to->GetFormID());
		if (SharedWorkshop::KindOfWire(from.has_value(), to.has_value(), from && to && from->first == to->first && from->first == hostId.load()) != SharedWorkshop::WireKind::kRequest) {
			return std::nullopt;
		}
		std::uint32_t benchId = 0;
		{
			std::scoped_lock l{ copyIdsLock };
			benchId = copyBenches.Of(a_from->GetFormID());
		}
		if (benchId == 0 || taught.IsNotOwned(benchId)) {
			return std::format("error: the copies' workbench {:08X} is none the host owns", benchId);
		}
		const auto seq = requests.Open(Protocol::WorkshopRequestOp::kWire, 0, benchId, Clock::now());
		if (!seq) {
			return std::string{ "error: too many requests wait" };
		}
		Protocol::WorkshopRequest request;
		request.seq = *seq;
		request.op = Protocol::WorkshopRequestOp::kWire;
		request.workshop = benchId;
		request.wireFrom = from->second;
		request.wireTo = to->second;
		PushShared(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest));
		++wiresAsked;
		return std::format("requested seq={} wire between player {}'s {:08X} and {:08X} at workbench {:08X}", *seq, from->first, from->second, to->second, benchId);
	}

	void Reset()
	{
		EngineWorld world;
		const auto  had = copies.size();
		const auto  waitedBefore = graveyard.size();
		WorkshopApply::RemoveAll(world, copies, twins, inbox);
		if (had > 0 || !graveyard.empty()) {
			const auto unloaded = std::ranges::count_if(graveyard, [](const Copy& a_copy) { return !a_copy.ref.get(); });
			REX::INFO("WorkshopSync: leaving: {} copies removed; {} wait to be deleted ({} already waited), {} of them until their cell loads", had, graveyard.size(),
				waitedBefore, unloaded);
		}
		ForgetAllCopies();
		ClearWalk();
		showsLogged = 0;  // the next session logs its first shows too
		graphsTold.Clear();
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
		ClearAudit();
		reporter.Clear();
		mirror.Clear();
		TakeResourcesOff("leaving");
		resources.Clear();
		theirResources.Clear();
		keptTied.clear();
		powerBenches.clear();
		benchWorld = 0;
		ResetShared();
		std::scoped_lock l{ outLock };
		outgoing.Clear();
	}

	void Sweep()
	{
		const auto now = Clock::now();
		HushStrays(now);  // a copy the save holds that no session keeps is shown unpowered, with or without a session
		if (graveyard.empty()) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell() || Hud::LoadingScreenUp()) {
			return;  // in the world, not between worlds (a load lets the graveyard go)
		}
		SweepGraveyard(now);
	}

	void BeforeLoad()
	{
		// Friends' pieces off the workbenches of the world going: the load sets them as its save has them, and should it not
		// (or should the load fail), nothing of ours is left on them. None go on again until the new world's clear
		// (Applier::BeforeLoad, OnGameLoaded): a load longer than the save/load hold's limit lets frames run before it.
		{
			SeeMainMenu();  // a load from the main menu: what the world before's workbenches no longer hold isn't taken off
			BenchEngine benches;
			if (const auto off = onBenches.BeforeLoad(benches); off > 0) {
				REX::INFO("WorkshopSync: loading: {} values of friends' pieces taken off our workbenches", off);
			}
			NoteResourcesOn();
		}
		ForgetOwnPieces("loading");
		std::scoped_lock l{ litLock };
		litCopies.clear();
	}

	void OnGameLoaded(bool a_newGame)
	{
		// The copies' handles name other things now, or nothing (see RemotePlayers::OnGameLoaded): they are
		// let go unread. The copies the save holds are found again, at their spots, when the server sends
		// the session's building again after the load (FindExisting); one it had removed since the save
		// stays. What waits in line, or for its cell, is reports, not handles, and stays.
		// Our reports still in line are the world before's: they go ahead of the load's request for the session's changes
		// (TakeFromWorldBefore), never after it, where the server would take them for the loaded world's. Fenced first, so
		// what this load reports below (our power, sent again) waits behind the request.
		{
			std::scoped_lock l{ outLock };
			outgoing.Fence();
		}
		REX::INFO("WorkshopSync: load: forgot {} copies, {} in the graveyard", copies.size(), graveyard.size());
		ForgetShared();  // what waited for the host's answer, or for its 3D, was the old world's
		copies.clear();
		strays.Clear();  // the loaded world's are looked for afresh
		twins.clear();  // the session's building sent again twins them again with the copies found
		graveyard.clear();
		unbuiltWires.clear();
		ForgetAllCopies();
		ClearWalk();
		ClearAudit();  // the server sends what it keeps as ours again after the load, to be looked for in this world
		// Our pieces are sent again (the load may have set them otherwise), and friends' are shown again on the copies
		// found or made after the load. The workbenches are looked for again.
		reporter.SendAll();
		mirror.Invalidate();
		graphsTold.Clear();  // the copies' graphs are made anew, and an address may come back for another
		powerBenches.clear();
		benchWorld = 0;
		// The workbenches are the save's (or the new game's) now, which hold none of friends' pieces (BeforeSave took them
		// off): what we had put on the old world's is forgotten, and theirs go on again as their copies are found. One clear
		// for each, never both, and neither takes off more than the workbench holds (ResourceMirror::TakeOffHeld), so none
		// leaves a value the save didn't hold:
		// - a load took it all off before (BeforeLoad), and what that couldn't reach was the old world's: nothing is taken
		//   off for it, only what the save's record says it held (a hush that failed);
		// - a new game from the main menu has no such word, so what is still on a workbench of the same ID is taken off
		//   (Applier::ForgetWorld), and no save's record is read.
		ClearResourcesForNewWorld(a_newGame);
		// Our own pieces tracked were the world before's: a friend's game is told they count for nothing, and those this world
		// has are found and sent again (a load in session, a death's reload, a new game; BeforeLoad did it for a load).
		ForgetOwnPieces(a_newGame ? "a new game" : "a load");
	}

	void AfterLoadWork(bool a_newGame)
	{
		if (!onBenches.Loading()) {
			return;  // OnGameLoaded did the clear (or no load set the block)
		}
		// The load is over (this runs from its end's message), and nothing went on since its start (the block): the clear
		// OnGameLoaded would have done, done now. Without it the block would last until the next load or new game.
		REX::WARN("WorkshopSync: the load's work didn't run; the new world's clear of friends' pieces' worth done now, and their block lifted");
		ClearResourcesForNewWorld(a_newGame);
	}

	void OnWorldGone()
	{
		ClearAudit();
		ForgetShared();
		copies.clear();
		strays.Clear();
		twins.clear();
		graveyard.clear();
		unbuiltWires.clear();
		reporter.Clear();
		mirror.Invalidate();
		graphsTold.Clear();  // the copies' graphs are made anew, and an address may come back for another
		powerBenches.clear();
		benchWorld = 0;
		ForgetAllCopies();
		resources.Clear();  // the world loaded is no session's (its workbenches hold none of ours: OnGameLoaded forgot them)
	}

	void ApplyPower(const Protocol::PowerStates& a_power)
	{
		for (const auto& entry : a_power.entries) {
			mirror.Set(a_power.playerId, entry);
		}
		powerReceived += static_cast<std::uint32_t>(a_power.entries.size());
	}

	void ApplyResources(const Protocol::ResourceStates& a_resources)
	{
		for (const auto& entry : a_resources.entries) {
			theirResources.Set(a_resources.playerId, entry);
		}
		resourcesReceived += static_cast<std::uint32_t>(a_resources.entries.size());
	}

	bool SetPowerWantForDev(std::uint32_t a_player, std::uint32_t a_ref, bool a_powered)
	{
		return mirror.Set(a_player, WorkshopPower::Entry{ a_ref, a_powered });
	}

	void OnPlayerJoined()
	{
		reporter.SendAll();
		resources.SendAll();
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		mirror.Freeze(a_playerId);
		// Their pieces count here no more: off our workbenches now, not at the next look.
		theirResources.Forget(a_playerId);
		PutResourcesOn(Clock::now());
	}

	void BeforeSave()
	{
		SharedBeforeSave();  // nothing half-made of the shared workshop goes into the save (then the audits net the rest)
		AuditPower("save");
		// A copy shown powered is saved running (its graph keeps the state), and a load brings it back running with no
		// session to stop it: shown unpowered for the save, powered again after (AfterSave). A light's light is let be (no
		// save keeps it: ShowPoweredHow).
		MirrorEngine engine;
		engine.forSave = true;
		const auto   hushed = mirror.Hush(engine, Clock::now());
		if (hushed > 0) {
			REX::INFO("WorkshopSync: {} friends' pieces shown unpowered for the save", hushed);
		}
	}

	void BeforeSaveResources()
	{
		// What friends' pieces count for on our workbenches is the session's alone: off for the save, on again after
		// (AfterSave). What couldn't come off (no such workbench here now), or anything still on should the hush throw, goes
		// in this save's record, for a load to take off; never an earlier save's.
		{
			std::scoped_lock l{ resourceRecordLock };
			resourceRecord.BeforeSave();
		}
		const auto said = [] {
			std::scoped_lock l{ resourceRecordLock };
			resourceRecord.Hushed(onBenches.On());
		};
		BenchEngine benches;
		std::size_t off = 0;
		try {
			SeeMainMenu();  // a coc world's first save: the world before's values aren't taken off it as if they were on
			off = onBenches.Hush(benches, Clock::now());
		} catch (...) {
			said();
			throw;
		}
		said();
		resourcesHushed += static_cast<std::uint32_t>(off);
		if (off > 0) {
			REX::INFO("WorkshopSync: {} values of friends' pieces taken off our workbenches for the save", off);
		}
		if (!onBenches.On().empty()) {
			REX::WARN("WorkshopSync: {} workbenches kept friends' pieces' worth through the hush; the save's record names them", onBenches.On().size());
		}
	}

	void AfterSave()
	{
		mirror.SaveDone();
		onBenches.SaveDone();
		PutResourcesOn(Clock::now());
	}

	std::vector<std::uint8_t> ResourceSaveRecord()
	{
		std::scoped_lock l{ resourceRecordLock };
		return resourceRecord.Take();
	}

	void LoadResourceRecord(std::span<const std::uint8_t> a_bytes, const std::function<std::optional<std::uint32_t>(std::uint32_t)>& a_resolve)
	{
		auto held = ResourceMirror::DecodeRecord(a_bytes);
		std::erase_if(held, [&](auto& a_bench) {
			a_bench.first = a_resolve(a_bench.first).value_or(0);
			return a_bench.first == 0;
		});
		std::scoped_lock l{ resourceRecordLock };
		resourcesLoaded = std::move(held);
	}

	void RevertResourceRecord()
	{
		std::scoped_lock l{ resourceRecordLock };
		resourcesLoaded.clear();
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
		std::size_t loaded = 0;
		std::string list;
		for (const auto& [key, copy] : copies) {
			const auto ref = Live(copy);
			loaded += ref != nullptr;
			list += std::format("; {:08X}{}={}:{:08X}", copy.id, ref ? "" : " (not loaded)", key.first, key.second);
		}
		const auto unloaded = std::ranges::count_if(graveyard, [](const Copy& a_copy) { return !a_copy.ref.get(); });
		return std::format("{} copies ({} loaded), found again={}, graveyard={} ({} not loaded), lightsMade={}", copies.size(), loaded, refound, graveyard.size(), unloaded,
			       copyLightsMade.load()) +
		       list;
	}

	std::string DescribePower()
	{
		return std::format("power: block={} blocked={} audits={} found={} detached={} retired={} wiresRemoved={} benches={} tracked={} sent={} received={} mirrored={} shown={} showCalls={} givenUp={} redrawsCapped={} hushed={} lightLapses={} beltsSwitched={} graphRefused={} graphAlready={} graphWaits={} mirrorDropped={} keptTied={} auditSkips={} strays={} strayHushed={} strayGivenUp={} pollMsMax={:.2f}",
			blockCopies, copiesBlocked, audits, untied.found, untied.detached, untied.retired, wiresUntied, powerBenches.size(), reporter.Size(), powerSent, powerReceived, mirror.Size(),
			mirror.Shown(), powerShown, mirror.GivenUp(), mirror.RedrawsCapped(), mirror.Hushed(), mirror.Lapses(), beltsSwitched, graphRefused, graphAlready, graphWaits, mirror.Dropped(), keptTied.size(), auditSkips,
			strays.Size(), strays.Hushed(), strays.GivenUp(), static_cast<double>(pollUsMax) / 1000.0);
	}

	std::string DescribeResources()
	{
		std::string out = std::format("resources: tracked={} counting={} owed={} sent={} received={} kept={} dropped={} benches={} changes={} hushed={} hushing={} loading={} mainMenu={} leftover={} pollMsMax={:.2f}",
			resources.Size(), resources.Counting(), resources.Owed(), resourcesSent, resourcesReceived, theirResources.Size(), theirResources.Dropped(), onBenches.On().size(), resourceChanges,
			resourcesHushed, onBenches.Hushed(), onBenches.Loading(), onBenches.AtMainMenu(), resourcesLeftover, static_cast<double>(resourcePollUsMax) / 1000.0);
		for (const auto& [bench, values] : onBenches.On()) {
			out += std::format("; {:08X}", bench);
			for (std::size_t k = 0; k < ResourceMirror::KINDS; ++k) {
				if (values[k] != 0.0f) {
					out += std::format(" {}={:.2f}", ResourceMirror::NAMES[k], values[k]);
				}
			}
		}
		return out;
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
		       std::format(" framesCleared={} craftingAtCopies={} craftingUnbenched={}", counts.framesCleared, craftingAtCopies.load(), craftingUnbenched.load()) + [] {
			       std::scoped_lock a{ auditLock };
			       return std::format(" ownLooking={} ownFound={} ownGone={} ownLostLooking={} ownBack={} auditCells={}", audit.Waiting(), audit.Found(), audit.Lost(), audit.LostWaiting(), audit.Restored(), cellAges.Size());
		       }();
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
		if (const auto belt = BeltRuns(*a_ref)) {
			out += std::format(" belt={}", *belt ? "running" : "stopped");  // a conveyor belt (ShowPoweredHow)
		}
		// Its owner: a workshop object's assigned settler is its actor owner (how a settler's job shows).
		if (const auto owner = a_ref->GetOwner()) {
			out += std::format(" owner={:08X}", owner->GetFormID());
		}
		for (const auto& [key, copy] : copies) {
			if (copy.id == a_ref->GetFormID() && Live(copy).get() == a_ref) {
				const auto wanted = mirror.Wanted(key);
				out += std::format(" builder={} theirRef={:08X} theirPower={}", key.first, key.second, wanted ? (*wanted ? "on" : "off") : "unknown");
				// What shows its power here: its graph (loaded, still loading, or none named, and what we last told it) and,
				// for a light, whether its light is made (the game gives a lit light an ExtraLight) and whether we show it
				// powered (LightPoweredHook lets its light be made).
				const auto model = a_ref->Get3D();
				const bool namesGraph = model && NamesGraph(*model);
				const auto graph = LoadedGraph(*a_ref);
				static constexpr std::array<const char*, 3> told{ "nothing", "Powered", "Unpowered" };
				out += std::format(" graph={} told={}", graph != 0 ? "loaded" : namesGraph ? "loading" : "none",
					told[static_cast<std::size_t>(graphsTold.Of(copy.id, graph))]);
				if (IsLightBase(*a_ref)) {
					bool shownLit = false;
					{
						std::scoped_lock l{ litLock };
						shownLit = litCopies.contains(copy.id);
					}
					out += std::format(" lit={} shownLit={}", a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kLight) ? "yes" : "no", shownLit ? "yes" : "no");
				}
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
