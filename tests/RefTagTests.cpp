#include "Test.h"

#include "game/RefTag.h"

namespace
{
	struct FakeRef
	{
		std::uint32_t id = 0xFF001234;
		const void*   root = nullptr;

		std::uint32_t GetFormID() const { return id; }
		const void*   Get3D() const { return root; }
	};
}

TEST("ref tag: the form ID, the address and the 3D state, on one short line")
{
	CHECK(RefTag::Format(0xFF001234, 0x1F2A3B4C5D0, true) == "FF001234 @1F2A3B4C5D0 3d=yes");
	CHECK(RefTag::Format(0x00000014, 0x10, false) == "00000014 @10 3d=no");
}

TEST("ref tag: a reference names its own address and whether it has 3D")
{
	int           node = 0;
	FakeRef       bare;
	FakeRef       drawn{ .root = &node };
	const auto    bareAddress = reinterpret_cast<std::uintptr_t>(&bare);
	CHECK(RefTag::Of(&bare) == RefTag::Format(0xFF001234, bareAddress, false));
	CHECK(RefTag::Of(&drawn) == RefTag::Format(0xFF001234, reinterpret_cast<std::uintptr_t>(&drawn), true));
	CHECK(RefTag::Of(static_cast<const FakeRef*>(nullptr)) == "none");
}
