#include "game/HostRules.h"

#include "game/Downed.h"

namespace HostRules
{
	namespace
	{
		Protocol::SessionRules rules;
		bool                   inSession = false;
	}

	void Set(const Protocol::SessionRules& a_rules)
	{
		rules = a_rules.Clamped();
		inSession = true;
		Downed::SetRules(rules);
		REX::INFO("Session: the host's rules: {}", Describe(rules));
	}

	void Clear()
	{
		rules = {};
		inSession = false;
		Downed::SetRules(rules);
	}

	bool InSession()
	{
		return inSession;
	}

	const Protocol::SessionRules& Get()
	{
		return rules;
	}
}
