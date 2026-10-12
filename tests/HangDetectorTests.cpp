#include "Test.h"

#include "HangDetector.h"

using namespace std::chrono_literals;
using HangWatch::Detector;
using HangWatch::Phase;
using HangWatch::Sample;
using Event = HangWatch::Detector::Event;

namespace
{
	// The defaults: a hang after 15 s, 60 s while saving, loading or on a loading screen, and Windows'
	// five seconds before it calls a window not responding.
	Detector MakeDetector() { return Detector{ 15s, 60s, 5s }; }

	constexpr Sample STALLED{ .responsive = false };
	constexpr Sample ANSWERS{};

	constexpr Sample Stalled(Phase a_phase) { return Sample{ .responsive = false, .phase = a_phase }; }

	// One of the watch's looks, a_into a save, with the phase made the way the watch makes it (with the
	// default long limit). a_saving: the save's end message hasn't come yet.
	Sample Look(HangWatch::Clock::duration a_into, bool a_saving, bool a_responsive)
	{
		return Sample{ .responsive = a_responsive, .phase = HangWatch::CurrentPhase(false, a_saving ? Phase::kSave : Phase::kPlay, a_into, 60s) };
	}

	std::vector<std::uint8_t> Bytes(std::initializer_list<std::uint8_t> a_bytes) { return a_bytes; }
}

TEST("hang watch: a game that keeps answering never hangs")
{
	auto d = MakeDetector();
	auto now = HangWatch::Clock::time_point{} + 1h;
	for (int i = 0; i < 600; ++i) {
		now += 1s;
		CHECK(d.Tick(now, ANSWERS).event == Event::kNone);
		CHECK(d.Tick(now, Sample{ .phase = Phase::kLoadingScreen }).event == Event::kNone);
		CHECK(d.Tick(now, Sample{ .phase = Phase::kSave }).event == Event::kNone);
	}
	CHECK(!d.Hung());
}

TEST("hang watch: a stall is reported once at the limit, and once when the game answers again")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	CHECK(d.Tick(start, ANSWERS).event == Event::kNone);

	// First seen not responding at +1 s: it has been stalled for five seconds already.
	CHECK(d.Tick(start + 1s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 10s, STALLED).event == Event::kNone);  // 14 s
	const auto hang = d.Tick(start + 11s, STALLED);              // 15 s
	CHECK(hang.event == Event::kHang);
	CHECK(hang.stalled == 15s);
	CHECK(d.Hung());
	for (auto t = 12s; t < 60s; t += 1s) {
		CHECK(d.Tick(start + t, STALLED).event == Event::kNone);
	}

	const auto back = d.Tick(start + 60s, ANSWERS);
	CHECK(back.event == Event::kRecovered);
	CHECK(back.stalled == 64s);
	CHECK(!d.Hung());
	CHECK(d.Tick(start + 61s, ANSWERS).event == Event::kNone);

	// The next freeze is a new hang.
	CHECK(d.Tick(start + 70s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 80s, STALLED).event == Event::kHang);
}

TEST("hang watch: short stalls never add up to a hang")
{
	auto d = MakeDetector();
	auto now = HangWatch::Clock::time_point{} + 1h;
	for (int i = 0; i < 20; ++i) {
		// Nine looks at a game not responding (up to 13 s stalled), then one answer.
		for (int s = 0; s < 9; ++s) {
			now += 1s;
			CHECK(d.Tick(now, STALLED).event == Event::kNone);
		}
		now += 1s;
		CHECK(d.Tick(now, ANSWERS).event == Event::kNone);
	}
}

TEST("hang watch: a loading screen, a save and a load get the longer limit")
{
	for (const auto phase : { Phase::kLoadingScreen, Phase::kSave, Phase::kLoad }) {
		auto       d = MakeDetector();
		const auto start = HangWatch::Clock::time_point{} + 1h;
		CHECK(d.Limit(phase) == 60s);
		CHECK(d.Tick(start, Stalled(phase)).event == Event::kNone);
		CHECK(d.Tick(start + 30s, Stalled(phase)).event == Event::kNone);  // 35 s: a slow save or load, not a hang
		CHECK(d.Tick(start + 54s, Stalled(phase)).event == Event::kNone);
		const auto hang = d.Tick(start + 55s, Stalled(phase));  // 60 s
		CHECK(hang.event == Event::kHang);
		CHECK(hang.phase == phase);
		CHECK(d.Tick(start + 56s, ANSWERS).event == Event::kRecovered);
	}
	CHECK(MakeDetector().Limit(Phase::kPlay) == 15s);
}

