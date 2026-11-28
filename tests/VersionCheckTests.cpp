// Who may join whom by the mod's version (common/VersionCheck.h): the same major.minor joins, the patch never matters.

#include "Test.h"

#include "Protocol.h"
#include "VersionCheck.h"

#include <string>

using VersionCheck::Verdict;

TEST("version check: versions parse as three numbers, and anything else is no version")
{
	const auto v = VersionCheck::Parse("0.24.1");
	REQUIRE(v);
	CHECK(v->major == 0 && v->minor == 24 && v->patch == 1);
	CHECK(VersionCheck::Format(*v) == "0.24.1");
	CHECK(VersionCheck::Format(*VersionCheck::Parse("1.0.12")) == "1.0.12");
	CHECK(VersionCheck::Format(*VersionCheck::Parse("00.024.1")) == "0.24.1");  // as a person reads it
	for (const char* bad : { "", "0", "0.24", "0.24.", ".24.1", "0..1", "0.24.1.0", "0.24.1-rc1", "v0.24.1", " 0.24.1", "0.24.x", "1234567.0.0", "0.24.-1" }) {
		CHECK(!VersionCheck::Parse(bad));
	}
}

TEST("version check: the same major.minor joins whatever the patch; another minor or major is refused and says who updates")
{
	CHECK(VersionCheck::Compare("0.24.0", "0.24.3") == Verdict::kJoins);
	CHECK(VersionCheck::Compare("0.24.3", "0.24.0") == Verdict::kJoins);
	CHECK(VersionCheck::Compare("0.24.7", "0.24.7") == Verdict::kJoins);

	CHECK(VersionCheck::Compare("0.24.0", "0.25.0") == Verdict::kHostOlder);
	CHECK(VersionCheck::Compare("0.24.9", "0.25.0") == Verdict::kHostOlder);  // the patch doesn't weigh against the minor
	CHECK(VersionCheck::Compare("0.25.0", "0.24.3") == Verdict::kHostNewer);
	CHECK(VersionCheck::Compare("0.24.1", "1.0.0") == Verdict::kHostOlder);
	CHECK(VersionCheck::Compare("1.0.2", "0.24.1") == Verdict::kHostNewer);
	CHECK(VersionCheck::Compare("1.0.0", "0.99.0") == Verdict::kHostNewer);

	CHECK(VersionCheck::Compare("0.24.0", "") == Verdict::kUnreadable);
	CHECK(VersionCheck::Compare("0.24.0", "0.24") == Verdict::kUnreadable);
	CHECK(VersionCheck::Compare("0.24.0", "garbage") == Verdict::kUnreadable);
	CHECK(VersionCheck::Compare("", "0.24.0") == Verdict::kUnreadable);

	// This build's own version is one, and joins itself.
	CHECK(VersionCheck::Compare(VersionCheck::OWN, VersionCheck::OWN) == Verdict::kJoins);
}

TEST("version check: the refusal names both full versions, fits any version's reject, and reads back")
{
	const auto older = VersionCheck::Refusal(true, "0.24.1", "0.25.0");
	CHECK(older == "The host has F4Multiplayer 0.24.1 and you have 0.25.0. Both players need the same version: the host should update.");
	const auto newer = VersionCheck::Refusal(false, "1.0.0", "0.24.3");
	CHECK(newer == "The host has F4Multiplayer 1.0.0 and you have 0.24.3. Both players need the same version: update to the latest from the Nexus Files tab.");

	const auto back = VersionCheck::ReadRefusal(older);
	REQUIRE(back);
	CHECK(back->hostOlder);
	CHECK(back->host == "0.24.1");
	CHECK(back->you == "0.25.0");
	const auto back2 = VersionCheck::ReadRefusal(newer);
	REQUIRE(back2);
	CHECK(!back2->hostOlder);
	CHECK(back2->host == "1.0.0");
	CHECK(back2->you == "0.24.3");

	// The longest names there can be still fit the 200 bytes a reason has on the wire (older copies refuse a longer one).
	const std::string longest(VersionCheck::MAX_NAMED_LENGTH, '9');
	CHECK(VersionCheck::Refusal(false, longest, longest).size() <= Protocol::MAX_REASON_LENGTH);
	CHECK(VersionCheck::Refusal(true, longest, longest).size() <= Protocol::MAX_REASON_LENGTH);

	CHECK(!VersionCheck::ReadRefusal("Server is full"));
	CHECK(!VersionCheck::ReadRefusal("The host has F4Multiplayer 0.24.1 and you have 0.25.0."));
	CHECK(!VersionCheck::ReadRefusal("The host has F4Multiplayer  and you have 0.25.0. Both players need the same version: the host should update."));
}
