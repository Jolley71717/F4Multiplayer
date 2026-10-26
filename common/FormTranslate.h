#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Each game's edge between its own form IDs and the session's (FormIds.h): every packet a game sends goes through
// Translate(kToSession) and every packet it gets through Translate(kToLocal), before anything reads it, so the rest of
// the game only ever sees its own IDs, and the server only the session's. With the same plugins in the same places
// (the usual case) nothing is decoded at all.
//
// What has no ID on the other side (a form of a plugin that game lacks) is skipped, never guessed:
// - a message about it is dropped: an NPC's death, a door, a shot by it, a friend standing in a mod's cell;
// - an entry of a list is left out: one NPC of a batch of states, an item someone wears, a head part;
// - a detail becomes 0, which every message reads as none: a gun's sound, the weather, a hair colour, a location;
// - a container change the server numbered keeps its number, emptied (Protocol::IsEmpty): the ledger counts it and
//   applies nothing, so the count of changes stays the server's.
// Result says what was skipped, and which IDs had none, for the log. Free of the game: tests/FormTranslateTests.cpp.
namespace FormTranslate
{
	enum class Way
	{
		kToSession,  // ours, going out
		kToLocal,    // the session's, coming in
	};

	struct Result
	{
		enum class Kind
		{
			kSame,     // send or read the packet as it is
			kChanged,  // send or read `packet` instead
			kDropped,  // its subject has no ID on the other side: neither
		};
		Kind                       kind = Kind::kSame;
		std::vector<std::uint8_t>  packet;
		std::uint32_t              entries = 0;  // list entries left out (and container changes emptied)
		std::uint32_t              fields = 0;   // details set to none
		std::vector<std::uint32_t> unmapped;     // the IDs that had none, in the numbering they came in (the first few)
		// The players whose state was left out of a PlayerStates because the cell or worldspace it names has no ID on this
		// side: they are somewhere this game lacks, which is not the same as their states not coming (RemotePlayers).
		std::vector<std::uint32_t> placeless;
	};

	inline constexpr std::size_t MAX_UNMAPPED_KEPT = 8;

	namespace detail
	{
		using Protocol::MessageType;

		struct Context
		{
			const FormIds::Mapper& mapper;
			Way                    way;
			Result&                out;
			bool                   changed = false;

			std::optional<std::uint32_t> Map(std::uint32_t a_id)
			{
				auto mapped = way == Way::kToSession ? mapper.ToSession(a_id) : mapper.ToLocal(a_id);
				if (!mapped && out.unmapped.size() < MAX_UNMAPPED_KEPT && !std::ranges::contains(out.unmapped, a_id)) {
					out.unmapped.push_back(a_id);
				}
				return mapped;
			}

			// What the message is about (0 stays 0): false when it has no ID on the other side.
			bool Key(std::uint32_t& a_id)
			{
				if (a_id == 0) {
					return true;
				}
				const auto mapped = Map(a_id);
				if (!mapped) {
					return false;
				}
				changed = changed || *mapped != a_id;
				a_id = *mapped;
				return true;
			}

			// A detail: none (0) when it has no ID on the other side. False then.
			bool Detail(std::uint32_t& a_id)
			{
				if (Key(a_id)) {
					return true;
				}
				a_id = 0;
				++out.fields;
				changed = true;
				return false;
			}

			// The entries a_keep keeps (it maps their IDs as it goes).
			template <class T, class Keep>
			void Entries(std::vector<T>& a_list, Keep a_keep)
			{
				const auto left = std::erase_if(a_list, [&](T& a_entry) { return !a_keep(a_entry); });
				out.entries += static_cast<std::uint32_t>(left);
				changed = changed || left != 0;
			}

			void Ids(std::vector<std::uint32_t>& a_list)
			{
				Entries(a_list, [&](std::uint32_t& a_id) { return Key(a_id); });
			}

			// A container change: both its IDs, or emptied (a numbered one) / dropped (a report).
			bool Change(Protocol::ContainerChange& a_change)
			{
				auto container = a_change.container;
				auto item = a_change.item;
				if (Key(container) && Key(item)) {
					a_change.container = container;
					a_change.item = item;
					return true;
				}
				return false;
			}

			void Numbered(Protocol::IndexedContainerChange& a_change)
			{
				if (!Change(a_change.change)) {
					a_change.change = {};
					++out.entries;
					changed = true;
				}
			}

