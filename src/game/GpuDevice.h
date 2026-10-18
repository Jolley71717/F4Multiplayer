#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// The game's Direct3D device, lost to the graphics driver. When the driver resets the GPU (a hang it recovered
// from, a driver fault, in a VM the host's driver resetting the guest's GPU partition), every call on the device
// fails from then on with DXGI_ERROR_DEVICE_REMOVED, and Fallout 4 does not check: it crashes a moment later in
// its renderer with no plugin frame on the stack, wherever it next uses a texture. On the test VM (a GPU-P
// partition of the host's GPU) six such crashes in a day matched six driver resets in the host's System log, one
// for one, each 3-5 s before: BSGraphics::RegisterD3DTexture on the resource creator's thread with a null
// texture and RAX = 0x887A0005 (DXGI_ERROR_DEVICE_REMOVED, what CreateTexture2D had returned), and d3d11.dll in
// Scaleform's TextureManager::processTextureKillList under UI::EndRender. Nothing the plugin does can keep the
// game drawing after that, so this only says so in the log, once, before the crash: what the driver gave as
// the reason, and what the plugin was doing to the renderer at the time (a loading screen up, fast loading's
// vsync off), so a crash log with no plugin frame can be told from one of ours. The frame watch's thread asks
// the device once a second (ID3D11Device::GetDeviceRemovedReason, which any thread may call). Free of the game,
// so it is unit tested.
namespace GpuDevice
{
	using Clock = std::chrono::steady_clock;

	// The HRESULTs GetDeviceRemovedReason gives (winerror.h's values, which the tests keep free of Windows).
	inline constexpr std::uint32_t OK = 0;
	inline constexpr std::uint32_t DEVICE_REMOVED = 0x887A0005;         // DXGI_ERROR_DEVICE_REMOVED
	inline constexpr std::uint32_t DEVICE_HUNG = 0x887A0006;            // DXGI_ERROR_DEVICE_HUNG
	inline constexpr std::uint32_t DEVICE_RESET = 0x887A0007;           // DXGI_ERROR_DEVICE_RESET
	inline constexpr std::uint32_t DRIVER_INTERNAL_ERROR = 0x887A0020;  // DXGI_ERROR_DRIVER_INTERNAL_ERROR
	inline constexpr std::uint32_t INVALID_CALL = 0x887A0001;           // DXGI_ERROR_INVALID_CALL
	inline constexpr std::uint32_t OUT_OF_MEMORY = 0x8007000E;          // E_OUTOFMEMORY

	// The reason's name and what it means, for the log.
	constexpr std::string_view Name(std::uint32_t a_reason)
	{
		switch (a_reason) {
		case OK:
			return "S_OK";
		case DEVICE_REMOVED:
			return "DXGI_ERROR_DEVICE_REMOVED (the GPU was taken away: the driver reset or was updated, or the device went)";
		case DEVICE_HUNG:
			return "DXGI_ERROR_DEVICE_HUNG (the GPU stopped responding and the driver reset it)";
		case DEVICE_RESET:
			return "DXGI_ERROR_DEVICE_RESET (the driver reset the GPU over a bad command)";
		case DRIVER_INTERNAL_ERROR:
			return "DXGI_ERROR_DRIVER_INTERNAL_ERROR (the graphics driver failed)";
		case INVALID_CALL:
			return "DXGI_ERROR_INVALID_CALL";
		case OUT_OF_MEMORY:
			return "E_OUTOFMEMORY";
		default:
			return "an unknown reason";
		}
	}

	// What the plugin was doing to the renderer when the device was found lost.
	struct Context
	{
		bool            loadingScreen = false;  // a loading screen was up (the plugin's view, Hud::LoadingScreenUp)
		Clock::duration vsyncOffFor{};          // how long fast loading had had vsync off (zero: it was on)
	};

	// The one report of a lost device.
	struct Lost
	{
		std::uint32_t   reason = OK;
		Context         context;
		std::uint32_t   checks = 0;  // how many times the device had been asked before, all fine
	};

	// The log line for a lost device.
	inline std::string Describe(const Lost& a_lost)
	{
		const auto offFor = std::chrono::duration_cast<std::chrono::seconds>(a_lost.context.vsyncOffFor).count();
		const auto vsync = a_lost.context.vsyncOffFor > Clock::duration::zero() ? std::format("fast loading had vsync off for {} s", offFor) : std::string{ "vsync was on" };
		return std::format(
			"GPU: the graphics driver took the game's Direct3D device away: 0x{:08X} {}. The game cannot draw from here on, and "
			"crashes in its renderer the next time it uses a texture, with no plugin frame on the stack: the fault is the driver's "
			"or the GPU's (in a VM, the host's driver resetting the guest's GPU partition; its System log has the reset). {}; {}; "
			"the device had been fine on {} check(s) before",
			a_lost.reason, Name(a_lost.reason), a_lost.context.loadingScreen ? "A loading screen was up" : "No loading screen was up", vsync, a_lost.checks);
	}

	// Asked once a second (the frame watch's thread). Reports the first reason that is not S_OK, once: the device
	// never comes back (the game would have to make a new one, which Fallout 4 does not).
	class Watch
	{
	public:
		// a_reason: what GetDeviceRemovedReason gave now; a_context: what the plugin is doing to the renderer.
		[[nodiscard]] std::optional<Lost> Check(std::uint32_t a_reason, const Context& a_context)
		{
			std::scoped_lock l{ lock };
			if (reported) {
				return std::nullopt;
			}
			if (a_reason == OK) {
				++checks;
				return std::nullopt;
			}
			reported = true;
			lostReason = a_reason;
			return Lost{ .reason = a_reason, .context = a_context, .checks = checks };
		}

		// No device to ask (the renderer not made yet): neither fine nor lost.
		void NoDevice()
		{
			std::scoped_lock l{ lock };
			++missing;
		}

		[[nodiscard]] bool IsLost() const
		{
			std::scoped_lock l{ lock };
			return reported;
		}

		// For the developer's `frames` command: that the watch runs, and what it found.
		[[nodiscard]] std::string Describe() const
		{
			std::scoped_lock l{ lock };
			if (reported) {
				return std::format("gpu=lost(0x{:08X}) checks={}", lostReason, checks);
			}
			return missing > 0 && checks == 0 ? std::format("gpu=none checks=0") : std::format("gpu=ok checks={}", checks);
		}

	private:
		mutable std::mutex lock;  // the frame watch's thread checks, the developer's command reads
		bool          reported = false;
		std::uint32_t lostReason = OK;
		std::uint32_t checks = 0;   // the device found fine
		std::uint32_t missing = 0;  // no device to ask
	};
}
