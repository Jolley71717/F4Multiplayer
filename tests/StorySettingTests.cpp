#include "Test.h"

#include "StorySetting.h"

TEST("story setting: sStory reads shared or own whatever its case, quotes or spaces; anything else is neither")
{
	CHECK(StorySetting::Parse("shared") == true);
	CHECK(StorySetting::Parse("Shared") == true);
	CHECK(StorySetting::Parse("SHARED") == true);
	CHECK(StorySetting::Parse(" \"shared\" ") == true);
	CHECK(StorySetting::Parse("own") == false);
	CHECK(StorySetting::Parse("Own") == false);
	CHECK(StorySetting::Parse("'own'") == false);
	CHECK(!StorySetting::Parse("").has_value());
	CHECK(!StorySetting::Parse("\"\"").has_value());
	CHECK(!StorySetting::Parse("yes").has_value());
	CHECK(!StorySetting::Parse("shared story").has_value());
	CHECK(StorySetting::Name(true) == "shared");
	CHECK(StorySetting::Name(false) == "own");
}
