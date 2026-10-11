#include "game/Menu.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/Party.h"
#include "net/Session.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"

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
		std::string         notice;  // shown once at the top of the main page (why hosting failed)

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
				Hud::Notify("Multiplayer: the menu isn't available");
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

		// A list longer than a page: two entries per page with "More" to turn it (three when they all
		// fit on one), and "Back". a_page wraps around.
		void ShowPaged(const std::string& a_body, std::vector<Button> a_entries, std::size_t a_page, void (*a_turn)(std::size_t), Button a_back)
		{
			const std::size_t perPage = a_entries.size() > MAX_BUTTONS - 1 ? MAX_BUTTONS - 2 : MAX_BUTTONS - 1;
			const std::size_t pages = (std::max<std::size_t>)(1, (a_entries.size() + perPage - 1) / perPage);
			const std::size_t page = a_page % pages;
			std::vector<Button> shown;
			for (std::size_t i = page * perPage; i < a_entries.size() && i < (page + 1) * perPage; ++i) {
				shown.push_back(std::move(a_entries[i]));
			}
			if (pages > 1) {
				shown.push_back({ std::format("More ({}/{})", page + 1, pages), [=] { a_turn(page + 1); } });
			}
			shown.push_back(std::move(a_back));
			Show(a_body, std::move(shown));
		}

		const char* OnOff(bool a_on)
		{
			return a_on ? "On" : "Off";
		}

		// "Sam's game (Steam)" or "192.168.1.5:7779".
		std::string DescribeAddress(const std::string& a_address)
		{
			if (const auto id = SteamTransport::ParseAddress(a_address)) {
				const auto name = Steam::NameOf(id);
				return (name.empty() ? std::format("Steam user {}", id) : name + "'s game") + " (Steam)";
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
					body += std::format("Hosting on UDP port {} ({} join by address).", settings.port, settings.transport == "enet" ? "friends" : "no Steam: friends");
				} else if (Steam::LobbyOpen()) {
					body += "Hosting through Steam. Friends join from their friends list (Join Game) or your invite.";
				} else {
					body += "Hosting through Steam; opening the lobby...";
				}
				if (status.link != Session::Status::Link::kJoined) {
					body += "\nYour own game is still connecting to it.";
				}
			} else {
				switch (status.link) {
				case Session::Status::Link::kNone:
					body += "Not in a session. Host one, or join a friend who is hosting.";
					break;
				case Session::Status::Link::kConnecting:
					body += "Connecting to " + DescribeAddress(status.address) + "...";
					if (!status.rejectReason.empty()) {
						body += "\nRefused: " + status.rejectReason + " (trying again in a minute)";
					}
					break;
				case Session::Status::Link::kConnected:
					body += "Connected to " + DescribeAddress(status.address) + ", joining...";
					break;
				case Session::Status::Link::kJoined:
					body += "In " + DescribeAddress(status.address);
					if (const auto ping = Party::RoundTripMs()) {
						body += std::format(" (ping {} ms)", ping);
					}
					break;
				}
			}
			const auto players = Party::PlayerLines();
			if (status.link == Session::Status::Link::kJoined) {
				body += players.empty() ? "\nNobody else is here yet." : std::format("\nWith you ({}):", players.size());
				for (const auto& line : players) {
					body += "\n" + line;
				}
			}
			return body;
		}

		void Host()
		{
			if (const auto error = Session::StartHosting(); !error.empty()) {
				notice = "Could not host: " + error + ".";
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
			if (status.hosting) {
				if (status.steam && Steam::LobbyOpen()) {
					entries.push_back({ "Invite friends", [] { Steam::OpenInviteDialog(); } });
				}
				entries.push_back({ "Stop hosting", StopHosting });
			} else if (status.link != Session::Status::Link::kNone) {
				entries.push_back({ "Leave", [] { Session::Leave(); ShowMain(); } });
				entries.push_back({ "Join someone else", [] { RequestJoin(0); } });
			} else {
				entries.push_back({ "Host a session", Host });
				entries.push_back({ "Join a friend", [] { RequestJoin(0); } });
			}
			entries.push_back({ "Settings", [] { ShowSettings(0); } });
			entries.push_back({ "Close", {} });
			Show(MainBody(), std::move(entries));
		}

		void ShowJoin(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			const auto  scan = Steam::ScanFriends();
			std::string body;
			std::vector<Button> entries;
			if (!scan.hosting.empty()) {
				body = "Steam friends hosting Fallout 4 co-op right now:";
				for (const auto& host : scan.hosting) {
					body += "\n" + host.name;
					entries.push_back({ host.name, [host] {
										   Steam::Join(host.lobby);
										   Hud::Notify(std::format("Multiplayer: joining {}'s game", host.name));
									   } });
				}
			} else if (!Steam::Available()) {
				body = "Steam isn't available in this game, so friends can't be listed.";
			} else {
				// The counts tell a player (and us) where the list stops: no friends online, none
				// in the game, or in the game but not hosting (or hosting with private game details).
				body = std::format("None of your Steam friends is hosting right now ({} friends, {} online, {} in Fallout 4).\nThe host can invite you from the Steam overlay (Shift+Tab), or pick Join Game on them in your friends list.",
					scan.friends, scan.online, scan.inGame);
			}
			if (!settings.serverAddress.empty()) {
				body += "\nAddress from the ini: " + settings.serverAddress;
				entries.push_back({ "Join " + settings.serverAddress, [address = settings.serverAddress] {
									   Session::ConnectTo(address);
									   Hud::Notify("Multiplayer: connecting to " + address);
								   } });
			}
			if (entries.empty()) {
				Show(body, { { "Look again", [] { RequestJoin(0); } }, { "Back", ShowMain } });
				return;
			}
			ShowPaged(body, std::move(entries), a_page, ShowJoin, { "Back", ShowMain });
		}

		// A setting the menu toggles: shown in the body and on its button, applied to the running
		// game at once and written to the ini.
		struct Toggle
		{
			const char* name;
			const char* what;  // what "On" means
			bool Config::Settings::*value;
			const char* key;
			const char* onValue;
			const char* offValue;
		};
		constexpr std::array<Toggle, 5> TOGGLES{ {
			{ "Friendly fire", "players can hurt each other (host's setting, from the next session hosted)", &Config::Settings::friendlyFire, "bFriendlyFire", "true", "false" },
			{ "Shared story", "the main story and factions move along together (host's setting, from the next session hosted)", &Config::Settings::sharedStory, "sStory", "shared", "own" },
			{ "Voice chat", "hold the voice key to talk to friends", &Config::Settings::voiceChat, "bVoiceChat", "true", "false" },
			{ "Compass markers", "friends on the compass", &Config::Settings::compassMarkers, "bCompassMarkers", "true", "false" },
			{ "Fast loading", "vsync off during loading screens", &Config::Settings::fastLoading, "bFastLoading", "true", "false" },
		} };

		void ShowSettings(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			std::string body;
			std::vector<Button> entries;
			for (const auto& toggle : TOGGLES) {
				const bool on = settings.*toggle.value;
				body += std::format("{}{}: {} ({})", body.empty() ? "" : "\n", toggle.name, OnOff(on), toggle.what);
				entries.push_back({ std::format("{}: {}", toggle.name, OnOff(on)), [&toggle, a_page] {
									   auto&      s = Config::Mutable();
									   const bool now = !(s.*toggle.value);
									   s.*toggle.value = now;
									   Config::Write(toggle.key, now ? toggle.onValue : toggle.offValue);
									   ShowSettings(a_page);
								   } });
			}
			body += "\nChanges are saved to F4Multiplayer.ini.";
			ShowPaged(body, std::move(entries), a_page, ShowSettings, { "Back", ShowMain });
		}
	}

	void Open()
	{
		ShowMain();
	}

	void Frame()
	{
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
		return std::format("menu: pages={} presses={} serial={} pendingJoin={} buttons='{}'", pagesShown, pressesHandled, serial, pendingJoin.has_value(), labels);
	}
}