			bool Place(std::uint32_t& a_cell, std::uint32_t& a_worldspace) { return Key(a_cell) && Key(a_worldspace); }
		};

		// The packet a translated message makes, or the verdict.
		template <class T, class Decode, class Map, class Encode>
		void Apply(std::span<const std::uint8_t> a_packet, Context& a_ctx, Decode a_decode, Map a_map, Encode a_encode)
		{
			auto msg = a_decode(a_packet);
			if (!msg) {
				return;  // as it is: the reader refuses it as it would have
			}
			if (!a_map(static_cast<T&>(*msg))) {
				a_ctx.out.kind = Result::Kind::kDropped;
				return;
			}
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = a_encode(static_cast<const T&>(*msg));
			}
		}

		inline bool MapEquipment(Context& a_ctx, Protocol::Equipment& a_msg)
		{
			a_ctx.Ids(a_msg.items);
			// The mods of an item left out go with it (its ID has none either); the rest follow their item's new ID.
			a_ctx.Entries(a_msg.mods, [&](Protocol::ItemMods& a_entry) {
				if (!a_ctx.Key(a_entry.item)) {
					return false;
				}
				a_ctx.Entries(a_entry.mods, [&](Protocol::ItemMod& a_mod) { return a_ctx.Key(a_mod.mod); });
				return true;
			});
			return true;
		}

		inline bool MapStatus(Context& a_ctx, Protocol::PlayerStatus& a_msg)
		{
			a_ctx.Detail(a_msg.location);
			a_ctx.Detail(a_msg.cell);
			a_ctx.Detail(a_msg.powerArmorFrame);
			a_ctx.Detail(a_msg.powerArmorFrameRef);
			if (a_msg.parked) {
				auto& parked = *a_msg.parked;
				if (!a_ctx.Key(parked.base) || !a_ctx.Place(parked.cell, parked.worldspace)) {
					a_msg.parked.reset();
					++a_ctx.out.fields;
					a_ctx.changed = true;
				} else {
					a_ctx.Detail(parked.ref);  // none: found by base and place, as one made in their game
				}
			}
			if (a_msg.seat) {
				if (!a_ctx.Key(a_msg.seat->base)) {
					a_msg.seat.reset();
					++a_ctx.out.fields;
					a_ctx.changed = true;
				} else {
					a_ctx.Detail(a_msg.seat->ref);
				}
			}
			return true;
		}

		inline bool MapLine(Context& a_ctx, Protocol::Line& a_msg)
		{
			if (!a_ctx.Key(a_msg.speaker)) {
				return false;
			}
			// A line whose recording is of a plugin we lack is shown as text only.
			if (!a_ctx.Detail(a_msg.info)) {
				a_msg.response = 0;
				a_msg.voiceType = 0;
			} else {
				a_ctx.Detail(a_msg.voiceType);
			}
			return true;
		}

		inline bool MapFace(Context& a_ctx, Protocol::Face& a_msg)
		{
			if (!a_ctx.Key(a_msg.race)) {
				return false;  // the look only applies to a stand-in of the same race
			}
			a_ctx.Ids(a_msg.headParts);
			a_ctx.Detail(a_msg.hairColor);
			a_ctx.Detail(a_msg.facialHairColor);
			a_ctx.Detail(a_msg.faceDetails);
			a_ctx.Detail(a_msg.skin);  // the race's skin, as without a skin of their own (LooksMenu's overlays name no form)
			return true;
		}

		// An item given by dropping it: its base, every mod and its place, or not at all. A copy without one of its mods
		// would be another item (the giver's legendary, say, as a plain gun), and the receiving game makes a copy only of
		// an item whose base and every mod it has anyway (Drops).
		inline bool MapDrop(Context& a_ctx, Protocol::DroppedItem& a_item)
		{
			if (!a_ctx.Key(a_item.base) || !a_ctx.Place(a_item.cell, a_item.worldspace)) {
				return false;
			}
			return std::ranges::all_of(a_item.mods, [&](Protocol::ItemMod& a_mod) { return a_ctx.Key(a_mod.mod); });
		}

