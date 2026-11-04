#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The dev channel's virtual gamepad (VirtualPad.cpp hooks the game's XInput calls and answers them with this), for
// scripted rig tests: each game process has its own pad, read by the game's own input code as a real one is, with the
// window in front or not, and analog sticks instead of key taps. What the pad holds, the `pad` command's parsing, the
// walkto steering and its progress are here; pure, so they are unit tested. Dev channel only: no player has a pad of ours.
namespace PadPlan
{
	using Clock = std::chrono::steady_clock;

	// XINPUT_GAMEPAD, as XInputGetState hands it over.
	struct Gamepad
	{
		std::uint16_t buttons = 0;
		std::uint8_t  leftTrigger = 0;
		std::uint8_t  rightTrigger = 0;
		std::int16_t  leftX = 0;
		std::int16_t  leftY = 0;
		std::int16_t  rightX = 0;
		std::int16_t  rightY = 0;

		friend bool operator==(const Gamepad&, const Gamepad&) = default;
	};
	static_assert(sizeof(Gamepad) == 12);

	// XInput's own dead zones (XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, _RIGHT_, _TRIGGER_THRESHOLD): a game reads nothing
	// inside them, so a stick value maps past them (Stick) and a small push still moves.
	constexpr int LEFT_DEADZONE = 7849;
	constexpr int RIGHT_DEADZONE = 8689;
	constexpr int TRIGGER_THRESHOLD = 30;

	// -1..1 stick values to the axes: the middle is 0, and any push lies past the dead zone, scaled over what is left of
	// the travel (so the game's own dead zone takes off what this put on), along the stick's own direction, so a
	// diagonal stays a diagonal (a dead zone is measured on the stick's length, not on each axis).
	inline std::pair<std::int16_t, std::int16_t> Stick(float a_x, float a_y, int a_deadzone)
	{
		const float x = std::isfinite(a_x) ? a_x : 0.0f;
		const float y = std::isfinite(a_y) ? a_y : 0.0f;
		const float length = std::hypot(x, y);
		if (!(length > 0.0f)) {
			return { 0, 0 };
		}
		const float scaled = std::min(length, 1.0f);
		const float raw = static_cast<float>(a_deadzone) + scaled * static_cast<float>(32767 - a_deadzone);
		const auto  clampAxis = [](float a_v) { return static_cast<std::int16_t>(std::clamp(std::lround(a_v), -32767L, 32767L)); };
		return { clampAxis(x / length * raw), clampAxis(y / length * raw) };
	}

	// 0..1 to a trigger: 0 is let go, anything else past the threshold.
	inline std::uint8_t Trigger(float a_value)
	{
		if (!(a_value > 0.0f)) {
			return 0;
		}
		const float v = std::min(a_value, 1.0f);
		return static_cast<std::uint8_t>(std::lround(TRIGGER_THRESHOLD + v * (255 - TRIGGER_THRESHOLD)));
	}

	struct ButtonName
	{
		std::string_view name;
		std::uint16_t    mask;
	};
	inline constexpr std::array<ButtonName, 14> BUTTONS{ {
		{ "up", 0x0001 },
		{ "down", 0x0002 },
		{ "left", 0x0004 },
		{ "right", 0x0008 },
		{ "start", 0x0010 },
		{ "back", 0x0020 },
		{ "ls", 0x0040 },  // the left stick pressed in (sprint)
		{ "rs", 0x0080 },  // the right stick pressed in
		{ "lb", 0x0100 },
		{ "rb", 0x0200 },
		{ "a", 0x1000 },
		{ "b", 0x2000 },
		{ "x", 0x4000 },
		{ "y", 0x8000 },
	} };

	inline std::optional<std::uint16_t> ButtonMask(std::string_view a_name)
	{
		for (const auto& button : BUTTONS) {
			if (button.name == a_name) {
				return button.mask;
			}
		}
		return std::nullopt;
	}

	inline std::string ButtonList(std::uint16_t a_mask)
	{
		std::string out;
		for (const auto& button : BUTTONS) {
			if ((a_mask & button.mask) != 0) {
				out += out.empty() ? "" : "+";
				out += button.name;
			}
		}
		return out.empty() ? "none" : out;
	}

	constexpr auto BUTTON_TAP = std::chrono::milliseconds(100);  // a button with no time given: a tap
	constexpr auto MAX_HOLD = std::chrono::milliseconds(600000);

