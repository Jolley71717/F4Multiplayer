#include "Test.h"

#include "LocalAddress.h"

TEST("local address: only an address a machine can be reached at on a network is usable")
{
	CHECK(LocalAddress::IsUsableV4("192.0.2.10"));
	CHECK(LocalAddress::IsUsableV4("10.0.0.1"));
	CHECK(LocalAddress::IsUsableV4("172.16.5.200"));
	CHECK(!LocalAddress::IsUsableV4(""));
	CHECK(!LocalAddress::IsUsableV4("127.0.0.1"));        // this machine only
	CHECK(!LocalAddress::IsUsableV4("0.0.0.0"));
	CHECK(!LocalAddress::IsUsableV4("0.1.2.3"));
	CHECK(!LocalAddress::IsUsableV4("169.254.10.20"));   // self-assigned, no DHCP answered
	CHECK(!LocalAddress::IsUsableV4("224.0.0.1"));        // multicast
	CHECK(!LocalAddress::IsUsableV4("255.255.255.255"));
	CHECK(!LocalAddress::IsUsableV4("192.0.2"));
	CHECK(!LocalAddress::IsUsableV4("192.0.2.10.5"));
	CHECK(!LocalAddress::IsUsableV4("192.0.2.256"));
	CHECK(!LocalAddress::IsUsableV4("192.0.2.x"));
	CHECK(!LocalAddress::IsUsableV4("host.example"));
	CHECK(!LocalAddress::IsUsableV4("192.0.2.10:7779"));
}

TEST("local address: the default address, when there is one, is usable and heads the list")
{
	const auto all = LocalAddress::All();
	const auto first = LocalAddress::Default();
	for (const auto& address : all) {
		CHECK(LocalAddress::IsUsableV4(address));
	}
	if (!first.empty()) {  // a machine with no route has none
		CHECK(LocalAddress::IsUsableV4(first));
		REQUIRE(!all.empty());
		CHECK(all.front() == first);
	}
}
