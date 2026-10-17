#include "Test.h"

#include "Protocol.h"
#include "ProtocolSamples.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <vector>

// Tests for the wire format in common/Protocol.h: every message round trips, truncated or padded
// packets are rejected, the documented validation rules hold, and random input never crashes.

namespace
{
	using namespace ProtocolSamples;

	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr float Inf = std::numeric_limits<float>::infinity();

	Bytes WithTrailingByte(Bytes a_packet)
	{
		a_packet.push_back(0);
		return a_packet;
	}

	std::string Text(std::size_t a_length, char a_fill = 'x')
	{
		return std::string(a_length, a_fill);
	}
}

// ---- framing -------------------------------------------------------------------

TEST("protocol: every sample packet decodes")
{
	for (const auto& sample : Samples()) {
		if (!sample.decode(sample.packet)) {
			Test::Fail(__FILE__, __LINE__, sample.name + " did not decode");
		}
	}
}

TEST("protocol: every message type has a sample packet")
{
	const auto samples = Samples();
	int        types = 0;
	for (int byte = 0; byte <= 0xFF; ++byte) {
		if (!IsMessageType(static_cast<std::uint8_t>(byte))) {
			continue;
		}
		++types;
		const auto has = std::ranges::any_of(samples, [&](const Sample& a_s) { return a_s.packet[0] == byte; });
		if (!has) {
			Test::Fail(__FILE__, __LINE__, std::format("message type {} has no entry in Samples()", byte));
		}
	}
	for (const auto& sample : samples) {
		if (!IsMessageType(sample.packet[0])) {
			Test::Fail(__FILE__, __LINE__, sample.name + ": its type is not a message type");
		}
	}
	CHECK(types > 0);
}

TEST("protocol: PeekType reads the type byte of every message")
{
	CHECK(!PeekType({}));
	for (const auto& sample : Samples()) {
		const auto type = PeekType(sample.packet);
		if (!type || std::to_underlying(*type) != sample.packet[0]) {
			Test::Fail(__FILE__, __LINE__, sample.name + ": PeekType mismatch");
		}
	}
	CHECK(PeekType(Encode(MakeHello())) == MT::kHello);
	CHECK(PeekType(Encode(Welcome{})) == MT::kWelcome);
	CHECK(PeekType(Encode(WorldState{})) == MT::kWorldState);
	CHECK(PeekType(Encode(IndexedContainerChange{})) == MT::kContainerChanged);
	CHECK(PeekType(EncodeRelease({})) == MT::kReleaseActors);
	CHECK(PeekType(Encode(std::vector<ActorClaim>{})) == MT::kClaimActors);
	CHECK(PeekType(Encode(std::vector<ActorOwner>{})) == MT::kActorOwners);
	CHECK(PeekType(Encode(Heartbeat{}, MT::kHeartbeatAck)) == MT::kHeartbeatAck);
	CHECK(PeekType(Encode(Ping{}, MT::kPinged)) == MT::kPinged);
	const std::uint8_t one[] = { 200 };
	CHECK(PeekType(one) == static_cast<MT>(200));
}

TEST("protocol: values are little-endian after the type byte")
{
	CHECK(Encode(Heartbeat{ 0x11223344 }, MT::kHeartbeat) == (Bytes{ 20, 0x44, 0x33, 0x22, 0x11 }));
	const auto hello = Encode(MakeHello());
	REQUIRE(hello.size() > 7);
	CHECK(hello[0] == 1);
	CHECK(hello[1] == 'F' && hello[2] == '4' && hello[3] == 'M' && hello[4] == 'P');
	CHECK(hello[5] == VERSION && hello[6] == 0);
}

TEST("protocol: every strict prefix of a packet is rejected")
{
	for (const auto& sample : Samples()) {
		for (std::size_t length = 0; length < sample.packet.size(); ++length) {
			if (sample.decode(std::span(sample.packet.data(), length))) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: prefix of {} of {} bytes decoded", sample.name, length, sample.packet.size()));
			}
		}
	}
}

TEST("protocol: a trailing extra byte is rejected")
{
	for (const auto& sample : Samples()) {
		if (sample.rejectsTrailing && sample.decode(WithTrailingByte(sample.packet))) {
			Test::Fail(__FILE__, __LINE__, sample.name + ": trailing byte accepted");
		}
	}
}

// Hello, Welcome and Reject don't check for the end, so a client and server of different versions
// can still read the version and the reason they were turned away.
TEST("protocol: handshake messages still decode with extra trailing bytes")
{
	auto hello = Encode(MakeHello());
	hello.insert(hello.end(), { 1, 2, 3, 4 });
	const auto h = DecodeHello(hello);
	REQUIRE(h);
	CHECK(h->name == "Alice");
	CHECK(h->identity == MakeHello().identity);

	auto welcome = Encode(Welcome{ 3, 4, false, true });
	welcome.push_back(9);
	CHECK(DecodeWelcome(welcome));

	auto reject = Encode(Reject{ "Version mismatch" });
	reject.push_back(9);
	const auto rj = DecodeReject(reject);
	REQUIRE(rj);
	CHECK(rj->reason == "Version mismatch");
}

// ---- round trips ---------------------------------------------------------------

TEST("protocol: Hello round trips, including identity")
{
	const auto in = MakeHello();
	const auto out = DecodeHello(Encode(in));
	REQUIRE(out);
	CHECK(out->magic == MAGIC);
	CHECK(out->version == VERSION);
	CHECK(out->contentHash == in.contentHash);
	CHECK(out->name == in.name);
	CHECK(out->password == in.password);
	CHECK(out->appearance == in.appearance);
	CHECK(out->world.sessionId == in.world.sessionId);
	CHECK(out->world.containerFrom == in.world.containerFrom);
	CHECK(out->identity == in.identity);

	Hello other;
	other.magic = 0x12345678;
	other.version = 7;
	const auto old = DecodeHello(Encode(other));
	REQUIRE(old);
	CHECK(old->magic == 0x12345678);
	CHECK(old->version == 7);
	CHECK(old->name.empty());
	CHECK(old->identity == 0);
}

TEST("protocol: WorldRequest round trips")
{
	const auto out = DecodeWorldRequest(Encode(WorldRequest{ 0xABCDEF0123456789ull, 4321 }));
	REQUIRE(out);
	CHECK(out->sessionId == 0xABCDEF0123456789ull);
	CHECK(out->containerFrom == 4321);
}

namespace
{
	bool SameState(const PlayerState& a, const PlayerState& b)
	{
		return a.sequence == b.sequence && a.cell == b.cell && a.worldspace == b.worldspace && a.x == b.x && a.y == b.y &&
		       a.z == b.z && a.heading == b.heading && a.speed == b.speed && a.moveMode == b.moveMode && a.flags == b.flags && a.sentMs == b.sentMs &&
		       a.direction == b.direction && a.pitch == b.pitch;
	}

	bool SameActorState(const ActorState& a, const ActorState& b)
	{
		return a.refId == b.refId && a.cell == b.cell && a.worldspace == b.worldspace && a.x == b.x && a.y == b.y &&
		       a.z == b.z && a.heading == b.heading && a.speed == b.speed && a.moveMode == b.moveMode && a.flags == b.flags && a.direction == b.direction &&
		       a.gait == b.gait && a.gaitRate == b.gaitRate && a.aimPitch == b.aimPitch;
	}
}

TEST("protocol: PlayerState round trips")
{
	const auto in = MakeState(12);
	const auto out = DecodePlayerState(Encode(in));
	REQUIRE(out);
	CHECK(SameState(*out, in));
}

TEST("protocol: Welcome round trips, including sharedStory")
{
	for (const bool ff : { false, true }) {
		for (const bool story : { false, true }) {
			const auto out = DecodeWelcome(Encode(Welcome{ 42, 0x1122334455667788ull, ff, story }));
			REQUIRE(out);
			CHECK(out->playerId == 42);
			CHECK(out->sessionId == 0x1122334455667788ull);
			CHECK(out->friendlyFire == ff);
			CHECK(out->sharedStory == story);
		}
	}
}

TEST("protocol: Reject, PlayerJoined and PlayerLeft round trip")
{
	const auto reject = DecodeReject(Encode(Reject{ "Wrong password" }));
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password");

	const auto empty = DecodeReject(Encode(Reject{}));
	REQUIRE(empty);
	CHECK(empty->reason.empty());

	const auto joined = DecodePlayerJoined(Encode(PlayerJoined{ 7, "Nick Valentine", 0x00002F25 }));
	REQUIRE(joined);
	CHECK(joined->playerId == 7);
	CHECK(joined->name == "Nick Valentine");
	CHECK(joined->appearance == 0x00002F25);

	const auto left = DecodePlayerLeft(Encode(PlayerLeft{ 0xFFFFFFFF }));
	REQUIRE(left);
	CHECK(left->playerId == 0xFFFFFFFF);
}

