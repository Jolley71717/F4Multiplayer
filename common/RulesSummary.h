#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Protocol.h"

// What a player is told of the host's rules for being down (Protocol::SessionRules): only what differs from the defaults,
// one part per rule, each a text key with its placeholders' values (the plugin's Text puts them in the player's language).
// The defaults are what every session had before there were rules, so a session with them has nothing to tell.
namespace RulesSummary
{
	struct Part
	{
		std::string_view                                    key;
		std::vector<std::pair<std::string_view, std::string>> args;

		bool operator==(const Part&) const = default;
	};

	// Revive off is said alone: none of the other rules matter then. Real-time VATS is not a rule for being down.
	inline std::vector<Part> Of(const Protocol::SessionRules& a_rules)
	{
		const auto                   rules = a_rules.Clamped();
		const Protocol::SessionRules defaults;
		if (!rules.revive) {
			return { { "rules.revive_off", {} } };
		}
		std::vector<Part> parts;
		if (rules.maxRevives > 0) {
			if (rules.reviveWindowMinutes > 0) {
				parts.push_back({ "rules.revives_window", { { "n", std::to_string(rules.maxRevives) }, { "minutes", std::to_string(rules.reviveWindowMinutes) } } });
			} else {
				parts.push_back({ "rules.revives_fight", { { "n", std::to_string(rules.maxRevives) } } });
			}
		}
		if (rules.bleedOutSeconds != defaults.bleedOutSeconds) {
			parts.push_back({ "rules.bleed_out", { { "n", std::to_string(rules.bleedOutSeconds) } } });
		}
		if (rules.helpTenths != defaults.helpTenths) {
			// 3 s, 2.5 s: tenths only when there are some.
			const auto seconds = rules.helpTenths % 10 == 0 ? std::to_string(rules.helpTenths / 10) : std::format("{}.{}", rules.helpTenths / 10, rules.helpTenths % 10);
			parts.push_back({ "rules.help_time", { { "n", seconds } } });
		}
		if (rules.reviveHealth != defaults.reviveHealth) {
			parts.push_back({ "rules.revive_health", { { "n", std::to_string(rules.reviveHealth) } } });
		}
		if (rules.downCooldownSeconds > 0) {
			parts.push_back({ "rules.cooldown", { { "n", std::to_string(rules.downCooldownSeconds) } } });
		}
		if (rules.reviveOutsideCombat) {
			parts.push_back({ "rules.out_of_combat", {} });
		}
		return parts;
	}
}
