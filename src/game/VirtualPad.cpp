#include "game/VirtualPad.h"

#include "Config.h"
#include "game/Hotkeys.h"
#include "game/PadPlan.h"

#include <atomic>
#include <cctype>
#include <mutex>

namespace VirtualPad
{
	namespace
	{
		using Clock = PadPlan::Clock;

		constexpr unsigned long PAD_INDEX = 0;  // the XInput user index the pad answers as
		constexpr unsigned long OK = 0;         // ERROR_SUCCESS

		// XINPUT_STATE, XINPUT_VIBRATION, XINPUT_CAPABILITIES
		struct XState
		{
			std::uint32_t     packet;
			PadPlan::Gamepad  pad;
		};
		static_assert(sizeof(XState) == 16);
		struct XVibration
		{
			std::uint16_t left;
			std::uint16_t right;
		};
		struct XCapabilities
		{
			std::uint8_t     type;
			std::uint8_t     subType;
			std::uint16_t    flags;
			PadPlan::Gamepad pad;
			XVibration       vibration;
		};
		static_assert(sizeof(XCapabilities) == 20);

		using GetStateFn = unsigned long (*)(unsigned long, XState*);
		using GetCapabilitiesFn = unsigned long (*)(unsigned long, unsigned long, XCapabilities*);
		using SetStateFn = unsigned long (*)(unsigned long, XVibration*);

		GetStateFn        realGetState = nullptr;
		GetStateFn        realGetStateEx = nullptr;
		GetCapabilitiesFn realGetCapabilities = nullptr;
		SetStateFn        realSetState = nullptr;
		std::string       hooked;  // what Install patched, for the status

		std::mutex          lock;  // the pad's state: set on the main thread, read wherever the game polls
		PadPlan::Pad        pad;
		PadPlan::Packets    packets;
		std::atomic<bool>   plugged{ false };
		std::atomic<bool>   muted{ false };  // the game's keyboard and mouse skip their polls
		std::atomic<std::uint32_t> reads[4]{};       // XInputGetState calls per user index
		std::atomic<std::uint32_t> answered{ 0 };    // of them, answered with our pad
		std::atomic<std::uint32_t> capabilityReads{ 0 };
		std::atomic<std::uint32_t> mutedPolls{ 0 };
		std::atomic<Clock::rep>    lastAnswer{ 0 };  // when the game last read our pad
		std::atomic<unsigned long> readerThread{ 0 };

		unsigned long Answer(unsigned long a_index, XState* a_state, GetStateFn a_real)
		{
			++reads[std::min<unsigned long>(a_index, 3)];
			if (a_index == PAD_INDEX && plugged.load() && a_state) {
				const auto now = Clock::now();
				{
					std::scoped_lock l{ lock };
					a_state->pad = pad.State(now);
					a_state->packet = packets.Number(a_state->pad);
				}
				++answered;
				lastAnswer = now.time_since_epoch().count();
				readerThread = REX::W32::GetCurrentThreadId();
				return OK;
			}
			return a_real ? a_real(a_index, a_state) : 1167;  // ERROR_DEVICE_NOT_CONNECTED
		}

		unsigned long GetStateHook(unsigned long a_index, XState* a_state) { return Answer(a_index, a_state, realGetState); }
		unsigned long GetStateExHook(unsigned long a_index, XState* a_state) { return Answer(a_index, a_state, realGetStateEx); }

		unsigned long GetCapabilitiesHook(unsigned long a_index, unsigned long a_flags, XCapabilities* a_caps)
		{
			++capabilityReads;
			if (a_index == PAD_INDEX && plugged.load() && a_caps) {
				*a_caps = {};
				a_caps->type = 1;     // XINPUT_DEVTYPE_GAMEPAD
				a_caps->subType = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
				a_caps->pad = { 0xF3FF, 0xFF, 0xFF, -64, -64, -64, -64 };  // every button, full resolution
				a_caps->vibration = { 0xFFFF, 0xFFFF };
				return OK;
			}
			return realGetCapabilities ? realGetCapabilities(a_index, a_flags, a_caps) : 1167;
		}