TEST("protocol: PlayerStates round trips")
{
	PlayerStates in{ 123456, { { 1, MakeState(1) }, { 2, MakeState(2) }, { 9, MakeState(9) } } };
	const auto out = DecodePlayerStates(Encode(in));
	REQUIRE(out);
	CHECK(out->serverTick == 123456);
	REQUIRE(out->players.size() == 3);
	for (std::size_t i = 0; i < 3; ++i) {
		CHECK(out->players[i].playerId == in.players[i].playerId);
		CHECK(SameState(out->players[i].state, in.players[i].state));
	}

	const auto none = DecodePlayerStates(Encode(PlayerStates{ 5, {} }));
	REQUIRE(none);
	CHECK(none->players.empty());
}

TEST("protocol: ActorDeath, ActorHealth, RefPickedUp round trip with both message types")
{
	for (const auto type : { MT::kReportDeath, MT::kActorDied }) {
		for (const bool killed : { false, true }) {
			const auto packet = Encode(ActorDeath{ REF_B, 4, killed }, type);
			CHECK(PeekType(packet) == type);
			const auto out = DecodeActorDeath(packet);
			REQUIRE(out);
			CHECK(out->refId == REF_B);
			CHECK(out->playerId == 4);
			CHECK(out->killed == killed);
		}
	}
	for (const auto type : { MT::kReportHealth, MT::kActorHealth }) {
		const auto packet = Encode(ActorHealth{ REF_A, 0.625f }, type);
		CHECK(PeekType(packet) == type);
		const auto out = DecodeActorHealth(packet);
		REQUIRE(out);
		CHECK(out->refId == REF_A);
		CHECK(out->health == 0.625f);
	}
	for (const auto type : { MT::kReportPickup, MT::kRefPickedUp }) {
		const auto packet = Encode(RefPickedUp{ REF_B }, type);
		CHECK(PeekType(packet) == type);
		const auto out = DecodeRefPickedUp(packet);
		REQUIRE(out);
		CHECK(out->refId == REF_B);
	}
}

TEST("protocol: ContainerChange and IndexedContainerChange round trip")
{
	const auto change = DecodeContainerChange(Encode(ContainerChange{ REF_A, 0x0000000F, -MAX_ITEM_COUNT }, MT::kReportContainer));
	REQUIRE(change);
	CHECK(change->container == REF_A);
	CHECK(change->item == 0x0000000F);
	CHECK(change->count == -MAX_ITEM_COUNT);

	const auto indexed = DecodeIndexedContainerChange(Encode(IndexedContainerChange{ 0xFFFFFFF0, 3, { REF_B, 0x00012345, 99 } }));
	REQUIRE(indexed);
	CHECK(indexed->index == 0xFFFFFFF0);
	CHECK(indexed->playerId == 3);
	CHECK(indexed->change.container == REF_B);
	CHECK(indexed->change.item == 0x00012345);
	CHECK(indexed->change.count == 99);
}

TEST("protocol: RefState round trips every value")
{
	for (const auto type : { MT::kReportRefState, MT::kRefStateChanged }) {
		for (std::uint8_t open = 0; open <= RefState::kUnknown; ++open) {
			for (std::uint8_t locked = 0; locked <= RefState::kUnknown; ++locked) {
				const RefState in{ REF_A, open, locked };
				const auto     out = DecodeRefState(Encode(in, type));
				REQUIRE(out);
				CHECK(*out == in);
			}
		}
	}
}

TEST("protocol: Equipment round trips")
{
	for (const auto type : { MT::kEquipment, MT::kPlayerEquipment }) {
		Equipment in{ 8, {} };
		for (std::uint32_t i = 0; i < MAX_EQUIPMENT; ++i) {
			in.items.push_back(0x00010000 + i);
		}
		const auto out = DecodeEquipment(Encode(in, type));
		REQUIRE(out);
		CHECK(out->playerId == 8);
		CHECK(out->items == in.items);

		const auto none = DecodeEquipment(Encode(Equipment{ 8, {} }, type));
		REQUIRE(none);
		CHECK(none->items.empty());
		CHECK(none->mods.empty());
	}
}

TEST("protocol: Equipment carries the mods on each item, and only for items it lists")
{
	for (const auto type : { MT::kEquipment, MT::kPlayerEquipment }) {
		Equipment in{ 8, { 0x0000DF42, 0x0001EED7, 0x0000E27B }, {} };
		in.mods.push_back({ 0x0000DF42, 3, { { 0x0004F21D, 0, 0 }, { 0x0004F21F, 1, 2 }, { 0x0004F221, 2, 255 } } });
		in.mods.push_back({ 0x0001EED7, 0xFFFF, {} });  // an instance extra with nothing on it
		ItemMods full{ 0x0000E27B, 7, {} };
		for (std::uint32_t i = 0; i < MAX_ITEM_MODS; ++i) {
			full.mods.push_back({ 0x00050000 + i, static_cast<std::uint8_t>(i), 0 });
		}
		in.mods.push_back(full);
		const auto out = DecodeEquipment(Encode(in, type));
		REQUIRE(out);
		CHECK(*out == in);
	}

	// Left off on encode: the mods of an item not in the list (or one cut off it), a second entry for an item, and mods
	// past the limit.
	Equipment extra{ 1, { 0x0000DF42 }, {} };
	extra.mods.push_back({ 0x0000DF42, 0, { { 0x0004F21D, 0, 0 } } });
	extra.mods.push_back({ 0x0000DF42, 0, { { 0x0004F21F, 0, 0 } } });
	extra.mods.push_back({ 0x0000E27B, 0, { { 0x0004F21F, 0, 0 } } });
	extra.mods[0].mods.resize(MAX_ITEM_MODS + 3, { 0x0004F21D, 0, 0 });
	const auto trimmed = DecodeEquipment(Encode(extra, MT::kEquipment));
	REQUIRE(trimmed);
	REQUIRE(trimmed->mods.size() == 1);
	CHECK(trimmed->mods[0].item == 0x0000DF42);
	CHECK(trimmed->mods[0].mods.size() == MAX_ITEM_MODS);
	Equipment cut{ 1, std::vector<std::uint32_t>(MAX_EQUIPMENT, 0x00011111), {} };
	cut.items.push_back(0x0000DF42);
	cut.mods.push_back({ 0x0000DF42, 0, { { 0x0004F21D, 0, 0 } } });
	const auto cutOut = DecodeEquipment(Encode(cut, MT::kEquipment));
	REQUIRE(cutOut);
	CHECK(cutOut->mods.empty());

	// Rejected on the wire: mods for an item the list doesn't name, a second entry for an item, too many mods, and an
	// entry cut short.
	const auto wire = [](std::uint32_t a_item, int a_entries, std::uint8_t a_modCount, int a_modsWritten) {
		Writer w{ MT::kPlayerEquipment };
		w.U32(1);
		w.U8(1);
		w.U32(0x0000DF42);
		w.U8(static_cast<std::uint8_t>(a_entries));
		for (int e = 0; e < a_entries; ++e) {
			w.U32(a_item);
			w.U16(0);
			w.U8(a_modCount);
			for (int m = 0; m < a_modsWritten; ++m) {
				w.U32(0x0004F21D);
				w.U8(0);
				w.U8(0);
			}
		}
		return w.Data();
	};
	CHECK(DecodeEquipment(wire(0x0000DF42, 1, 2, 2)));
	CHECK(!DecodeEquipment(wire(0x0000E27B, 1, 2, 2)));
	CHECK(!DecodeEquipment(wire(0x0000DF42, 2, 1, 1)));
	CHECK(!DecodeEquipment(wire(0x0000DF42, 1, static_cast<std::uint8_t>(MAX_ITEM_MODS + 1), MAX_ITEM_MODS + 1)));
	CHECK(!DecodeEquipment(wire(0x0000DF42, 1, 3, 2)));
	// The list as an older build sent it, without the mods count: refused rather than read short (a different protocol
	// version is turned away at hello anyway).
	Writer old{ MT::kPlayerEquipment };
	old.U32(1);
	old.U8(1);
	old.U32(0x0000DF42);
	CHECK(!DecodeEquipment(old.Data()));
}

TEST("protocol: actor claim, release and owner lists round trip")
{
	const std::vector<ActorClaim> claims{ { REF_A, ClaimReason::kUnowned }, { REF_B, ClaimReason::kInteract }, { 0x00033333, ClaimReason::kCompanion, true } };
	const auto                    c = DecodeClaims(Encode(claims));
	REQUIRE(c);
	REQUIRE(c->size() == 3);
	for (std::size_t i = 0; i < 3; ++i) {
		CHECK((*c)[i].refId == claims[i].refId);
		CHECK((*c)[i].reason == claims[i].reason);
		CHECK((*c)[i].essential == claims[i].essential);
	}

	const std::vector<std::uint32_t> released{ REF_A, REF_B, 7 };
	const auto                       r = DecodeRelease(EncodeRelease(released));
	REQUIRE(r);
	CHECK(*r == released);

	const std::vector<ActorOwner> owners{ { REF_A, 2 }, { REF_B, 0 } };
	const auto                    o = DecodeOwners(Encode(owners));
	REQUIRE(o);
	REQUIRE(o->size() == 2);
	CHECK((*o)[0].refId == REF_A);
	CHECK((*o)[0].playerId == 2);
	CHECK((*o)[1].refId == REF_B);
	CHECK((*o)[1].playerId == 0);

	const auto empty = DecodeRelease(EncodeRelease({}));
	REQUIRE(empty);
	CHECK(empty->empty());
}

