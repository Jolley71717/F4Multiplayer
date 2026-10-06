#include "DevChannel.h"

#include "Config.h"
#include "DevCommands.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ShlObj.h>
#include <bcrypt.h>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace DevChannel
{
	namespace
	{
		constexpr auto        MAIN_THREAD_TIMEOUT = 5s;
		constexpr std::size_t MAX_LINE = 4096;

		std::string token;

		std::optional<std::filesystem::path> TokenPath()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				return std::nullopt;
			}
			std::filesystem::path path{ docs };
			CoTaskMemFree(docs);
			return path / "My Games" / "Fallout4" / "F4SE" / "F4Multiplayer_dev.token";
		}

		bool CreateToken()
		{
			std::array<std::uint8_t, 32> bytes{};
			if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
				return false;
			}

			token.clear();
			for (const auto b : bytes) {
				token += std::format("{:02x}", b);
			}

			const auto path = TokenPath();
			if (!path) {
				return false;
			}
			std::ofstream file{ *path, std::ios::trunc };
			file << token;
			return static_cast<bool>(file);
		}

		bool TokenMatches(std::string_view a_candidate)
		{
			if (a_candidate.size() != token.size()) {
				return false;
			}
			unsigned char diff = 0;
			for (std::size_t i = 0; i < token.size(); ++i) {
				diff |= static_cast<unsigned char>(token[i] ^ a_candidate[i]);
			}
			return diff == 0;
		}

		// Game state may only be touched on the main thread, so commands that need it
		// are queued through F4SE's task interface and the listener waits for the result.
		std::string RunOnMainThread(std::function<std::string()> a_fn)
		{
			auto promise = std::make_shared<std::promise<std::string>>();
			auto future = promise->get_future();

			F4SE::GetTaskInterface()->AddTask([promise, fn = std::move(a_fn)]() {
				promise->set_value(fn());
			});

			if (future.wait_for(MAIN_THREAD_TIMEOUT) != std::future_status::ready) {
				return "error: timed out waiting for game thread";
			}
			return future.get();
		}

		std::string HandleCommand(std::string_view a_line)
		{
			const auto split = a_line.find(' ');
			const auto cmd = a_line.substr(0, split);
			const auto args = split == std::string_view::npos ? std::string_view{} : a_line.substr(split + 1);

			if (cmd == "ping") {
				return "pong";
			}
			if (cmd == "help") {
				return DevCommands::Help();
			}
			if (const auto handler = DevCommands::Find(cmd)) {
				return RunOnMainThread([handler, args = std::string{ args }]() { return handler(args); });
			}
			return std::format("error: unknown command '{}' (try help)", cmd);
		}

		void Send(SOCKET a_client, std::string_view a_reply)
		{
			const auto line = std::string{ a_reply } + "\n";
			send(a_client, line.data(), static_cast<int>(line.size()), 0);
		}

		void ServeClient(SOCKET a_client)
		{
			std::string buffer;
			char        chunk[512];
			bool        authed = false;

			for (;;) {
				const int received = recv(a_client, chunk, sizeof(chunk), 0);
				if (received <= 0) {
					break;
				}
				buffer.append(chunk, static_cast<std::size_t>(received));
				if (buffer.size() > MAX_LINE && buffer.find('\n') == std::string::npos) {
					break;
				}

				std::size_t newline;
				while ((newline = buffer.find('\n')) != std::string::npos) {
					auto line = buffer.substr(0, newline);
					buffer.erase(0, newline + 1);
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					if (line.empty()) {
						continue;
					}

					if (!authed) {
						if (!line.starts_with("auth ") || !TokenMatches(std::string_view{ line }.substr(5))) {
							REX::WARN("DevChannel: rejected unauthenticated connection");
							Send(a_client, "error: not authorized");
							closesocket(a_client);
							return;
						}
						authed = true;
						Send(a_client, "ok");
						continue;
					}

					Send(a_client, HandleCommand(line));
				}
			}

			closesocket(a_client);
		}

		void Listen(std::uint16_t a_port)
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
				REX::ERROR("DevChannel: WSAStartup failed");
				return;
			}

			const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET) {
				REX::ERROR("DevChannel: socket failed ({})", WSAGetLastError());
				return;
			}

			// Loopback only: this channel must never be reachable from other machines.
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(a_port);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
				listen(listener, 1) == SOCKET_ERROR) {
				REX::ERROR("DevChannel: could not listen on 127.0.0.1:{} ({})", a_port, WSAGetLastError());
				closesocket(listener);
				return;
			}

			REX::INFO("DevChannel: listening on 127.0.0.1:{}", a_port);

			for (;;) {
				const SOCKET client = accept(listener, nullptr, nullptr);
				if (client == INVALID_SOCKET) {
					continue;
				}
				ServeClient(client);
			}
		}
	}

	void Start()
	{
		const auto& settings = Config::Get();
		if (!settings.devChannel) {
			return;
		}

		if (!CreateToken()) {
			REX::ERROR("DevChannel: could not create auth token, not starting");
			return;
		}

		std::thread(Listen, settings.devChannelPort).detach();
	}
}
