#pragma once

// A ServerTransport the tests drive by hand: they connect players, send packets as them and read
// what the server sent back. The server polls it from its own thread, hence the lock.

#include "Transport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

class FakeTransport final : public ServerTransport
{
public:
	struct Sent
	{
		PeerId                    peer;
		std::vector<std::uint8_t> data;
		bool                      reliable;
	};

	// --- the test's side ---

	void Connect(PeerId a_peer) { Push({ TransportEvent::Type::kConnect, a_peer, {} }); }
	void Drop(PeerId a_peer) { Push({ TransportEvent::Type::kDisconnect, a_peer, {} }); }
	void Deliver(PeerId a_peer, std::vector<std::uint8_t> a_data) { Push({ TransportEvent::Type::kPacket, a_peer, std::move(a_data) }); }

	// Everything the server sent to a_peer so far, with how each was sent, and forgets it (the sim loses
	// or reorders only what goes unreliably, as the real transports do).
	std::vector<Sent> TakeSent(PeerId a_peer)
	{
		std::lock_guard   lock{ mutex };
		std::vector<Sent> out;
		std::erase_if(sent, [&](Sent& a_sent) {
			if (a_sent.peer != a_peer) {
				return false;
			}
			out.push_back(std::move(a_sent));
			return true;
		});
		return out;
	}

	// Everything the server sent to a_peer so far (and forgets it).
	std::vector<std::vector<std::uint8_t>> Take(PeerId a_peer)
	{
		std::lock_guard lock{ mutex };
		std::vector<std::vector<std::uint8_t>> out;
		std::erase_if(sent, [&](Sent& a_sent) {
			if (a_sent.peer != a_peer) {
				return false;
			}
			out.push_back(std::move(a_sent.data));
			return true;
		});
		return out;
	}

	// Waits up to a_timeout for the server to send a_peer a packet whose first byte is a_type,
	// and returns it (packets before it stay queued for Take).
	std::optional<std::vector<std::uint8_t>> WaitFor(PeerId a_peer, std::uint8_t a_type, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		std::unique_lock lock{ mutex };
		std::optional<std::vector<std::uint8_t>> found;
		changed.wait_for(lock, a_timeout, [&] {
			const auto it = std::ranges::find_if(sent, [&](const Sent& a_sent) {
				return a_sent.peer == a_peer && !a_sent.data.empty() && a_sent.data[0] == a_type;
			});
			if (it == sent.end()) {
				return false;
			}
			found = std::move(it->data);
			sent.erase(it);
			return true;
		});
		return found;
	}

	// Waits until the server has acted on everything delivered so far: it took it all (Poll) and came back
	// for more, which it does only once it has handled what it took. What the server queued for itself
	// meanwhile (a disconnect it asked for) is waited for too. False if it didn't in time (it stopped).
	bool Settle(std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		std::unique_lock lock{ mutex };
		do {
			if (!changed.wait_for(lock, a_timeout, [&] { return pending.empty(); })) {
				return false;
			}
			const auto seen = polls;
			if (!changed.wait_for(lock, a_timeout, [&] { return polls > seen; })) {
				return false;
			}
		} while (!pending.empty());
		return true;
	}

	// Waits up to a_timeout for the server to send a_peer a packet whose first byte is a_type, and returns everything it
	// sent a_peer up to and including that one, in the order sent (empty when none came; the rest stay queued).
	std::vector<std::vector<std::uint8_t>> TakeThrough(PeerId a_peer, std::uint8_t a_type, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		std::unique_lock                       lock{ mutex };
		std::vector<std::vector<std::uint8_t>> out;
		changed.wait_for(lock, a_timeout, [&] {
			return std::ranges::any_of(sent, [&](const Sent& a_sent) { return a_sent.peer == a_peer && !a_sent.data.empty() && a_sent.data[0] == a_type; });
		});
		const auto last = std::ranges::find_if(sent, [&](const Sent& a_sent) { return a_sent.peer == a_peer && !a_sent.data.empty() && a_sent.data[0] == a_type; });
		if (last == sent.end()) {
			return out;
		}
		const auto end = last + 1;
		for (auto it = sent.begin(); it != end; ++it) {
			if (it->peer == a_peer) {
				out.push_back(std::move(it->data));
			}
		}
		sent.erase(std::remove_if(sent.begin(), end, [&](const Sent& a_sent) { return a_sent.peer == a_peer; }), end);
		return out;
	}

	bool Disconnected(PeerId a_peer)
	{
		std::lock_guard lock{ mutex };
		return disconnected.contains(a_peer);
	}

	// Waits up to a_timeout for the server to drop a_peer: it sends a refusal before it disconnects, from its own
	// thread, so a test that saw the refusal can get there first (on a slow machine such as a CI runner).
	bool WaitDisconnected(PeerId a_peer, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		std::unique_lock lock{ mutex };
		return changed.wait_for(lock, a_timeout, [&] { return disconnected.contains(a_peer); });
	}

	void SetLocal(PeerId a_peer) { std::lock_guard lock{ mutex }; local[a_peer] = true; }

	// What Describe says about a_peer in the server's log, e.g. a real transport's "udp 1.2.3.4:5678".
	void SetAddress(PeerId a_peer, std::string a_address) { std::lock_guard lock{ mutex }; addresses[a_peer] = std::move(a_address); }
	// Who a_peer is to the transport (as Steam knows a friend by their Steam ID); 0 = can't say.
	void SetIdentity(PeerId a_peer, std::uint64_t a_identity) { std::lock_guard lock{ mutex }; identities[a_peer] = a_identity; }

	// --- the server's side ---

	void Poll(std::vector<TransportEvent>& a_out, int) override
	{
		{
			std::lock_guard lock{ mutex };
			for (auto& event : pending) {
				a_out.push_back(std::move(event));
			}
			pending.clear();
			++polls;
		}
		changed.notify_all();
	}

	void Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable) override
	{
		{
			std::lock_guard lock{ mutex };
			sent.push_back({ a_peer, { a_data.begin(), a_data.end() }, a_reliable });
		}
		changed.notify_all();
	}

	void Disconnect(PeerId a_peer) override
	{
		{
			std::lock_guard lock{ mutex };
			disconnected[a_peer] = true;
			pending.push_back({ TransportEvent::Type::kDisconnect, a_peer, {} });
		}
		changed.notify_all();
	}

	void Close() override {}

	[[nodiscard]] std::string Describe(PeerId a_peer) const override
	{
		std::lock_guard lock{ mutex };
		const auto it = addresses.find(a_peer);
		return it != addresses.end() ? it->second : std::format("fake {}", a_peer);
	}

	[[nodiscard]] bool IsLocal(PeerId a_peer) const override
	{
		std::lock_guard lock{ mutex };
		const auto it = local.find(a_peer);
		return it != local.end() && it->second;
	}

	[[nodiscard]] std::uint64_t Identity(PeerId a_peer) const override
	{
		std::lock_guard lock{ mutex };
		const auto it = identities.find(a_peer);
		return it != identities.end() ? it->second : 0;
	}

private:
	void Push(TransportEvent a_event)
	{
		std::lock_guard lock{ mutex };
		pending.push_back(std::move(a_event));
	}

	mutable std::mutex            mutex;
	std::condition_variable       changed;
	std::vector<TransportEvent>   pending;
	std::vector<Sent>             sent;
	std::map<PeerId, bool>        disconnected;
	std::map<PeerId, bool>        local;
	std::uint64_t                 polls = 0;  // times the server came for events
	std::map<PeerId, std::string> addresses;
	std::map<PeerId, std::uint64_t> identities;
};