	// What the pad holds. Each part is held until it is set again, or until its time is up when one was given (a
	// stick given for 500 ms is back in the middle after that, read or not).
	class Pad
	{
	public:
		using Until = std::optional<Clock::time_point>;  // none: held until set again

		void SetStick(bool a_right, float a_x, float a_y, Until a_until)
		{
			auto& stick = a_right ? right : left;
			stick = { a_x, a_y, a_until };
		}

		void SetTrigger(bool a_right, float a_value, Until a_until)
		{
			auto& trigger = a_right ? rightTrigger : leftTrigger;
			trigger = { a_value, a_until };
		}

		// A button down until the time given (a press while it is down holds it down until the later time).
		void PressButton(std::uint16_t a_mask, Clock::time_point a_until)
		{
			for (std::size_t i = 0; i < BUTTONS.size(); ++i) {
				if ((a_mask & BUTTONS[i].mask) != 0) {
					auto& held = buttons[i];
					held = held && *held > a_until ? held : std::optional{ a_until };
				}
			}
		}

		// Everything let go (the pad stays plugged in).
		void Neutral()
		{
			left = right = {};
			leftTrigger = rightTrigger = {};
			buttons = {};
		}

		[[nodiscard]] Gamepad State(Clock::time_point a_now) const
		{
			Gamepad out;
			const auto live = [&](const Until& a_until) { return !a_until || a_now < *a_until; };
			if (live(left.until)) {
				std::tie(out.leftX, out.leftY) = Stick(left.x, left.y, LEFT_DEADZONE);
			}
			if (live(right.until)) {
				std::tie(out.rightX, out.rightY) = Stick(right.x, right.y, RIGHT_DEADZONE);
			}
			if (live(leftTrigger.until)) {
				out.leftTrigger = Trigger(leftTrigger.value);
			}
			if (live(rightTrigger.until)) {
				out.rightTrigger = Trigger(rightTrigger.value);
			}
			for (std::size_t i = 0; i < BUTTONS.size(); ++i) {
				if (buttons[i] && a_now < *buttons[i]) {
					out.buttons |= BUTTONS[i].mask;
				}
			}
			return out;
		}

	private:
		struct StickHold
		{
			float x = 0.0f, y = 0.0f;
			Until until = Clock::time_point{};  // let go
		};
		struct TriggerHold
		{
			float value = 0.0f;
			Until until = Clock::time_point{};
		};

		StickHold                                         left, right;
		TriggerHold                                       leftTrigger, rightTrigger;
		std::array<std::optional<Clock::time_point>, 14> buttons{};
	};

	// XInput's packet number: a game may read a state as new only when the number moves, so it moves whenever what is
	// handed over changes.
	class Packets
	{
	public:
		std::uint32_t Number(const Gamepad& a_state)
		{
			if (!sent || a_state != last) {
				++number;
				last = a_state;
				sent = true;
			}
			return number;
		}

	private:
		Gamepad       last;
		std::uint32_t number = 0;
		bool          sent = false;
	};

	inline std::string Describe(const Gamepad& a_state)
	{
		return std::format("lx={} ly={} rx={} ry={} lt={} rt={} buttons={}", a_state.leftX, a_state.leftY, a_state.rightX, a_state.rightY, a_state.leftTrigger,
			a_state.rightTrigger, ButtonList(a_state.buttons));
	}

	// ---- the pad command ----

	inline constexpr std::string_view USAGE =
		"usage: pad [status] | pad stick l|r <x> <y> [ms] | pad button <name>[+<name>...] [ms] | pad trigger l|r <0-1> [ms] | pad off | pad unplug "
		"(sticks -1..1, y up; buttons a b x y lb rb back start ls rs up down left right)";

	struct Command
	{
		enum class Kind : std::uint8_t
		{
			kStatus,
			kStick,
			kButton,
			kTrigger,
			kOff,     // everything let go, still plugged in
			kUnplug,  // the game's own XInput again
		};
		Kind                                     kind = Kind::kStatus;
		bool                                     right = false;
		float                                    x = 0.0f, y = 0.0f;  // a stick; a trigger's value is x
		std::uint16_t                            buttons = 0;
		std::optional<std::chrono::milliseconds> hold;
	};