TEST("protocol: an NPC state carries its owner's graph's gait and clip rate, a person's jog and a creature's range alike")
{
	// What NpcSync sends for a jogging person (no rate known for the jog), a running one, and a creature's range.
	for (const auto [gait, rate] : { std::pair<std::uint8_t, std::uint8_t>{ 1, 0 }, { 2, 69 }, { 10, 0 }, { 255, 255 } }) {
		ActorState in = MakeActorState(3);
		in.flags = kOwnGait | kWeaponDrawn;
		in.speed = 0.0f;  // its graph stands while it is carried
		in.gait = gait;
		in.gaitRate = rate;
		const auto out = DecodeActorStates(Encode(std::vector<ActorState>{ in }, MT::kActorStatesRelay));
		REQUIRE(out);
		REQUIRE(out->size() == 1);
		CHECK(SameActorState(out->front(), in));
	}
	// The layout is unchanged by what the gait means: one state is still 39 bytes after the type and count.
	const auto one = Encode(std::vector<ActorState>{ MakeActorState(0) }, MT::kActorStates);
	const auto two = Encode(std::vector<ActorState>{ MakeActorState(0), MakeActorState(1) }, MT::kActorStates);
	CHECK(two.size() - one.size() == 39);
}

TEST("protocol: ActorStates round trip with both message types")
{
	for (const auto type : { MT::kActorStates, MT::kActorStatesRelay }) {
		std::vector<ActorState> in;
		for (std::uint32_t i = 0; i < MAX_ACTORS_PER_PACKET; ++i) {
			in.push_back(MakeActorState(i));
		}
		// One whose owner's copy aims at nothing, and ones aiming straight up and down.
		in[1].aimPitch = NO_AIM;
		in[2].aimPitch = 90;
		in[3].aimPitch = -90;
		const auto packet = Encode(in, type);
		CHECK(PeekType(packet) == type);
		const auto out = DecodeActorStates(packet);
		REQUIRE(out);
		REQUIRE(out->size() == in.size());
		for (std::size_t i = 0; i < in.size(); ++i) {
			CHECK(SameActorState((*out)[i], in[i]));
		}
	}
}

TEST("protocol: PlayerHit round trips byPlayer and attacker")
{
	for (const auto type : { MT::kPlayerHit, MT::kPlayerDamaged }) {
		const auto npc = DecodePlayerHit(Encode(PlayerHit{ 2, 33.25f, false, REF_A }, type));
		REQUIRE(npc);
		CHECK(npc->playerId == 2);
		CHECK(npc->damage == 33.25f);
		CHECK(!npc->byPlayer);
		CHECK(npc->attacker == REF_A);

		const auto player = DecodePlayerHit(Encode(PlayerHit{ 3, 0.0f, true, 0 }, type));
		REQUIRE(player);
		CHECK(player->playerId == 3);
		CHECK(player->damage == 0.0f);
		CHECK(player->byPlayer);
		CHECK(player->attacker == 0);
	}
}

TEST("protocol: Shot round trips, for the player and for an NPC")
{
	for (const auto type : { MT::kReportShot, MT::kShotFired }) {
		const auto self = DecodeShot(Encode(Shot{ 4, 0 }, type));
		REQUIRE(self);
		CHECK(self->playerId == 4);
		CHECK(self->refId == 0);

		const auto npc = DecodeShot(Encode(Shot{ 4, REF_B }, type));
		REQUIRE(npc);
		CHECK(npc->refId == REF_B);
		CHECK(npc->action == ShotAction::kShot);
		CHECK(npc->sound == 0);  // none sent: the receiver uses its own copy's gun sound

		// The shooter's gun sound (a keyword sound mapping) arrives as sent, for the player and an NPC.
		const auto sounded = DecodeShot(Encode(Shot{ 4, 0, ShotAction::kShot, 0x0012A3B4 }, type));
		REQUIRE(sounded);
		CHECK(sounded->sound == 0x0012A3B4);
		const auto npcSounded = DecodeShot(Encode(Shot{ 4, REF_B, ShotAction::kShot, 0x0100F00D }, type));
		REQUIRE(npcSounded);
		CHECK(npcSounded->refId == REF_B);
		CHECK(npcSounded->sound == 0x0100F00D);

		for (const auto action : { ShotAction::kReload, ShotAction::kAimStart, ShotAction::kAimStop, ShotAction::kLightOn, ShotAction::kLightOff, ShotAction::kPoint, ShotAction::kCheer, ShotAction::kClap, ShotAction::kThrow }) {
			const auto other = DecodeShot(Encode(Shot{ 4, 0, action }, type));
			REQUIRE(other);
			CHECK(other->action == action);
		}
		CHECK(!DecodeShot(Encode(Shot{ 4, 0, ShotAction::kMax + 1 }, type)));  // an action we don't know
	}
}

TEST("protocol: PlayerStatus round trips the downed, opening, light and power armor flags")
{
	for (const auto type : { MT::kReportStatus, MT::kPlayerStatus }) {
		for (const bool downed : { false, true }) {
			for (const bool opening : { false, true }) {
				const bool         light = downed != opening;
				const bool         armor = downed && opening;
				const PlayerStatus in{ 5, 0x0001D5E0, 0x0001A2B3, 100, 65535, downed, opening, light, armor, armor ? 0x0017F6E4u : 0u, armor ? REF_B : 0u };
				const auto         packet = Encode(in, type);
				// downed is bit 0, opening bit 1, the Pip-Boy light bit 2 and power armor bit 3 of the flags byte, then the
				// frame base and the frame reference.
				CHECK(packet[packet.size() - 9] == ((downed ? 1 : 0) | (opening ? 2 : 0) | (light ? 4 : 0) | (armor ? 8 : 0)));
				const auto out = DecodePlayerStatus(packet);
				REQUIRE(out);
				CHECK(*out == in);
			}
		}
	}
}

TEST("protocol: PlayerStatus reads only the flag bits it knows")
{
	Writer w{ MT::kPlayerStatus };
	w.U32(1);
	w.U32(2);
	w.U32(3);
	w.U8(50);
	w.U16(10);
	w.U8(0xCE);  // opening set, downed clear, unknown bits set (bit 4, a parked frame, and bit 5, a seat, would have their fields follow)
	w.U32(0);  // no frame
	w.U32(0);  // no frame reference
	const auto out = DecodePlayerStatus(w.Data());
	REQUIRE(out);
	CHECK(!out->downed);
	CHECK(out->opening);
	CHECK(out->powerArmor);  // bit 3 is power armor now
}

TEST("protocol: WorldTime, Ping, Heartbeat, XpGain round trip")
{
	for (const auto type : { MT::kReportTime, MT::kWorldTime }) {
		const auto t = DecodeWorldTime(Encode(WorldTime{ 3, 23.5f, 1234.5f, 0x3C, 0x0001F2A3 }, type));
		REQUIRE(t);
		CHECK(t->playerId == 3);
		CHECK(t->gameHour == 23.5f);
		CHECK(t->daysPassed == 1234.5f);
		CHECK(t->worldspace == 0x3C);
		CHECK(t->weather == 0x0001F2A3);
	}
	for (const auto type : { MT::kPing, MT::kPinged }) {
		const auto p = DecodePing(Encode(Ping{ 6, 0x0001A2B3, 0, -1.5f, 2.5f, -3.5f }, type));
		REQUIRE(p);
		CHECK(p->playerId == 6);
		CHECK(p->cell == 0x0001A2B3);
		CHECK(p->worldspace == 0);
		CHECK(p->x == -1.5f);
		CHECK(p->y == 2.5f);
		CHECK(p->z == -3.5f);
	}
	for (const auto type : { MT::kHeartbeat, MT::kHeartbeatAck }) {
		const auto h = DecodeHeartbeat(Encode(Heartbeat{ 0xCAFEBABE }, type));
		REQUIRE(h);
		CHECK(h->token == 0xCAFEBABE);
	}
	for (const auto type : { MT::kReportXp, MT::kPartyXp }) {
		const auto x = DecodeXpGain(Encode(XpGain{ 2, 12.75f }, type));
		REQUIRE(x);
		CHECK(x->playerId == 2);
		CHECK(x->xp == 12.75f);
	}
}