		// A ping: its place, or not at all. For an NPC or an item, the target and its base are details (a ping of one the
		// other game can't name shows as its name at the spot); a friend's target is a player id, no form.
		inline bool MapPing(Context& a_ctx, Protocol::Ping& a_msg)
		{
			if (!a_ctx.Place(a_msg.cell, a_msg.worldspace)) {
				return false;
			}
			if (a_msg.kind == Protocol::PingKind::kActor || a_msg.kind == Protocol::PingKind::kItem) {
				a_ctx.Detail(a_msg.target);
				a_ctx.Detail(a_msg.base);
			}
			return true;
		}

		// A radiant's start: its quest, or not at all. A fill the other side can't name becomes RADIANT_UNSHARED_REF, which
		// no game resolves (a location included): the receiver treats it as one it doesn't have (RadiantRules::MakePlan
		// falls back for an alias each game picks, and leaves the others), never as an empty alias, which a left-out fill or
		// 0 would read as.
		inline bool MapRadiantStart(Context& a_ctx, Protocol::RadiantStart& a_msg)
		{
			if (!a_ctx.Key(a_msg.quest)) {
				return false;
			}
			for (auto& fill : a_msg.fills) {
				if (fill.form == 0 || fill.form == Protocol::RADIANT_UNSHARED_REF) {
					continue;
				}
				if (!a_ctx.Key(fill.form)) {
					fill.form = Protocol::RADIANT_UNSHARED_REF;
					++a_ctx.out.fields;
					a_ctx.changed = true;
				}
			}
			return true;
		}

		inline bool MapWorkshopItem(Context& a_ctx, Protocol::WorkshopItem& a_item)
		{
			return a_ctx.Key(a_item.refId) && a_ctx.Key(a_item.base) && a_ctx.Key(a_item.workshop);
		}

		inline bool MapWorldState(Context& a_ctx, Protocol::WorldState& a_msg)
		{
			a_ctx.Ids(a_msg.deadActors);
			for (auto& change : a_msg.containerChanges) {
				a_ctx.Numbered(change);
			}
			a_ctx.Ids(a_msg.pickedUp);
			a_ctx.Entries(a_msg.refStates, [&](Protocol::RefState& a_state) { return a_ctx.Key(a_state.refId); });
			a_ctx.Entries(a_msg.questStages, [&](Protocol::QuestStage& a_stage) { return a_ctx.Key(a_stage.quest); });
			return true;
		}
	}

	// A packet in the other numbering (see the top). Every message type is here: one added to the enum and not here stops
	// the build (C4062 made an error for this switch; gcc's -Wswitch), so no new message carries raw form IDs by mistake.