TEST("hang watch: a 70 s save is one hang while saving, at the longer limit")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;  // the save begins and the main thread stops reading its messages
	std::vector<std::pair<HangWatch::Clock::duration, Detector::Result>> events;
	for (auto t = 1s; t <= 80s; t += 1s) {
		// Windows calls the game not responding 5 s in; the save's end message comes at 70 s, and
		// Windows sees the game answer a look later.
		const auto result = d.Tick(start + t, Look(t, t < 70s, t < 5s || t > 70s));
		if (result.event != Event::kNone) {
			events.emplace_back(t, result);
		}
	}
	REQUIRE(events.size() == 2);
	CHECK(events[0].first == 60s);
	CHECK(events[0].second.event == Event::kHang);
	CHECK(events[0].second.stalled == 60s);
	CHECK(events[0].second.phase == Phase::kSave);
	CHECK(d.Limit(events[0].second.phase) == 60s);
	CHECK(events[1].first == 71s);
	CHECK(events[1].second.event == Event::kRecovered);
	CHECK(events[1].second.stalled == 71s);
}

TEST("hang watch: a slow save whose end comes before Windows sees the game answer is no hang")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	for (auto t = 1s; t <= 60s; t += 1s) {
		// A 50 s save; Windows still calls the game not responding for two looks after its end message.
		CHECK(d.Tick(start + t, Look(t, t < 50s, t < 5s || t >= 52s)).event == Event::kNone);
	}
	CHECK(!d.Hung());
}

TEST("hang watch: a stall keeps the longer limit of a save it was seen in, but the next stall doesn't")
{
	const auto start = HangWatch::Clock::time_point{} + 1h;
	{
		// The save is over, the game still doesn't answer: a hang, but one that began in the save.
		auto d = MakeDetector();
		CHECK(d.Tick(start, Stalled(Phase::kSave)).event == Event::kNone);        // 5 s
		CHECK(d.Tick(start + 10s, Stalled(Phase::kSave)).event == Event::kNone);  // 15 s: fine while saving
		CHECK(d.Tick(start + 11s, STALLED).event == Event::kNone);
		CHECK(d.Tick(start + 54s, STALLED).event == Event::kNone);                // 59 s
		const auto hang = d.Tick(start + 55s, STALLED);
		CHECK(hang.event == Event::kHang);
		CHECK(hang.phase == Phase::kSave);
	}
	{
		// The main thread was already stalled when the save's message came.
		auto d = MakeDetector();
		CHECK(d.Tick(start, STALLED).event == Event::kNone);                      // 5 s
		CHECK(d.Tick(start + 9s, STALLED).event == Event::kNone);                 // 14 s
		CHECK(d.Tick(start + 10s, Stalled(Phase::kSave)).event == Event::kNone);  // 15 s
		CHECK(d.Tick(start + 54s, STALLED).event == Event::kNone);
		CHECK(d.Tick(start + 55s, STALLED).phase == Phase::kSave);
		// A load says more than the loading screen it put up.
		auto load = MakeDetector();
		CHECK(load.Tick(start, Stalled(Phase::kLoadingScreen)).phase == Phase::kLoadingScreen);
		CHECK(load.Tick(start + 1s, Stalled(Phase::kLoad)).phase == Phase::kLoad);
		CHECK(load.Tick(start + 2s, Stalled(Phase::kLoadingScreen)).phase == Phase::kLoad);
	}
	{
		// A 20 s save the game answers right after, then a freeze in play: the normal limit again.
		auto d = MakeDetector();
		for (auto t = 1s; t <= 25s; t += 1s) {
			CHECK(d.Tick(start + t, Look(t, t < 20s, t < 5s || t >= 21s)).event == Event::kNone);
		}
		CHECK(d.Tick(start + 40s, Look(40s, false, false)).event == Event::kNone);  // 5 s
		CHECK(d.Tick(start + 49s, Look(49s, false, false)).event == Event::kNone);  // 14 s
		const auto hang = d.Tick(start + 50s, Look(50s, false, false));               // 15 s
		CHECK(hang.event == Event::kHang);
		CHECK(hang.stalled == 15s);
		CHECK(hang.phase == Phase::kPlay);
	}
}

