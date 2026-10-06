#pragma once

// A connection from this game to the session server, over ENet (NetClient) or Steam (SteamClient).
// The network runs on a background thread so the connection stays alive through loading screens;
// the game thread exchanges packets through queues.
class ClientConnection
{
public:
	enum class Status
	{
		kDisconnected,
		kConnecting,
		kConnected,  // transport is up; the server may still reject the Hello
	};

	struct Event
	{
		enum class Type
		{
			kConnected,
			kDisconnected,
			kPacket,
		};

		Type                      type;
		std::vector<std::uint8_t> data;
	};

	ClientConnection() = default;
	virtual ~ClientConnection() = default;  // derived classes call Disconnect() in their destructors

	ClientConnection(const ClientConnection&) = delete;
	ClientConnection& operator=(const ClientConnection&) = delete;

	// Starts connecting in the background. Returns false if networking can't start.
	bool Connect(const std::string& a_address, std::uint16_t a_defaultPort)
	{
		Disconnect();
		{
			// Events of an earlier connection stay in the inbox until polled.
			std::scoped_lock l{ queueLock };
			outbox.clear();
		}
		stopRequested = false;
		status = Status::kConnecting;
		thread = std::thread([this, a_address, a_defaultPort] { Run(a_address, a_defaultPort); });
		REX::INFO("Connection: connecting to {}", a_address);
		return true;
	}

	void Disconnect()
	{
		if (!thread.joinable()) {
			return;
		}
		stopRequested = true;
		thread.join();
		status = Status::kDisconnected;
	}

	void Send(std::vector<std::uint8_t> a_data, bool a_reliable)
	{
		if (status != Status::kConnected) {
			return;
		}
		std::scoped_lock l{ queueLock };
		outbox.push_back({ std::move(a_data), a_reliable });
	}

	// Returns all events received since the last call. Call from the game thread.
	std::vector<Event> Poll()
	{
		std::scoped_lock l{ queueLock };
		return std::exchange(inbox, {});
	}

	[[nodiscard]] Status GetStatus() const { return status; }

protected:
	struct Outgoing
	{
		std::vector<std::uint8_t> data;
		bool                      reliable;
	};

	// The background thread. Runs until stopRequested is set or the connection ends, and must
	// push kDisconnected (and set status) when it returns.
	virtual void Run(std::string a_address, std::uint16_t a_defaultPort) = 0;

	std::vector<Outgoing> TakeOutgoing()
	{
		std::scoped_lock l{ queueLock };
		return std::exchange(outbox, {});
	}

	void Push(Event a_event)
	{
		std::scoped_lock l{ queueLock };
		inbox.push_back(std::move(a_event));
	}

	void Finish()
	{
		status = Status::kDisconnected;
		Push({ Event::Type::kDisconnected, {} });
	}

	std::atomic<bool>   stopRequested{ false };
	std::atomic<Status> status{ Status::kDisconnected };

private:
	std::thread           thread;
	std::mutex            queueLock;
	std::vector<Outgoing> outbox;
	std::vector<Event>    inbox;
};
