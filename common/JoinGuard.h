#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>

// What keeps strangers from wearing a reachable UDP port out: per remote address (ServerTransport::Source, a salted
// mix that is never logged or saved), a limit on wrong passwords, on connections that aren't in yet and on connects a
// minute. Used on the server thread only. A source of 0 (this machine, Steam) is never limited.
class JoinGuard
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr int                  MAX_WRONG = 5;  // wrong passwords ...
	static constexpr std::chrono::seconds WRONG_WINDOW{ 60 };  // ... within this ...
	static constexpr std::chrono::minutes BLOCK{ 5 };  // ... block the source this long
	static constexpr int                  MAX_PENDING = 2;  // connections of one source not yet welcomed (or refused)
	static constexpr int                  MAX_CONNECTS = 20;  // connects of one source ...
	static constexpr std::chrono::seconds CONNECT_WINDOW{ 60 };  // ... within this
	static constexpr std::size_t          MAX_SOURCES = 1024;  // the one longest unheard of is forgotten

	// Counts a connect; false = turn it away (too many open, or too many a minute).
	[[nodiscard]] bool MayConnect(std::uint64_t a_source, Clock::time_point a_now)
	{
		if (a_source == 0) {
			return true;
		}
		auto& entry = Touch(a_source, a_now);
		while (!entry.connects.empty() && a_now - entry.connects.front() >= CONNECT_WINDOW) {
			entry.connects.pop_front();
		}
		if (entry.pending >= MAX_PENDING || static_cast<int>(entry.connects.size()) >= MAX_CONNECTS) {
			return false;
		}
		entry.connects.push_back(a_now);
		++entry.pending;
		return true;
	}

	// A connection that counted in MayConnect is in the session (or gone): one fewer waiting. Called once for each.
	void OnSettled(std::uint64_t a_source)
	{
		if (const auto it = sources.find(a_source); a_source != 0 && it != sources.end() && it->second.pending > 0) {
			--it->second.pending;
		}
	}

	// Too many wrong passwords lately.
	[[nodiscard]] bool Blocked(std::uint64_t a_source, Clock::time_point a_now) const
	{
		const auto it = sources.find(a_source);
		return a_source != 0 && it != sources.end() && it->second.blockedUntil > a_now;
	}

	void OnWrongPassword(std::uint64_t a_source, Clock::time_point a_now)
	{
		if (a_source == 0) {
			return;
		}
		auto& entry = Touch(a_source, a_now);
		while (!entry.wrong.empty() && a_now - entry.wrong.front() >= WRONG_WINDOW) {
			entry.wrong.pop_front();
		}
		entry.wrong.push_back(a_now);
		if (static_cast<int>(entry.wrong.size()) >= MAX_WRONG) {
			entry.blockedUntil = a_now + BLOCK;
			entry.wrong.clear();
		}
	}

	[[nodiscard]] std::size_t Size() const { return sources.size(); }

private:
	struct Entry
	{
		int                          pending = 0;
		std::deque<Clock::time_point> connects;
		std::deque<Clock::time_point> wrong;
		Clock::time_point            blockedUntil{};
		Clock::time_point            lastSeen{};
	};

	Entry& Touch(std::uint64_t a_source, Clock::time_point a_now)
	{
		if (sources.size() >= MAX_SOURCES && !sources.contains(a_source)) {
			// Forget the one unheard of longest that has nobody waiting and isn't blocked, else the longest.
			auto oldest = sources.end();
			for (auto it = sources.begin(); it != sources.end(); ++it) {
				const bool quiet = it->second.pending == 0 && it->second.blockedUntil <= a_now;
				const bool oldestQuiet = oldest != sources.end() && oldest->second.pending == 0 && oldest->second.blockedUntil <= a_now;
				if (oldest == sources.end() || (quiet && !oldestQuiet) || (quiet == oldestQuiet && it->second.lastSeen < oldest->second.lastSeen)) {
					oldest = it;
				}
			}
			if (oldest != sources.end()) {
				sources.erase(oldest);
			}
		}
		auto& entry = sources[a_source];
		entry.lastSeen = a_now;
		return entry;
	}

	std::unordered_map<std::uint64_t, Entry> sources;
};