TEST("hang watch: the phase is the save or load until it ends, then the loading screen")
{
	using HangWatch::CurrentPhase;
	CHECK(CurrentPhase(false, Phase::kPlay, 0s, 60s) == Phase::kPlay);
	CHECK(CurrentPhase(true, Phase::kPlay, 0s, 60s) == Phase::kLoadingScreen);
	CHECK(CurrentPhase(false, Phase::kSave, 10s, 60s) == Phase::kSave);
	CHECK(CurrentPhase(true, Phase::kLoad, 10s, 60s) == Phase::kLoad);  // a load's own loading screen
	// A save or load slower than the long limit is still one.
	CHECK(CurrentPhase(false, Phase::kSave, 70s, 60s) == Phase::kSave);
	CHECK(CurrentPhase(true, Phase::kLoad, 5min, 60s) == Phase::kLoad);
	// An end message that never came doesn't keep the longer limit on for good.
	CHECK(CurrentPhase(false, Phase::kSave, 10min, 60s) == Phase::kPlay);
	CHECK(CurrentPhase(true, Phase::kLoad, 1h, 60s) == Phase::kLoadingScreen);
}

TEST("hang watch: a paused game (the PC slept, a debugger) starts the stall over")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	CHECK(d.Tick(start, STALLED).event == Event::kNone);         // 5 s
	CHECK(d.Tick(start + 8s, STALLED).event == Event::kNone);    // 13 s
	// The watch thread wakes up minutes later: the main thread had no time to answer yet.
	CHECK(d.Tick(start + 300s, Sample{ .responsive = false, .paused = true }).event == Event::kNone);
	CHECK(d.Tick(start + 314s, STALLED).event == Event::kNone);  // 14 s since the pause
	CHECK(d.Tick(start + 315s, STALLED).event == Event::kHang);

	// A pause in the middle of a reported hang doesn't report it again.
	CHECK(d.Tick(start + 900s, Sample{ .responsive = false, .paused = true }).event == Event::kNone);
	CHECK(d.Tick(start + 930s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 931s, ANSWERS).event == Event::kRecovered);
}

TEST("hang watch: a return address is one just past a call instruction")
{
	using HangWatch::EndsWithCall;
	CHECK(EndsWithCall(Bytes({ 0x90, 0x90, 0xE8, 0x10, 0x20, 0x30, 0x40 })));  // call rel32
	CHECK(EndsWithCall(Bytes({ 0x90, 0xFF, 0x15, 0x10, 0x20, 0x30, 0x40 })));  // call [rip+disp32]
	CHECK(EndsWithCall(Bytes({ 0x48, 0xFF, 0x15, 0x10, 0x20, 0x30, 0x40 })));  // the same with REX.W
	CHECK(EndsWithCall(Bytes({ 0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xD0 })));  // call rax
	CHECK(EndsWithCall(Bytes({ 0x90, 0x90, 0x90, 0x90, 0x41, 0xFF, 0xD3 })));  // call r11
	CHECK(EndsWithCall(Bytes({ 0x90, 0x90, 0x90, 0x90, 0xFF, 0x50, 0x08 })));  // call [rax+8]
	CHECK(EndsWithCall(Bytes({ 0x90, 0x90, 0x90, 0xFF, 0x54, 0x24, 0x20 })));  // call [rsp+20h]
	CHECK(EndsWithCall(Bytes({ 0x90, 0xFF, 0x90, 0x10, 0x01, 0x00, 0x00 })));  // call [rax+110h]

	CHECK(!EndsWithCall(Bytes({ 0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xE0 })));  // jmp rax
	CHECK(!EndsWithCall(Bytes({ 0x90, 0x90, 0xE9, 0x10, 0x20, 0x30, 0x40 })));  // jmp rel32
	CHECK(!EndsWithCall(Bytes({ 0x90, 0xFF, 0x25, 0x10, 0x20, 0x30, 0x40 })));  // jmp [rip+disp32]
	CHECK(!EndsWithCall(Bytes({ 0, 0, 0, 0, 0, 0, 0 })));
	CHECK(!EndsWithCall(Bytes({ 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40 })));  // mov rax, [rip+disp32]
	CHECK(!EndsWithCall(Bytes({})));
}

TEST("hang watch: a module is named by its file name only, never its path")
{
	using HangWatch::ModuleName;
	CHECK(ModuleName(L"\\Device\\HarddiskVolume3\\Users\\someone\\Games\\Fallout 4\\Fallout4.exe") == "Fallout4.exe");
	CHECK(ModuleName(L"C:/Windows/System32/ntdll.dll") == "ntdll.dll");
	CHECK(ModuleName(L"F4Multiplayer.dll") == "F4Multiplayer.dll");
	CHECK(ModuleName(L"\\Device\\HarddiskVolume3\\Mods\\My Mod.dll") == "My Mod.dll");
	CHECK(ModuleName(L"\\Device\\HarddiskVolume3\\x\\caf\u00e9.dll") == "caf?.dll");
	CHECK(ModuleName(L"").empty());
}

