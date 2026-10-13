#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Keeping other people out of the log. Players attach their log to public bug reports, so a log
// line never names a player (log "player N"), never holds a whole SteamID64 (log an alias) and
// never holds a whole IP address (log it masked). Scrub is the safety net run over every line on
// its way to the file; the call sites are expected to have done it already.
namespace Redact
{
	// A random number picked once per process. Aliases depend on it, so the same account reads the
	// same all through one log but cannot be looked up from it, or matched across two launches.
	std::uint64_t Salt();

	// "~" and six hex digits standing for a SteamID64 (or any other id), e.g. "~3fa2c1".
	std::string Alias(std::uint64_t a_id);
	std::string Alias(std::uint64_t a_id, std::uint64_t a_salt);

	// "198.51.100.7", 4321 -> "198.51.100.x:4321"; IPv6 keeps its first four groups and goes in
	// brackets. Loopback addresses (this machine) stay as they are.
	std::string MaskAddress(std::string_view a_ip, std::uint16_t a_port);

	// The line with every SteamID64 (17 digits starting 7656) replaced by its alias and every IPv4
	// address other than loopback masked. Lobby ids (18 digits), form ids and hex hashes stay.
	std::string Scrub(std::string_view a_line);

	// a_text without its control characters (line breaks among them): text someone else chose, such as a
	// host's refusal reason with their plugin names in it, stays one log line and can't forge another.
	std::string OneLine(std::string_view a_text);
}