TEST("protocol: Line round trips for the player and for an NPC")
{
	for (const auto type : { MT::kReportLine, MT::kLineSpoken }) {
		const auto npc = DecodeLine(Encode(Line{ 2, REF_A, "Hey, you got a minute?" }, type));
		REQUIRE(npc);
		CHECK(npc->info == 0);
		CHECK(npc->response == 0);
		CHECK(npc->voiceType == 0);
		const auto voiced = DecodeLine(Encode(Line{ 2, REF_A, "Hey, you got a minute?", 0x0005E686, 2, 0x0002F7A1 }, type));
		REQUIRE(voiced);
		CHECK(voiced->info == 0x0005E686);
		CHECK(voiced->response == 2);
		CHECK(voiced->voiceType == 0x0002F7A1);
		CHECK(voiced->text == "Hey, you got a minute?");
		const auto own = DecodeLine(Encode(Line{ 2, 0, "Hi", 0x000A1B2C, 255 }, type));  // a player's own line, its voice type left to the hearer
		REQUIRE(own);
		CHECK(own->info == 0x000A1B2C);
		CHECK(own->response == 255);
		CHECK(own->voiceType == 0);
		CHECK(npc->playerId == 2);
		CHECK(npc->speaker == REF_A);
		CHECK(npc->text == "Hey, you got a minute?");

		const auto self = DecodeLine(Encode(Line{ 2, 0, "Hi" }, type));
		REQUIRE(self);
		CHECK(self->speaker == 0);
		CHECK(self->text == "Hi");
	}
}

TEST("protocol: Markers round trip")
{
	for (const auto type : { MT::kReportMarkers, MT::kMarkersFound }) {
		MarkersFound in{ 0, {} };
		for (std::uint32_t i = 0; i < MAX_MARKERS_PER_PACKET; ++i) {
			in.markers.push_back({ 0x00010000 + i, static_cast<std::uint8_t>(1 + i % 3) });
		}
		const auto out = DecodeMarkers(Encode(in, type));
		REQUIRE(out);
		CHECK(out->playerId == 0);
		REQUIRE(out->markers.size() == in.markers.size());
		for (std::size_t i = 0; i < in.markers.size(); ++i) {
			CHECK(out->markers[i].refId == in.markers[i].refId);
			CHECK(out->markers[i].flags == in.markers[i].flags);
		}
	}
}

TEST("protocol: Voice round trips")
{
	for (const auto type : { MT::kVoice, MT::kVoiceRelay }) {
		VoiceData in{ 9, {} };
		for (std::size_t i = 0; i < MAX_VOICE_BYTES; ++i) {
			in.data.push_back(static_cast<std::uint8_t>(i * 7));
		}
		const auto out = DecodeVoice(Encode(in, type));
		REQUIRE(out);
		CHECK(out->playerId == 9);
		CHECK(out->data == in.data);

		const auto one = DecodeVoice(Encode(VoiceData{ 1, { 0 } }, type));
		REQUIRE(one);
		CHECK(one->data == Bytes{ 0 });
	}
}

TEST("protocol: Revive, QuestDone, QuestStage round trip")
{
	for (const auto type : { MT::kRevive, MT::kRevived }) {
		const auto r = DecodeRevive(Encode(Revive{ 77 }, type));
		REQUIRE(r);
		CHECK(r->playerId == 77);
	}
	for (const auto type : { MT::kReportQuestDone, MT::kQuestDone }) {
		const auto q = DecodeQuestDone(Encode(QuestDone{ 3, 0x0001ED86 }, type));
		REQUIRE(q);
		CHECK(q->playerId == 3);
		CHECK(q->quest == 0x0001ED86);
	}
	for (const auto type : { MT::kReportQuestStage, MT::kQuestStage }) {
		const QuestStage in{ 0x0001ED86, 65535, 9 };
		const auto       q = DecodeQuestStage(Encode(in, type));
		REQUIRE(q);
		CHECK(*q == in);
		CHECK(q->playerId == 9);
	}
}

TEST("protocol: WorldState round trips, with container indices and player IDs")
{
	const auto in = MakeWorldState();
	const auto out = DecodeWorldState(Encode(in));
	REQUIRE(out);
	CHECK(out->deadActors == in.deadActors);
	CHECK(out->containerFirstIndex == in.containerFirstIndex);
	REQUIRE(out->containerChanges.size() == in.containerChanges.size());
	for (std::size_t i = 0; i < in.containerChanges.size(); ++i) {
		const auto& a = out->containerChanges[i];
		const auto& b = in.containerChanges[i];
		CHECK(a.index == in.containerFirstIndex + i);  // indices aren't sent: numbered from the first
		CHECK(a.playerId == b.playerId);
		CHECK(a.change.container == b.change.container);
		CHECK(a.change.item == b.change.item);
		CHECK(a.change.count == b.change.count);
	}
	CHECK(out->pickedUp == in.pickedUp);
	CHECK(out->refStates == in.refStates);
	CHECK(out->questStages == in.questStages);

	const auto empty = DecodeWorldState(Encode(WorldState{}));
	REQUIRE(empty);
	CHECK(empty->deadActors.empty());
	CHECK(empty->containerChanges.empty());
	CHECK(empty->pickedUp.empty());
	CHECK(empty->refStates.empty());
	CHECK(empty->questStages.empty());
}

TEST("protocol: WorldState container indices are numbered from containerFirstIndex, not from what the sender put in")
{
	WorldState in;
	in.containerFirstIndex = 50;
	in.containerChanges = { { 999, 1, { REF_A, 1, 1 } }, { 3, 2, { REF_A, 1, 1 } } };
	const auto out = DecodeWorldState(Encode(in));
	REQUIRE(out);
	REQUIRE(out->containerChanges.size() == 2);
	CHECK(out->containerChanges[0].index == 50);
	CHECK(out->containerChanges[1].index == 51);
}

// ---- validation ----------------------------------------------------------------

TEST("protocol: IsShareableRef excludes 0, the player and runtime references")
{
	CHECK(!IsShareableRef(0));
	CHECK(!IsShareableRef(PLAYER_REF_ID));
	CHECK(!IsShareableRef(RUNTIME_REF));
	CHECK(!IsShareableRef(0xFF000000));
	CHECK(!IsShareableRef(0xFFFFFFFF));
	CHECK(IsShareableRef(1));
	CHECK(IsShareableRef(0x15));
	CHECK(IsShareableRef(REF_A));
	CHECK(IsShareableRef(0xFE000800));  // light plugin space
}

TEST("protocol: strings are truncated to their maximum on encode")
{
	Hello hello = MakeHello();
	hello.name = Text(MAX_NAME_LENGTH + 10, 'n');
	hello.password = Text(MAX_PASSWORD_LENGTH + 10, 'p');
	const auto h = DecodeHello(Encode(hello));
	REQUIRE(h);
	CHECK(h->name == Text(MAX_NAME_LENGTH, 'n'));
	CHECK(h->password == Text(MAX_PASSWORD_LENGTH, 'p'));
	CHECK(h->identity == hello.identity);  // fields after the strings are still in place

	const auto r = DecodeReject(Encode(Reject{ Text(1000, 'r') }));
	REQUIRE(r);
	CHECK(r->reason == Text(MAX_REASON_LENGTH, 'r'));

	const auto j = DecodePlayerJoined(Encode(PlayerJoined{ 1, Text(300, 'j'), 5 }));
	REQUIRE(j);
	CHECK(j->name == Text(MAX_NAME_LENGTH, 'j'));
	CHECK(j->appearance == 5);

	const auto l = DecodeLine(Encode(Line{ 1, 0, Text(MAX_LINE_LENGTH + 50, 'l') }, MT::kReportLine));
	REQUIRE(l);
	CHECK(l->text == Text(MAX_LINE_LENGTH, 'l'));

	const auto exact = DecodeLine(Encode(Line{ 1, 0, Text(MAX_LINE_LENGTH, 'e') }, MT::kReportLine));
	REQUIRE(exact);
	CHECK(exact->text.size() == MAX_LINE_LENGTH);
}

TEST("protocol: strings longer than their maximum on the wire are rejected")
{
	Writer hello{ MT::kHello };
	hello.U32(MAGIC);
	hello.U16(VERSION);
	hello.U32(0);
	hello.U8(static_cast<std::uint8_t>(MAX_NAME_LENGTH + 1));
	for (std::size_t i = 0; i <= MAX_NAME_LENGTH; ++i) {
		hello.U8('a');
	}
	hello.U8(0);  // password
	hello.U32(0);
	hello.U64(0);
	hello.U32(0);
	hello.U64(0);
	CHECK(!DecodeHello(hello.Data()));

	Writer reject{ MT::kReject };
	reject.U8(static_cast<std::uint8_t>(MAX_REASON_LENGTH + 1));
	for (std::size_t i = 0; i <= MAX_REASON_LENGTH; ++i) {
		reject.U8('a');
	}
	CHECK(!DecodeReject(reject.Data()));

	Writer junk{ MT::kReject };  // control characters in a reason are dropped before anyone sees it
	junk.U8(4);
	junk.U8('o'), junk.U8('k'), junk.U8('\n'), junk.U8(0);
	REQUIRE(DecodeReject(junk.Data()));
	CHECK(DecodeReject(junk.Data())->reason == "ok");

	Writer line{ MT::kLineSpoken };
	line.U32(1);
	line.U32(0);
	line.U8(static_cast<std::uint8_t>(MAX_LINE_LENGTH + 1));
	for (std::size_t i = 0; i <= MAX_LINE_LENGTH; ++i) {
		line.U8('a');
	}
	CHECK(!DecodeLine(line.Data()));
}

