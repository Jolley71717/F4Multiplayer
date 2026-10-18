#include "game/Menu.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/Party.h"
#include "net/Session.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"
#include "Text.h"

namespace Menu
{
	namespace
	{
		constexpr std::size_t MAX_BUTTONS = 4;  // what MessageMenuManager::Create takes
		constexpr auto        HEADER = "F4Multiplayer";

		// A button and what it does. The action runs on the main thread in Frame, after the box has
		// closed; an empty action just closes the menu.
		struct Button
		{
			std::string           label;
			std::function<void()> action;
		};

		struct ButtonPress
		{
			std::uint32_t serial;
			std::uint8_t  button;
		};

		// The engine calls this with the index of the button pressed, from whichever thread runs the
		// menu; the press waits here for Frame. The engine owns the object (reference counted with
		// the box's data), and a serial tells a stale box's press apart from the current page's.
		class Callback final : public RE::IMessageBoxCallback
		{
		public:
			explicit Callback(std::uint32_t a_serial) :
				serial(a_serial)
			{}

			void operator()(std::uint8_t a_button) override
			{
				std::scoped_lock l{ pressedLock };
				pressed = ButtonPress{ serial, a_button };
			}

			static inline std::mutex           pressedLock;
			static inline std::optional<ButtonPress> pressed;

		private:
			std::uint32_t serial;
		};

		std::uint32_t       serial = 0;
		std::vector<Button> buttons;  // the page showing, by button index
		std::uint32_t       pagesShown = 0;
		std::uint32_t       pressesHandled = 0;
		std::string         notice;    // shown once at the top of the main page (why hosting failed)
		std::string         lastBody;  // the page showing, for Describe
		// The menu key was pressed while the UI was held (a loading screen, a save or load, the moments after; see
		// UiHold): the box is one of the game's menus, and is shown once the UI has settled, from Frame.
		bool                openWaiting = false;

		// "Join a friend" first asks Steam for the friends' rich presence (a host's names their lobby,
		// and Steam hands it out only once asked), then shows the page when the answers are in or
		// after a moment. Nothing is on screen meanwhile, so the moment is short.
		using Clock = std::chrono::steady_clock;
		constexpr auto PRESENCE_WAIT = 1500ms;
		struct PendingJoin
		{
			std::size_t       page;
			Clock::time_point deadline;
			std::uint32_t     updatesBefore;
			std::size_t       asked;
		};
		std::optional<PendingJoin> pendingJoin;

		// Names and reject reasons come from other players: the box's text field reads markup, so
		// '<' and '>' and control characters (other than line breaks) are dropped, as the HUD does.
		std::string Clean(std::string_view a_text)
		{
			std::string clean;
			clean.reserve(a_text.size());
			for (const unsigned char c : a_text) {
				if ((c >= 0x20 && c != 0x7F && c != '<' && c != '>') || c == '\n') {
					clean.push_back(static_cast<char>(c));
				}
			}
			return clean;
		}

		void Show(const std::string& a_body, std::vector<Button> a_buttons)
		{
			const auto manager = RE::MessageMenuManager::GetSingleton();
			if (!manager) {
				Hud::Notify(Text::Get("menu.unavailable"));
				return;
			}
			if (a_buttons.size() > MAX_BUTTONS) {
				a_buttons.resize(MAX_BUTTONS);
			}
			buttons = std::move(a_buttons);
			for (auto& button : buttons) {
				button.label = Clean(button.label);  // friends' names end up on buttons
			}
			++serial;
			++pagesShown;
			const auto label = [&](std::size_t a_index) -> const char* { return a_index < buttons.size() ? buttons[a_index].label.c_str() : nullptr; };
			const auto body = Clean(a_body);
			lastBody = body;
			// The game's own warning dialog, with our text and buttons. It copies the strings and keeps
			// the callback alive for as long as the box exists.
			manager->Create(HEADER, body.c_str(), new Callback(serial), RE::WARNING_TYPES::kDefault, label(0), label(1), label(2), label(3));
		}

