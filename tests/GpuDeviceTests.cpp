#include "Test.h"

#include "game/GpuDevice.h"

using namespace std::chrono_literals;
using GpuDevice::Context;
using GpuDevice::Watch;

TEST("gpu device: a device that stays fine is never reported, and every check is counted")
{
	Watch watch;
	CHECK(watch.Describe() == "gpu=ok checks=0");
	for (int i = 0; i < 90; ++i) {
		CHECK(!watch.Check(GpuDevice::OK, {}));
	}
	CHECK(!watch.IsLost());
	CHECK(watch.Describe() == "gpu=ok checks=90");
}

TEST("gpu device: the first reason that is not S_OK is reported once, with what the plugin was doing then")
{
	Watch watch;
	CHECK(!watch.Check(GpuDevice::OK, {}));
	CHECK(!watch.Check(GpuDevice::OK, {}));
	// The VM's crashes: the device went while a loading screen was up, fast loading's vsync off for 4 s.
	const Context during{ .loadingScreen = true, .vsyncOffFor = 4s };
	const auto    lost = watch.Check(GpuDevice::DEVICE_HUNG, during);
	REQUIRE(lost.has_value());
	CHECK(lost->reason == GpuDevice::DEVICE_HUNG);
	CHECK(lost->context.loadingScreen);
	CHECK(lost->context.vsyncOffFor == 4s);
	CHECK(lost->checks == 2);
	CHECK(watch.IsLost());
	// The device never comes back: the ticks after say nothing more, whatever they read (a crash follows anyway).
	CHECK(!watch.Check(GpuDevice::DEVICE_HUNG, during));
	CHECK(!watch.Check(GpuDevice::DEVICE_REMOVED, {}));
	CHECK(!watch.Check(GpuDevice::OK, {}));
	CHECK(watch.Describe() == "gpu=lost(0x887A0006) checks=2");
}

TEST("gpu device: the log line names the reason and says the crash that follows is the driver's")
{
	const GpuDevice::Lost lost{ .reason = GpuDevice::DEVICE_REMOVED, .context = { .loadingScreen = true, .vsyncOffFor = 4500ms }, .checks = 1860 };
	const auto            line = GpuDevice::Describe(lost);
	CHECK(line.starts_with("GPU: the graphics driver took the game's Direct3D device away: 0x887A0005 DXGI_ERROR_DEVICE_REMOVED"));
	CHECK(line.find("no plugin frame on the stack") != std::string::npos);
	CHECK(line.find("the driver's or the GPU's") != std::string::npos);
	CHECK(line.find("A loading screen was up; fast loading had vsync off for 4 s") != std::string::npos);
	CHECK(line.ends_with("fine on 1860 check(s) before"));
	// Outside a loading screen, vsync on (the main menu's Scaleform kill-list crash had no load of ours to blame).
	const GpuDevice::Lost idle{ .reason = GpuDevice::DRIVER_INTERNAL_ERROR, .context = {}, .checks = 0 };
	const auto            idleLine = GpuDevice::Describe(idle);
	CHECK(idleLine.find("0x887A0020 DXGI_ERROR_DRIVER_INTERNAL_ERROR") != std::string::npos);
	CHECK(idleLine.find("No loading screen was up; vsync was on") != std::string::npos);
}

TEST("gpu device: every reason GetDeviceRemovedReason gives has a name, and an unknown one says so")
{
	CHECK(GpuDevice::Name(GpuDevice::OK) == "S_OK");
	CHECK(GpuDevice::Name(GpuDevice::DEVICE_REMOVED).starts_with("DXGI_ERROR_DEVICE_REMOVED "));
	CHECK(GpuDevice::Name(GpuDevice::DEVICE_HUNG).starts_with("DXGI_ERROR_DEVICE_HUNG "));
	CHECK(GpuDevice::Name(GpuDevice::DEVICE_RESET).starts_with("DXGI_ERROR_DEVICE_RESET "));
	CHECK(GpuDevice::Name(GpuDevice::DRIVER_INTERNAL_ERROR).starts_with("DXGI_ERROR_DRIVER_INTERNAL_ERROR "));
	CHECK(GpuDevice::Name(GpuDevice::INVALID_CALL) == "DXGI_ERROR_INVALID_CALL");
	CHECK(GpuDevice::Name(GpuDevice::OUT_OF_MEMORY) == "E_OUTOFMEMORY");
	CHECK(GpuDevice::Name(0x80004005) == "an unknown reason");
	// An unknown failure is still a lost device: any reason but S_OK is.
	Watch watch;
	const auto lost = watch.Check(0x80004005, {});
	REQUIRE(lost.has_value());
	CHECK(GpuDevice::Describe(*lost).find("0x80004005 an unknown reason") != std::string::npos);
}

TEST("gpu device: no renderer yet is neither fine nor lost")
{
	Watch watch;
	watch.NoDevice();
	watch.NoDevice();
	CHECK(!watch.IsLost());
	CHECK(watch.Describe() == "gpu=none checks=0");
	// The renderer made: checks count from there.
	CHECK(!watch.Check(GpuDevice::OK, {}));
	CHECK(watch.Describe() == "gpu=ok checks=1");
}
