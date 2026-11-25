#pragma once

#include <string>
#include <string_view>
#include <vector>

// This machine's own IPv4 addresses, for a host who lets friends join by address. Never logged by these functions or
// their callers.
namespace LocalAddress
{
	// a_text is a dotted IPv4 address that a machine can be reached at on a network: four numbers 0-255, not 0.x.x.x,
	// not loopback (127.x.x.x), not self-assigned (169.254.x.x, no DHCP answered) and not a multicast or reserved address.
	[[nodiscard]] bool IsUsableV4(std::string_view a_text);

	// The address of the adapter the default route uses ("" when none). A UDP socket is connected to a documentation
	// address; nothing is sent.
	[[nodiscard]] std::string Default();

	// Every usable IPv4 address of this machine (IsUsableV4), Default() first.
	[[nodiscard]] std::vector<std::string> All();
}