		void ShowMain();
		void ShowJoin(std::size_t a_page);
		void ShowSettings(std::size_t a_page);

		// The way to the join page: ask Steam about the friends first (see PendingJoin).
		void RequestJoin(std::size_t a_page)
		{
			const auto asked = Steam::RefreshFriends();
			if (asked == 0) {
				ShowJoin(a_page);  // nobody online, or no Steam: nothing to wait for
				return;
			}
			pendingJoin = PendingJoin{ a_page, Clock::now() + PRESENCE_WAIT, Steam::PresenceUpdates(), asked };
		}

		// The first button of every page is the harmless one (Back or Close): the game's back key, Tab,
		// presses the first button of a message box, and a player who leaves this menu the way they
		// leave every other menu must not host, leave or stop hosting by accident (two players did).
		// A list longer than a page: two entries per page with "More" to turn it (three when they all
		// fit on one), and "Back" first. a_page wraps around.
		void ShowPaged(const std::string& a_body, std::vector<Button> a_entries, std::size_t a_page, void (*a_turn)(std::size_t), Button a_back)
		{
			const std::size_t perPage = a_entries.size() > MAX_BUTTONS - 1 ? MAX_BUTTONS - 2 : MAX_BUTTONS - 1;
			const std::size_t pages = (std::max<std::size_t>)(1, (a_entries.size() + perPage - 1) / perPage);
			const std::size_t page = a_page % pages;
			std::vector<Button> shown;
			shown.push_back(std::move(a_back));
			for (std::size_t i = page * perPage; i < a_entries.size() && i < (page + 1) * perPage; ++i) {
				shown.push_back(std::move(a_entries[i]));
			}
			if (pages > 1) {
				shown.push_back({ Text::Format("menu.more", { { "page", page + 1 }, { "pages", pages } }), [=] { a_turn(page + 1); } });
			}
			Show(a_body, std::move(shown));
		}

		std::string OnOff(bool a_on)
		{
			return Text::Get(a_on ? "menu.on" : "menu.off");
		}

		// "Sam's game (Steam)" or "192.168.1.5:7779".
		std::string DescribeAddress(const std::string& a_address)
		{
			if (const auto id = SteamTransport::ParseAddress(a_address)) {
				const auto name = Steam::NameOf(id);
				return name.empty() ? Text::Format("menu.address_steam_unnamed", { { "id", id } }) : Text::Format("menu.address_steam_named", { { "player", name } });
			}
			return a_address;
		}

		std::string MainBody()
		{
			const auto& settings = Config::Get();
			const auto  status = Session::GetStatus();
			std::string body;
			if (!notice.empty()) {
				body += std::exchange(notice, {}) + "\n";
			}
			if (status.hosting) {
				if (!status.steam) {
					body += Text::Format(settings.transport == "enet" ? "menu.hosting_udp" : "menu.hosting_udp_nosteam", { { "port", settings.port } });
				} else if (Steam::LobbyOpen()) {
					body += Text::Get("menu.hosting_steam");
				} else {
					body += Text::Get("menu.hosting_steam_opening");
				}
				if (status.link != Session::Status::Link::kJoined) {
					body += "\n" + Text::Get("menu.own_game_connecting");
				}
			} else {
				switch (status.link) {
				case Session::Status::Link::kNone:
					body += Text::Get(status.hostWaiting ? "menu.host_waiting" : "menu.not_in_session");
					break;
				case Session::Status::Link::kConnecting:
					body += Text::Format("menu.connecting", { { "address", DescribeAddress(status.address) } });
					if (!status.rejectReason.empty()) {
						body += "\n" + Text::Format(status.retrySoon ? "menu.refused_soon" : "menu.refused_later", { { "reason", Text::Reject(status.rejectReason) } });
					}
					break;
				case Session::Status::Link::kConnected:
					body += Text::Format("menu.connected", { { "address", DescribeAddress(status.address) } });
					break;
				case Session::Status::Link::kJoined:
					body += Text::Format("menu.in_session", { { "address", DescribeAddress(status.address) } });
					if (const auto ping = Party::RoundTripMs()) {
						body += " (" + Text::Format("party.ping_info", { { "ms", ping } }) + ")";
					}
					break;
				}
			}
			const auto players = Party::PlayerLines();
			if (status.link == Session::Status::Link::kJoined) {
				body += players.empty() ? "\n" + Text::Get("menu.nobody_else") : "\n" + Text::Format("menu.with_you", { { "n", players.size() } });
				for (const auto& line : players) {
					body += "\n" + line;
				}
			}
			return body;
		}