#ifdef _MSC_VER
#	pragma warning(push)
#	pragma warning(error : 4062)
#else
#	pragma GCC diagnostic push
#	pragma GCC diagnostic error "-Wswitch"
#endif
	inline Result Translate(std::span<const std::uint8_t> a_packet, const FormIds::Mapper& a_mapper, Way a_way)
	{
		using namespace Protocol;
		using detail::Apply;
		Result out;
		if (a_mapper.Identity()) {
			return out;
		}
		const auto type = PeekType(a_packet);
		if (!type) {
			return out;
		}
		detail::Context ctx{ a_mapper, a_way, out };
		const auto      as = [&](auto a_encode) { return [a_encode, t = *type](const auto& a_msg) { return a_encode(a_msg, t); }; };
		const auto      plain = [](const auto& a_msg) { return Protocol::Encode(a_msg); };
		const auto      typed = as([](const auto& a_msg, MessageType a_type) { return Protocol::Encode(a_msg, a_type); });

		switch (*type) {
		// No form IDs: as they are. The hello goes out before the session's table is known: the server translates its
		// one form ID (the appearance) itself, from the hello's own list.
		case MessageType::kHello:
		case MessageType::kRequestWorldState:
		case MessageType::kReportXp:
		case MessageType::kHeartbeat:
		case MessageType::kRevive:
		case MessageType::kVoice:
		case MessageType::kRename:
		case MessageType::kRequestLoadOrder:
		case MessageType::kWelcome:
		case MessageType::kReject:
		case MessageType::kPlayerLeft:
		case MessageType::kPlayerReloaded:
		case MessageType::kPartyXp:
		case MessageType::kHeartbeatAck:
		case MessageType::kRevived:
		case MessageType::kVoiceRelay:
		case MessageType::kRenamed:
		case MessageType::kLoadOrder:
		case MessageType::kPluginTable:
		// Drops by drop ID and nonce, and the host controls: player ids, ban ids and a password.
		case MessageType::kClaimDrop:
		case MessageType::kDropRecord:
		case MessageType::kDropGone:
		case MessageType::kHostCommand:
		case MessageType::kRequestHostInfo:
		case MessageType::kRemoved:
		case MessageType::kHostInfo:
			break;

		case MessageType::kPlayerState:
			Apply<PlayerState>(a_packet, ctx, DecodePlayerState, [&](PlayerState& m) { return ctx.Place(m.cell, m.worldspace); }, plain);
			break;
		case MessageType::kPlayerStates:
			Apply<PlayerStates>(a_packet, ctx, DecodePlayerStates, [&](PlayerStates& m) {
				ctx.Entries(m.players, [&](RemoteState& a_entry) {
					if (ctx.Place(a_entry.state.cell, a_entry.state.worldspace)) {
						return true;
					}
					ctx.out.placeless.push_back(a_entry.playerId);
					return false;
				});
				return true;
			}, plain);
			break;
		case MessageType::kPlayerJoined:
			Apply<PlayerJoined>(a_packet, ctx, DecodePlayerJoined, [&](PlayerJoined& m) { ctx.Detail(m.appearance); return true; }, plain);
			break;
		case MessageType::kReportDeath:
		case MessageType::kActorDied:
			Apply<ActorDeath>(a_packet, ctx, DecodeActorDeath, [&](ActorDeath& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kReportHealth:
		case MessageType::kActorHealth:
		case MessageType::kReportDamage:
		case MessageType::kActorDamaged:
			Apply<ActorHealth>(a_packet, ctx, DecodeActorHealth, [&](ActorHealth& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kReportContainer:
			Apply<ContainerChange>(a_packet, ctx, DecodeContainerChange, [&](ContainerChange& m) { return ctx.Change(m); }, typed);
			break;
		case MessageType::kContainerChanged:
			Apply<IndexedContainerChange>(a_packet, ctx, DecodeIndexedContainerChange, [&](IndexedContainerChange& m) { ctx.Numbered(m); return true; }, plain);
			break;
		case MessageType::kReportPickup:
		case MessageType::kRefPickedUp:
			Apply<RefPickedUp>(a_packet, ctx, DecodeRefPickedUp, [&](RefPickedUp& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kReportRefState:
		case MessageType::kRefStateChanged:
			Apply<RefState>(a_packet, ctx, DecodeRefState, [&](RefState& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kEquipment:
		case MessageType::kPlayerEquipment:
			Apply<Equipment>(a_packet, ctx, DecodeEquipment, [&](Equipment& m) { return detail::MapEquipment(ctx, m); }, typed);
			break;
		case MessageType::kClaimActors:
			Apply<std::vector<ActorClaim>>(a_packet, ctx, DecodeClaims, [&](std::vector<ActorClaim>& m) {
				ctx.Entries(m, [&](ActorClaim& a_claim) { return ctx.Key(a_claim.refId); });
				return true;
			}, plain);
			break;
		case MessageType::kReleaseActors:
			Apply<std::vector<std::uint32_t>>(a_packet, ctx, DecodeRelease, [&](std::vector<std::uint32_t>& m) { ctx.Ids(m); return true; }, EncodeRelease);
			break;
		case MessageType::kActorOwners:
			Apply<std::vector<ActorOwner>>(a_packet, ctx, DecodeOwners, [&](std::vector<ActorOwner>& m) {
				ctx.Entries(m, [&](ActorOwner& a_owner) { return ctx.Key(a_owner.refId); });
				return true;
			}, plain);
			break;
		case MessageType::kActorStates:
		case MessageType::kActorStatesRelay:
			Apply<std::vector<ActorState>>(a_packet, ctx, DecodeActorStates, [&](std::vector<ActorState>& m) {
				ctx.Entries(m, [&](ActorState& a_state) { return ctx.Key(a_state.refId) && ctx.Place(a_state.cell, a_state.worldspace); });
				return true;
			}, typed);
			break;
		case MessageType::kPlayerHit:
		case MessageType::kPlayerDamaged:
			Apply<PlayerHit>(a_packet, ctx, DecodePlayerHit, [&](PlayerHit& m) { return ctx.Key(m.attacker); }, typed);
			break;
		case MessageType::kReportQuestStage:
		case MessageType::kQuestStage:
			Apply<QuestStage>(a_packet, ctx, DecodeQuestStage, [&](QuestStage& m) { return ctx.Key(m.quest); }, typed);
			break;
		case MessageType::kReportShot:
		case MessageType::kShotFired:
			Apply<Shot>(a_packet, ctx, DecodeShot, [&](Shot& m) {
				if (!ctx.Key(m.refId)) {
					return false;  // an NPC we can't name: never the shooter themselves (refId 0)
				}
				ctx.Detail(m.sound);
				return true;
			}, typed);
			break;
		case MessageType::kReportStatus:
		case MessageType::kPlayerStatus:
			Apply<PlayerStatus>(a_packet, ctx, DecodePlayerStatus, [&](PlayerStatus& m) { return detail::MapStatus(ctx, m); }, typed);
			break;
		case MessageType::kReportTime:
		case MessageType::kWorldTime:
			Apply<WorldTime>(a_packet, ctx, DecodeWorldTime, [&](WorldTime& m) {
				ctx.Detail(m.worldspace);
				ctx.Detail(m.weather);
				return true;
			}, typed);
			break;
		case MessageType::kPing:
		case MessageType::kPinged:
			Apply<Ping>(a_packet, ctx, DecodePing, [&](Ping& m) { return detail::MapPing(ctx, m); }, typed);
			break;
		case MessageType::kReportQuestDone:
		case MessageType::kQuestDone:
			Apply<QuestDone>(a_packet, ctx, DecodeQuestDone, [&](QuestDone& m) { return ctx.Key(m.quest); }, typed);
			break;
		case MessageType::kReportMarkers:
		case MessageType::kMarkersFound:
			Apply<MarkersFound>(a_packet, ctx, DecodeMarkers, [&](MarkersFound& m) {
				ctx.Entries(m.markers, [&](MapMarker& a_marker) { return ctx.Key(a_marker.refId); });
				return true;
			}, typed);
			break;
		case MessageType::kReportLine:
		case MessageType::kLineSpoken:
			Apply<Line>(a_packet, ctx, DecodeLine, [&](Line& m) { return detail::MapLine(ctx, m); }, typed);
			break;
		case MessageType::kReportNpcEquipment:
		case MessageType::kNpcEquipment:
			Apply<NpcEquipment>(a_packet, ctx, DecodeNpcEquipment, [&](NpcEquipment& m) {
				if (!ctx.Key(m.refId)) {
					return false;
				}
				ctx.Ids(m.items);
				return true;
			}, typed);
			break;
		case MessageType::kReportNpcIdentity:
		case MessageType::kNpcIdentity:
			// A slot whose NPC the other side lacks becomes 0 with its bit kept (the roll is not whole there). Without the
			// NPC it was placed as there is no roll to make: the slots go. A faction it lacks is left out.
			Apply<NpcIdentity>(a_packet, ctx, DecodeNpcIdentity, [&](NpcIdentity& m) {
				if (!ctx.Key(m.refId)) {
					return false;
				}
				if (m.originalBase != 0 && !ctx.Detail(m.originalBase)) {
					m.mask = 0;
					m.templates = {};
				}
				for (auto& slot : m.templates) {
					ctx.Detail(slot);
				}
				ctx.Entries(m.factions, [&](FactionRank& a_entry) { return ctx.Key(a_entry.faction); });
				return true;
			}, typed);
			break;
		case MessageType::kReportFace:
		case MessageType::kPlayerFace:
			Apply<Face>(a_packet, ctx, DecodeFace, [&](Face& m) { return detail::MapFace(ctx, m); }, typed);
			break;
		case MessageType::kReportWorkshopItem:
		case MessageType::kWorkshopItem:
			Apply<WorkshopItem>(a_packet, ctx, DecodeWorkshopItem, [&](WorkshopItem& m) { return detail::MapWorkshopItem(ctx, m); }, typed);
			break;
		case MessageType::kWorkshopItems:
			Apply<std::vector<WorkshopItem>>(a_packet, ctx, DecodeWorkshopItems, [&](std::vector<WorkshopItem>& m) {
				ctx.Entries(m, [&](WorkshopItem& a_item) { return detail::MapWorkshopItem(ctx, a_item); });
				return true;
			}, plain);
			break;
		case MessageType::kReportExplosion:
		case MessageType::kExplosion:
			Apply<Explosion>(a_packet, ctx, DecodeExplosion, [&](Explosion& m) { return ctx.Key(m.base) && ctx.Place(m.cell, m.worldspace); }, typed);
			break;
		case MessageType::kReportActivation:
		case MessageType::kActivation:
			Apply<Activation>(a_packet, ctx, DecodeActivation, [&](Activation& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kReportTrap:
			Apply<TrapState>(a_packet, ctx, DecodeTrapState, [&](TrapState& m) { return ctx.Key(m.refId) && ctx.Place(m.cell, m.worldspace); }, plain);
			break;
		case MessageType::kTraps:
			Apply<TrapStates>(a_packet, ctx, DecodeTrapStates, [&](TrapStates& m) {
				ctx.Entries(m.traps, [&](TrapState& a_trap) { return ctx.Key(a_trap.refId) && ctx.Place(a_trap.cell, a_trap.worldspace); });
				return !m.traps.empty();  // a live one whose trap has no ID here is about nothing
			}, plain);
			break;
		case MessageType::kReportSpawn:
		case MessageType::kSpawned:
			// The spawn id is the session's own (runtime range): only what to place, and where.
			Apply<Spawn>(a_packet, ctx, DecodeSpawn, [&](Spawn& m) { return ctx.Key(m.base) && ctx.Place(m.cell, m.worldspace); }, typed);
			break;
		case MessageType::kReportDrop:
		case MessageType::kDropPlaced:
			Apply<DroppedItem>(a_packet, ctx, DecodeDroppedItem, [&](DroppedItem& m) { return detail::MapDrop(ctx, m); }, typed);
			break;
		case MessageType::kDropList:
			// Its `last` stays: the list still ends where the server's does.
			Apply<DropList>(a_packet, ctx, DecodeDropList, [&](DropList& m) {
				ctx.Entries(m.drops, [&](DroppedItem& a_item) { return detail::MapDrop(ctx, a_item); });
				return true;
			}, plain);
			break;
		case MessageType::kReportObjectHold:
		case MessageType::kObjectHeld:
			Apply<ObjectHold>(a_packet, ctx, DecodeObjectHold, [&](ObjectHold& m) { return ctx.Key(m.refId); }, typed);
			break;
		case MessageType::kReportObjectRest:
		case MessageType::kObjectRested:
			Apply<ObjectRest>(a_packet, ctx, DecodeObjectRest, [&](ObjectRest& m) { return ctx.Key(m.refId) && ctx.Place(m.cell, m.worldspace); }, typed);
			break;
		case MessageType::kReportDestruction:
		case MessageType::kDestructions:
			Apply<Destructions>(a_packet, ctx, DecodeDestructions, [&](Destructions& m) {
				ctx.Entries(m.entries, [&](Destruction& a_entry) { return ctx.Key(a_entry.refId) && ctx.Place(a_entry.cell, a_entry.worldspace); });
				return !m.entries.empty();
			}, typed);
			break;
		case MessageType::kReportWorkshopPower:
		case MessageType::kWorkshopPower:
			Apply<PowerStates>(a_packet, ctx, DecodePowerStates, [&](PowerStates& m) {
				ctx.Entries(m.entries, [&](PowerState& a_entry) { return ctx.Key(a_entry.refId); });
				return true;
			}, typed);
			break;
		case MessageType::kReportRadiantStart:
		case MessageType::kRadiantStarted:
			Apply<RadiantStart>(a_packet, ctx, DecodeRadiantStart, [&](RadiantStart& m) { return detail::MapRadiantStart(ctx, m); }, typed);
			break;
		case MessageType::kReportRadiantStage:
		case MessageType::kRadiantStage:
			// origin is a player id, serial that game's own number.
			Apply<RadiantStage>(a_packet, ctx, DecodeRadiantStage, [&](RadiantStage& m) { return ctx.Key(m.quest); }, typed);
			break;
		case MessageType::kWorldState:
			Apply<WorldState>(a_packet, ctx, DecodeWorldState, [&](WorldState& m) { return detail::MapWorldState(ctx, m); }, plain);
			break;
		}
		return out;
	}
#ifdef _MSC_VER
#	pragma warning(pop)
#else
#	pragma GCC diagnostic pop
#endif
}
