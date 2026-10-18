#include "Test.h"

#include "Name.h"

#include <string>

// A player's name as the session shows it (common/Name.h): UTF-8 of any script kept, the rest dropped.

namespace
{
	// UTF-8 literals, spelled out so the file's own encoding plays no part.
	const std::string CYRILLIC = "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82";                  // Нейт (4 characters, 8 bytes)
	const std::string CJK = "\xE5\x86\x85\xE7\x89\xB9";                              // 内特 (2 characters, 6 bytes)
	const std::string EMOJI = "\xF0\x9F\x98\x80";                                    // 😀 (1 character, 4 bytes)
	const std::string ACCENTED = "Jos\xC3\xA9";                                      // José
	const std::string JAPANESE = "\xE3\x83\x8D\xE3\x82\xA4\xE3\x83\x88";              // ネイト

	std::string Repeat(const std::string& a_piece, std::size_t a_times)
	{
		std::string out;
		for (std::size_t i = 0; i < a_times; ++i) {
			out += a_piece;
		}
		return out;
	}
}

TEST("name: well-formed UTF-8 is recognised, and every way of breaking it is not")
{
	CHECK(Name::ValidUtf8(""));
	CHECK(Name::ValidUtf8("Nate"));
	CHECK(Name::ValidUtf8(CYRILLIC));
	CHECK(Name::ValidUtf8(CJK));
	CHECK(Name::ValidUtf8(EMOJI));
	CHECK(Name::ValidUtf8(ACCENTED));
	CHECK(Name::ValidUtf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF, the last code point
	CHECK(!Name::ValidUtf8("\x80"));              // a continuation byte on its own
	CHECK(!Name::ValidUtf8("\xD0"));              // a sequence cut short
	CHECK(!Name::ValidUtf8("\xE5\x86"));
	CHECK(!Name::ValidUtf8("\xD0" "Nate"));       // a lead byte followed by ASCII
	CHECK(!Name::ValidUtf8("\xC0\x80"));          // an overlong NUL
	CHECK(!Name::ValidUtf8("\xC1\x81"));          // an overlong 'A'
	CHECK(!Name::ValidUtf8("\xE0\x80\x80"));      // overlong, 3 bytes
	CHECK(!Name::ValidUtf8("\xF0\x80\x80\x80"));  // overlong, 4 bytes
	CHECK(!Name::ValidUtf8("\xED\xA0\x80"));      // a surrogate (U+D800)
	CHECK(!Name::ValidUtf8("\xF4\x90\x80\x80"));  // past U+10FFFF
	CHECK(!Name::ValidUtf8("\xF8\x88\x80\x80\x80"));  // a 5-byte form UTF-8 no longer has
	CHECK(!Name::ValidUtf8("\xFF\xFE"));
}

TEST("name: one character is read at a time, with its length")
{
	const auto n = Name::Decode("N", 0);
	REQUIRE(n);
	CHECK(n->point == U'N' && n->length == 1);
	const auto cyr = Name::Decode(CYRILLIC, 0);
	REQUIRE(cyr);
	CHECK(cyr->point == 0x41D && cyr->length == 2);  // Н
	const auto cjk = Name::Decode(CJK, 3);
	REQUIRE(cjk);
	CHECK(cjk->point == 0x7279 && cjk->length == 3);  // 特
	const auto emoji = Name::Decode(EMOJI, 0);
	REQUIRE(emoji);
	CHECK(emoji->point == 0x1F600 && emoji->length == 4);
	CHECK(!Name::Decode("", 0));
	CHECK(!Name::Decode("N", 1));         // past the end
	CHECK(!Name::Decode(CYRILLIC, 1));    // the middle of a character
}

TEST("name: characters are counted, not bytes")
{
	CHECK(Name::Characters("") == 0);
	CHECK(Name::Characters("Nate") == 4);
	CHECK(Name::Characters(CYRILLIC) == 4);
	CHECK(Name::Characters(CJK) == 2);
	CHECK(Name::Characters(EMOJI) == 1);
	CHECK(Name::Characters(ACCENTED) == 4);
	CHECK(Name::Characters("Nate " + CYRILLIC + EMOJI) == 10);
	CHECK(Name::Characters("\xFF\xFE") == 2);  // a byte that isn't UTF-8 counts as one
}

TEST("name: letters and digits of every script stay")
{
	CHECK(Name::Sanitize("Nate") == "Nate");
	CHECK(Name::Sanitize("Nate Jr. 2") == "Nate Jr. 2");
	CHECK(Name::Sanitize(CYRILLIC) == CYRILLIC);
	CHECK(Name::Sanitize(CJK) == CJK);
	CHECK(Name::Sanitize(JAPANESE) == JAPANESE);
	CHECK(Name::Sanitize(EMOJI) == EMOJI);
	CHECK(Name::Sanitize(ACCENTED) == ACCENTED);
	CHECK(Name::Sanitize("Nate " + CYRILLIC + " " + CJK + EMOJI) == "Nate " + CYRILLIC + " " + CJK + EMOJI);
	// Punctuation a name may have, as before.
	CHECK(Name::Sanitize("Sam-1_x (the 2nd) & co.") == "Sam-1_x (the 2nd) & co.");
}

TEST("name: control characters, markup and the invisible characters go")
{
	CHECK(Name::Sanitize("\x01" "Al\ti\nce\x7F") == "Alice");
	CHECK(Name::Sanitize("\xC2\x85" "A\xC2\x9F" "B") == "AB");  // C1 controls (U+0085, U+009F)
	CHECK(Name::Sanitize("<b>Nate</b>") == "bNate/b");
	CHECK(Name::Sanitize("a<font color='#ff0000'>b") == "afont color='#ff0000'b");
	CHECK(Name::Sanitize("A\xE2\x80\xA8" "B\xE2\x80\xA9" "C") == "ABC");  // line and paragraph separators
	CHECK(Name::Sanitize("A\xE2\x80\x8B" "B\xE2\x80\x8D" "C\xE2\x80\x8F" "D") == "ABCD");  // zero width space, joiner, RLM
	CHECK(Name::Sanitize("\xE2\x80\xAE" "etaN\xE2\x80\xAC") == "etaN");  // a right-to-left override and its pop
	CHECK(Name::Sanitize("A\xE2\x81\xA6" "B\xE2\x81\xA9") == "AB");       // bidi isolates
	CHECK(Name::Sanitize("\xEF\xBB\xBF" "Nate") == "Nate");               // a byte order mark
	CHECK(Name::Sanitize("A\xEF\xBF\xBE" "B\xEF\xBF\xBF") == "AB");       // the noncharacters U+FFFE and U+FFFF
	CHECK(Name::Sanitize("\x01\x02\x1F").empty());
	CHECK(Name::Sanitize("<>").empty());
}

TEST("name: bytes that aren't UTF-8 are dropped and the rest kept")
{
	CHECK(Name::Sanitize("\xFF\xFE").empty());
	CHECK(Name::Sanitize("Na\xFFte") == "Nate");
	CHECK(Name::Sanitize("\xD0" "Nate") == "Nate");         // a lead byte with nothing after it
	CHECK(Name::Sanitize("Nate\xD0") == "Nate");            // cut short at the end
	CHECK(Name::Sanitize("Na\xC0\x80te") == "Nate");        // an overlong encoding goes, both bytes
	CHECK(Name::Sanitize("Na\xED\xA0\x80te") == "Nate");    // a surrogate
	CHECK(Name::Sanitize(CYRILLIC + "\x80" + CJK) == CYRILLIC + CJK);
	// Latin-1 bytes (a name from a program that never spoke UTF-8) aren't UTF-8: they go rather than show as garbage.
	CHECK(Name::Sanitize("Jos\xE9") == "Jos");
}

TEST("name: the cap is counted in characters, so a long Cyrillic name keeps as many letters as a Latin one")
{
	const auto latin = Repeat("N", 40);
	CHECK(Name::Sanitize(latin) == Repeat("N", 32));
	CHECK(Name::Sanitize(Repeat("N", 32)) == Repeat("N", 32));
	const auto cyrillic = Repeat(CYRILLIC, 10);  // 40 characters, 80 bytes
	CHECK(Name::Sanitize(cyrillic) == Repeat(CYRILLIC, 8));
	CHECK(Name::Characters(Name::Sanitize(cyrillic)) == 32);
	const auto emoji = Repeat(EMOJI, 40);  // 160 bytes
	CHECK(Name::Characters(Name::Sanitize(emoji)) == 32);
	CHECK(Name::Sanitize(emoji).size() == 128);
	CHECK(Name::Sanitize(emoji).size() <= Protocol::MAX_NAME_LENGTH);  // every capped name fits the wire
	// A character is never cut in two: the cap falls between characters.
	CHECK(Name::ValidUtf8(Name::Sanitize(Repeat(CJK, 20))));
	CHECK(Name::Sanitize("abc", 2) == "ab");
	CHECK(Name::Sanitize(CYRILLIC, 2) == CYRILLIC.substr(0, 4));
	// Dropped characters don't count towards it.
	CHECK(Name::Sanitize("\x01\x02" + Repeat("N", 32)) == Repeat("N", 32));
}

TEST("name: spaces at either end go, and a name of nothing but spaces is no name")
{
	CHECK(Name::Sanitize("  Nate  ") == "Nate");
	CHECK(Name::Sanitize("Nate Jr") == "Nate Jr");
	CHECK(Name::Sanitize("   ").empty());
	CHECK(Name::Sanitize("").empty());
	CHECK(Name::Sanitize("\t Nate \n") == "Nate");
}

TEST("name: a name nothing is left of is shown as Player N")
{
	CHECK(Name::Shown("Nate", 3) == "Nate");
	CHECK(Name::Shown(CYRILLIC, 3) == CYRILLIC);
	CHECK(Name::Shown("", 3) == "Player 3");
	CHECK(Name::Shown("\x01\x02\x1F", 7) == "Player 7");
	CHECK(Name::Shown("<>", 7) == "Player 7");
	CHECK(Name::Shown("\xFF\xFE", 12) == "Player 12");
	CHECK(Name::Shown("   ", 12) == "Player 12");
}

TEST("name: what is allowed is decided per character")
{
	CHECK(Name::Allowed(U'A'));
	CHECK(Name::Allowed(U' '));
	CHECK(Name::Allowed(U'~'));
	CHECK(Name::Allowed(0x41D));    // Н
	CHECK(Name::Allowed(0x7279));   // 特
	CHECK(Name::Allowed(0x1F600));  // 😀
	CHECK(Name::Allowed(0xA0));     // a no-break space is the first past the C1 controls
	CHECK(!Name::Allowed(0x1F));
	CHECK(!Name::Allowed(0x7F));
	CHECK(!Name::Allowed(0x80));
	CHECK(!Name::Allowed(0x9F));
	CHECK(!Name::Allowed(U'<'));
	CHECK(!Name::Allowed(U'>'));
	CHECK(!Name::Allowed(0x200B));
	CHECK(!Name::Allowed(0x200F));
	CHECK(Name::Allowed(0x2010));  // a hyphen, just past the marks
	CHECK(!Name::Allowed(0x202A));
	CHECK(!Name::Allowed(0x202E));
	CHECK(Name::Allowed(0x202F));  // a narrow no-break space, just past the overrides
	CHECK(!Name::Allowed(0x2066));
	CHECK(!Name::Allowed(0x2069));
	CHECK(!Name::Allowed(0xFEFF));
	CHECK(!Name::Allowed(0xFFFE));
	CHECK(!Name::Allowed(0xFFFF));
}