	inline std::vector<std::string_view> Words(std::string_view a_args)
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
		return words;
	}

	inline std::optional<float> Number(std::string_view a_word)
	{
		float      value{};
		const auto [ptr, ec] = std::from_chars(a_word.data(), a_word.data() + a_word.size(), value);
		if (a_word.empty() || ec != std::errc{} || ptr != a_word.data() + a_word.size() || !std::isfinite(value)) {
			return std::nullopt;
		}
		return value;
	}

	inline std::optional<std::chrono::milliseconds> Millis(std::string_view a_word)
	{
		int        ms = 0;
		const auto [ptr, ec] = std::from_chars(a_word.data(), a_word.data() + a_word.size(), ms);
		if (a_word.empty() || ec != std::errc{} || ptr != a_word.data() + a_word.size() || ms < 0 || std::chrono::milliseconds(ms) > MAX_HOLD) {
			return std::nullopt;
		}
		return std::chrono::milliseconds(ms);
	}

	inline std::variant<Command, std::string> Parse(std::string_view a_args)
	{
		const auto words = Words(a_args);
		const auto error = [](std::string_view a_what) { return std::string{ "error: " } + std::string{ a_what } + "; " + std::string{ USAGE }; };
		Command    command;
		if (words.empty() || (words.size() == 1 && words[0] == "status")) {
			return command;
		}
		const auto verb = words[0];
		const auto side = [&](std::size_t a_at) -> std::optional<bool> {
			if (words.size() <= a_at) {
				return std::nullopt;
			}
			if (words[a_at] == "l" || words[a_at] == "left") {
				return false;
			}
			if (words[a_at] == "r" || words[a_at] == "right") {
				return true;
			}
			return std::nullopt;
		};
		// An optional hold at a_at, and nothing after it.
		const auto tail = [&](std::size_t a_at) -> std::optional<std::string> {
			if (words.size() > a_at + 1) {
				return error("too many words");
			}
			if (words.size() == a_at + 1) {
				const auto hold = Millis(words[a_at]);
				if (!hold) {
					return error("a hold is milliseconds, 0 to 600000");
				}
				command.hold = hold;
			}
			return std::nullopt;
		};
		if (verb == "off" || verb == "unplug") {
			if (words.size() != 1) {
				return error("too many words");
			}
			command.kind = verb == "off" ? Command::Kind::kOff : Command::Kind::kUnplug;
			return command;
		}
		if (verb == "stick") {
			const auto right = side(1);
			const auto x = words.size() > 2 ? Number(words[2]) : std::nullopt;
			const auto y = words.size() > 3 ? Number(words[3]) : std::nullopt;
			if (!right || !x || !y) {
				return error("stick wants l|r and x y");
			}
			if (std::abs(*x) > 1.0f || std::abs(*y) > 1.0f) {
				return error("a stick's x and y are -1 to 1");
			}
			command.kind = Command::Kind::kStick;
			command.right = *right;
			command.x = *x;
			command.y = *y;
			if (auto bad = tail(4)) {
				return *bad;
			}
			return command;
		}
		if (verb == "trigger") {
			const auto right = side(1);
			const auto value = words.size() > 2 ? Number(words[2]) : std::nullopt;
			if (!right || !value || *value < 0.0f || *value > 1.0f) {
				return error("trigger wants l|r and 0 to 1");
			}
			command.kind = Command::Kind::kTrigger;
			command.right = *right;
			command.x = *value;
			if (auto bad = tail(3)) {
				return *bad;
			}
			return command;
		}
		if (verb == "button") {
			if (words.size() < 2) {
				return error("button wants a name");
			}
			auto names = words[1];
			while (true) {  // name+name+...: an empty one (a+, a++b) is no button
				const auto plus = names.find('+');
				const auto mask = ButtonMask(names.substr(0, plus));
				if (!mask) {
					return error(std::format("no button '{}'", names.substr(0, plus)));
				}
				command.buttons |= *mask;
				if (plus == std::string_view::npos) {
					break;
				}
				names.remove_prefix(plus + 1);
			}
			if (command.buttons == 0) {
				return error("button wants a name");
			}
			command.kind = Command::Kind::kButton;
			if (auto bad = tail(2)) {
				return *bad;
			}
			return command;
		}
		return error(std::format("no pad command '{}'", verb));
	}

	// ---- walkto: steering with the sticks ----

	constexpr float TO_DEGREES = 180.0f / std::numbers::pi_v<float>;

	// The game's heading: degrees clockwise from north (+y), as data.angle.z is.
	inline float Bearing(float a_dx, float a_dy)
	{
		float b = std::atan2(a_dx, a_dy) * TO_DEGREES;
		return b < 0.0f ? b + 360.0f : b;
	}

	// a - b, the short way round: -180..180.
	inline float AngleDiff(float a_a, float a_b)
	{
		float d = std::fmod(a_a - a_b, 360.0f);
		if (d > 180.0f) {
			d -= 360.0f;
		} else if (d < -180.0f) {
			d += 360.0f;
		}
		return d;
	}

	struct Sticks
	{
		float leftX = 0.0f, leftY = 0.0f;  // move: x right, y forward, as seen by the player
		float rightX = 0.0f;               // turn: + to the right
	};

	struct SteerSettings
	{
		float speed = 1.0f;        // the left stick's push, 0..1 (the game walks at a small push and runs at a full one)
		float radius = 80.0f;      // arrived within this far
		float slowFrom = 3.0f;     // radii from the target at which the push starts to ease off
		float slowest = 0.4f;      // of speed, at the radius
		float turnFull = 60.0f;    // degrees off the target's bearing at which the right stick is pushed all the way
		float turnMax = 0.6f;      // the right stick's furthest push
		float turnDeadband = 3.0f; // degrees: closer to the bearing than this, no turn
	};

	// One frame's sticks toward a target: the left stick pushes toward it as the player faces now (so a heading that is
	// off still moves straight at it), and the right stick turns the player to face it, as a hand on a pad would.
	inline Sticks Steer(float a_dx, float a_dy, float a_yawDegrees, const SteerSettings& a_settings)
	{
		Sticks     out;
		const auto distance = std::hypot(a_dx, a_dy);
		if (!(distance > a_settings.radius)) {
			return out;
		}
		const float off = AngleDiff(Bearing(a_dx, a_dy), a_yawDegrees);
		const float near = a_settings.radius * a_settings.slowFrom;
		const float ease = near > a_settings.radius ? std::clamp((distance - a_settings.radius) / (near - a_settings.radius), 0.0f, 1.0f) : 1.0f;
		const float push = a_settings.speed * (a_settings.slowest + (1.0f - a_settings.slowest) * ease);
		const float r = off / TO_DEGREES;
		out.leftX = std::sin(r) * push;
		out.leftY = std::cos(r) * push;
		if (std::abs(off) > a_settings.turnDeadband) {
			out.rightX = std::clamp(off / a_settings.turnFull, -1.0f, 1.0f) * a_settings.turnMax;
		}
		return out;
	}

	// A walk's progress: arrived, given up as stuck (no ground gained for a while: a wall, or a game that drops the
	// player's input), or out of time.
	class Walk
	{
	public:
		enum class State : std::uint8_t
		{
			kWalking,
			kArrived,
			kStuck,
			kTimedOut,
		};

		static constexpr auto  STUCK_AFTER = std::chrono::seconds(4);
		static constexpr float STUCK_GAIN = 30.0f;  // units nearer the target within STUCK_AFTER, or stuck

		// The first check comes after twice STUCK_AFTER: a game that has just seen the pad plugged in takes a moment to
		// read it, and a standing start takes a moment to get going.
		Walk(Clock::time_point a_start, std::chrono::milliseconds a_timeout) :
			start(a_start), deadline(a_start + a_timeout), checkAt(a_start + STUCK_AFTER * 2) {}

		// One frame at this distance from the target (within the radius: arrived).
		State Step(Clock::time_point a_now, float a_distance, float a_radius)
		{
			if (state != State::kWalking) {
				return state;
			}
			if (!best || a_distance < *best) {
				best = a_distance;
			}
			if (a_distance <= a_radius) {
				return state = State::kArrived;
			}
			if (a_now >= deadline) {
				return state = State::kTimedOut;
			}
			if (!checkBest) {
				checkBest = a_distance;
			}
			if (a_now >= checkAt) {
				if (*checkBest - *best < STUCK_GAIN) {
					return state = State::kStuck;
				}
				checkBest = *best;
				checkAt = a_now + STUCK_AFTER;
			}
			return state;
		}

		[[nodiscard]] State Now() const { return state; }
		[[nodiscard]] float Closest() const { return best.value_or(-1.0f); }
		[[nodiscard]] Clock::time_point Started() const { return start; }

	private:
		Clock::time_point    start, deadline, checkAt;
		std::optional<float> best, checkBest;
		State                state = State::kWalking;
	};

	inline std::string_view Name(Walk::State a_state)
	{
		switch (a_state) {
		case Walk::State::kWalking:
			return "walking";
		case Walk::State::kArrived:
			return "arrived";
		case Walk::State::kStuck:
			return "stuck";
		case Walk::State::kTimedOut:
			return "timed out";
		}
		return "?";
	}

	// walkto <ref> [speed] [radius] [timeoutS] | walkto <x> <y> [speed] [radius] [timeoutS] | walkto stop | walkto
	// A reference is a form id: 0x-prefixed, eight hex digits, or with a hex letter in it; anything else is x y.
	struct WalkCommand
	{
		enum class Kind : std::uint8_t
		{
			kStatus,
			kStop,
			kRef,
			kPoint,
		};
		Kind                      kind = Kind::kStatus;
		std::uint32_t             ref = 0;
		float                     x = 0.0f, y = 0.0f;
		float                     speed = 1.0f;
		float                     radius = 80.0f;
		std::chrono::milliseconds timeout = std::chrono::seconds(120);
	};

	inline constexpr std::string_view WALK_USAGE =
		"usage: walkto <refHex> [speed 0-1] [radius] [timeoutS] | walkto <x> <y> [speed 0-1] [radius] [timeoutS] | walkto stop | walkto";

	inline std::optional<std::uint32_t> FormId(std::string_view a_word)
	{
		const bool prefixed = a_word.starts_with("0x") || a_word.starts_with("0X");
		if (prefixed) {
			a_word.remove_prefix(2);
		}
		const bool letter = std::ranges::any_of(a_word, [](char c) { return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
		if (!prefixed && !letter && a_word.size() != 8) {
			return std::nullopt;
		}
		std::uint32_t value{};
		const auto [ptr, ec] = std::from_chars(a_word.data(), a_word.data() + a_word.size(), value, 16);
		if (a_word.empty() || ec != std::errc{} || ptr != a_word.data() + a_word.size()) {
			return std::nullopt;
		}
		return value;
	}

	inline std::variant<WalkCommand, std::string> ParseWalk(std::string_view a_args)
	{
		const auto  words = Words(a_args);
		const auto  error = [](std::string_view a_what) { return std::string{ "error: " } + std::string{ a_what } + "; " + std::string{ WALK_USAGE }; };
		WalkCommand command;
		if (words.empty()) {
			return command;
		}
		if (words[0] == "stop") {
			if (words.size() != 1) {
				return error("too many words");
			}
			command.kind = WalkCommand::Kind::kStop;
			return command;
		}
		std::size_t next = 0;
		if (const auto id = FormId(words[0])) {
			command.kind = WalkCommand::Kind::kRef;
			command.ref = *id;
			next = 1;
		} else {
			const auto x = Number(words[0]);
			const auto y = words.size() > 1 ? Number(words[1]) : std::nullopt;
			if (!x || !y) {
				return error("a target is a reference or x y");
			}
			command.kind = WalkCommand::Kind::kPoint;
			command.x = *x;
			command.y = *y;
			next = 2;
		}
		if (words.size() > next + 3) {
			return error("too many words");
		}
		if (words.size() > next) {
			const auto speed = Number(words[next]);
			if (!speed || !(*speed > 0.0f) || *speed > 1.0f) {
				return error("speed is above 0, up to 1");
			}
			command.speed = *speed;
		}
		if (words.size() > next + 1) {
			const auto radius = Number(words[next + 1]);
			if (!radius || !(*radius > 0.0f)) {
				return error("radius is above 0");
			}
			command.radius = *radius;
		}
		if (words.size() > next + 2) {
			const auto seconds = Number(words[next + 2]);
			if (!seconds || !(*seconds > 0.0f) || *seconds > 600.0f) {
				return error("timeout is seconds, up to 600");
			}
			command.timeout = std::chrono::milliseconds(static_cast<long long>(*seconds * 1000.0f));
		}
		return command;
	}
}
