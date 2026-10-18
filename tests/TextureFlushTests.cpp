#include "Test.h"

#include "game/TextureFlush.h"

#include <array>

using TextureFlush::Counters;

TEST("texture flush: the wait's condition is the engine's")
{
	CHECK(TextureFlush::Waiting({ .pending = true, .done = 0, .total = 1 }));
	CHECK(!TextureFlush::Waiting({ .pending = true, .done = 1, .total = 1 }));   // all done: the loop ends
	CHECK(!TextureFlush::Waiting({ .pending = true, .done = 5, .total = 1 }));   // late finishes after a reset count past total
	CHECK(!TextureFlush::Waiting({ .pending = false, .done = 0, .total = 9 }));  // not pending: the loop ends
}

TEST("texture flush: only a pending batch with requests the main thread still holds is handed over")
{
	// The hazard: the walk handed a full batch over (pending), then counted three more it still holds.
	CHECK(TextureFlush::NeedsFlush({ .pending = true, .done = 40, .total = 47 }, 3));
	// Everything counted was handed over: the streamer finishes it, nothing is owed.
	CHECK(!TextureFlush::NeedsFlush({ .pending = true, .done = 40, .total = 47 }, 0));
	// Not pending: the wait's first step releases the batch itself, counting what it holds as done.
	CHECK(!TextureFlush::NeedsFlush({ .pending = false, .done = 0, .total = 3 }, 3));
	CHECK(!TextureFlush::NeedsFlush({}, 0));
}

TEST("texture flush: a near call's target is read from its five bytes")
{
	// Main::OnIdle+0x37F in 1.11.240 (image base 0x140000000): call PositionPlayerJob::Run (+C3B030).
	constexpr std::array<std::uint8_t, 5> pump{ 0xE8, 0xDC, 0x72, 0x00, 0x00 };
	CHECK(TextureFlush::CallTarget(pump, 0x140C33D4F) == std::optional<std::uintptr_t>{ 0x140C3B030 });
	// Backwards: Main::StartUp+0x12E5, call BSTSingletonImplicit<BGSDefaultObjectManager>::QInstance (+308100).
	constexpr std::array<std::uint8_t, 5> back{ 0xE8, 0x26, 0x6E, 0x6D, 0xFF };
	CHECK(TextureFlush::CallTarget(back, 0x140C312D5) == std::optional<std::uintptr_t>{ 0x140308100 });
	// Not a call (mov rcx,[rip+...]; a jmp; another plugin's patch): nothing.
	constexpr std::array<std::uint8_t, 5> mov{ 0x48, 0x8B, 0x0D, 0xED, 0x95 };
	CHECK(!TextureFlush::CallTarget(mov, 0x140C33D7C));
	constexpr std::array<std::uint8_t, 5> jmp{ 0xE9, 0xDC, 0x72, 0x00, 0x00 };
	CHECK(!TextureFlush::CallTarget(jmp, 0x140C33D4F));
}

TEST("texture flush: a function on a stack is told by the return addresses inside it")
{
	constexpr std::uintptr_t PUMP = 0x7FF776BEB030;  // PositionPlayerJob::Run, 0xEF bytes
	const std::array<std::uintptr_t, 4> inPump{ 0x7FFB10001234, 0x7FF6FFFF0000, PUMP + 0x8B, 0x7FF776BE3D54 };
	CHECK(TextureFlush::OnStack(inPump, PUMP, PUMP + 0xEF));
	const std::array<std::uintptr_t, 3> outside{ PUMP - 1, PUMP + 0xEF, 0x7FF776BE3D54 };
	CHECK(!TextureFlush::OnStack(outside, PUMP, PUMP + 0xEF));
	CHECK(!TextureFlush::OnStack({}, PUMP, PUMP + 0xEF));
	// An unknown range (the function wasn't found) is on no stack.
	CHECK(!TextureFlush::OnStack(inPump, 0, 0));
}
