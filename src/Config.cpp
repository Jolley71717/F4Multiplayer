#include "Config.h"

namespace Config
{
	namespace
	{
		constexpr auto PATH = "Data/F4SE/Plugins/F4Multiplayer.ini"sv;

		Settings settings;

		std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r");
			return a_str.substr(first, last - first + 1);
		}

		bool ParseBool(std::string_view a_value)
		{
			return a_value == "1" || a_value == "true" || a_value == "True" || a_value == "TRUE";
		}

		template <class T>
		void ParseInt(std::string_view a_value, T& a_out)
		{
			T value{};
			const auto [ptr, ec] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value);
			if (ec == std::errc{} && ptr == a_value.data() + a_value.size()) {
				a_out = value;
			}
		}
	}

	void Load()
	{
		std::ifstream file{ std::string{ PATH } };
		if (!file) {
			REX::INFO("Config: {} not found, using defaults", PATH);
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			auto view = Trim(line);
			if (view.empty() || view.front() == '#' || view.front() == ';' || view.front() == '[') {
				continue;
			}

			const auto eq = view.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}

			const auto key = Trim(view.substr(0, eq));
			const auto value = Trim(view.substr(eq + 1));

			if (key == "bDevChannel") {
				settings.devChannel = ParseBool(value);
			} else if (key == "iDevChannelPort") {
				ParseInt(value, settings.devChannelPort);
			} else {
				REX::WARN("Config: unknown key '{}'", key);
			}
		}

		REX::INFO("Config: loaded {}", PATH);
	}

	const Settings& Get()
	{
		return settings;
	}
}
