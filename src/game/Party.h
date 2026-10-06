#pragma once

#include "Protocol.h"

// Playing together: the player list, teleporting to friends, pings, shared kill XP, the kill feed
// and connection warnings. Main thread only.
namespace Party
{
	// Our player ID once welcomed, 0 when not in a session.
	void SetLocalPlayer(std::uint32_t a_id);

	// Sends our status and XP, handles hotkeys and pending teleports. Call every frame.
	void Frame();

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	void ApplyStatus(const Protocol::PlayerStatus& a_status);
	void ApplyPing(const Protocol::Ping& a_ping);
	void ApplyXp(const Protocol::XpGain& a_gain);
	void ApplyHeartbeatAck(const Protocol::Heartbeat& a_beat);

	// Another player's game reported a death (for the kill feed).
	void OnActorDied(const Protocol::ActorDeath& a_death);

	void OnPlayerLeft(std::uint32_t a_id);

	// What the hotkeys do, also reachable from dev commands.
	void ShowPlayerList();
	void PickTeleportTarget();
	void TeleportToTarget();
	void SendPing();

	// Round trip to the server in milliseconds (0 = not measured yet).
	[[nodiscard]] std::uint32_t RoundTripMs();

	void Reset();

	[[nodiscard]] std::string Describe();
}
