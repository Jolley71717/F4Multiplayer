#include "game/GpuWatch.h"

#include "Log.h"
#include "game/FastLoad.h"
#include "game/GpuDevice.h"
#include "game/Hud.h"

namespace GpuWatch
{
	namespace
	{
		GpuDevice::Watch watch;
	}

	void Tick(bool (*a_quitting)())
	{
		if (watch.IsLost() || a_quitting()) {
			return;  // said once, the device never comes back; or the engine is letting it go
		}
		const auto renderer = RE::BSGraphics::GetRendererData();
		const auto device = renderer ? renderer->device : nullptr;
		if (!device) {
			watch.NoDevice();
			return;
		}
		// The device is free-threaded (the engine creates textures on a thread of its own), and this call only reads
		// its state: any thread may make it.
		const auto reason = static_cast<std::uint32_t>(device->GetDeviceRemovedReason());
		const GpuDevice::Context context{ .loadingScreen = Hud::LoadingScreenUp(), .vsyncOffFor = FastLoad::VsyncOffFor() };
		if (const auto lost = watch.Check(reason, context)) {
			REX::ERROR("{}", GpuDevice::Describe(*lost));
			Log::Flush();  // the crash comes within seconds
		}
	}

	std::string Describe()
	{
		return watch.Describe();
	}
}
