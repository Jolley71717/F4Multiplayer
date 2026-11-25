#pragma once

#include "Redact.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// What an address typed in the multiplayer menu may hold, how it shows, and the file the joined ones are kept in.
// Kept free of the game so it can be tested. An address is the host's own place on the internet: it is shown whole
// only in the box it is typed in (and while the show button is held), masked everywhere else, and never logged.
namespace UiAddressEntry
{
	inline constexpr std::size_t   MAX_LENGTH = 80;  // a host name with a port; what the box shows is cut (ShownTail)
	inline constexpr std::size_t   MAX_RECENT = 5;
	inline constexpr std::uint16_t DEFAULT_PORT = 7779;

	// Letters, digits, '.', '-' and ':' (no spaces, no '<' '>' which the panel's text would read).
	[[nodiscard]] constexpr bool Allowed(std::uint32_t a_character)
	{
		return (a_character >= 'a' && a_character <= 'z') || (a_character >= 'A' && a_character <= 'Z') || (a_character >= '0' && a_character <= '9') ||
		       a_character == '.' || a_character == '-' || a_character == ':';
	}

	// True if a_character was added: one that is allowed, and room for it.
	inline bool Add(std::string& a_typed, std::uint32_t a_character, std::size_t a_max = MAX_LENGTH)
	{
		if (!Allowed(a_character) || a_typed.size() >= a_max) {
			return false;
		}
		a_typed.push_back(static_cast<char>(a_character));
		return true;
	}

