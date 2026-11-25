#include "Test.h"

#include "ConfigRead.h"
#include "Net.h"
#include "game/UiAddressEntry.h"

#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <vector>

// The address typed in the menu to join a host: what may be typed, which texts are addresses, how one shows (masked, never
// whole outside its box), the recent list and its file, and what the log says of an address the menu wrote. Documentation
// addresses (192.0.2.x) throughout.
namespace
{
	std::filesystem::path TempFile()
	{
		return std::filesystem::temp_directory_path() / std::format("f4mp-recent-{:016X}.txt", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
	}
}

TEST("address entry: letters, digits, dots, dashes and a colon are taken; spaces, angle brackets and control characters are not")
{
	std::string typed;
	for (const char c : std::string_view{ "Host-1.example:7779" }) {
		CHECK(UiAddressEntry::Add(typed, static_cast<unsigned char>(c)));
	}
	CHECK(typed == "Host-1.example:7779");
	for (const std::uint32_t c : { ' ', '<', '>', '\t', '\n', 0u, '/', '_', '@', 0xE9u }) {
		CHECK(!UiAddressEntry::Add(typed, c));
	}
	CHECK(typed == "Host-1.example:7779");
}

TEST("address entry: a text stops at 80 characters")
{
	std::string typed;
	for (int i = 0; i < 100; ++i) {
		UiAddressEntry::Add(typed, 'a');
	}
	CHECK(typed.size() == UiAddressEntry::MAX_LENGTH);
	CHECK(typed.size() == 80u);
}

TEST("address entry: an IPv4, a name, and either with a port are valid; the rest are not")
{
	for (const char* good : { "192.0.2.10", "192.0.2.10:7779", "host.example", "host.example:7000", "localhost", "a-b.example:65535", "Host" }) {
		CHECK(UiAddressEntry::Valid(good));
	}
	for (const char* bad : { "", ":7779", "192.0.2.10:", "192.0.2.10:0", "192.0.2.10:65536", "192.0.2.10:7779:1", "a::b", "-host.example", "host-.example", "host..example", "host.example.", ".host",
	                         "host example", "host.example:port", "<b>", "192.0.2.10:-1" }) {
		CHECK(!UiAddressEntry::Valid(bad));
	}
	CHECK(!UiAddressEntry::Valid(std::string(81, 'a')));
}

TEST("address entry: a paste takes the first line without the spaces around it, and nothing when a character is not allowed or it is too long")
{
	CHECK(UiAddressEntry::FromClipboard("  192.0.2.10:7779 \r\nsecond line") == "192.0.2.10:7779");
	CHECK(UiAddressEntry::FromClipboard("host.example\n") == "host.example");
	CHECK(UiAddressEntry::FromClipboard("").empty());
	CHECK(UiAddressEntry::FromClipboard("   ").empty());
	CHECK(UiAddressEntry::FromClipboard("http://host.example/").empty());
	CHECK(UiAddressEntry::FromClipboard("host example").empty());
	CHECK(UiAddressEntry::FromClipboard(std::string(81, 'a')).empty());
}

TEST("address entry: the status shows an IPv4 with its last part hidden and a name not at all")
{
	CHECK(UiAddressEntry::Masked("192.0.2.10") == "192.0.2.x:7779");
	CHECK(UiAddressEntry::Masked("192.0.2.10:7000") == "192.0.2.x:7000");
	CHECK(UiAddressEntry::Masked("192.0.2.10", 7800) == "192.0.2.x:7800");
	CHECK(UiAddressEntry::Masked("host.example") == "(name):7779");
	CHECK(UiAddressEntry::Masked("host.example:7000") == "(name):7000");
	CHECK(UiAddressEntry::Masked("300.0.2.10") == "(name):7779");  // not an IPv4: treated as a name, which shows nothing
	CHECK(UiAddressEntry::Masked("::::") == "(address)");
	// Nothing of a host name, nor of an IPv4 last part, is left in a masked text.
	CHECK(UiAddressEntry::Masked("secret-host.example:1234").find("secret") == std::string::npos);
	CHECK(UiAddressEntry::Masked("198.51.100.222").find("222") == std::string::npos);
}

TEST("address entry: the box shows the end of a long text")
{
	CHECK(UiAddressEntry::ShownTail("192.0.2.10") == "192.0.2.10");
	const std::string longText = std::string(30, 'a') + std::string(30, 'b');
	const auto        shown = UiAddressEntry::ShownTail(longText);
	CHECK(shown.size() == 40u);
	CHECK(shown.starts_with("..."));
	CHECK(shown.ends_with(std::string(30, 'b')));
}

TEST("recent addresses: the newest is first, a repeat moves up instead of doubling, the sixth falls off")
{
	std::vector<std::string> recent;
	for (const char* address : { "192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4", "192.0.2.5" }) {
		UiAddressEntry::Remember(recent, address);
	}
	CHECK(recent.size() == 5u);
	CHECK(recent.front() == "192.0.2.5");
	UiAddressEntry::Remember(recent, "192.0.2.2");
	CHECK(recent.size() == 5u);
	CHECK(recent.front() == "192.0.2.2");
	UiAddressEntry::Remember(recent, "HOST.example");
	UiAddressEntry::Remember(recent, "host.EXAMPLE");  // the same address in another case
	CHECK(recent.size() == 5u);
	CHECK(recent.front() == "host.EXAMPLE");
	CHECK(std::ranges::count(recent, std::string{ "192.0.2.1" }) == 0);  // the oldest fell off
	UiAddressEntry::Remember(recent, "not an address");
	CHECK(recent.size() == 5u);
	CHECK(recent.front() == "host.EXAMPLE");
}

TEST("recent addresses: the text round-trips and drops entries that are not addresses")
{
	const std::vector<std::string> recent{ "192.0.2.10:7000", "host.example", "192.0.2.11" };
	CHECK(UiAddressEntry::ParseRecent(UiAddressEntry::FormatRecent(recent)) == recent);
	const auto parsed = UiAddressEntry::ParseRecent("192.0.2.1\r\nnot an address\n\n192.0.2.1\nhttp://x/\n192.0.2.2,192.0.2.3\n192.0.2.4\n192.0.2.5\n192.0.2.6\n");
	CHECK((parsed == std::vector<std::string>{ "192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4", "192.0.2.5" }));
}

TEST("recent addresses: the file keeps them, forgets them, and an empty list removes it")
{
	const auto                     path = TempFile();
	const std::vector<std::string> recent{ "192.0.2.10", "host.example:7000" };
	CHECK(UiAddressEntry::ReadFile(path).empty());  // no file: no addresses
	CHECK(UiAddressEntry::WriteFile(path, recent));
	CHECK(UiAddressEntry::ReadFile(path) == recent);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	CHECK(UiAddressEntry::WriteFile(path, {}));
	CHECK(!std::filesystem::exists(path));
	CHECK(UiAddressEntry::ReadFile(path).empty());
}

TEST("net: host and port split: a bare host takes the default port, one colon is a port, a bad port fails")
{
	std::string   host;
	std::uint16_t port = 0;
	CHECK(Net::SplitAddress("192.0.2.10", 7779, host, port));
	CHECK(host == "192.0.2.10" && port == 7779);
	CHECK(Net::SplitAddress("host.example:7000", 7779, host, port));
	CHECK(host == "host.example" && port == 7000);
	CHECK(!Net::SplitAddress("host.example:0", 7779, host, port));
	CHECK(!Net::SplitAddress("host.example:70000", 7779, host, port));
	CHECK(!Net::SplitAddress("host.example:abc", 7779, host, port));
	CHECK(!Net::SplitAddress(":7000", 7779, host, port));
	CHECK(!Net::SplitAddress("", 7779, host, port));
}

TEST("config: the log line of a written address or password holds neither")
{
	CHECK(Config::ShownValue("sServerAddress", "192.0.2.10:7000") == "(an address)");
	CHECK(Config::ShownValue("sServerAddress", "") == "(none)");
	CHECK(Config::ShownValue("sPassword", "k3m8pq2x") == "(a password)");
	CHECK(Config::ShownValue("sPassword", "") == "(none)");
	CHECK(Config::ShownValue("iPort", "7779") == "7779");
}