TEST("protocol: Line rejects empty text and speakers that aren't shared")
{
	CHECK(!DecodeLine(Encode(Line{ 1, 0, "" }, MT::kReportLine)));
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "" }, MT::kLineSpoken)));
	CHECK(!DecodeLine(Encode(Line{ 1, PLAYER_REF_ID, "hi" }, MT::kReportLine)));
	CHECK(!DecodeLine(Encode(Line{ 1, RUNTIME_REF, "hi" }, MT::kReportLine)));
}

TEST("protocol: Line names its recording whole or not at all")
{
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", 0x0002A2B3, 0 }, MT::kReportLine)));                 // an info with no response
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", 0, 1 }, MT::kReportLine)));                          // a response with no info
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", 0, 0, 0x0002F7A1 }, MT::kLineSpoken)));             // a voice with no line
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", RUNTIME_REF, 1 }, MT::kReportLine)));                // a line made at runtime
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", 0x0002A2B3, 1, RUNTIME_REF }, MT::kLineSpoken)));    // a voice made at runtime
	CHECK(!DecodeLine(Encode(Line{ 1, REF_A, "hi", PLAYER_REF_ID, 1 }, MT::kReportLine)));
	CHECK(DecodeLine(Encode(Line{ 1, REF_A, "hi", 0x0002A2B3, 1, 0x0002F7A1 }, MT::kLineSpoken)));

	Writer old{ MT::kLineSpoken };  // a line as protocol 30 sent it (the topic and nothing more) is refused
	old.U32(1);
	old.U32(REF_A);
	old.U8(2);
	old.U8('h'), old.U8('i');
	old.U32(0x0005E686);
	CHECK(!DecodeLine(old.Data()));
}

TEST("protocol: PlayerStatus health over 100 is rejected")
{
	CHECK(DecodePlayerStatus(Encode(PlayerStatus{ 1, 0, 0, 0, 1 }, MT::kReportStatus)));
	CHECK(DecodePlayerStatus(Encode(PlayerStatus{ 1, 0, 0, 100, 1 }, MT::kReportStatus)));
	CHECK(!DecodePlayerStatus(Encode(PlayerStatus{ 1, 0, 0, 101, 1 }, MT::kReportStatus)));
	CHECK(!DecodePlayerStatus(Encode(PlayerStatus{ 1, 0, 0, 255, 1 }, MT::kPlayerStatus)));
}

TEST("protocol: PlayerStatus frame reference is one every game shares, or none")
{
	PlayerStatus status{ 1, 0, 0, 100, 1, false, false, false, true, 0x0017F6E4, REF_A };
	CHECK(DecodePlayerStatus(Encode(status, MT::kReportStatus)));
	status.powerArmorFrameRef = 0;  // a frame made in their game
	CHECK(DecodePlayerStatus(Encode(status, MT::kReportStatus)));
	status.powerArmorFrameRef = RUNTIME_REF;  // another reference, or none, in every other game
	CHECK(!DecodePlayerStatus(Encode(status, MT::kReportStatus)));
	status.powerArmorFrameRef = PLAYER_REF_ID;
	CHECK(!DecodePlayerStatus(Encode(status, MT::kPlayerStatus)));
}

TEST("protocol: WorldTime rejects bad hours and day counts")
{
	const auto ok = [](float a_hour, float a_days) {
		return DecodeWorldTime(Encode(WorldTime{ 1, a_hour, a_days, 0, 0 }, MT::kReportTime)).has_value();
	};
	CHECK(ok(0.0f, 0.0f));
	CHECK(ok(23.999f, 1.0e6f));
	CHECK(!ok(24.0f, 1.0f));
	CHECK(!ok(25.0f, 1.0f));
	CHECK(!ok(-0.01f, 1.0f));
	CHECK(!ok(NaN, 1.0f));
	CHECK(!ok(Inf, 1.0f));
	CHECK(!ok(-Inf, 1.0f));
	CHECK(!ok(12.0f, NaN));
	CHECK(!ok(12.0f, Inf));
	CHECK(!ok(12.0f, -1.0f));
	CHECK(!ok(12.0f, 1.5e6f));
}

TEST("protocol: Ping rejects non-finite positions")
{
	CHECK(DecodePing(Encode(Ping{ 1, 0, 0x3C, 0, 0, 0 }, MT::kPing)));
	for (const float bad : { NaN, Inf, -Inf }) {
		CHECK(!DecodePing(Encode(Ping{ 1, 0, 0x3C, bad, 0, 0 }, MT::kPing)));
		CHECK(!DecodePing(Encode(Ping{ 1, 0, 0x3C, 0, bad, 0 }, MT::kPing)));
		CHECK(!DecodePing(Encode(Ping{ 1, 0, 0x3C, 0, 0, bad }, MT::kPinged)));
	}
}

TEST("protocol: XpGain must be positive, finite and at most MAX_XP_SHARE")
{
	const auto ok = [](float a_xp) { return DecodeXpGain(Encode(XpGain{ 1, a_xp }, MT::kReportXp)).has_value(); };
	CHECK(ok(0.001f));
	CHECK(ok(MAX_XP_SHARE));
	CHECK(!ok(0.0f));
	CHECK(!ok(-0.0f));
	CHECK(!ok(-5.0f));
	CHECK(!ok(MAX_XP_SHARE + 1.0f));
	CHECK(!ok(NaN));
	CHECK(!ok(Inf));
	CHECK(!ok(-Inf));
}

TEST("protocol: non-finite positions, health and damage are rejected")
{
	for (const float bad : { NaN, Inf, -Inf }) {
		auto state = MakeState(1);
		state.x = bad;
		CHECK(!DecodePlayerState(Encode(state)));
		state = MakeState(1);
		state.heading = bad;
		CHECK(!DecodePlayerState(Encode(state)));
		state = MakeState(1);
		state.speed = bad;
		CHECK(!DecodePlayerState(Encode(state)));

		auto broken = MakeState(2);
		broken.z = bad;
		CHECK(!DecodePlayerStates(Encode(PlayerStates{ 1, { { 1, MakeState(1) }, { 2, broken } } })));

		auto actor = MakeActorState(1);
		actor.y = bad;
		CHECK(!DecodeActorStates(Encode(std::vector<ActorState>{ MakeActorState(0), actor }, MT::kActorStates)));

		CHECK(!DecodeActorHealth(Encode(ActorHealth{ REF_A, bad }, MT::kReportHealth)));
		CHECK(!DecodePlayerHit(Encode(PlayerHit{ 1, bad, false, REF_A }, MT::kPlayerHit)));
	}
}

TEST("protocol: PlayerHit checks the attacker against byPlayer")
{
	const auto ok = [](float a_damage, bool a_byPlayer, std::uint32_t a_attacker) {
		return DecodePlayerHit(Encode(PlayerHit{ 1, a_damage, a_byPlayer, a_attacker }, MT::kPlayerHit)).has_value();
	};
	CHECK(ok(10.0f, true, 0));
	CHECK(ok(10.0f, false, REF_A));
	// Hit by a player: there is no NPC attacker.
	CHECK(!ok(10.0f, true, REF_A));
	CHECK(!ok(10.0f, true, PLAYER_REF_ID));
	// Hit by an NPC: it must be one everyone has, or a spawn (one made at runtime, shared under a spawn id).
	CHECK(!ok(10.0f, false, 0));
	CHECK(!ok(10.0f, false, PLAYER_REF_ID));
	CHECK(ok(10.0f, false, RUNTIME_REF));
	CHECK(!ok(10.0f, false, 0xFF000000));
	CHECK(!ok(-1.0f, false, REF_A));
}

TEST("protocol: Shot and QuestDone reject references that aren't shared")
{
	CHECK(!DecodeShot(Encode(Shot{ 1, PLAYER_REF_ID }, MT::kReportShot)));
	CHECK(DecodeShot(Encode(Shot{ 1, RUNTIME_REF }, MT::kShotFired)));  // a spawn's, under its spawn id
	CHECK(!DecodeShot(Encode(Shot{ 1, 0xFF000000 }, MT::kShotFired)));
	CHECK(!DecodeQuestDone(Encode(QuestDone{ 1, 0 }, MT::kReportQuestDone)));
	CHECK(!DecodeQuestDone(Encode(QuestDone{ 1, PLAYER_REF_ID }, MT::kQuestDone)));
	CHECK(!DecodeQuestDone(Encode(QuestDone{ 1, RUNTIME_REF }, MT::kQuestDone)));
}