		void Host()
		{
			if (const auto error = Session::StartHosting(); !error.empty()) {
				notice = Text::Format("menu.host_failed", { { "error", error } });
			} else {
				// As bHost would at startup, and remembered: next time the game starts, it hosts again.
				Config::Mutable().host = true;
				Config::Write("bHost", "true");
			}
			ShowMain();
		}

		void StopHosting()
		{
			Session::StopHosting();
			Config::Mutable().host = false;
			Config::Write("bHost", "false");
			ShowMain();
		}

		void ShowMain()
		{
			const auto          status = Session::GetStatus();
			std::vector<Button> entries;
			entries.push_back({ Text::Get("menu.close"), {} });  // first: Tab closes the menu (see ShowPaged)
			if (status.hosting) {
				if (status.steam && Steam::LobbyOpen()) {
					entries.push_back({ Text::Get("menu.invite"), [] { Steam::OpenInviteDialog(); } });
				}
				entries.push_back({ Text::Get("menu.stop_hosting"), StopHosting });
			} else if (status.hostWaiting) {
				entries.push_back({ Text::Get("menu.stop_hosting"), StopHosting });  // bHost off: it won't start
			} else if (status.link != Session::Status::Link::kNone) {
				entries.push_back({ Text::Get("menu.leave"), [] { Session::Leave(); ShowMain(); } });
				entries.push_back({ Text::Get("menu.join_other"), [] { RequestJoin(0); } });
			} else {
				entries.push_back({ Text::Get("menu.host"), Host });
				entries.push_back({ Text::Get("menu.join"), [] { RequestJoin(0); } });
			}
			entries.push_back({ Text::Get("menu.settings"), [] { ShowSettings(0); } });
			Show(MainBody(), std::move(entries));
		}

		void ShowJoin(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			const auto  scan = Steam::ScanFriends();
			std::string body;
			std::vector<Button> entries;
			if (!scan.hosting.empty()) {
				body = Text::Get("menu.friends_hosting");
				for (const auto& host : scan.hosting) {
					body += "\n" + host.name;
					entries.push_back({ host.name, [host] {
										   Steam::Join(host.lobby);
										   Hud::Notify(Text::Format("menu.joining_host", { { "player", host.name } }));
									   } });
				}
			} else if (!Steam::Available()) {
				body = Text::Get("menu.steam_missing");
			} else {
				// The counts tell a player (and us) where the list stops: no friends online, none
				// in the game, or in the game but not hosting (or hosting with private game details).
				body = Text::Format("menu.none_hosting", { { "friends", scan.friends }, { "online", scan.online }, { "ingame", scan.inGame } });
			}
			if (!settings.serverAddress.empty()) {
				body += "\n" + Text::Format("menu.ini_address", { { "address", settings.serverAddress } });
				entries.push_back({ Text::Format("menu.join_address", { { "address", settings.serverAddress } }), [address = settings.serverAddress] {
									   Session::ConnectTo(address);
									   Hud::Notify(Text::Format("menu.connecting_hud", { { "address", address } }));
								   } });
			}
			if (entries.empty()) {
				Show(body, { { Text::Get("menu.back"), ShowMain }, { Text::Get("menu.look_again"), [] { RequestJoin(0); } } });
				return;
			}
			ShowPaged(body, std::move(entries), a_page, ShowJoin, { Text::Get("menu.back"), ShowMain });
		}