	// "host" or "host:port" split. One colon means a port (1-65535); a second colon, or a port that is not one, gives false.
	[[nodiscard]] inline bool Split(std::string_view a_text, std::string_view& a_host, std::uint16_t& a_port, std::uint16_t a_defaultPort = DEFAULT_PORT)
	{
		a_port = a_defaultPort;
		a_host = a_text;
		const auto colon = a_text.find(':');
		if (colon == std::string_view::npos) {
			return true;
		}
		if (a_text.find(':', colon + 1) != std::string_view::npos) {
			return false;
		}
		const auto    digits = a_text.substr(colon + 1);
		std::uint16_t parsed{};
		const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
		if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size() || parsed == 0) {
			return false;
		}
		a_host = a_text.substr(0, colon);
		a_port = parsed;
		return true;
	}

	// "host" or "host:port": not empty, at most one ':', port 1-65535 when given, a host of labels [A-Za-z0-9-] split by
	// '.', none empty, none starting or ending with '-'.
	[[nodiscard]] inline bool Valid(std::string_view a_text)
	{
		std::string_view host;
		std::uint16_t    port{};
		if (a_text.empty() || a_text.size() > MAX_LENGTH || !Split(a_text, host, port) || host.empty()) {
			return false;
		}
		std::size_t from = 0;
		while (true) {
			const auto dot = host.find('.', from);
			const auto label = host.substr(from, dot == std::string_view::npos ? std::string_view::npos : dot - from);
			if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
				return false;
			}
			if (!std::ranges::all_of(label, [](char a_c) { return a_c == '-' || std::isalnum(static_cast<unsigned char>(a_c)); })) {
				return false;
			}
			if (dot == std::string_view::npos) {
				return true;
			}
			from = dot + 1;
		}
	}

	// What a paste gives: the first line, without spaces around it; "" when it is too long or holds any character Allowed
	// refuses (nothing half-pasted).
	[[nodiscard]] inline std::string FromClipboard(std::string_view a_text)
	{
		const auto line = a_text.substr(0, a_text.find_first_of("\r\n"));
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string_view::npos) {
			return {};
		}
		const auto trimmed = line.substr(first, line.find_last_not_of(" \t") - first + 1);
		if (trimmed.size() > MAX_LENGTH || !std::ranges::all_of(trimmed, [](char a_c) { return Allowed(static_cast<unsigned char>(a_c)); })) {
			return {};
		}
		return std::string{ trimmed };
	}

	// The end of a long text for the box: "..." and the tail, at most a_room characters in all.
	[[nodiscard]] inline std::string ShownTail(std::string_view a_text, std::size_t a_room = 40)
	{
		if (a_text.size() <= a_room) {
			return std::string{ a_text };
		}
		return "..." + std::string{ a_text.substr(a_text.size() - (a_room - 3)) };
	}

	// For the status line, the HUD, the rows and the log: "192.0.2.x:7779" for an IPv4 address, "(name):7779" for a host
	// name (the port is the default one's when none was typed). A text that is not an address gives "(address)".
	[[nodiscard]] inline std::string Masked(std::string_view a_address, std::uint16_t a_defaultPort = DEFAULT_PORT)
	{
		std::string_view host;
		std::uint16_t    port{};
		if (!Split(a_address, host, port, a_defaultPort) || host.empty()) {
			return "(address)";
		}
		// Four numbers of 0-255 are an IPv4 address; anything else is a name.
		int         parts = 0;
		bool        ipv4 = true;
		std::size_t from = 0;
		while (ipv4) {
			const auto dot = host.find('.', from);
			const auto part = host.substr(from, dot == std::string_view::npos ? std::string_view::npos : dot - from);
			unsigned   value = 0;
			const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
			ipv4 = !part.empty() && part.size() <= 3 && ec == std::errc{} && end == part.data() + part.size() && value <= 255;
			++parts;
			if (dot == std::string_view::npos) {
				break;
			}
			from = dot + 1;
		}
		if (ipv4 && parts == 4) {
			return Redact::MaskAddress(host, port);
		}
		return "(name):" + std::to_string(port);
	}

	namespace detail
	{
		inline bool SameAddress(std::string_view a_a, std::string_view a_b)
		{
			return std::ranges::equal(a_a, a_b, [](char a_x, char a_y) { return std::tolower(static_cast<unsigned char>(a_x)) == std::tolower(static_cast<unsigned char>(a_y)); });
		}
	}

	// The recent list, newest first, from a text of one address a line (a comma works too). Entries that are not addresses
	// are dropped, a repeat (any case) is dropped, and at most MAX_RECENT are kept.
	[[nodiscard]] inline std::vector<std::string> ParseRecent(std::string_view a_text)
	{
		std::vector<std::string> list;
		std::string              entry;
		const auto               flush = [&] {
            const auto text = FromClipboard(entry);
            entry.clear();
            if (text.empty() || !Valid(text) || list.size() >= MAX_RECENT) {
                return;
            }
            if (std::ranges::none_of(list, [&](const std::string& a_other) { return detail::SameAddress(a_other, text); })) {
                list.push_back(text);
            }
        };
		for (const char c : a_text) {
			if (c == '\n' || c == ',') {
				flush();
			} else {
				entry.push_back(c);
			}
		}
		flush();
		return list;
	}

	[[nodiscard]] inline std::string FormatRecent(const std::vector<std::string>& a_recent)
	{
		std::string text;
		for (const auto& address : a_recent) {
			text += address + "\n";
		}
		return text;
	}

	// To the front; one the same in any case is removed first; cut to MAX_RECENT. Not an address: nothing.
	inline void Remember(std::vector<std::string>& a_recent, std::string_view a_address)
	{
		if (!Valid(a_address)) {
			return;
		}
		std::erase_if(a_recent, [&](const std::string& a_other) { return detail::SameAddress(a_other, a_address); });
		a_recent.insert(a_recent.begin(), std::string{ a_address });
		if (a_recent.size() > MAX_RECENT) {
			a_recent.resize(MAX_RECENT);
		}
	}

	// The file of the recent addresses, beside the settings file and not in it: sharing the ini never shares an address.
	[[nodiscard]] inline std::vector<std::string> ReadFile(const std::filesystem::path& a_path)
	{
		std::ifstream      in(a_path, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		return ParseRecent(text.str());
	}

	// False when it could not be written. An empty list removes the file.
	inline bool WriteFile(const std::filesystem::path& a_path, const std::vector<std::string>& a_recent)
	{
		std::error_code ec;
		if (a_recent.empty()) {
			std::filesystem::remove(a_path, ec);
			return !std::filesystem::exists(a_path, ec);
		}
		auto temp = a_path;
		temp += ".tmp";
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			out << FormatRecent(a_recent);
			if (!out) {
				return false;
			}
		}
		std::filesystem::rename(temp, a_path, ec);
		if (ec) {
			std::filesystem::remove(temp, ec);
			return false;
		}
		return true;
	}
}
