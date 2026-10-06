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

	// Starts connecting in the background (the address is resolved there too, so a slow DNS
	// lookup never stalls the game). Returns false if networking can't start.
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

	void Run(std::string a_address, std::uint16_t a_defaultPort);

	std::thread         thread;
	std::atomic<bool>   stopRequested{ false };
	std::atomic<Status> status{ Status::kDisconnected };

	std::mutex            queueLock;
	std::vector<Outgoing> outbox;
	std::vector<Event>    inbox;
};
