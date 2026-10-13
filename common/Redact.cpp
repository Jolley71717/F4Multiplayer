#include "Redact.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <random>

namespace Redact
{
	namespace
	{
		bool IsDigit(char a_c) { return a_c >= '0' && a_c <= '9'; }

		bool IsAlnum(char a_c) { return IsDigit(a_c) || (a_c >= 'a' && a_c <= 'z') || (a_c >= 'A' && a_c <= 'Z'); }

		// The length of the IPv4 address at a_pos (0 if there is none there). "1.2.3.4.5" and the
		// like are version numbers or other things, not addresses.
		std::size_t AddressAt(std::string_view a_text, std::size_t a_pos, bool& a_loopback)
		{
			std::size_t i = a_pos;
			int         first = 0;
			for (int part = 0; part < 4; ++part) {
				if (part > 0) {
					if (i >= a_text.size() || a_text[i] != '.') {
						return 0;
					}
					++i;
				}
				const auto start = i;
				int        value = 0;
				while (i < a_text.size() && IsDigit(a_text[i]) && i - start < 4) {
					value = value * 10 + (a_text[i] - '0');
					++i;
				}
				if (i == start || i - start > 3 || value > 255) {
					return 0;
				}
				if (part == 0) {
					first = value;
				}
			}
			if (i < a_text.size() && (IsDigit(a_text[i]) || (a_text[i] == '.' && i + 1 < a_text.size() && IsDigit(a_text[i + 1])))) {
				return 0;
			}
			a_loopback = first == 127;
			return i - a_pos;
		}

		// "a.b.c.x" for an address AddressAt accepted.
		std::string MaskQuad(std::string_view a_quad)
		{
			return std::string{ a_quad.substr(0, a_quad.rfind('.') + 1) } + "x";
		}
	}

	std::uint64_t Salt()
	{
		static const std::uint64_t salt = [] {
			std::random_device rd;
			return (static_cast<std::uint64_t>(rd()) << 32) | rd();
		}();
		return salt;
	}

	std::string Alias(std::uint64_t a_id)
	{
		return Alias(a_id, Salt());
	}

	std::string Alias(std::uint64_t a_id, std::uint64_t a_salt)
	{
		// FNV-1a over the salt, then the id.
		std::uint64_t hash = 0xCBF29CE484222325ull;
		for (const auto value : { a_salt, a_id }) {
			for (int i = 0; i < 8; ++i) {
				hash ^= (value >> (i * 8)) & 0xFF;
				hash *= 0x100000001B3ull;
			}
		}
		return std::format("~{:06x}", (hash ^ (hash >> 32)) & 0xFFFFFF);
	}

	std::string MaskAddress(std::string_view a_ip, std::uint16_t a_port)
	{
		if (a_ip.find(':') != std::string_view::npos) {
			if (a_ip == "::1") {
				return std::format("[{}]:{}", a_ip, a_port);
			}
			// The first four groups name the network, not the machine; stop at "::" (zeros left out).
			std::string kept;
			int         groups = 0;
			std::size_t pos = 0;
			while (groups < 4 && pos < a_ip.size()) {
				const auto end = (std::min)(a_ip.find(':', pos), a_ip.size());
				if (end == pos) {
					break;
				}
				kept += a_ip.substr(pos, end - pos);
				kept += ':';
				++groups;
				pos = end + 1;
			}
			return std::format("[{}x]:{}", kept, a_port);
		}
		bool loopback = false;
		if (!a_ip.empty() && AddressAt(a_ip, 0, loopback) == a_ip.size()) {
			return std::format("{}:{}", loopback ? std::string{ a_ip } : MaskQuad(a_ip), a_port);
		}
		return std::format("?:{}", a_port);
	}

	std::string Scrub(std::string_view a_line)
	{
		std::string out;
		out.reserve(a_line.size());
		std::size_t i = 0;
		while (i < a_line.size()) {
			if (!IsDigit(a_line[i])) {
				out += a_line[i++];
				continue;
			}
			const bool joined = i > 0 && (IsAlnum(a_line[i - 1]) || a_line[i - 1] == '.');
			bool       loopback = false;
			if (const auto length = joined ? 0 : AddressAt(a_line, i, loopback); length != 0) {
				const auto quad = a_line.substr(i, length);
				out += loopback ? std::string{ quad } : MaskQuad(quad);
				i += length;
				continue;
			}
			auto end = i;
			while (end < a_line.size() && IsDigit(a_line[end])) {
				++end;
			}
			const auto    run = a_line.substr(i, end - i);
			std::uint64_t id = 0;
			if (run.size() == 17 && run.starts_with("7656") && std::from_chars(run.data(), run.data() + run.size(), id).ec == std::errc{}) {
				out += Alias(id);
			} else {
				out += run;
			}
			i = end;
		}
		return out;
	}

	std::string OneLine(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		std::ranges::copy_if(a_text, std::back_inserter(out), [](unsigned char a_c) { return a_c >= 0x20 && a_c != 0x7F; });
		return out;
	}
}