		unsigned long SetStateHook(unsigned long a_index, XVibration* a_vibration)
		{
			if (a_index == PAD_INDEX && plugged.load()) {
				return OK;  // the rumble goes nowhere
			}
			return realSetState ? realSetState(a_index, a_vibration) : 1167;
		}

		struct ImportDescriptor  // IMAGE_IMPORT_DESCRIPTOR
		{
			std::uint32_t names;  // OriginalFirstThunk
			std::uint32_t timeDateStamp;
			std::uint32_t forwarderChain;
			std::uint32_t dll;
			std::uint32_t slots;  // FirstThunk
		};

		bool StartsWithNoCase(std::string_view a_text, std::string_view a_prefix)
		{
			return a_text.size() >= a_prefix.size() && std::equal(a_prefix.begin(), a_prefix.end(), a_text.begin(), [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
		}

		// The exe's import table, read in memory: each XInput function it imports (by ordinal, as Fallout4.exe does, or by
		// name) gets its slot pointed at our hook. The game calls through the slot every time, so this works whenever it
		// runs; at plugin load is before the game makes its gamepad.
		void PatchImports()
		{
			const auto base = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
			const auto nt = base + *reinterpret_cast<const std::uint32_t*>(base + 0x3C);
			if (*reinterpret_cast<const std::uint32_t*>(nt) != 0x4550 || *reinterpret_cast<const std::uint16_t*>(nt + 24) != 0x20B) {  // "PE\0\0", PE32+
				REX::WARN("Pad: the game's headers are not PE32+; the virtual pad is off");
				return;
			}
			const auto directory = nt + 24 + 0x70 + 8;  // DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
			const auto rva = *reinterpret_cast<const std::uint32_t*>(directory);
			if (rva == 0) {
				return;
			}
			for (auto d = reinterpret_cast<const ImportDescriptor*>(base + rva); d->dll != 0; ++d) {
				const std::string_view dll{ reinterpret_cast<const char*>(base + d->dll) };
				if (!StartsWithNoCase(dll, "xinput") || d->slots == 0) {
					continue;
				}
				const auto names = reinterpret_cast<const std::uint64_t*>(base + (d->names ? d->names : d->slots));
				const auto slots = base + d->slots;
				for (std::size_t i = 0; names[i] != 0; ++i) {
					const bool       byOrdinal = (names[i] >> 63) != 0;
					const auto       ordinal = byOrdinal ? static_cast<std::uint16_t>(names[i] & 0xFFFF) : std::uint16_t{ 0 };
					const std::string_view name = byOrdinal ? std::string_view{} : std::string_view{ reinterpret_cast<const char*>(base + (names[i] & 0x7FFFFFFF) + 2) };
					const auto       slot = slots + i * sizeof(std::uintptr_t);
					const auto       current = *reinterpret_cast<const std::uintptr_t*>(slot);
					std::uintptr_t   hook = 0;
					std::string_view what;
					if (ordinal == 2 || name == "XInputGetState") {
						realGetState = reinterpret_cast<GetStateFn>(current);
						hook = reinterpret_cast<std::uintptr_t>(&GetStateHook);
						what = "GetState";
					} else if (ordinal == 100) {
						realGetStateEx = reinterpret_cast<GetStateFn>(current);
						hook = reinterpret_cast<std::uintptr_t>(&GetStateExHook);
						what = "GetStateEx";
					} else if (ordinal == 4 || name == "XInputGetCapabilities") {
						realGetCapabilities = reinterpret_cast<GetCapabilitiesFn>(current);
						hook = reinterpret_cast<std::uintptr_t>(&GetCapabilitiesHook);
						what = "GetCapabilities";
					} else if (ordinal == 3 || name == "XInputSetState") {
						realSetState = reinterpret_cast<SetStateFn>(current);
						hook = reinterpret_cast<std::uintptr_t>(&SetStateHook);
						what = "SetState";
					}
					if (hook != 0 && current != hook) {
						REL::WriteSafeData(slot, hook);
						hooked += std::format("{}{} ({})", hooked.empty() ? "" : ", ", what, dll);
					}
				}
			}
			if (hooked.empty()) {
				REX::WARN("Pad: the game imports no XInput function; the virtual pad is off");
			} else {
				REX::INFO("Pad: hooked the game's XInput imports for the dev channel's virtual pad: {}", hooked);
			}
		}

		// ---- the game's keyboard and mouse, muted while the pad keeps polling on with the window not in front ----
		using PollFn = void (*)(RE::BSInputDevice*, float);
		PollFn keyboardPoll = nullptr;
		PollFn mousePoll = nullptr;
		bool   devicesHooked = false;

		void KeyboardPollHook(RE::BSInputDevice* a_this, float a_delta)
		{
			if (muted.load()) {
				++mutedPolls;
				return;
			}
			keyboardPoll(a_this, a_delta);
		}

		void MousePollHook(RE::BSInputDevice* a_this, float a_delta)
		{
			if (muted.load()) {
				return;
			}
			mousePoll(a_this, a_delta);
		}

		constexpr std::size_t POLL_INDEX = 1;  // BSInputDevice::Poll

		// The keyboard's and the mouse's Poll, hooked in their classes' vtables (read from the devices themselves).
		bool HookDesktopDevices(RE::BSInputDeviceManager& a_input)
		{
			if (devicesHooked) {
				return true;
			}
			const auto keyboard = a_input.devices[std::to_underlying(RE::BSInputDeviceManager::BS_DEVICES::kKeyboard)];
			const auto mouse = a_input.devices[std::to_underlying(RE::BSInputDeviceManager::BS_DEVICES::kMouse)];
			if (!keyboard || !mouse) {
				return false;
			}
			const auto keyboardSlot = *reinterpret_cast<std::uintptr_t*>(keyboard) + sizeof(void*) * POLL_INDEX;
			const auto mouseSlot = *reinterpret_cast<std::uintptr_t*>(mouse) + sizeof(void*) * POLL_INDEX;
			if (keyboardSlot == mouseSlot) {
				REX::WARN("Pad: the keyboard and the mouse share a class; they are not muted in the background");
				devicesHooked = true;
				return false;
			}
			keyboardPoll = *reinterpret_cast<PollFn*>(keyboardSlot);
			mousePoll = *reinterpret_cast<PollFn*>(mouseSlot);
			REL::WriteSafeData(keyboardSlot, reinterpret_cast<std::uintptr_t>(&KeyboardPollHook));
			REL::WriteSafeData(mouseSlot, reinterpret_cast<std::uintptr_t>(&MousePollHook));
			devicesHooked = true;
			REX::INFO("Pad: hooked the keyboard's and the mouse's polls (muted while the pad reads in the background)");
			return true;
		}

		void ClearDesktopDevices(RE::BSInputDeviceManager& a_input)
		{
			for (const auto which : { RE::BSInputDeviceManager::BS_DEVICES::kKeyboard, RE::BSInputDeviceManager::BS_DEVICES::kMouse }) {
				if (const auto device = a_input.devices[std::to_underlying(which)]) {
					device->ClearInputState();
				}
			}
		}

		// ---- walkto ----
		struct Walking
		{
			PadPlan::WalkCommand   command;
			PadPlan::Walk          walk;
			PadPlan::SteerSettings settings;
			Clock::time_point      nextLog;
		};
		std::optional<Walking> walking;
		std::string            lastWalk = "none yet";

		constexpr auto STEER_HOLD = std::chrono::milliseconds(500);  // the sticks fall back to the middle if the frames stop

		void Plug()
		{
			if (!plugged.exchange(true)) {
				REX::INFO("Pad: the virtual pad is plugged in (pad {})", PAD_INDEX);
			}
		}

		void LetGo()
		{
			std::scoped_lock l{ lock };
			pad.Neutral();
		}

		std::optional<RE::NiPoint3> Target(const PadPlan::WalkCommand& a_command)
		{
			if (a_command.kind == PadPlan::WalkCommand::Kind::kPoint) {
				return RE::NiPoint3{ a_command.x, a_command.y, 0.0f };
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_command.ref);
			if (!ref) {
				return std::nullopt;
			}
			return ref->data.location;
		}

		std::string DescribeWalk(const Walking& a_walk, const RE::NiPoint3& a_me, float a_distance)
		{
			const auto seconds = std::chrono::duration<float>(Clock::now() - a_walk.walk.Started()).count();
			return std::format("{} to {} dist={:.0f} closest={:.0f} at=({:.0f},{:.0f},{:.0f}) after {:.1f} s",
				PadPlan::Name(a_walk.walk.Now()),
				a_walk.command.kind == PadPlan::WalkCommand::Kind::kRef ? std::format("{:08X}", a_walk.command.ref) : std::format("({:.0f},{:.0f})", a_walk.command.x, a_walk.command.y),
				a_distance, a_walk.walk.Closest(), a_me.x, a_me.y, a_me.z, seconds);
		}

		void EndWalk(std::string a_why)
		{
			LetGo();
			lastWalk = std::move(a_why);
			REX::INFO("Pad: walkto {}", lastWalk);
			walking.reset();
		}

		std::string Status()
		{
			const auto input = RE::BSInputDeviceManager::GetSingleton();
			const auto last = lastAnswer.load();
			const auto ago = last == 0 ? std::string{ "never" } :
			                             std::format("{} ms ago", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Clock::time_point{ Clock::duration{ last } }).count());
			PadPlan::Gamepad state;
			{
				std::scoped_lock l{ lock };
				state = pad.State(Clock::now());
			}
			return std::format("plugged={} hooks=[{}] reads=[{},{},{},{}] answered={} lastRead={} caps={} thread={} gameSeesPad={} front={} polling={} muted={} mutedPolls={} {} walk={}",
				plugged.load(), hooked.empty() ? "none" : hooked, reads[0].load(), reads[1].load(), reads[2].load(), reads[3].load(), answered.load(), ago,
				capabilityReads.load(), readerThread.load(), input && input->IsGamepadConnected(), Hotkeys::GameInFront(), input && input->pollingEnabled, muted.load(), mutedPolls.load(),
				PadPlan::Describe(state), walking ? "on" : lastWalk);
		}
	}

