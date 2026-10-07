#pragma once

// A random number that stays the same on this PC (kept in
// Documents\My Games\Fallout4\F4SE\F4Multiplayer_player.id), so the server recognises a player
// who reconnects and gives them their old player ID back. Not a secret: it only says "the same
// person as before" within one hosting session.
namespace Identity
{
	[[nodiscard]] std::uint64_t Get();
}
