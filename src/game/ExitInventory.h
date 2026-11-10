#pragma once

#include <string>

// Quitting the game from workshop mode crashed at the end of the shutdown (2026-11-05, the host, 4 of 4 tries; vanilla
// too, once past the music check ExitMusic mends). Main::Shutdown's HandleManager::KillSDM destroys the references
// still alive then; one is a runtime copy of the workshop workbench (CONT 000C1AEB, no cell) the workshop menu's root
// category strip displays. Its destructor destroys its inventory list, and ~BGSInventoryList (1.11.240 +34D730, Address
// Library 2194154) ends by telling the inventory interface to drop the list's agents: at +0x7C it loads the interface's
// singleton (+30EC470, 4796586) and at +0x87 calls BGSInventoryInterface::RemoveAllAgents (+3486A0, 2194033) with it,
// unchecked. The shutdown has deleted that singleton by then, so the call reads its agent array through null (+0xC8)
// and the game crashes, after its last save. This puts a null check in front of that one call: a list destroyed once
// the interface is gone has no agents left to drop. The call is patched only where the five bytes are 1.11.240's
// call (another version or another plugin's patch leave the game as it is, logged).
namespace ExitInventory
{
	// At plugin load. Whether the hook is in.
	bool Install();

	// For `net status`: whether the hook is in, and how many lists were destroyed with the interface gone.
	[[nodiscard]] std::string Describe();
}