	void Install()
	{
		if (!Config::Get().devChannel) {
			return;
		}
		PatchImports();
	}

	bool Plugged()
	{
		return plugged.load();
	}

	std::string Command(std::string_view a_args)
	{
		if (hooked.empty()) {
			return "error: the game's XInput isn't hooked (see the log)";
		}
		auto parsed = PadPlan::Parse(a_args);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			return *error;
		}
		const auto& command = std::get<PadPlan::Command>(parsed);
		using Kind = PadPlan::Command::Kind;
		const auto now = Clock::now();
		const auto until = [&](std::optional<std::chrono::milliseconds> a_default) -> PadPlan::Pad::Until {
			const auto hold = command.hold ? command.hold : a_default;
			return hold ? std::optional{ now + *hold } : std::nullopt;
		};
		switch (command.kind) {
		case Kind::kStatus:
			return Status();
		case Kind::kOff:
			if (walking) {
				EndWalk("stopped by pad off");
			}
			LetGo();
			return "let go of everything (still plugged in) " + Status();
		case Kind::kUnplug:
			if (walking) {
				EndWalk("stopped by pad unplug");
			}
			LetGo();
			plugged = false;
			REX::INFO("Pad: the virtual pad is unplugged");
			return "unplugged " + Status();
		case Kind::kStick:
		case Kind::kTrigger:
		case Kind::kButton:
			break;
		}
		Plug();
		std::string what;
		{
			std::scoped_lock l{ lock };
			if (command.kind == Kind::kStick) {
				pad.SetStick(command.right, command.x, command.y, until(std::nullopt));
				what = std::format("{} stick {:.2f} {:.2f}", command.right ? "right" : "left", command.x, command.y);
			} else if (command.kind == Kind::kTrigger) {
				pad.SetTrigger(command.right, command.x, until(std::nullopt));
				what = std::format("{} trigger {:.2f}", command.right ? "right" : "left", command.x);
			} else {
				pad.PressButton(command.buttons, *until(PadPlan::BUTTON_TAP));
				what = std::format("button {}", PadPlan::ButtonList(command.buttons));
			}
		}
		const auto hold = command.kind == Kind::kButton ? command.hold.value_or(PadPlan::BUTTON_TAP) : command.hold.value_or(std::chrono::milliseconds{ -1 });
		return std::format("{} {}", what, hold.count() < 0 ? std::string{ "held" } : std::format("for {} ms", hold.count()));
	}

