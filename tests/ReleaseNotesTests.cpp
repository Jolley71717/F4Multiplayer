#include "Test.h"

#include "game/ReleaseNotes.h"

TEST("release notes: a release's file or a candidate's, and nothing else")
{
	const auto release = ReleaseNotes::Parse("RELEASE-0.11.2.md");
	CHECK(release.has_value());
	CHECK((release->version == ReleaseNotes::Version{ 0, 11, 2 }));
	CHECK(release->candidate == 0);
	const auto candidate = ReleaseNotes::Parse("RELEASE-0.11.2-rc3.md");
	CHECK(candidate.has_value());
	CHECK(candidate->candidate == 3);
	for (const char* name : { "RELEASE-0.11.md", "RELEASE-0.11.2-rc0.md", "RELEASE-0.11.2-rc.md", "RELEASE-0.11.2.txt", "NEXUS-0.11.2.md", "RELEASE-a.b.c.md", "RELEASE-0.11.2-beta.md" }) {
		CHECK(!ReleaseNotes::Parse(name).has_value());
	}
}

TEST("release notes: newest first, a release over its candidates, the latest candidate before there's a release")
{
	const std::vector<std::string> files{ "RELEASE-0.9.0.md", "RELEASE-0.10.0-rc1.md", "RELEASE-0.10.0.md", "RELEASE-0.10.0-rc2.md", "RELEASE-0.11.0-rc1.md", "RELEASE-0.11.0-rc2.md", "README.md" };
	const auto picked = ReleaseNotes::Pick(files, { 1, 0, 0 }, 10);
	CHECK((picked == std::vector<std::string>{ "RELEASE-0.11.0-rc2.md", "RELEASE-0.10.0.md", "RELEASE-0.9.0.md" }));
}

TEST("release notes: nothing newer than the running version, and no more than asked for")
{
	const std::vector<std::string> files{ "RELEASE-0.9.0.md", "RELEASE-0.10.0.md", "RELEASE-0.11.0.md" };
	CHECK((ReleaseNotes::Pick(files, { 0, 10, 0 }, 10) == std::vector<std::string>{ "RELEASE-0.10.0.md", "RELEASE-0.9.0.md" }));
	CHECK((ReleaseNotes::Pick(files, { 1, 0, 0 }, 1) == std::vector<std::string>{ "RELEASE-0.11.0.md" }));
	CHECK(ReleaseNotes::Pick({}, { 1, 0, 0 }, 5).empty());
}
