#pragma once

#include "Config.h"
#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

// The session's rules (Protocol::SessionRules): downed and revive, and what runs in real time. The host's ini sets them,
// its server sends them with every welcome (the host's own game is welcomed too), and every game plays by the ones it was
// welcomed with until the session ends. Out of a session nothing here applies. The conversion is free of the game, so
// it is tested; the session's copy lives in HostRules.cpp (main thread).
namespace HostRules
{
	// What a host's ini asks for, within the limits the wire allows.
	inline Protocol::SessionRules FromSettings(const Config::Settings& a_settings)
	{
		Protocol::SessionRules rules;
		constexpr auto         u16 = [](std::uint32_t a_value) { return static_cast<std::uint16_t>((std::min)(a_value, 65535u)); };
		constexpr auto         u8 = [](std::uint32_t a_value) { return static_cast<std::uint8_t>((std::min)(a_value, 255u)); };
		rules.revive = a_settings.revive;
		rules.bleedOutSeconds = u16(a_settings.bleedOutSeconds);
		const float seconds = std::isfinite(a_settings.reviveSeconds) ? a_settings.reviveSeconds : 2.0f;
		rules.helpTenths = static_cast<std::uint16_t>(std::lround(std::clamp(seconds, 0.0f, 1000.0f) * 10.0f));
		rules.reviveHealth = u8(a_settings.reviveHealth);
		rules.maxRevives = u8(a_settings.maxRevives);
		rules.reviveWindowMinutes = u16(a_settings.reviveWindowMinutes);
		rules.downCooldownSeconds = u16(a_settings.reviveCooldownSeconds);
		rules.reviveOutsideCombat = a_settings.reviveOutOfCombat;
		rules.realTimeVats = a_settings.realTimeVats;
		return rules.Clamped();
	}

	// One line for the log and the dev channel.
	inline std::string Describe(const Protocol::SessionRules& a_rules)
	{
		return std::format("revive={} bleedOut={}s help={:.1f}s health={}% maxRevives={} window={} cooldown={}s outOfCombat={} realTimeVats={}",
			a_rules.revive, a_rules.bleedOutSeconds, a_rules.helpTenths / 10.0, a_rules.reviveHealth, a_rules.maxRevives,
			a_rules.reviveWindowMinutes ? std::format("{}min", a_rules.reviveWindowMinutes) : std::string{ "fight" }, a_rules.downCooldownSeconds,
			a_rules.reviveOutsideCombat, a_rules.realTimeVats);
	}

	// The game's side (HostRules.cpp), main thread.

	// Welcomed into a session with these rules.
	void Set(const Protocol::SessionRules& a_rules);

	// The session ended (disconnected, left, stopped hosting).
	void Clear();

	// In a session (welcomed): its rules apply.
	[[nodiscard]] bool InSession();

	// The session's rules; out of a session, the defaults.
	[[nodiscard]] const Protocol::SessionRules& Get();
}