TEST("hang watch: the report says the phase, how long, where the main thread was, and no paths")
{
	HangWatch::Report report;
	report.version = "0.8.6";
	report.time = "2026-10-11 23:05:09";
	report.phase = Phase::kSave;
	report.stalledSeconds = 61;
	report.limitSeconds = 60;
	report.threadId = 1234;
	report.where = "ntdll.dll+0x9F2A4";
	report.callers = { "KERNELBASE.dll+0x3A1F0", "Fallout4.exe+0xC2A1B7" };
	const auto text = HangWatch::ReportText(report);
	CHECK(text.find("0.8.6") != std::string::npos);
	CHECK(text.find("2026-10-11 23:05:09") != std::string::npos);
	CHECK(text.find("thread 1234") != std::string::npos);
	CHECK(text.find("for 61 s (while saving; the limit there is 60 s)") != std::string::npos);
	CHECK(text.find("Where it was: ntdll.dll+0x9F2A4") != std::string::npos);
	CHECK(text.find("KERNELBASE.dll+0x3A1F0") < text.find("Fallout4.exe+0xC2A1B7"));
	CHECK(text.find('\\') == std::string::npos);

	report.callers.clear();
	CHECK(HangWatch::ReportText(report).find("none found") != std::string::npos);
	report.where.clear();
	CHECK(HangWatch::ReportText(report).find("unknown") != std::string::npos);
}

TEST("hang watch: file names sort by time, and only the newest three of each kind are kept")
{
	using HangWatch::DUMP_SUFFIX;
	using HangWatch::HangFileName;
	using HangWatch::REPORT_SUFFIX;
	CHECK(HangFileName(2026, 10, 11, 9, 5, 3, REPORT_SUFFIX) == "F4Multiplayer-hang-2026-10-11_09-05-03.txt");
	CHECK(HangFileName(2026, 10, 11, 9, 5, 3, DUMP_SUFFIX) == "F4Multiplayer-hang-2026-10-11_09-05-03.dmp");
	CHECK(HangFileName(2026, 10, 11, 9, 5, 3, REPORT_SUFFIX) < HangFileName(2026, 10, 11, 10, 0, 0, REPORT_SUFFIX));
	CHECK(HangFileName(2026, 9, 30, 23, 59, 59, REPORT_SUFFIX) < HangFileName(2026, 10, 1, 0, 0, 0, REPORT_SUFFIX));
	CHECK(HangWatch::IsHangFile(HangFileName(2026, 10, 11, 9, 5, 3, REPORT_SUFFIX), REPORT_SUFFIX));
	CHECK(!HangWatch::IsHangFile("F4Multiplayer-hang-notes.txt", REPORT_SUFFIX));
	CHECK(!HangWatch::IsHangFile("F4Multiplayer-hang-2026-10-11_09-05-0x.txt", REPORT_SUFFIX));

	const auto report = [](int a_day, int a_hour) { return HangFileName(2026, 10, a_day, a_hour, 0, 0, REPORT_SUFFIX); };
	const auto dump = [](int a_day, int a_hour) { return HangFileName(2026, 10, a_day, a_hour, 0, 0, DUMP_SUFFIX); };
	const std::vector<std::string> files{
		"F4Multiplayer.log",
		report(11, 12),
		report(9, 8),
		report(10, 8) + ".tmp",  // the game was closed while it was written
		report(11, 9),
		dump(11, 9),
		"crash-2026-10-01-12-00-00.dmp",  // someone else's
		report(10, 8),
		dump(1, 1),
		dump(2, 2),
		dump(3, 3) + ".tmp",
		dump(4, 4),
		"F4Multiplayer-hang-notes.txt",  // the player's own
		"F4Multiplayer-hang-notes.txt.tmp",
	};
	auto remove = HangWatch::HangFilesToRemove(files, 3);
	std::ranges::sort(remove);
	std::vector<std::string> expected{ report(9, 8), report(10, 8) + ".tmp", dump(1, 1), dump(3, 3) + ".tmp" };
	std::ranges::sort(expected);
	CHECK(remove == expected);
	CHECK(HangWatch::HangFilesToRemove(files, 10).size() == 2);  // only the half-written ones
	CHECK(HangWatch::HangFilesToRemove({}, 3).empty());
}
