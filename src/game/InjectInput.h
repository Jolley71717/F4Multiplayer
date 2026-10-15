#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The input dev command's plan (DevCommands.cpp sends it): a button press handed to the game's own input
// receivers (the menus' and the player's controls) as the game's input code makes one, after Windows and
// DirectInput and the control map. A key sent through Windows reaches the game only through all three,
// and a dialogue option picked that way stopped registering after the first exchange; this tells a game
// that ignores the press apart from a press that never got there. Pure, so it is unit tested.
namespace InjectInput
{
	using Clock = std::chrono::steady_clock;

	enum class Device : std::uint8_t
	{
		kKeyboard,
		kMouse,
		kGamepad,
	};

	constexpr auto DEFAULT_HOLD = std::chrono::milliseconds(100);
	constexpr auto MAX_HOLD = std::chrono::milliseconds(10000);

	// The shortest hold a release tells the game: one frame. The game counts a button released only when
	// its release says it was held some time (ButtonEvent::QReleased), so hold=0 would never let go.
	constexpr auto MIN_RELEASE_HOLD = std::chrono::milliseconds(16);

	struct Press
	{
		std::string                  userEvent;  // the control map's name for it: Activate, Accept, Cancel, Up, Pipboy...
		Device                       device = Device::kKeyboard;
		std::optional<std::uint32_t> id;         // the button's code (a DirectInput scan code for a key); none: the control map's
		std::chrono::milliseconds    hold = DEFAULT_HOLD;
	};

	inline constexpr std::string_view USAGE = "usage: input <userEvent> [hold=<ms>] [device=key|mouse|pad] [id=<hex>]";

	// input <userEvent> [hold=<ms>] [device=key|mouse|pad] [id=<hex>], the options in any order. A Press,
	// or what is wrong with the line.
	inline std::variant<Press, std::string> Parse(std::string_view a_args)
	{
		std::vector<std::string_view> words;
		while (!a_args.empty()) {
			const auto start = a_args.find_first_not_of(' ');
			if (start == std::string_view::npos) {
				break;
			}
			a_args.remove_prefix(start);
			const auto end = a_args.find(' ');
			words.push_back(a_args.substr(0, end));
			a_args.remove_prefix(end == std::string_view::npos ? a_args.size() : end);
		}
		if (words.empty() || words[0].find('=') != std::string_view::npos) {
			return std::string{ USAGE };
		}
		Press press{ .userEvent = std::string{ words[0] } };
		bool  seenHold = false, seenDevice = false, seenId = false;
		for (std::size_t i = 1; i < words.size(); ++i) {
			const auto word = words[i];
			const auto eq = word.find('=');
			const auto key = word.substr(0, eq);
			const auto value = eq == std::string_view::npos ? std::string_view{} : word.substr(eq + 1);
			if (key == "hold" && !seenHold) {
				int        ms = 0;
				const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
				if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() || ms < 0 || std::chrono::milliseconds(ms) > MAX_HOLD) {
					return std::string{ "error: hold is milliseconds, 0 to 10000" };
				}
				press.hold = std::chrono::milliseconds(ms);
				seenHold = true;
			} else if (key == "device" && !seenDevice) {
				if (value == "key") {
					press.device = Device::kKeyboard;
				} else if (value == "mouse") {
					press.device = Device::kMouse;
				} else if (value == "pad") {
					press.device = Device::kGamepad;
				} else {
					return std::string{ "error: device is key, mouse or pad" };
				}
				seenDevice = true;
			} else if (key == "id" && !seenId) {
				auto hex = value;
				if (hex.starts_with("0x") || hex.starts_with("0X")) {
					hex.remove_prefix(2);
				}
				std::uint32_t id = 0;
				const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
				if (hex.empty() || ec != std::errc{} || ptr != hex.data() + hex.size()) {
					return std::string{ "error: id is a hex code" };
				}
				press.id = id;
				seenId = true;
			} else {
				return std::string{ USAGE };
			}
		}
		return press;
	}

	// The seconds the release of the press says it was held (ButtonEvent::heldDownSecs): its hold, at least
	// a frame. The game tells a tap from a hold by it (a held Pip-Boy button is the light).
	[[nodiscard]] inline float ReleaseHeldSecs(const Press& a_press)
	{
		return std::chrono::duration<float>((std::max)(a_press.hold, MIN_RELEASE_HOLD)).count();
	}

	// Presses waiting for their release, stepped once a frame. A button is never pressed twice without a
	// release between (no real one can be): a new press of one still held releases it first, saying it
	// was held as long as it really was.
	class Releases
	{
	public:
		struct Pending
		{
			Press             press;
			Clock::time_point due;
		};

		// The press goes out now; what must be released before it, in order, is returned.
		[[nodiscard]] std::vector<Press> Pressed(const Press& a_press, Clock::time_point a_now)
		{
			std::vector<Press> first;
			std::erase_if(pending, [&](const Pending& a_pending) {
				if (Same(a_pending.press, a_press)) {
					auto       released = a_pending.press;
					const auto pressedAt = a_pending.due - released.hold;
					released.hold = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(a_now - pressedAt), std::chrono::milliseconds::zero(), released.hold);
					first.push_back(std::move(released));
					return true;
				}
				return false;
			});
			pending.push_back({ a_press, a_now + a_press.hold });
			return first;
		}

		// The presses whose hold is over, in the order they are due; they are forgotten.
		[[nodiscard]] std::vector<Press> Due(Clock::time_point a_now)
		{
			std::vector<Pending> due;
			std::erase_if(pending, [&](const Pending& a_pending) {
				if (a_pending.due <= a_now) {
					due.push_back(a_pending);
					return true;
				}
				return false;
			});
			std::ranges::stable_sort(due, {}, &Pending::due);
			std::vector<Press> out;
			for (auto& entry : due) {
				out.push_back(std::move(entry.press));
			}
			return out;
		}

		[[nodiscard]] std::size_t Waiting() const { return pending.size(); }

	private:
		static bool Same(const Press& a_left, const Press& a_right) { return a_left.userEvent == a_right.userEvent && a_left.device == a_right.device && a_left.id == a_right.id; }

		std::vector<Pending> pending;
	};
}
