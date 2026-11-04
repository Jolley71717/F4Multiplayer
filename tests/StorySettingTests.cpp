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

TEST("story setting: sQuests reads safe or story whatever its case, quotes or spaces; anything else is neither")
{
	CHECK(StorySetting::ParseQuests("safe") == true);
	CHECK(StorySetting::ParseQuests("SAFE") == true);
	CHECK(StorySetting::ParseQuests(" 'Safe' ") == true);
	CHECK(StorySetting::ParseQuests("story") == false);
	CHECK(StorySetting::ParseQuests("\"Story\"") == false);
	CHECK(!StorySetting::ParseQuests("").has_value());
	CHECK(!StorySetting::ParseQuests("all").has_value());
	CHECK(!StorySetting::ParseQuests("shared").has_value());
	CHECK(StorySetting::QuestsName(true) == "safe");
	CHECK(StorySetting::QuestsName(false) == "story");
}
