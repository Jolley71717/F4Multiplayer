#pragma once

// A ServerTransport the tests drive by hand: they connect players, send packets as them and read
// what the server sent back. The server polls it from its own thread, hence the lock.

#include "Transport.h"

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
	std::optional<std::vector<std::uint8_t>> WaitFor(PeerId a_peer, std::uint8_t a_type, std::chrono::milliseconds a_timeout = std::chrono::milliseconds(2000))
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

	bool Disconnected(PeerId a_peer)
	{
		std::lock_guard lock{ mutex };
		return disconnected.contains(a_peer);
	}

	void SetLocal(PeerId a_peer) { std::lock_guard lock{ mutex }; local[a_peer] = true; }

	// --- the server's side ---

	void Poll(std::vector<TransportEvent>& a_out, int) override
	{
		std::lock_guard lock{ mutex };
		for (auto& event : pending) {
			a_out.push_back(std::move(event));
		}
		pending.clear();
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

	[[nodiscard]] std::string Describe(PeerId a_peer) const override { return std::format("fake {}", a_peer); }

	[[nodiscard]] bool IsLocal(PeerId a_peer) const override
	{
		std::lock_guard lock{ mutex };
		const auto it = local.find(a_peer);
		return it != local.end() && it->second;
	}

private:
	void Push(TransportEvent a_event)
	{
		std::lock_guard lock{ mutex };
		pending.push_back(std::move(a_event));
	}

	mutable std::mutex          mutex;
	std::condition_variable     changed;
	std::vector<TransportEvent> pending;
	std::vector<Sent>           sent;
	std::map<PeerId, bool>      disconnected;
	std::map<PeerId, bool>      local;
};