TEST("protocol: Voice rejects empty data and more than MAX_VOICE_BYTES")
{
	CHECK(!DecodeVoice(Encode(VoiceData{ 1, {} }, MT::kVoice)));
	CHECK(!DecodeVoice(Encode(VoiceData{ 1, Bytes(MAX_VOICE_BYTES + 1, 7) }, MT::kVoice)));
	CHECK(!DecodeVoice(Encode(VoiceData{ 1, Bytes(70000, 7) }, MT::kVoiceRelay)));  // over the 16-bit length too
	CHECK(DecodeVoice(Encode(VoiceData{ 1, Bytes(MAX_VOICE_BYTES, 7) }, MT::kVoice)));
}

TEST("protocol: Markers drop entries without flags or with unshared references")
{
	MarkersFound in{ 3, {
		                    { REF_A, 1 },
		                    { REF_B, 0 },           // no flags: dropped
		                    { 0, 1 },               // not a reference: dropped
		                    { PLAYER_REF_ID, 2 },   // the player: dropped
		                    { RUNTIME_REF, 3 },     // runtime-created: dropped
		                    { 0x00012345, 0xFC },   // only unknown bits: dropped
		                    { 0x00054321, 0xFE },   // unknown bits are cleared
		                } };
	const auto out = DecodeMarkers(Encode(in, MT::kReportMarkers));
	REQUIRE(out);
	CHECK(out->playerId == 3);
	REQUIRE(out->markers.size() == 2);
	CHECK(out->markers[0].refId == REF_A);
	CHECK(out->markers[0].flags == 1);
	CHECK(out->markers[1].refId == 0x00054321);
	CHECK(out->markers[1].flags == 2);
}

TEST("protocol: Markers over MAX_MARKERS_PER_PACKET are truncated on encode and rejected on the wire")
{
	MarkersFound in{ 1, {} };
	for (std::uint32_t i = 0; i < MAX_MARKERS_PER_PACKET + 50; ++i) {
		in.markers.push_back({ 0x00010000 + i, 1 });
	}
	const auto out = DecodeMarkers(Encode(in, MT::kMarkersFound));
	REQUIRE(out);
	CHECK(out->markers.size() == MAX_MARKERS_PER_PACKET);

	Writer w{ MT::kMarkersFound };
	w.U32(1);
	w.U16(static_cast<std::uint16_t>(MAX_MARKERS_PER_PACKET + 1));
	for (std::uint32_t i = 0; i <= MAX_MARKERS_PER_PACKET; ++i) {
		w.U32(0x00010000 + i);
		w.U8(1);
	}
	CHECK(!DecodeMarkers(w.Data()));
}

TEST("protocol: lists over their limit are truncated on encode and rejected on the wire")
{
	// Equipment
	Equipment equipment{ 1, std::vector<std::uint32_t>(MAX_EQUIPMENT + 5, 0x00011111) };
	const auto e = DecodeEquipment(Encode(equipment, MT::kEquipment));
	REQUIRE(e);
	CHECK(e->items.size() == MAX_EQUIPMENT);

	Writer ew{ MT::kPlayerEquipment };
	ew.U32(1);
	ew.U8(static_cast<std::uint8_t>(MAX_EQUIPMENT + 1));
	for (std::size_t i = 0; i <= MAX_EQUIPMENT; ++i) {
		ew.U32(0x00011111);
	}
	ew.U8(0);
	CHECK(!DecodeEquipment(ew.Data()));

	// Actor lists
	const auto r = DecodeRelease(EncodeRelease(std::vector<std::uint32_t>(MAX_ACTORS_PER_PACKET + 10, REF_A)));
	REQUIRE(r);
	CHECK(r->size() == MAX_ACTORS_PER_PACKET);

	const auto s = DecodeActorStates(Encode(std::vector<ActorState>(MAX_ACTORS_PER_PACKET + 1, MakeActorState(0)), MT::kActorStates));
	REQUIRE(s);
	CHECK(s->size() == MAX_ACTORS_PER_PACKET);

	Writer rw{ MT::kReleaseActors };
	rw.U8(static_cast<std::uint8_t>(MAX_ACTORS_PER_PACKET + 1));
	for (std::size_t i = 0; i <= MAX_ACTORS_PER_PACKET; ++i) {
		rw.U32(REF_A);
	}
	CHECK(!DecodeRelease(rw.Data()));

	// Player states: up to 255 per packet.
	PlayerStates many{ 1, {} };
	for (std::uint32_t i = 0; i < 300; ++i) {
		many.players.push_back({ i, MakeState(i) });
	}
	const auto p = DecodePlayerStates(Encode(many));
	REQUIRE(p);
	CHECK(p->players.size() == 255);
}

TEST("protocol: claims with an unknown reason are rejected")
{
	Writer w{ MT::kClaimActors };
	w.U8(2);
	w.U32(REF_A);
	w.U8(std::to_underlying(ClaimReason::kCompanion));
	w.U8(0);
	w.U32(REF_B);
	w.U8(std::to_underlying(ClaimReason::kCompanion) + 1);
	w.U8(0);
	CHECK(!DecodeClaims(w.Data()));
}

TEST("protocol: a claim's essential mark is 0 or 1, and one without it is refused")
{
	const auto claim = [](std::uint8_t a_essential) {
		Writer w{ MT::kClaimActors };
		w.U8(1);
		w.U32(REF_A);
		w.U8(std::to_underlying(ClaimReason::kCompanion));
		w.U8(a_essential);
		return w.Data();
	};
	REQUIRE(DecodeClaims(claim(0)));
	CHECK(!DecodeClaims(claim(0))->front().essential);
	REQUIRE(DecodeClaims(claim(1)));
	CHECK(DecodeClaims(claim(1))->front().essential);
	CHECK(!DecodeClaims(claim(2)));
	// What protocol 29 sent: no mark.
	Writer old{ MT::kClaimActors };
	old.U8(1);
	old.U32(REF_A);
	old.U8(std::to_underlying(ClaimReason::kCompanion));
	CHECK(!DecodeClaims(old.Data()));
}

TEST("protocol: RefState values above kUnknown are rejected")
{
	CHECK(!DecodeRefState(Encode(RefState{ REF_A, 3, RefState::kNo }, MT::kReportRefState)));
	CHECK(!DecodeRefState(Encode(RefState{ REF_A, RefState::kNo, 3 }, MT::kRefStateChanged)));

	WorldState w;
	w.refStates = { { REF_A, RefState::kYes, 200 } };
	CHECK(!DecodeWorldState(Encode(w)));
}

TEST("protocol: an indexed container change must be a valid change")
{
	const auto ok = [](ContainerChange a_change) {
		return DecodeIndexedContainerChange(Encode(IndexedContainerChange{ 1, 1, a_change })).has_value();
	};
	CHECK(ok({ REF_A, 0x0F, 1 }));
	CHECK(ok({ REF_A, 0x0F, MAX_ITEM_COUNT }));
	CHECK(ok({ REF_A, 0x0F, -MAX_ITEM_COUNT }));
	CHECK(!ok({ 0, 0x0F, 1 }));
	CHECK(!ok({ PLAYER_REF_ID, 0x0F, 1 }));
	CHECK(!ok({ RUNTIME_REF, 0x0F, 1 }));
	CHECK(!ok({ REF_A, 0, 1 }));
	CHECK(!ok({ REF_A, 0x0F, 0 }));
	CHECK(!ok({ REF_A, 0x0F, MAX_ITEM_COUNT + 1 }));
	CHECK(!ok({ REF_A, 0x0F, -MAX_ITEM_COUNT - 1 }));
	CHECK(!ok({ REF_A, 0x0F, std::numeric_limits<std::int32_t>::min() }));

	CHECK(IsValid(ContainerChange{ REF_A, 0x0F, -1 }));
	CHECK(!IsValid(ContainerChange{ REF_A, 0x0F, 0 }));
}

TEST("protocol: WorldState lists over MAX_WORLD_STATE_ACTORS are truncated on encode and rejected on the wire")
{
	WorldState big;
	big.deadActors.assign(MAX_WORLD_STATE_ACTORS + 1, REF_A);
	const auto out = DecodeWorldState(Encode(big));
	REQUIRE(out);
	CHECK(out->deadActors.size() == MAX_WORLD_STATE_ACTORS);

	// Claims a huge list but carries nothing: rejected before reading (or allocating) it.
	for (int list = 0; list < 5; ++list) {
		Writer w{ MT::kWorldState };
		const auto count = [&](int a_index) { return static_cast<std::uint32_t>(a_index == list ? MAX_WORLD_STATE_ACTORS + 1 : 0); };
		w.U32(count(0));   // dead actors
		w.U32(0);          // first container index
		w.U32(count(1));   // container changes
		w.U32(count(2));   // picked up
		w.U32(count(3));   // ref states
		w.U32(count(4));   // quest stages
		CHECK(!DecodeWorldState(w.Data()));

		Writer huge{ MT::kWorldState };
		huge.U32(list == 0 ? 0xFFFFFFFF : 0);
		huge.U32(0);
		huge.U32(list == 1 ? 0xFFFFFFFF : 0);
		huge.U32(list == 2 ? 0xFFFFFFFF : 0);
		huge.U32(list == 3 ? 0xFFFFFFFF : 0);
		huge.U32(list == 4 ? 0xFFFFFFFF : 0);
		CHECK(!DecodeWorldState(huge.Data()));
	}
}

