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
				ReportRef(a_event.objectDestroyed.get(), a_event.workshop.get(), Protocol::WorkshopOp::kScrapped);
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

	void Frame()
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
		std::scoped_lock l{ outLock };
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("workshop: reported={} copies={} placed={} moved={} scrapped={} worldScrapped={} adopted={} refused={} pending={}", reported, copies.size(), placed, moved, scrapped,
			worldScrapped, adopted, refused, pending.size());
	}
}