	std::string WalkTo(std::string_view a_args)
	{
		if (hooked.empty()) {
			return "error: the game's XInput isn't hooked (see the log)";
		}
		auto parsed = PadPlan::ParseWalk(a_args);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			return *error;
		}
		const auto& command = std::get<PadPlan::WalkCommand>(parsed);
		using Kind = PadPlan::WalkCommand::Kind;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (command.kind == Kind::kStatus) {
			if (!walking || !player) {
				return std::format("not walking; last: {}", lastWalk);
			}
			const auto  target = Target(walking->command);
			const auto& me = player->data.location;
			return DescribeWalk(*walking, me, target ? std::hypot(target->x - me.x, target->y - me.y) : -1.0f);
		}
		if (command.kind == Kind::kStop) {
			if (walking) {
				EndWalk("stopped");
			}
			return std::format("stopped; last: {}", lastWalk);
		}
		if (!player || !player->GetParentCell()) {
			return "error: not in game";
		}
		const auto target = Target(command);
		if (!target) {
			return "error: no such reference";
		}
		if (walking) {
			EndWalk("replaced by a new walkto");
		}
		Plug();
		const auto now = Clock::now();
		walking.emplace(Walking{ command, PadPlan::Walk{ now, command.timeout }, PadPlan::SteerSettings{ .speed = command.speed, .radius = command.radius }, now });
		const auto& me = player->data.location;
		return std::format("walking to ({:.0f},{:.0f}) from ({:.0f},{:.0f}), {:.0f} away, speed {:.2f}, radius {:.0f}", target->x, target->y, me.x, me.y,
			std::hypot(target->x - me.x, target->y - me.y), command.speed, command.radius);
	}

	void Frame()
	{
		if (!walking) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			EndWalk("stopped: not in game");
			return;
		}
		const auto target = Target(walking->command);
		if (!target) {
			EndWalk("stopped: the target is gone");
			return;
		}
		const auto  now = Clock::now();
		const auto& me = player->data.location;
		const float dx = target->x - me.x;
		const float dy = target->y - me.y;
		const float distance = std::hypot(dx, dy);
		const auto  state = walking->walk.Step(now, distance, walking->settings.radius);
		if (state != PadPlan::Walk::State::kWalking) {
			EndWalk(DescribeWalk(*walking, me, distance));
			return;
		}
		const float yaw = player->data.angle.z * PadPlan::TO_DEGREES;
		const auto  sticks = PadPlan::Steer(dx, dy, yaw, walking->settings);
		{
			std::scoped_lock l{ lock };
			pad.SetStick(false, sticks.leftX, sticks.leftY, now + STEER_HOLD);
			pad.SetStick(true, sticks.rightX, 0.0f, now + STEER_HOLD);
		}
		if (now >= walking->nextLog) {
			walking->nextLog = now + std::chrono::seconds(1);
			REX::INFO("Pad: walkto {} yaw={:.0f} off={:.0f} sticks=({:.2f},{:.2f}) turn={:.2f}", DescribeWalk(*walking, me, distance), yaw,
				PadPlan::AngleDiff(PadPlan::Bearing(dx, dy), yaw), sticks.leftX, sticks.leftY, sticks.rightX);
		}
	}

	void GateFrame(RE::BSInputDeviceManager& a_input, bool a_front)
	{
		static std::uint32_t logged = 0;
		const bool hold = plugged.load() && !a_front;
		if (hold) {
			if (!HookDesktopDevices(a_input)) {
				return;  // the keyboard and mouse can't be muted: polling stays as the gate has it
			}
			if (!muted.exchange(true)) {
				ClearDesktopDevices(a_input);
				if (logged++ < 6) {
					REX::INFO("Pad: the game's window is not in front: input polling stays on for the virtual pad, the keyboard and mouse muted");
				}
			}
			a_input.pollingEnabled = true;
		} else if (muted.exchange(false)) {
			ClearDesktopDevices(a_input);
			if (logged++ < 6) {
				REX::INFO("Pad: {}: the keyboard and mouse are read again", a_front ? "the game's window is in front" : "the pad is unplugged");
			}
		}
	}
}
