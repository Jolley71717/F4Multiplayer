#pragma once

#include "net/ClientConnection.h"

// Connection to the server over ENet (UDP), to an "ip" or "ip:port" address.
class NetClient final : public ClientConnection
{
public:
	~NetClient() override { Disconnect(); }

private:
	void Run(std::string a_address, std::uint16_t a_defaultPort) override;
};
