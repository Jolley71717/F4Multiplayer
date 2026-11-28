#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#ifndef F4MP_VERSION
#	error "F4MP_VERSION comes from xmake.lua (add_defines), the same as set_version"
#endif

// Who may join whom, by the mod's version. Since 0.24.0 a joiner's hello carries the version it was built as ("0.24.1",
// xmake.lua's set_version), and the server lets in a joiner of its own major.minor: the patch number never matters, so
// a patch release always plays with the others of its minor. A change to the wire protocol or to the session file's
// layout is a minor (or major) version (docs/RELEASING.md, CONTRIBUTING.md). Versions are shown as they are written,
// "0.24.1"; no other number names a version anywhere a person reads it.
namespace VersionCheck
{
	struct Version
	{
		std::uint32_t major = 0;
		std::uint32_t minor = 0;
		std::uint32_t patch = 0;

		auto operator<=>(const Version&) const = default;
	};

	// This build's version, from xmake.lua.
	inline constexpr std::string_view OWN = F4MP_VERSION;

	// The longest version string a hello may carry (Protocol::Hello::modVersion).
	inline constexpr std::size_t MAX_LENGTH = 32;

	// "0.24.1": three numbers of digits only (at most six each), a dot between each two. Anything else is none: a
	// hello's version is the joiner's to say, and one that isn't a version is never shown or compared.
	[[nodiscard]] constexpr std::optional<Version> Parse(std::string_view a_text)
	{
		std::uint32_t parts[3] = {};
		std::size_t   part = 0;
		std::size_t   digits = 0;
		for (const char c : a_text) {
			if (c == '.') {
				if (digits == 0 || part == 2) {
					return std::nullopt;
				}
				++part;
				digits = 0;
				continue;
			}
			if (c < '0' || c > '9' || ++digits > 6) {
				return std::nullopt;
			}
			parts[part] = parts[part] * 10 + static_cast<std::uint32_t>(c - '0');
		}
		if (part != 2 || digits == 0) {
			return std::nullopt;
		}
		return Version{ parts[0], parts[1], parts[2] };
	}

	static_assert(Parse(OWN).has_value(), "xmake.lua's version must be X.Y.Z");

	// As a person reads it: "0.24.1".
	[[nodiscard]] inline std::string Format(const Version& a_version)
	{
		return std::format("{}.{}.{}", a_version.major, a_version.minor, a_version.patch);
	}

	// Two builds play together when their major and minor are the same, whatever their patch.
	[[nodiscard]] constexpr bool SameMinor(const Version& a_one, const Version& a_other)
	{
		return a_one.major == a_other.major && a_one.minor == a_other.minor;
	}

	enum class Verdict
	{
		kJoins,       // the same major.minor
		kHostOlder,   // the host's major.minor is below the joiner's: the host updates
		kHostNewer,   // the host's is above: the joiner updates
		kUnreadable,  // the joiner's (or the host's) isn't a version
	};

	// Whether a joiner of a_joiner's version may join a host of a_host's.
	[[nodiscard]] constexpr Verdict Compare(std::string_view a_host, std::string_view a_joiner)
	{
		const auto host = Parse(a_host);
		const auto joiner = Parse(a_joiner);
		if (!host || !joiner) {
			return Verdict::kUnreadable;
		}
		if (SameMinor(*host, *joiner)) {
			return Verdict::kJoins;
		}
		const auto hostMinor = Version{ host->major, host->minor, 0 };
		const auto joinerMinor = Version{ joiner->major, joiner->minor, 0 };
		return hostMinor < joinerMinor ? Verdict::kHostOlder : Verdict::kHostNewer;
	}

	// The server's refusal for a version mismatch, in English on the wire, short enough for any version's reject
	// (Protocol::MAX_REASON_LENGTH): "The host has F4Multiplayer 0.24.1 and you have 0.25.0. Both players need the
	// same version: the host should update." A copy at 0.23 or older shows it as it came; a newer one reads it back
	// (ReadRefusal) and shows reject.version_older or reject.version_newer in the player's language (Text::Reject).
	inline constexpr std::string_view REFUSAL_START = "The host has F4Multiplayer ";
	inline constexpr std::string_view REFUSAL_MIDDLE = " and you have ";
	inline constexpr std::string_view REFUSAL_HOST_OLDER = ". Both players need the same version: the host should update.";
	inline constexpr std::string_view REFUSAL_HOST_NEWER = ". Both players need the same version: update to the latest from the Nexus Files tab.";
	// The longest a version named in a refusal may be ("0.13.0 to 0.15.1" for an old copy's, src/ModVersions.h).
	inline constexpr std::size_t MAX_NAMED_LENGTH = 40;

	// a_hostOlder: the host is the one to update (Verdict::kHostOlder). a_host and a_you as a person reads them.
	[[nodiscard]] inline std::string Refusal(bool a_hostOlder, std::string_view a_host, std::string_view a_you)
	{
		return std::format("{}{}{}{}{}", REFUSAL_START, a_host.substr(0, MAX_NAMED_LENGTH), REFUSAL_MIDDLE, a_you.substr(0, MAX_NAMED_LENGTH),
			a_hostOlder ? REFUSAL_HOST_OLDER : REFUSAL_HOST_NEWER);
	}

	struct Refused
	{
		bool        hostOlder = false;
		std::string host;
		std::string you;
	};

	// A refusal Refusal wrote, read back; none for any other text.
	[[nodiscard]] inline std::optional<Refused> ReadRefusal(std::string_view a_reason)
	{
		if (!a_reason.starts_with(REFUSAL_START)) {
			return std::nullopt;
		}
		Refused out;
		auto rest = a_reason.substr(REFUSAL_START.size());
		if (rest.ends_with(REFUSAL_HOST_OLDER)) {
			out.hostOlder = true;
			rest.remove_suffix(REFUSAL_HOST_OLDER.size());
		} else if (rest.ends_with(REFUSAL_HOST_NEWER)) {
			rest.remove_suffix(REFUSAL_HOST_NEWER.size());
		} else {
			return std::nullopt;
		}
		const auto middle = rest.find(REFUSAL_MIDDLE);
		if (middle == std::string_view::npos) {
			return std::nullopt;
		}
		const auto host = rest.substr(0, middle);
		const auto you = rest.substr(middle + REFUSAL_MIDDLE.size());
		if (host.empty() || you.empty() || host.size() > MAX_NAMED_LENGTH || you.size() > MAX_NAMED_LENGTH) {
			return std::nullopt;
		}
		out.host = host;
		out.you = you;
		return out;
	}
}