TEST("protocol: list counts larger than the data are rejected")
{
	Writer equipment{ MT::kEquipment };
	equipment.U32(1);
	equipment.U8(3);
	equipment.U32(0x00011111);
	equipment.U8(0);
	CHECK(!DecodeEquipment(equipment.Data()));

	Writer markers{ MT::kReportMarkers };
	markers.U32(1);
	markers.U16(2);
	markers.U32(REF_A);
	markers.U8(1);
	CHECK(!DecodeMarkers(markers.Data()));

	Writer voice{ MT::kVoice };
	voice.U32(1);
	voice.U16(10);
	voice.U8(1);
	CHECK(!DecodeVoice(voice.Data()));

	Writer states{ MT::kPlayerStates };
	states.U32(1);
	states.U8(2);
	states.U32(1);
	WriteState(states, MakeState(1));
	CHECK(!DecodePlayerStates(states.Data()));
}

// ---- robustness ----------------------------------------------------------------

TEST("protocol: decoders survive random bytes")
{
	std::mt19937 rng{ 0xF4F4F4F4 };
	std::size_t  decoded = 0;
	for (const auto& sample : Samples()) {
		for (int round = 0; round < 3000; ++round) {
			Bytes buffer(rng() % 301);
			for (auto& byte : buffer) {
				byte = static_cast<std::uint8_t>(rng());
			}
			if (!buffer.empty()) {
				buffer[0] = sample.packet[0];
			}
			decoded += sample.decode(buffer) ? 1 : 0;
		}
	}
	// Nothing to check beyond not crashing: random data may or may not happen to be valid.
	CHECK(decoded < Samples().size() * 3000);
}

TEST("protocol: decoders survive corrupted valid packets")
{
	std::mt19937 rng{ 1234 };
	for (const auto& sample : Samples()) {
		for (int round = 0; round < 2000; ++round) {
			Bytes      buffer = sample.packet;
			const auto flips = 1 + rng() % 4;
			for (std::uint32_t i = 0; i < flips && buffer.size() > 1; ++i) {
				const auto at = 1 + rng() % (buffer.size() - 1);
				buffer[at] = static_cast<std::uint8_t>(rng());
			}
			if (rng() % 4 == 0) {
				buffer.resize(rng() % (buffer.size() + 8));
			}
			(void)sample.decode(buffer);
		}
	}
	CHECK(true);
}

TEST("protocol: WorkshopItem round trips, and refuses a placement without a base or a bad scale")
{
	WorkshopItem item;
	item.playerId = 2, item.refId = 0xFF000042, item.base = 0x0001F2A1, item.workshop = REF_A, item.scale = 0.5f;
	item.position[0] = 1.0f, item.position[1] = 2.0f, item.position[2] = 3.0f, item.rotation[2] = 1.5f;
	const auto back = DecodeWorkshopItem(Encode(item, MT::kWorkshopItem));
	REQUIRE(back);
	CHECK(*back == item);

	auto scrapped = item;
	scrapped.op = WorkshopOp::kScrapped, scrapped.base = 0;
	CHECK(DecodeWorkshopItem(Encode(scrapped, MT::kReportWorkshopItem)));  // scrapping needs no base

	auto baseless = item;
	baseless.base = 0;
	CHECK(!DecodeWorkshopItem(Encode(baseless, MT::kReportWorkshopItem)));
	auto flat = item;
	flat.scale = 0.0f;
	CHECK(!DecodeWorkshopItem(Encode(flat, MT::kReportWorkshopItem)));
	auto unknownOp = item;
	unknownOp.op = 7;
	CHECK(!DecodeWorkshopItem(Encode(unknownOp, MT::kReportWorkshopItem)));
}

TEST("protocol: Hello carries the load order, capped, and refuses too many plugins")
{
	auto hello = MakeHello();
	const auto back = DecodeHello(Encode(hello));
	REQUIRE(back);
	CHECK((back->plugins == std::vector<std::string>{ "Fallout4.esm", "DLCCoast.esm", "MyMod.esp" }));

	hello.plugins.assign(MAX_PLUGINS + 20, "x.esp");
	const auto capped = DecodeHello(Encode(hello));
	REQUIRE(capped);
	CHECK(capped->plugins.size() == MAX_PLUGINS);

	hello.plugins.clear();
	auto packet = Encode(hello);
	packet.back() = static_cast<std::uint8_t>(MAX_PLUGINS + 1);  // the count byte is last when the list is empty
	CHECK(!DecodeHello(packet));
}

TEST("protocol: LoadOrderDifference names what only one side has, or the order")
{
	const std::vector<std::string> session{ "Fallout4.esm", "DLCCoast.esm", "A.esp" };
	CHECK(LoadOrderDifference({ "Fallout4.esm", "DLCCoast.esm", "A.esp", "B.esp" }, session).find("Only you have: B.esp") != std::string::npos);
	CHECK(LoadOrderDifference({ "Fallout4.esm", "DLCCoast.esm" }, session).find("You are missing: A.esp") != std::string::npos);
	CHECK(LoadOrderDifference({ "Fallout4.esm", "a.esp", "DLCCoast.esm" }, session).find("different order") != std::string::npos);
	CHECK(LoadOrderDifference({}, session).find("same mods in the same order") != std::string::npos);
	CHECK(LoadOrderDifference({ "Fallout4.esm", "X.esp", "Y.esp" }, session).size() <= MAX_REASON_LENGTH);
}

TEST("protocol: SameLoadOrderIgnoring leaves the host's ignored plugins out of both lists where they move no other plugin")
{
	const std::vector<std::string> session{ "Fallout4.esm", "A.esp", "HDTextures.esp" };
	const std::vector<std::string> ignore{ "hdtextures.esp" };
	CHECK(SameLoadOrderIgnoring({ "Fallout4.esm", "A.esp" }, session, ignore));
	CHECK(SameLoadOrderIgnoring({ "fallout4.esm", "a.esp", "HDTextures.esp" }, session, ignore));
	CHECK(!SameLoadOrderIgnoring({ "Fallout4.esm", "A.esp", "B.esp" }, session, ignore));
	CHECK(!SameLoadOrderIgnoring({ "A.esp", "Fallout4.esm" }, session, ignore));  // order still counts
	CHECK(!SameLoadOrderIgnoring({ "Fallout4.esm", "A.esp" }, session, {}));
	CHECK(!SameLoadOrderIgnoring({}, session, ignore));
}

TEST("protocol: SameLoadOrderIgnoring refuses an ignored plugin that moves a kept one's form IDs, counting full and light plugins apart")
{
	const std::vector<std::string> ignore{ "Textures.esp", "Textures.esl" };
	const std::vector<std::string> before{ "Fallout4.esm", "Textures.esp", "Story.esp" };
	CHECK(!SameLoadOrderIgnoring({ "Fallout4.esm", "Story.esp" }, before, ignore));  // Story.esp is 02 there, 01 here
	CHECK(!SameLoadOrderIgnoring({ "Fallout4.esm", "Story.esp", "Textures.esp" }, before, ignore));
	CHECK(SameLoadOrderIgnoring({ "Fallout4.esm", "Textures.esp", "Story.esp" }, before, ignore));

	const auto light = [](std::vector<std::string> a_full, std::initializer_list<std::string> a_light) {
		return ComparedLoadOrder(std::move(a_full), std::vector<std::string>(a_light));
	};
	// A full plugin last, then light ones: the light ones keep their places.
	CHECK(SameLoadOrderIgnoring(light({ "Fallout4.esm", "Story.esp", "Textures.esp" }, { "Weapons.esl" }), light({ "Fallout4.esm", "Story.esp" }, { "Weapons.esl" }), ignore));
	// A light one before another light one moves it.
	CHECK(!SameLoadOrderIgnoring(light({ "Fallout4.esm" }, { "Textures.esl", "Weapons.esl" }), light({ "Fallout4.esm" }, { "Weapons.esl" }), ignore));
	CHECK(SameLoadOrderIgnoring(light({ "Fallout4.esm" }, { "Weapons.esl", "Textures.esl" }), light({ "Fallout4.esm" }, { "Weapons.esl" }), ignore));
	// A light plugin is not the full plugin of the same place.
	CHECK(!SameLoadOrderIgnoring(light({ "Fallout4.esm", "Weapons.esl" }, {}), light({ "Fallout4.esm" }, { "Weapons.esl" }), ignore));
}

