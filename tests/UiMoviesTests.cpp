#include "SourceTree.h"
#include "Test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

// The menu movies (ui/<Name>/<Name>.swf) are committed beside their ActionScript, so building, packaging and playing
// need no Java (docs/UI.md). The build records what each was built from (<Name>.sources: every .as of ui/<Name> and
// ui/shared, the SHA-256 of its text with LF line endings); here the record is checked against the sources, so a change
// to them without a rebuild (tools/flash/build.sh or build.ps1) fails.

namespace
{
	// SHA-256 (FIPS 180-4), for the record's hashes: small, and the same on every platform the tests build on.
	std::string Sha256(const std::string& a_data)
	{
		constexpr std::array<std::uint32_t, 64> K{ 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
			0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
			0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
			0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
			0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee,
			0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
		std::array<std::uint32_t, 8> h{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		const auto rotr = [](std::uint32_t a_x, int a_n) { return (a_x >> a_n) | (a_x << (32 - a_n)); };

		std::string message = a_data;
		const std::uint64_t bits = static_cast<std::uint64_t>(a_data.size()) * 8;
		message += static_cast<char>(0x80);
		while (message.size() % 64 != 56) {
			message += '\0';
		}
		for (int i = 7; i >= 0; --i) {
			message += static_cast<char>((bits >> (i * 8)) & 0xFF);
		}
		for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
			std::array<std::uint32_t, 64> w{};
			for (int i = 0; i < 16; ++i) {
				const auto byte = [&](int a_k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + i * 4 + a_k])); };
				w[i] = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
			}
			for (int i = 16; i < 64; ++i) {
				const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			auto a = h;
			for (int i = 0; i < 64; ++i) {
				const auto s1 = rotr(a[4], 6) ^ rotr(a[4], 11) ^ rotr(a[4], 25);
				const auto ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
				const auto t1 = a[7] + s1 + ch + K[i] + w[i];
				const auto s0 = rotr(a[0], 2) ^ rotr(a[0], 13) ^ rotr(a[0], 22);
				const auto maj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
				const auto t2 = s0 + maj;
				a = { t1 + t2, a[0], a[1], a[2], a[3] + t1, a[4], a[5], a[6] };
			}
			for (int i = 0; i < 8; ++i) {
				h[i] += a[i];
			}
		}
		std::string hex;
		for (const auto word : h) {
			hex += std::format("{:08x}", word);
		}
		return hex;
	}

	std::string WithLf(std::string a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] == '\r' && i + 1 < a_text.size() && a_text[i + 1] == '\n') {
				continue;
			}
			out += a_text[i];
		}
		return out;
	}

	// The record as the build writes it, without its comment line: "<sha256>  <path under ui/>" in ordinal order.
	std::vector<std::string> Expected(const std::filesystem::path& a_ui, const std::string& a_name)
	{
		std::vector<std::string> paths;
		for (const auto top : { a_name, std::string{ "shared" } }) {
			std::error_code error;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(a_ui / top, error)) {
				if (entry.is_regular_file() && entry.path().extension() == ".as") {
					paths.push_back(std::filesystem::relative(entry.path(), a_ui).generic_string());
				}
			}
		}
		std::ranges::sort(paths);
		std::vector<std::string> lines;
		for (const auto& path : paths) {
			lines.push_back(std::format("{}  {}", Sha256(WithLf(SourceTree::Read(a_ui / path))), path));
		}
		return lines;
	}

	std::vector<std::string> Recorded(const std::filesystem::path& a_file)
	{
		std::vector<std::string> lines;
		std::string              line;
		for (const char c : WithLf(SourceTree::Read(a_file)) + "\n") {
			if (c != '\n') {
				line += c;
				continue;
			}
			if (!line.empty() && !line.starts_with('#')) {
				lines.push_back(line);
			}
			line.clear();
		}
		return lines;
	}
}

TEST("ui: SHA-256 gives the standard's answers")
{
	CHECK(Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(Sha256(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST("ui: every committed menu movie was built from the sources beside it (run tools/flash/build.sh or build.ps1)")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto ui = *root / "ui";
	int movies = 0;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(ui, error)) {
		const auto name = entry.path().filename().string();
		if (!entry.is_directory() || name == "shared") {
			continue;
		}
		const auto movie = entry.path() / (name + ".swf");
		const auto record = entry.path() / (name + ".sources");
		if (!std::filesystem::exists(movie) || !std::filesystem::exists(record)) {
			Test::Fail(__FILE__, __LINE__, std::format("ui/{0}: {0}.swf and {0}.sources must be committed (tools/flash/build.sh)", name));
			continue;
		}
		++movies;
		// Uncompressed SWF 15, as the build makes it (its compile time cleared, so the same sources give the same bytes).
		const auto bytes = SourceTree::Read(movie);
		CHECK(bytes.size() > 8 && bytes.starts_with("FWS") && static_cast<unsigned char>(bytes[3]) == 15);
		if (Expected(ui, name) != Recorded(record)) {
			Test::Fail(__FILE__, __LINE__, std::format("ui/{}: the sources changed since {}.swf was built: rebuild it (tools/flash/build.sh) and commit it", name, name));
		}
	}
	CHECK(movies > 0);
}
