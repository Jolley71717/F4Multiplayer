#pragma once

// One valid packet of every message type, with the decoder that reads it back. The protocol tests
// put each through prefix, trailing-byte and corruption checks; the server tests send them as a
// player would, broken in the same ways, and read back what the server relays.

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ProtocolSamples
{
	using namespace Protocol;
	using MT = MessageType;
	using Bytes = std::vector<std::uint8_t>;
	using Decoder = std::function<bool(std::span<const std::uint8_t>)>;

	// A plugin reference that every game shares (not the player, not runtime-created).
	inline constexpr std::uint32_t REF_A = 0x0001F00D;
	inline constexpr std::uint32_t REF_B = 0x0A0B0C0D;
	inline constexpr std::uint32_t RUNTIME_REF = 0xFF001234;

	template <class Fn>
	Decoder D(Fn a_decode)
	{
		return [a_decode](std::span<const std::uint8_t> a_data) { return a_decode(a_data).has_value(); };
	}

	// One valid encoding of each message, with the decoder that reads it back.
	struct Sample
	{
		std::string name;
		Bytes       packet;
		Decoder     decode;
		bool        rejectsTrailing = true;
	};

	inline PlayerState MakeState(std::uint32_t a_seed)
	{
		PlayerState s;
		s.sequence = a_seed;
		s.cell = 0x0001A2B3 + a_seed;
		s.worldspace = 0x0000003C;
		s.x = 1234.5f + a_seed;
		s.y = -987.25f;
		s.z = 42.0f;
		s.heading = 3.14159f;
		s.speed = 250.0f;
		s.moveMode = 0x1234;
		s.flags = kRunning | kWeaponDrawn | kSwimming;
		return s;
	}

	inline ActorState MakeActorState(std::uint32_t a_seed)
	{
		ActorState a;
		a.refId = REF_A + a_seed;
		a.cell = 0x0001A2B3;
		a.worldspace = a_seed;
		a.x = -1.5f;
		a.y = 2.25f + a_seed;
		a.z = 1e6f;
		a.heading = -0.5f;
		a.speed = 0.0f;
		a.moveMode = 0xBEEF;
		a.flags = kSneaking | kInAir | kJumping;
		return a;
	}

	inline Hello MakeHello()
	{
		Hello h;
		h.contentHash = 0xC0FFEE11;
		h.name = "Alice";
		h.password = "hunter2";
		h.appearance = 0x0002F1E7;
		h.world.sessionId = 0x0123456789ABCDEFull;
		h.world.containerFrom = 77;
		h.identity = 0xFEDCBA9876543210ull;
		h.plugins = { "Fallout4.esm", "DLCCoast.esm", "MyMod.esp" };
		return h;
	}

	inline WorldState MakeWorldState()
	{
		WorldState w;
		w.deadActors = { REF_A, REF_B, 0x00012345 };
		w.containerFirstIndex = 1000;
		w.containerChanges = {
			{ 1000, 1, { REF_A, 0x0000000F, 250 } },
			{ 1001, 2, { REF_B, 0x0004D00D, -3 } },
			{ 1002, 7, { REF_A, 0x00000010, MAX_ITEM_COUNT } },
		};
		w.pickedUp = { 0x00055555, REF_B };
		w.refStates = { { REF_A, RefState::kYes, RefState::kNo }, { REF_B, RefState::kUnknown, RefState::kYes } };
		w.questStages = { { 0x0001ED86, 10 }, { 0x0002A1B2, 65535 } };
		return w;
	}

	inline Face MakeFace()
	{
		Face f;
		f.playerId = 4;
		f.female = true;
		f.race = 0x00013746;
		f.headParts = { 0x0004DA7A, 0x00047B2B };
		f.hairColor = 0x0005D7A1;
		f.weight[0] = 0.2f, f.weight[1] = 0.5f, f.weight[2] = 0.3f;
		f.sliders = { 0.0f, 0.25f, -0.5f };
		f.bones = { { 7, { 0.1f, 0.0f, -0.2f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.1f } } };
		f.tints = { { 1001, 1, 90, 0xFFC08060, 3 }, { 1003, 2, 100, 0, 0 } };
		f.bodyTint[0] = 200, f.bodyTint[3] = 255;
		return f;
	}

	inline WorkshopItem MakeWorkshopItem()
	{
		WorkshopItem item;
		item.playerId = 2, item.refId = 0xFF000042, item.base = 0x0001F2A1, item.workshop = REF_A, item.scale = 0.5f;
		item.position[0] = 1.0f, item.position[1] = -2.0f, item.position[2] = 3.0f, item.rotation[2] = 1.5f;
		return item;
	}

	inline std::vector<Sample> Samples()
	{
		std::vector<Sample> s;
		s.push_back({ "Hello", Encode(MakeHello()), D(DecodeHello), false });
		s.push_back({ "WorldRequest", Encode(WorldRequest{ 0xABCDEF0123456789ull, 5 }), D(DecodeWorldRequest) });
		s.push_back({ "PlayerState", Encode(MakeState(3)), D(DecodePlayerState) });
		s.push_back({ "Welcome", Encode(Welcome{ 9, 0x1122334455667788ull, true, true }), D(DecodeWelcome), false });
		s.push_back({ "Reject", Encode(Reject{ "Server is full" }), D(DecodeReject), false });
		s.push_back({ "PlayerJoined", Encode(PlayerJoined{ 4, "Bob", 0x0001ABCD }), D(DecodePlayerJoined), false });
		s.push_back({ "PlayerLeft", Encode(PlayerLeft{ 4 }), D(DecodePlayerLeft), false });
		s.push_back({ "PlayerStates", Encode(PlayerStates{ 99, { { 1, MakeState(1) }, { 2, MakeState(2) } } }), D(DecodePlayerStates) });
		for (const auto type : { MT::kReportDeath, MT::kActorDied }) {
			s.push_back({ "ActorDeath", Encode(ActorDeath{ REF_A, 3, true }, type), D(DecodeActorDeath) });
			s.push_back({ "ActorDeath found dead", Encode(ActorDeath{ .refId = REF_A, .playerId = 3, .foundDead = true }, type), D(DecodeActorDeath) });
		}
		for (const auto type : { MT::kReportHealth, MT::kActorHealth }) {
			s.push_back({ "ActorHealth", Encode(ActorHealth{ REF_A, 0.25f }, type), D(DecodeActorHealth) });
		}
		s.push_back({ "ContainerChange", Encode(ContainerChange{ REF_A, 0x0000000F, -12 }, MT::kReportContainer), D(DecodeContainerChange) });
		s.push_back({ "IndexedContainerChange", Encode(IndexedContainerChange{ 17, 2, { REF_A, 0x0000000F, 5 } }), D(DecodeIndexedContainerChange) });
		for (const auto type : { MT::kReportPickup, MT::kRefPickedUp }) {
			s.push_back({ "RefPickedUp", Encode(RefPickedUp{ REF_B }, type), D(DecodeRefPickedUp) });
		}
		for (const auto type : { MT::kReportRefState, MT::kRefStateChanged }) {
			s.push_back({ "RefState", Encode(RefState{ REF_A, RefState::kYes, RefState::kUnknown }, type), D(DecodeRefState) });
		}
		for (const auto type : { MT::kEquipment, MT::kPlayerEquipment }) {
			s.push_back({ "Equipment", Encode(Equipment{ 6, { 0x00011111, 0x00022222, 0x00033333 } }, type), D(DecodeEquipment) });
		}
		s.push_back({ "Claims", Encode(std::vector<ActorClaim>{ { REF_A, ClaimReason::kInteract }, { REF_B, ClaimReason::kCompanion } }), D(DecodeClaims) });
		s.push_back({ "Release", EncodeRelease({ REF_A, REF_B }), D(DecodeRelease) });
		s.push_back({ "Owners", Encode(std::vector<ActorOwner>{ { REF_A, 1 }, { REF_B, 0 } }), D(DecodeOwners) });
		for (const auto type : { MT::kActorStates, MT::kActorStatesRelay }) {
			s.push_back({ "ActorStates", Encode(std::vector<ActorState>{ MakeActorState(0), MakeActorState(1) }, type), D(DecodeActorStates) });
		}
		for (const auto type : { MT::kPlayerHit, MT::kPlayerDamaged }) {
			s.push_back({ "PlayerHit(npc)", Encode(PlayerHit{ 2, 12.5f, false, REF_A }, type), D(DecodePlayerHit) });
			s.push_back({ "PlayerHit(player)", Encode(PlayerHit{ 2, 7.0f, true, 0 }, type), D(DecodePlayerHit) });
		}
		for (const auto type : { MT::kReportShot, MT::kShotFired }) {
			s.push_back({ "Shot", Encode(Shot{ 3, REF_A }, type), D(DecodeShot) });
			s.push_back({ "Shot(reload)", Encode(Shot{ 3, 0, ShotAction::kReload }, type), D(DecodeShot) });
			s.push_back({ "Shot(sound)", Encode(Shot{ 3, 0, ShotAction::kShot, 0x0012A3B4 }, type), D(DecodeShot) });
		}
		for (const auto type : { MT::kReportStatus, MT::kPlayerStatus }) {
			s.push_back({ "PlayerStatus", Encode(PlayerStatus{ 3, 0x0001D5E0, 0x0001A2B3, 87, 42, true, true, false, false, 0 }, type), D(DecodePlayerStatus) });
			s.push_back({ "PlayerStatus(armor)", Encode(PlayerStatus{ 3, 0x0001D5E0, 0, 87, 42, false, false, false, true, 0x0017F6E4, REF_A }, type), D(DecodePlayerStatus) });
		}
		for (const auto type : { MT::kReportTime, MT::kWorldTime }) {
			s.push_back({ "WorldTime", Encode(WorldTime{ 1, 13.5f, 21.75f, 0x3C, 0x0001F2A3 }, type), D(DecodeWorldTime) });
		}
		for (const auto type : { MT::kPing, MT::kPinged }) {
			s.push_back({ "Ping", Encode(Ping{ 1, 0, 0x3C, 100.0f, -200.0f, 300.5f }, type), D(DecodePing) });
		}
		for (const auto type : { MT::kReportLine, MT::kLineSpoken }) {
			s.push_back({ "Line", Encode(Line{ 1, REF_A, "Welcome to Diamond City!" }, type), D(DecodeLine) });
		}
		for (const auto type : { MT::kHeartbeat, MT::kHeartbeatAck }) {
			s.push_back({ "Heartbeat", Encode(Heartbeat{ 0xDEADBEEF }, type), D(DecodeHeartbeat) });
		}
		for (const auto type : { MT::kReportXp, MT::kPartyXp }) {
			s.push_back({ "XpGain", Encode(XpGain{ 2, 37.5f }, type), D(DecodeXpGain) });
		}
		for (const auto type : { MT::kReportMarkers, MT::kMarkersFound }) {
			s.push_back({ "Markers", Encode(MarkersFound{ 2, { { REF_A, 1 }, { REF_B, 3 } } }, type), D(DecodeMarkers) });
		}
		for (const auto type : { MT::kVoice, MT::kVoiceRelay }) {
			s.push_back({ "Voice", Encode(VoiceData{ 2, { 1, 2, 3, 4, 5, 0, 255 } }, type), D(DecodeVoice) });
		}
		for (const auto type : { MT::kRevive, MT::kRevived }) {
			s.push_back({ "Revive", Encode(Revive{ 5 }, type), D(DecodeRevive) });
		}
		for (const auto type : { MT::kReportQuestDone, MT::kQuestDone }) {
			s.push_back({ "QuestDone", Encode(QuestDone{ 5, 0x0001ED86 }, type), D(DecodeQuestDone) });
		}
		for (const auto type : { MT::kReportQuestStage, MT::kQuestStage }) {
			s.push_back({ "QuestStage", Encode(QuestStage{ 0x0001ED86, 30 }, type), D(DecodeQuestStage) });
		}
		for (const auto type : { MT::kReportNpcEquipment, MT::kNpcEquipment }) {
			s.push_back({ "NpcEquipment", Encode(NpcEquipment{ 2, REF_A, { 0x00024F55, 0x000B3F4E } }, type), D(DecodeNpcEquipment) });
		}
		for (const auto type : { MT::kReportFace, MT::kPlayerFace }) {
			s.push_back({ "Face", Encode(MakeFace(), type), D(DecodeFace) });
		}
		for (const auto type : { MT::kReportWorkshopItem, MT::kWorkshopItem }) {
			s.push_back({ "WorkshopItem", Encode(MakeWorkshopItem(), type), D(DecodeWorkshopItem) });
		}
		s.push_back({ "WorkshopItems", Encode(std::vector<WorkshopItem>{ MakeWorkshopItem(), MakeWorkshopItem() }), D(DecodeWorkshopItems) });
		for (const auto type : { MT::kReportExplosion, MT::kExplosion }) {
			s.push_back({ "Explosion", Encode(Explosion{ 3, 0x0001D2EE, -59420.5f, 72560.25f, 6174.0f, 0, 0x0000003C }, type), D(DecodeExplosion) });
		}
		for (const auto type : { MT::kReportActivation, MT::kActivation }) {
			s.push_back({ "Activation", Encode(Activation{ 2, 0x000A1B2C }, type), D(DecodeActivation) });
		}
		s.push_back({ "WorldState", Encode(MakeWorldState()), D(DecodeWorldState) });
		return s;
	}

	// Whether a byte is a message type. The compiler keeps this list whole: a type added to the enum
	// and not here stops the tests building (C4062, turned into an error just for this switch), and the
	// protocol tests fail for a type here that has no entry in Samples().
#pragma warning(push)
#pragma warning(error : 4062)
	constexpr bool IsMessageType(std::uint8_t a_byte)
	{
		switch (static_cast<MT>(a_byte)) {
		case MT::kHello:
		case MT::kPlayerState:
		case MT::kReportDeath:
		case MT::kReportHealth:
		case MT::kReportContainer:
		case MT::kReportPickup:
		case MT::kReportRefState:
		case MT::kEquipment:
		case MT::kClaimActors:
		case MT::kReleaseActors:
		case MT::kActorStates:
		case MT::kPlayerHit:
		case MT::kReportQuestStage:
		case MT::kRequestWorldState:
		case MT::kReportShot:
		case MT::kReportStatus:
		case MT::kReportTime:
		case MT::kPing:
		case MT::kReportXp:
		case MT::kHeartbeat:
		case MT::kReportQuestDone:
		case MT::kRevive:
		case MT::kVoice:
		case MT::kReportMarkers:
		case MT::kReportLine:
		case MT::kReportNpcEquipment:
		case MT::kReportFace:
		case MT::kReportWorkshopItem:
		case MT::kReportExplosion:
		case MT::kReportActivation:
		case MT::kWelcome:
		case MT::kReject:
		case MT::kPlayerJoined:
		case MT::kPlayerLeft:
		case MT::kPlayerStates:
		case MT::kActorDied:
		case MT::kWorldState:
		case MT::kActorHealth:
		case MT::kContainerChanged:
		case MT::kRefPickedUp:
		case MT::kRefStateChanged:
		case MT::kPlayerEquipment:
		case MT::kActorOwners:
		case MT::kActorStatesRelay:
		case MT::kPlayerDamaged:
		case MT::kQuestStage:
		case MT::kShotFired:
		case MT::kPlayerStatus:
		case MT::kWorldTime:
		case MT::kPinged:
		case MT::kPartyXp:
		case MT::kHeartbeatAck:
		case MT::kQuestDone:
		case MT::kRevived:
		case MT::kVoiceRelay:
		case MT::kMarkersFound:
		case MT::kLineSpoken:
		case MT::kNpcEquipment:
		case MT::kPlayerFace:
		case MT::kWorkshopItem:
		case MT::kWorkshopItems:
		case MT::kExplosion:
		case MT::kActivation:
			return true;
		}
		return false;
	}
#pragma warning(pop)
}