TEST("protocol: the HD texture pack is left out of the compared load order only when no full plugin loads after it")
{
	const std::vector<std::string> official{ "Fallout4.esm", "DLCNukaWorld.esm" };
	auto withPack = official;
	withPack.emplace_back("dlcultrahighresolution.esm");
	CHECK(ComparedFullPlugins(withPack) == official);
	CHECK(LoadOrderHash(ComparedLoadOrder(withPack, { "a.esl" })) == LoadOrderHash(ComparedLoadOrder(official, { "a.esl" })));

	auto modded = withPack;
	modded.emplace_back("Story.esp");
	CHECK(ComparedFullPlugins(modded) == modded);
	auto moddedWithout = official;
	moddedWithout.emplace_back("Story.esp");
	CHECK(LoadOrderHash(ComparedLoadOrder(modded, {})) != LoadOrderHash(ComparedLoadOrder(moddedWithout, {})));

	// The hello's list carries the mark between the kinds; the hash ignores case.
	const auto order = ComparedLoadOrder(official, { "A.esl" });
	CHECK(order.size() == 4);
	CHECK(order[2] == LIGHT_PLUGINS);
	CHECK(LoadOrderHash(order) == LoadOrderHash(ComparedLoadOrder({ "FALLOUT4.ESM", "dlcnukaworld.esm" }, { "a.esl" })));
}

TEST("protocol: LoadOrderDifference never names the light plugins' mark, and says texture plugins may differ only last")
{
	const auto session = ComparedLoadOrder({ "Fallout4.esm", "Textures.esp", "Story.esp" }, { "A.esl" });
	const auto theirs = ComparedLoadOrder({ "Fallout4.esm", "Story.esp" }, { "A.esl" });
	const auto reason = LoadOrderDifference(theirs, session, { "textures.esp" });
	CHECK(reason.find("You are missing: Textures.esp.") != std::string::npos);
	CHECK(reason.find("Texture plugins may differ only when they load last.") != std::string::npos);
	CHECK(reason.find(LIGHT_PLUGINS) == std::string::npos);
	CHECK(LoadOrderDifference({ "Fallout4.esm", "A.esl" }, session).find(LIGHT_PLUGINS) == std::string::npos);  // a list without the mark
	CHECK(LoadOrderDifference(theirs, ComparedLoadOrder({ "Fallout4.esm", "Story.esp", "B.esp" }, { "A.esl" })).find("Texture plugins") == std::string::npos);
}

TEST("protocol: Activation round trips and needs a shareable ref")
{
	for (const auto type : { MT::kReportActivation, MT::kActivation }) {
		const Activation in{ 2, 0x000A1B2C };
		const auto       out = DecodeActivation(Encode(in, type));
		REQUIRE(out);
		CHECK(*out == in);
		CHECK(!DecodeActivation(Encode(Activation{ 2, 0xFF000123 }, type)));
		CHECK(!DecodeActivation(Encode(Activation{ 2, 0 }, type)));
	}
}

TEST("protocol: a RefState carries its reporter and an ActorDeath where the corpse lies")
{
	const RefState state{ REF_A, RefState::kYes, RefState::kNo, 7 };
	const auto     out = DecodeRefState(Encode(state, MT::kRefStateChanged));
	REQUIRE(out);
	CHECK(*out == state);
	CHECK(out->SameState(RefState{ REF_A, RefState::kYes, RefState::kNo, 9 }));

	const ActorDeath death{ REF_B, 3, true, -59420.5f, 72560.25f, 6174.0f };
	const auto       back = DecodeActorDeath(Encode(death, MT::kActorDied));
	REQUIRE(back);
	CHECK(*back == death);
	CHECK(!DecodeActorDeath(Encode(ActorDeath{ REF_B, 3, true, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f }, MT::kActorDied)));
}

TEST("protocol: an ActorDeath says whether it was found dead, and then that nobody saw it die")
{
	for (const auto type : { MT::kReportDeath, MT::kActorDied }) {
		const ActorDeath found{ .refId = REF_B, .playerId = 3, .foundDead = true };
		const auto       back = DecodeActorDeath(Encode(found, type));
		REQUIRE(back);
		CHECK(*back == found);
		CHECK(back->foundDead);

		// A death with no place (a corpse with no 3D) is still one seen happening.
		const auto seen = DecodeActorDeath(Encode(ActorDeath{ REF_B, 3, false }, type));
		REQUIRE(seen);
		CHECK(!seen->foundDead);

		// Found dead but killed by the reporter, or lying somewhere: not a found-dead report.
		CHECK(!DecodeActorDeath(Encode(ActorDeath{ .refId = REF_B, .playerId = 3, .killed = true, .foundDead = true }, type)));
		CHECK(!DecodeActorDeath(Encode(ActorDeath{ .refId = REF_B, .playerId = 3, .x = 100.0f, .foundDead = true }, type)));
		CHECK(!DecodeActorDeath(Encode(ActorDeath{ .refId = REF_B, .playerId = 3, .z = -1.0f, .foundDead = true }, type)));
	}
}

TEST("protocol: Explosion round trips and needs a base")
{
	for (const auto type : { MT::kReportExplosion, MT::kExplosion }) {
		const Explosion in{ 3, 0x0001D2EE, -59420.5f, 72560.25f, 6174.0f, 0, 0x0000003C };
		const auto      out = DecodeExplosion(Encode(in, type));
		REQUIRE(out);
		CHECK(*out == in);
		CHECK(!DecodeExplosion(Encode(Explosion{ 3, 0, 1.0f, 2.0f, 3.0f, 0, 0 }, type)));
	}
}

TEST("protocol: Explosion refuses a position that is not a number")
{
	for (const float bad : { NaN, Inf, -Inf }) {
		CHECK(!DecodeExplosion(Encode(Explosion{ 3, 0x0001D2EE, bad, 2.0f, 3.0f, 0, 0 }, MT::kReportExplosion)));
		CHECK(!DecodeExplosion(Encode(Explosion{ 3, 0x0001D2EE, 1.0f, bad, 3.0f, 0, 0 }, MT::kReportExplosion)));
		CHECK(!DecodeExplosion(Encode(Explosion{ 3, 0x0001D2EE, 1.0f, 2.0f, bad, 0, 0 }, MT::kExplosion)));
	}
}

TEST("protocol: WorkshopItem refuses a position, rotation or scale that is not a number")
{
	for (const float bad : { NaN, Inf, -Inf }) {
		for (int axis = 0; axis < 3; ++axis) {
			auto moved = MakeWorkshopItem();
			moved.position[axis] = bad;
			CHECK(!DecodeWorkshopItem(Encode(moved, MT::kReportWorkshopItem)));
			auto turned = MakeWorkshopItem();
			turned.rotation[axis] = bad;
			CHECK(!DecodeWorkshopItem(Encode(turned, MT::kReportWorkshopItem)));
		}
		auto scaled = MakeWorkshopItem();
		scaled.scale = bad;
		CHECK(!DecodeWorkshopItem(Encode(scaled, MT::kReportWorkshopItem)));
		// One bad item spoils a catch-up packet.
		CHECK(!DecodeWorkshopItems(Encode(std::vector<WorkshopItem>{ MakeWorkshopItem(), scaled })));
	}
}

TEST("protocol: Face refuses weights and bones that are not numbers")
{
	for (const float bad : { NaN, Inf }) {
		auto heavy = MakeFace();
		heavy.weight[2] = bad;
		CHECK(!DecodeFace(Encode(heavy, MT::kReportFace)));
		auto bent = MakeFace();
		bent.bones[0].rotation[1] = bad;
		CHECK(!DecodeFace(Encode(bent, MT::kReportFace)));
		auto stretched = MakeFace();
		stretched.bones[0].scale[0] = bad;
		CHECK(!DecodeFace(Encode(stretched, MT::kReportFace)));
	}
}

TEST("protocol: a player's own direction and aim pitch, as their graph has them, arrive as sent")
{
	// Live, the player's graph read Direction 0, 0.25, 0.5 and 0.75 going forward, right, back and left, and
	// Pitch +-25.5 looking up and down.
	CHECK(PackDirection(0.0f) == 0);
	CHECK(PackDirection(0.25f) == 64);
	CHECK(PackDirection(0.5f) == 128);
	CHECK(PackDirection(0.75f) == 192);
	CHECK(PackDirection(1.0f) == 0);
	CHECK(PackDirection(-0.25f) == 192);
	CHECK(PackDirection(std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(PackPitch(25.515f) == 26);
	CHECK(PackPitch(-25.515f) == -26);
	CHECK(PackPitch(400.0f) == 90);
	CHECK(PackPitch(std::numeric_limits<float>::infinity()) == 0);
	for (const auto [turns, degrees] : { std::pair{ 0.25f, 25.515f }, std::pair{ 0.75f, -25.515f }, std::pair{ 0.5f, 0.0f } }) {
		auto in = MakeState(3);
		in.direction = PackDirection(turns);
		in.pitch = PackPitch(degrees);
		const auto out = DecodePlayerState(Encode(in));
		REQUIRE(out);
		CHECK(UnpackDirection(out->direction) == turns);
		CHECK(std::abs(out->pitch - degrees) < 0.5f);
		// And in the server's batch to everyone else.
		const auto batch = DecodePlayerStates(Encode(PlayerStates{ 9, { { 7, in } } }));
		REQUIRE(batch);
		REQUIRE(batch->players.size() == 1);
		CHECK(batch->players[0].state.direction == in.direction);
		CHECK(batch->players[0].state.pitch == in.pitch);
	}
}
