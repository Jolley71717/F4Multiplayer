#include "game/FrameLoans.h"

#include "Config.h"
#include "Text.h"
#include "game/Equipment.h"
#include "game/FrameLoan.h"
#include "game/Hud.h"
#include "game/ItemCopies.h"
#include "game/NpcSync.h"
#include "game/Papyrus.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"

#include <algorithm>
#include <format>
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
		};
		Borrowed borrowed;

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
			bool                 asked = false;
			bool                 warned = false;
		};
		std::optional<Cleanup> cleanup;
		constexpr auto         CLEANUP_AFTER = std::chrono::seconds(2);   // the loaded world settles first
		constexpr auto         SEND_OUT_AGAIN = std::chrono::seconds(3);
		constexpr auto         CLEANUP_WARN = std::chrono::seconds(30);  // still in it then: logged, and tried on (a save then keeps the record)

		std::string       lastSaid;
		Clock::time_point lastSaidAt{};
		constexpr auto    SAY_AGAIN = std::chrono::seconds(3);

		std::uint32_t asked = 0, refusedHere = 0, lentHere = 0, worn = 0, returned = 0, ownerlessEnds = 0, lentOut = 0, givenBack = 0, piecesWorn = 0, piecesTaken = 0, movedBack = 0, cleanedUp = 0;

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
			const auto worn = Equipment::PowerArmorFrameOf(player);
			return (occupied && occupied->GetFormID() == a_frame) || (worn && worn->GetFormID() == a_frame);
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

		// A frame's power armor piece, or a fusion core (ammunition: what a frame holds of it).
		bool PieceOrCore(const RE::TESBoundObject* a_object)
		{
			return a_object && (a_object->GetFormType() == RE::ENUM_FORM_ID::kAMMO || Equipment::IsPowerArmorPiece(a_object));
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

		constexpr std::uint32_t FUSION_CORE = 0x00075FE4;  // Fallout4.esm: AmmoFusionCore

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

		// Our player's own fusion cores the engine moved into our copy as the lent one ran down go back to them: of each
		// base, at most as many as they have fewer of now than as they got in, and only what the copy still holds of it. Run
		// once the lent items are gathered (none of those is counted). Nothing else of the copy goes to them: whatever more
		// it holds goes with it.
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
		}

		// A frame of ours nobody is in, gone (one the session's book of spawns no longer holds).
		void DeleteFrame(RE::TESObjectREFR* a_frame)
		{
			Equipment::ReleasePowerArmorFrame(a_frame);
			a_frame->Disable();
			a_frame->SetDelete(true);
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
			const auto copy = borrowed.handle.get();
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
			if (copy && player) {
				GiveRest(copy.get(), player);
			}
			// Never our player's own suit (RemotePlayers::OurFrame, Spawns' Occupied).
			if (player && copy && player->lastUsedPowerArmor.get().get() == copy.get()) {
				player->lastUsedPowerArmor = RE::ObjectRefHandle{};
			}
			const auto items = FrameLoan::ComposeReturn(borrowed.tags, found, !copy);
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
			if (copy) {
				if (borrowed.released) {
					SaveGuard::Track(copy.get());  // ours again: kept out of saves as every copy is
				}
				const bool spawnsCopy = Spawns::LocalOf(borrowed.spawnId) == copy->GetFormID();
				if (!(spawnsCopy && Spawns::DropLentCopy(borrowed.spawnId)) && !PlayerIn(copy.get())) {
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
					const bool    core = object->GetFormType() == RE::ENUM_FORM_ID::kAMMO;
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
		// up, and each that did not come back taken out of it (its copy is somewhere in the borrower's world now). One item
		// of its stack only: one of a stack of the same (fusion cores at full charge share one) is split off with its own
		// extra data first, so the others keep theirs. False when our frame has no inventory here yet: tried again.
		bool PutBack(RE::TESObjectREFR* a_frame, const std::vector<Source>& a_kit, const std::vector<Protocol::ReturnedItem>& a_items)
		{
			if (!a_frame || !a_frame->inventoryList) {
				return false;
			}
			std::uint32_t wornDown = 0, removed = 0;
			for (const auto& source : a_kit) {
				const auto back = std::ranges::find(a_items, source.slot, &Protocol::ReturnedItem::slot);
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
				if (after.what == FrameLoan::Back::kRemove) {
					hit->count = 1;
					RemoveStack(a_frame, *hit);
					++removed;
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
			piecesTaken += removed;
			REX::INFO("FrameLoans: of our frame {:08X}'s lent items, {} came back worn down and {} did not come back (taken out of it)", a_frame->GetFormID(), wornDown, removed);
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
			if (frame && FrameLoan::ChoosePlace(a_return) == FrameLoan::Place::kMove) {
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

		void RunCleanup(Clock::time_point a_now)
		{
			auto&      job = *cleanup;
			const auto player = Player();
			if (!player || a_now - job.since < CLEANUP_AFTER) {
				return;
			}
			const auto copy = job.what == FrameLoan::AfterLoad::kAll ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(job.record.copy) : nullptr;
			if (copy && PlayerIn(copy)) {
				if (!job.warned && a_now - job.since >= CLEANUP_WARN) {
					job.warned = true;
					REX::WARN("FrameLoans: our player is still in {:08X}, the frame borrowed from player {} as this save was made; still sending them out", job.record.copy, job.record.owner);
				}
				if (!job.asked || a_now - job.askedAt >= SEND_OUT_AGAIN) {
					const bool first = !job.asked;
					job.asked = true;
					job.askedAt = a_now;
					const bool sent = SendOut(player);
					if (first) {
						REX::INFO("FrameLoans: this save was made in {:08X}, the frame borrowed from player {}; our player is sent out of it{}", job.record.copy, job.record.owner, sent ? "" : " (the call could not be made)");
					}
				}
				return;
			}
			if (Equipment::InPowerArmor(player)) {
				return;  // in another frame: nothing is gathered from them until they are out of it
			}
			std::vector<FrameLoan::Found> found;
			if (copy) {
				Gather(copy, job.record.tags, found);
			}
			Gather(player, job.record.tags, found);
			if (copy) {
				if (player->lastUsedPowerArmor.get().get() == copy) {
					player->lastUsedPowerArmor = RE::ObjectRefHandle{};
				}
				DeleteFrame(copy);
			}
			++cleanedUp;
			REX::INFO("FrameLoans: after the load, the frame borrowed from player {} ({:08X}) and {} lent items are gone", job.record.owner, job.record.copy, found.size());
			Say(Text::Get("party.frame_loan_over"));
			cleanup.reset();
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
		return a_id != 0 && a_id == borrowed.spawnId && borrowed.stage != Stage::kNone && borrowed.stage != Stage::kAsked;
	}

	bool KeepOut(RE::TESObjectREFR* a_frame, std::uint32_t a_spawnId, std::uint32_t a_owner)
	{
		if (!a_frame) {
			return true;
		}
		const auto local = a_frame->GetFormID();
		const bool mine = borrowed.stage != Stage::kNone && borrowed.spawnId == a_spawnId;
		const auto status = lends.find(a_owner);
		const auto loan = lentTo.find(a_spawnId);
		FrameLoan::PressFacts facts;
		facts.ownerHere = status != lends.end();
		facts.ownerLends = facts.ownerHere && status->second;
		facts.ours = mine && borrowed.stage != Stage::kAsked;
		facts.filled = facts.ours && borrowed.copy == local;
		facts.asked = mine && borrowed.stage == Stage::kAsked;
		facts.otherLoan = borrowed.stage != Stage::kNone && !mine;
		facts.lentToOther = loan != lentTo.end() && loan->second != NpcSync::LocalPlayer();
		const auto name = RemotePlayers::NameOf(a_owner);
		switch (FrameLoan::ChoosePress(facts)) {
		case FrameLoan::Press::kEnter:
			return false;
		case FrameLoan::Press::kAsk:
			{
				const auto base = a_frame->GetObjectReference();
				borrowed = {};
				borrowed.stage = Stage::kAsked;
				borrowed.spawnId = a_spawnId;
				borrowed.owner = a_owner;
				borrowed.copy = local;
				borrowed.base = base ? base->GetFormID() : 0;
				borrowed.handle = a_frame->GetHandle();
				borrowed.since = Clock::now();
				outgoing.push_back(Protocol::Encode(Protocol::FrameBorrow{ 0, a_spawnId }, Protocol::MessageType::kBorrowFrame));
				++asked;
				REX::INFO("FrameLoans: asking player {} for their frame {:08X} (our copy {:08X})", a_owner, a_spawnId, local);
				Say(Text::Format("party.frame_asking", { { "player", name } }));
				return true;
			}
		case FrameLoan::Press::kWait:
			Say(Text::Format("party.frame_asking", { { "player", name } }));
			return true;
		case FrameLoan::Press::kNotLending:
			Say(Text::Format("party.frame_not_lending", { { "player", name } }));
			return true;
		case FrameLoan::Press::kBusy:
			Say(Text::Get("party.frame_busy"));
			return true;
		case FrameLoan::Press::kLent:
		case FrameLoan::Press::kOwnerGone:
			Say(Text::Format("party.frame_refused", { { "player", name } }));
			return true;
		}
		return true;
	}

	bool IsLentItem(std::uint32_t a_base, std::string_view a_name)
	{
		if (borrowed.stage == Stage::kNone || borrowed.stage == Stage::kAsked) {
			return false;
		}
		return std::ranges::any_of(borrowed.tags, [&](const FrameLoan::Tag& a_tag) { return FrameLoan::IsTagged(a_tag, a_base, a_name); });
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
			record = FrameLoan::Record{ .copy = borrowed.copy, .base = borrowed.base, .spawnId = borrowed.spawnId, .owner = borrowed.owner, .tags = borrowed.tags };
			REX::INFO("FrameLoans: this save holds {:08X}, the frame our player wears on loan from player {}, and {} lent items", borrowed.copy, borrowed.owner, borrowed.tags.size());
		} else if (cleanup) {
			// The clean-up after the last load is not done yet (our player still in the frame, or the world settling): this
			// save holds what that one did, and its load cleans it up the same way.
			record = cleanup->record;
			REX::INFO("FrameLoans: this save still holds {:08X}, the frame borrowed from player {} as the save loaded was made; noted again", cleanup->record.copy, cleanup->record.owner);
		}
		std::scoped_lock l{ recordLock };
		recordAtSave = std::move(record);
	}

	void EndUnwornBeforeSave()
	{
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
		const auto us = NpcSync::LocalPlayer();
		if (a_lending.refusal == Protocol::LoanRefusal::kNone) {
			lentTo[a_lending.spawnId] = a_lending.borrowerId;
		}
		// Ours, lent.
		if (a_lending.ownerId == us && us != 0) {
			if (const auto it = lent.find(a_lending.spawnId); it != lent.end() && a_lending.refusal == Protocol::LoanRefusal::kNone && !it->second.granted) {
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
		// stand-in's, Equipment::SwitchIntoPowerArmor), and those would be pieces and a core from nowhere.
		RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", copy->GetFormID()).c_str());
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
			copy->AddObjectToContainer(object, ItemCopies::ExtraFor(made, object), 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
			borrowed.tags.push_back({ .slot = item.slot, .base = item.base, .name = made.name, .core = item.core });
		}
		borrowed.owner = a_lending.ownerId;
		borrowed.stage = Stage::kLent;
		borrowed.since = Clock::now();
		++lentHere;
		REX::INFO("FrameLoans: player {} lent us frame {:08X}; {} of its {} items are in our copy {:08X}", a_lending.ownerId, a_lending.spawnId, borrowed.tags.size(), a_lending.kit.size(), borrowed.copy);
		Say(Text::Format("party.frame_lent", { { "player", owner } }));
	}

	void OnFrameReturned(const Protocol::FrameReturn& a_return)
	{
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
		const auto now = Clock::now();
		if (cleanup) {
			RunCleanup(now);
		}
		if (borrowed.stage != Stage::kNone) {
			const auto copy = borrowed.handle.get();
			const FrameLoan::WearFacts facts{ .copyThere = copy && !copy->IsDeleted(), .in = PlayerIn(borrowed.copy), .inArmor = Equipment::InPowerArmor(Player()) };
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
					REX::INFO("FrameLoans: our player got into {:08X}, player {}'s frame lent to us", borrowed.copy, borrowed.owner);
					NoteOwnCores(Player());
				}
				borrowed.stage = Stage::kWorn;
				borrowed.since = now;
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
		}
		// Ours offered and never confirmed: ours again.
		std::erase_if(lent, [&](const auto& a_entry) {
			if (a_entry.second.granted || now - a_entry.second.since < LEND_WAIT) {
				return false;
			}
			REX::INFO("FrameLoans: our frame {:08X} offered to player {} was never confirmed; ours again", a_entry.first, a_entry.second.borrower);
			return true;
		});
		// Ours given back while it had no inventory here: what came back put on it once it has.
		std::erase_if(backs, [&](const PendingBack& a_back) {
			if (PutBack(RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_back.local), a_back.kit, a_back.items)) {
				return true;
			}
			if (now - a_back.since >= MOVE_LIMIT) {
				REX::WARN("FrameLoans: our frame {:08X} never had an inventory here again; what came back of its loan is not put on it", a_back.local);
				return true;
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
			if (!frame || !NpcSync::MoveOwnFrame(frame, a_move.at, a_move.heading)) {
				return !frame;
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

	void LoadRecord(const std::vector<std::uint8_t>& a_bytes)
	{
		auto record = FrameLoan::Decode(a_bytes);
		std::scoped_lock l{ recordLock };
		recordLoaded = std::move(record);
	}

	void RevertRecord()
	{
		std::scoped_lock l{ recordLock };
		recordLoaded.reset();
	}

	void OnGameLoaded()
	{
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
		if (!record) {
			return;
		}
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(record->copy);
		const auto base = ref ? ref->GetObjectReference() : nullptr;
		const auto what = FrameLoan::ChooseAfterLoad(*record, ref && !ref->IsDeleted(), base ? base->GetFormID() : 0, ref && ref->IsCreated());
		REX::INFO("FrameLoans: this save was made in {:08X}, a frame borrowed from player {}: {}", record->copy, record->owner,
			what == FrameLoan::AfterLoad::kAll ? "our player is sent out of it, and it goes with the lent items" : what == FrameLoan::AfterLoad::kItems ? "it is gone; the lent items go" : "nothing to do");
		if (what != FrameLoan::AfterLoad::kNothing) {
			cleanup = Cleanup{ .record = std::move(*record), .what = what, .since = Clock::now() };
		}
	}

	void Reset()
	{
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
		return std::format("loans: stage={} spawn={:08X} copy={:08X} tags={} ownerless={} lentOut={} heard={} moves={} asked={} refused={} lent={} worn={} returned={} ended={} ours: lent={} back={} worn={} moved={} cleaned={}", static_cast<int>(borrowed.stage),
			borrowed.spawnId, borrowed.copy, borrowed.tags.size(), borrowed.ownerless, lent.size(), lentTo.size(), moves.size(), asked, refusedHere, lentHere, worn, returned, ownerlessEnds, lentOut, givenBack, piecesWorn, movedBack, cleanedUp);
	}
}
