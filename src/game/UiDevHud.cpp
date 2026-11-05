#include "game/UiDevHud.h"

#include "Config.h"
#include "game/UiDevHudText.h"
#include "game/FrameStats.h"
#include "game/Hud.h"
#include "game/UiPanel.h"
#include "game/Party.h"
#include "game/Perf.h"
#include "game/RemotePlayers.h"
#include "net/Session.h"
#include "Redact.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace UiDevHud
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;
		using Clock = std::chrono::steady_clock;

		constexpr const char* NAME = "F4MPDevHud";
		constexpr const char* MOVIE = "Interface/F4Multiplayer/DevHud.swf";
		constexpr float       UNITS_PER_METRE = 70.0f;
		constexpr auto        TEXT_EVERY = std::chrono::milliseconds(250);
		constexpr auto        FRAMES_EVERY = std::chrono::seconds(1);  // frame rate and frame times are over this long
		constexpr auto        SHOW_EVERY = std::chrono::seconds(2);    // the engine rebuilds menus after a load: asked again, not at once

		struct Text
		{
			std::string   text;
			std::uint32_t color = 0;
		};
		std::mutex          textLock;
		std::optional<Text> nextText;  // under textLock
		std::atomic<std::uint32_t> applied{ 0 };
		std::atomic<std::uint32_t> made{ 0 };
		std::atomic<int>           alive{ 0 };
		std::atomic<bool>          noMovie{ false };  // it could not be loaded: not asked for again this run

		// Gathered on the main thread only.
		std::string              lastText;
		std::uint32_t            updates = 0;
		bool                     asked = false;  // we asked for it to show, and haven't asked it to hide
		Clock::time_point        nextGather{};
		Clock::time_point        nextShow{};
		Clock::time_point        windowStart{};
		Clock::time_point        lastFrame{};
		std::uint32_t            windowDrawn = 0;
		FrameStats::Window       window;
		UiDevHudText::Facts        facts;

		class Panel final : public RE::GameMenuBase
		{
		public:
			Panel()
			{
				++made;
				++alive;
				// Beside the game's HUD: no pause, cursor, input or menu context. Not through the game's menu renderer
				// (kCustomRendering): a menu that isn't draws in a later pass, over all the others.
				menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kRequiresUpdate, RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu,
					RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
				depthPriority = RE::UI_DEPTH_PRIORITY::kHUD;
				const auto scaleform = RE::BSScaleformManager::GetSingleton();
				if (!scaleform || !scaleform->LoadMovieEx(*this, MOVIE, "root")) {
					REX::ERROR("UiDevHud: {} could not be loaded; is it in Data/Interface?", MOVIE);
					noMovie = true;
				}
			}

			~Panel() override
			{
				--alive;
			}

			// Flash objects are written only here. Nothing is written while the UI is held (UiHold.h).
			void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
			{
				if (uiMovie && Hud::IsSettled()) {
					std::optional<Text> text;
					{
						std::scoped_lock l{ textLock };
						text.swap(nextText);
					}
					if (text) {
						const std::array<GValue, 2> args{ GValue(text->text.c_str()), GValue(static_cast<double>(text->color)) };
						if (menuObj.Invoke("SetText", nullptr, args.data(), args.size())) {
							++applied;
						} else {
							REX::WARN("UiDevHud: the movie has no SetText");
						}
					}
				}
				GameMenuBase::AdvanceMovie(a_timeDelta, a_time);
			}
		};

		RE::IMenu* Create(const RE::UIMessage&)
		{
			return new Panel();
		}

		void Message(RE::UI_MESSAGE_TYPE a_type)
		{
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(NAME, a_type);
			}
		}

		// Whether our menu exists, kept by the menu itself. Asking the UI would take a reference to it, and could be what
		// destroys a closing menu on this thread (OpenMenus.h).
		bool IsOpen()
		{
			return alive.load() > 0;
		}

		UiDevHudText::Link LinkOf(const Session::Status& a_status)
		{
			if (a_status.hosting) {
				return UiDevHudText::Link::kHosting;
			}
			if (a_status.hostWaiting) {
				return UiDevHudText::Link::kHostWaiting;
			}
			switch (a_status.link) {
			case Session::Status::Link::kJoined:
				return UiDevHudText::Link::kJoined;
			case Session::Status::Link::kConnecting:
			case Session::Status::Link::kConnected:
				return UiDevHudText::Link::kJoining;
			default:
				return UiDevHudText::Link::kDormant;
			}
		}

		// "127.0.x.x:7779" from "127.0.0.1:7779" or "127.0.0.1"; the address is masked as in the log, as this goes on screen.
		std::string Masked(std::string_view a_address)
		{
			const auto colon = a_address.rfind(':');
			if (colon == std::string_view::npos) {
				return Redact::MaskAddress(a_address, Config::Get().port);
			}
			std::uint16_t port = Config::Get().port;
			std::from_chars(a_address.data() + colon + 1, a_address.data() + a_address.size(), port);
			return Redact::MaskAddress(a_address.substr(0, colon), port);
		}

		// What the link's state doesn't say: where to, and why it isn't there yet.
		std::string LinkDetail(const Session::Status& a_status)
		{
			if (a_status.hosting || a_status.link == Session::Status::Link::kNone) {
				return a_status.rejectReason.empty() ? std::string{} : "refused: " + Redact::Scrub(a_status.rejectReason);
			}
			auto out = a_status.steam ? std::string{} : Masked(a_status.address);
			if (a_status.link == Session::Status::Link::kConnected) {
				out += " waiting for welcome";
			} else if (a_status.link == Session::Status::Link::kConnecting) {
				out += a_status.retrySoon ? " retrying" : " connecting";
			}
			if (!a_status.rejectReason.empty()) {
				out += " refused: " + Redact::Scrub(a_status.rejectReason);
			}
			return out;
		}

		// Frame rate and frame times over the last whole window, and our own cost. The window is the HUD's own:
		// FrameStats::Global's belongs to the dev channel's `frames`, and reading it starts it over.
		void CountFrame(Clock::time_point a_now)
		{
			if (lastFrame != Clock::time_point{}) {
				window.Add(a_now - lastFrame);
			}
			lastFrame = a_now;
			const auto state = RE::BSGraphics::State::GetSingleton();
			const auto drawn = state ? state->frameCount : 0;
			if (windowStart == Clock::time_point{}) {
				windowStart = a_now;
				windowDrawn = drawn;
				return;
			}
			if (a_now - windowStart < FRAMES_EVERY) {
				return;
			}
			const auto seconds = std::chrono::duration<float>(a_now - windowStart).count();
			facts.fps = static_cast<float>(drawn - windowDrawn) / seconds;
			facts.frameMeanMs = window.MeanMs();
			facts.frameWorstMs = window.WorstMs();
			facts.hitches = window.hitches;
			const auto ours = Perf::Global().Of(0);
			facts.oursP50Ms = ours.p50;
			facts.oursP99Ms = ours.p99;
			window = FrameStats::Window{};
			windowStart = a_now;
			windowDrawn = drawn;
		}

		void Gather(const Session::Status& a_status)
		{
			facts.link = LinkOf(a_status);
			facts.steam = a_status.steam;
			facts.linkDetail = LinkDetail(a_status);
			facts.friends.clear();
			facts.roundTripMs = Party::RoundTripMs();
			facts.update = ++updates;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			const auto location = player ? player->currentLocation : nullptr;
			facts.location = location && location->GetFullName() ? location->GetFullName() : "";
			facts.cell = cell ? cell->GetFormID() : 0;
			facts.x = player ? player->data.location.x : 0.0f;
			facts.y = player ? player->data.location.y : 0.0f;
			for (const auto& info : RemotePlayers::List()) {
				UiDevHudText::Facts::Friend entry{ .id = info.id, .unreachable = info.unreachable };
				if (const auto where = RemotePlayers::PositionOf(info.id); where && player) {
					entry.metres = player->data.location.GetDistance(*where) / UNITS_PER_METRE;
				}
				facts.friends.push_back(entry);
			}
			lastText = UiDevHudText::Format(facts);
			std::scoped_lock l{ textLock };
			nextText = Text{ lastText, UiPanel::HudColor() };
		}
	}

	void Register()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(NAME, &Create);
		}
	}

	void Frame()
	{
		if (!Config::Get().developerHud) {
			if (asked && Hud::IsSettled()) {
				asked = false;
				Message(RE::UI_MESSAGE_TYPE::kHide);
			}
			return;
		}
		const auto now = Clock::now();
		CountFrame(now);
		if (noMovie || !Hud::IsSettled() || now < nextGather) {
			return;
		}
		nextGather = now + TEXT_EVERY;
		const auto status = Session::GetStatus();
		if (!status.inWorld) {
			if (asked) {
				asked = false;
				Message(RE::UI_MESSAGE_TYPE::kHide);
			}
			return;
		}
		if (!IsOpen() && now >= nextShow) {
			nextShow = now + SHOW_EVERY;
			asked = true;
			Message(RE::UI_MESSAGE_TYPE::kShow);
		}
		Gather(status);
	}

	std::string Describe()
	{
		return std::format("on={} open={} made={} updates={} applied={} noMovie={} text='{}'", Config::Get().developerHud, IsOpen(), made.load(), updates, applied.load(), noMovie.load(), lastText);
	}
}
