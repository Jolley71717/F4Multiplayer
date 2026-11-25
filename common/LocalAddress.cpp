#include "LocalAddress.h"

#include "Net.h"

#include <algorithm>
#include <charconv>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace LocalAddress
{
	bool IsUsableV4(std::string_view a_text)
	{
		int parts[4]{};
		int count = 0;
		while (count < 4) {
			const auto dot = a_text.find('.');
			const auto part = a_text.substr(0, dot);
			if (part.empty() || part.size() > 3) {
				return false;
			}
			const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), parts[count]);
			if (error != std::errc{} || end != part.data() + part.size() || parts[count] > 255) {
				return false;
			}
			++count;
			if (dot == std::string_view::npos) {
				break;
			}
			a_text.remove_prefix(dot + 1);
			if (count == 4) {
				return false;  // more than four parts
			}
		}
		if (count != 4 || (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0)) {
			return false;
		}
		return parts[0] != 0 && parts[0] != 127 && parts[0] < 224 && !(parts[0] == 169 && parts[1] == 254);
	}

	namespace
	{
		std::string Text(const in_addr& a_address)
		{
			char text[INET_ADDRSTRLEN]{};
			return inet_ntop(AF_INET, &a_address, text, sizeof(text)) ? std::string{ text } : std::string{};
		}
	}

	std::string Default()
	{
		if (!Net::Initialize()) {
			return {};
		}
		std::string result;
		const auto  sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock != INVALID_SOCKET) {
			sockaddr_in target{};
			target.sin_family = AF_INET;
			target.sin_port = htons(9);
			inet_pton(AF_INET, "192.0.2.1", &target.sin_addr);  // a documentation address; a UDP connect sends nothing
			sockaddr_in local{};
			int         size = sizeof(local);
			if (::connect(sock, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0 &&
				::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
				result = Text(local.sin_addr);
			}
			::closesocket(sock);
		}
		Net::Shutdown();
		return IsUsableV4(result) ? result : std::string{};
	}

	std::vector<std::string> All()
	{
		std::vector<std::string> out;
		if (const auto first = Default(); !first.empty()) {
			out.push_back(first);
		}
		if (!Net::Initialize()) {
			return out;
		}
		char name[256]{};
		if (::gethostname(name, sizeof(name) - 1) == 0) {
			addrinfo  hints{};
			hints.ai_family = AF_INET;
			addrinfo* found = nullptr;
			if (::getaddrinfo(name, nullptr, &hints, &found) == 0) {
				for (auto* it = found; it; it = it->ai_next) {
					const auto text = Text(reinterpret_cast<const sockaddr_in*>(it->ai_addr)->sin_addr);
					if (IsUsableV4(text) && std::ranges::find(out, text) == out.end()) {
						out.push_back(text);
					}
				}
				::freeaddrinfo(found);
			}
		}
		Net::Shutdown();
		return out;
	}
}
