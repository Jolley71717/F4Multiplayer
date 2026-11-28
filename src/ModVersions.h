#pragma once

#include <cstdint>
#include <string_view>

// Which mod versions used which protocol number, so a refusal for a version mismatch with a copy at 0.23 or older can
// name versions (the numbers mean nothing to a player): a server of those says numbers, and this client turns them into
// names; this server names the version of an old copy that says hello. Every release from 0.9.0 to 0.23 is here (the
// numbers of the ones in between are taken from each tag's Protocol::VERSION); a number not here is older than that.
// The table is complete: from 0.24.0 on the hello carries the mod's version itself (common/VersionCheck.h), and no
// number is added again (Protocol::LAST_NUMBER is its last line).
namespace ModVersions
{
	struct Entry
	{
		std::uint32_t protocol;
		std::string_view versions;  // as a player says it: "0.22" is every 0.22.x
	};

	inline constexpr Entry TABLE[] = {
		{ 49, "0.9.0 or 0.9.1" },
		{ 50, "0.9.2" },
		{ 55, "0.10.0" },
		{ 56, "0.10.1" },
		{ 57, "0.11" },
		{ 60, "0.12" },
		{ 62, "0.13.0 to 0.15.1" },
		{ 63, "0.16" },
		{ 76, "0.17" },
		{ 83, "0.19.0" },
		{ 84, "0.20.0 to 0.21.3" },
		{ 85, "0.22" },
		{ 86, "0.23" },
	};

	// The version(s) that used a protocol number, or "" for one not in the table.
	[[nodiscard]] constexpr std::string_view NamesOf(std::uint32_t a_protocol)
	{
		for (const auto& entry : TABLE) {
			if (entry.protocol == a_protocol) {
				return entry.versions;
			}
		}
		return {};
	}
}
