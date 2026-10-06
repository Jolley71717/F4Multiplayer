// Test client that pretends to be a player: connects to a server and walks in a circle.
//
//   F4MPBot --server 127.0.0.1:7779 --name Bot --cell 0 --worldspace 0000003C
//           --x -80352 --y 89600 --z 7790 [--radius 300] [--content-hash HEX] [--seconds N]

#include "Net.h"
#include "Protocol.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>

namespace
{
	template <class T>
	bool ParseNumber(std::string_view a_str, T& a_out, int a_base = 10)
	{
		if constexpr (std::is_floating_point_v<T>) {
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		} else {
			if (a_base == 16 && (a_str.starts_with("0x") || a_str.starts_with("0X"))) {
				a_str.remove_prefix(2);
			}
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out, a_base);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		}
	}
}

int main(int argc, char* argv[])
{
	std::string   server = "127.0.0.1";
	std::string   name = "Bot";
	std::string   password;
	std::uint32_t cell = 0;
	std::uint32_t worldspace = 0x3C;  // Commonwealth
	std::uint32_t contentHash = 0;
	float         cx = 0, cy = 0, cz = 0, radius = 300;
	int           seconds = 0;  // 0 = run until killed

	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view key = argv[i];
		const std::string_view value = argv[i + 1];
		bool                   ok = true;
		if (key == "--server") {
			server = value;
		} else if (key == "--name") {
			name = value;
		} else if (key == "--password") {
			password = value;
		} else if (key == "--cell") {
			ok = ParseNumber(value, cell, 16);
		} else if (key == "--worldspace") {
			ok = ParseNumber(value, worldspace, 16);
		} else if (key == "--content-hash") {
			ok = ParseNumber(value, contentHash, 16);
		} else if (key == "--x") {
			ok = ParseNumber(value, cx);
		} else if (key == "--y") {
			ok = ParseNumber(value, cy);
		} else if (key == "--z") {
			ok = ParseNumber(value, cz);
		} else if (key == "--radius") {
			ok = ParseNumber(value, radius);
		} else if (key == "--seconds") {
			ok = ParseNumber(value, seconds);
		} else {
			ok = false;
		}
		if (!ok) {
			std::cerr << "bad argument: " << key << ' ' << value << '\n';
			return 2;
		}
	}

	if (!Net::Initialize()) {
		std::cerr << "enet init failed\n";
		return 1;
	}

	ENetAddress address{};
	if (!Net::ResolveAddress(server, Protocol::DEFAULT_PORT, address)) {
		std::cerr << "cannot resolve " << server << '\n';
		return 1;
	}

	const auto host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
	const auto peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
	if (!host || !peer) {
		std::cerr << "enet connect failed\n";
		return 1;
	}

	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto       nextSend = start;
	auto       lastStep = start;
	float      angle = 0.0f;  // position on the circle, radians
	bool       welcomed = false;
	std::uint32_t sequence = 0;

	for (;;) {
		ENetEvent event;
		while (enet_host_service(host, &event, 5) > 0) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				std::cout << "connected, sending hello\n";
				Net::Send(peer, Protocol::Encode(Protocol::Hello{ .contentHash = contentHash, .name = name, .password = password }), true);
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				{
					const std::span<const std::uint8_t> data{ event.packet->data, event.packet->dataLength };
					switch (Protocol::PeekType(data).value_or(Protocol::MessageType{})) {
					case Protocol::MessageType::kWelcome:
						if (const auto msg = Protocol::DecodeWelcome(data)) {
							std::cout << "welcomed as player " << msg->playerId << '\n';
							welcomed = true;
						}
						break;
					case Protocol::MessageType::kReject:
						if (const auto msg = Protocol::DecodeReject(data)) {
							std::cout << "rejected: " << msg->reason << '\n';
						}
						return 3;
					case Protocol::MessageType::kPlayerJoined:
						if (const auto msg = Protocol::DecodePlayerJoined(data)) {
							std::cout << "player joined: " << msg->playerId << " " << msg->name << '\n';
						}
						break;
					case Protocol::MessageType::kPlayerLeft:
						if (const auto msg = Protocol::DecodePlayerLeft(data)) {
							std::cout << "player left: " << msg->playerId << '\n';
						}
						break;
					default:
						break;
					}
					enet_packet_destroy(event.packet);
				}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				std::cout << "disconnected\n";
				return 4;
			default:
				break;
			}
		}

		const auto now = Clock::now();
		if (seconds > 0 && now - start > std::chrono::seconds(seconds)) {
			break;
		}

		if (welcomed && now >= nextSend) {
			nextSend = now + std::chrono::milliseconds(50);  // 20 Hz like the game client

			// Walk around the circle at walking speed (~150 units/s) for 4 s, then stand for 3 s,
			// so both locomotion start and stop get exercised.
			const float elapsed = std::chrono::duration<float>(now - start).count();
			const float dt = std::chrono::duration<float>(now - lastStep).count();
			lastStep = now;
			const bool  walking = std::fmod(elapsed, 7.0f) < 4.0f;
			const float walkSpeed = 150.0f;
			if (walking) {
				angle += dt * walkSpeed / radius;
			}
			const float a = angle;

			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.cell = cell;
			state.worldspace = cell ? 0 : worldspace;
			state.x = cx + radius * std::sin(a);
			state.y = cy + radius * std::cos(a);
			state.z = cz;
			state.heading = a + std::numbers::pi_v<float> / 2;  // tangent to the circle
			state.speed = walking ? walkSpeed : 0.0f;
			Net::Send(peer, Protocol::Encode(state), false);
		}
	}

	enet_peer_disconnect(peer, 0);
	enet_host_flush(host);
	enet_host_destroy(host);
	Net::Shutdown();
	return 0;
}