		// A setting the menu toggles: shown in the body and on its button, applied to the running
		// game at once and written to the ini.
		struct Toggle
		{
			const char* name;  // the key of its name
			const char* what;  // the key of what "On" means
			bool Config::Settings::*value;
			const char* key;
			const char* onValue;
			const char* offValue;
		};
		constexpr std::array<Toggle, 6> TOGGLES{ {
			{ "menu.friendly_fire", "menu.friendly_fire_what", &Config::Settings::friendlyFire, "bFriendlyFire", "true", "false" },
			{ "menu.shared_story", "menu.shared_story_what", &Config::Settings::sharedStory, "sStory", "shared", "own" },
			{ "menu.voice_chat", "menu.voice_chat_what", &Config::Settings::voiceChat, "bVoiceChat", "true", "false" },
			{ "menu.spoken_lines", "menu.spoken_lines_what", &Config::Settings::spokenLines, "bSpokenLines", "true", "false" },
			{ "menu.compass_markers", "menu.compass_markers_what", &Config::Settings::compassMarkers, "bCompassMarkers", "true", "false" },
			{ "menu.fast_loading", "menu.fast_loading_what", &Config::Settings::fastLoading, "bFastLoading", "true", "false" },
		} };

		void ShowSettings(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			std::string body;
			std::vector<Button> entries;
			for (const auto& toggle : TOGGLES) {
				const bool on = settings.*toggle.value;
				const auto name = Text::Get(toggle.name);
				body += (body.empty() ? "" : "\n") + Text::Format("menu.setting_line", { { "name", name }, { "state", OnOff(on) }, { "what", Text::Get(toggle.what) } });
				entries.push_back({ Text::Format("menu.setting_button", { { "name", name }, { "state", OnOff(on) } }), [&toggle, a_page] {
									   auto&      s = Config::Mutable();
									   const bool now = !(s.*toggle.value);
									   s.*toggle.value = now;
									   Config::Write(toggle.key, now ? toggle.onValue : toggle.offValue);
									   ShowSettings(a_page);
								   } });
			}
			body += "\n" + Text::Get("menu.settings_saved");
			ShowPaged(body, std::move(entries), a_page, ShowSettings, { Text::Get("menu.back"), ShowMain });
		}
	}

	void Open()
	{
		if (!Hud::IsSettled()) {
			openWaiting = true;  // shown from Frame once the UI has settled
			return;
		}
		ShowMain();
	}

	void Frame()
	{
		// No page while the UI is held (the engine is tearing its menus down or building them again): a press
		// made before waits here, as does a pending join and the menu key.
		if (!Hud::IsSettled()) {
			return;
		}
		if (std::exchange(openWaiting, false)) {
			ShowMain();
			return;  // a press from the page that was up before is stale by now
		}
		if (pendingJoin && (Clock::now() >= pendingJoin->deadline || Steam::PresenceUpdates() - pendingJoin->updatesBefore >= pendingJoin->asked)) {
			const auto page = pendingJoin->page;
			pendingJoin.reset();
			ShowJoin(page);
		}
		std::optional<ButtonPress> press;
		{
			std::scoped_lock l{ Callback::pressedLock };
			press = std::exchange(Callback::pressed, std::nullopt);
		}
		if (!press || press->serial != serial) {
			return;
		}
		++pressesHandled;
		// The action may show the next page, which replaces the buttons: take ours out first.
		auto pressed = std::exchange(buttons, {});
		if (press->button < pressed.size() && pressed[press->button].action) {
			pressed[press->button].action();
		}
	}

	void Press(std::uint8_t a_index)
	{
		std::scoped_lock l{ Callback::pressedLock };
		Callback::pressed = ButtonPress{ serial, a_index };
	}

	std::string Describe()
	{
		std::string labels;
		for (const auto& button : buttons) {
			labels += (labels.empty() ? "" : "|") + button.label;
		}
		// The body on one line (the dev channel's reply is one line).
		std::string body = lastBody;
		std::ranges::replace(body, '\n', '/');
		return std::format("menu: pages={} presses={} serial={} pendingJoin={} openWaiting={} buttons='{}' body='{}'", pagesShown, pressesHandled, serial, pendingJoin.has_value(), openWaiting, labels, body);
	}
}
