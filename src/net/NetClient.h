#pragma once

// Connection to the server. ENet runs on a background thread so the connection stays
// alive through loading screens; the game thread exchanges packets through queues.
class NetClient
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

	NetClient() = default;
	~NetClient();

	NetClient(const NetClient&) = delete;
	NetClient& operator=(const NetClient&) = delete;

	// Starts connecting in the background. Returns false if the address can't be resolved.
	bool Connect(const std::string& a_address, std::uint16_t a_defaultPort);
	void Disconnect();

	void Send(std::vector<std::uint8_t> a_data, bool a_reliable);

	// Returns all events received since the last call. Call from the game thread.
	std::vector<Event> Poll();

	[[nodiscard]] Status GetStatus() const { return status; }

private:
	struct Outgoing
	{
		std::vector<std::uint8_t> data;
		bool                      reliable;
	};

	// Host is in network byte order, as ENet stores it.
	void Run(std::uint32_t a_host, std::uint16_t a_port);

	std::thread         thread;
	std::atomic<bool>   stopRequested{ false };
	std::atomic<Status> status{ Status::kDisconnected };

	std::mutex            queueLock;
	std::vector<Outgoing> outbox;
	std::vector<Event>    inbox;
};
