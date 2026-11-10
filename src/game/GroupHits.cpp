#include "game/GroupHits.h"

#include "GroupDifficulty.h"

#include <array>
#include <cmath>
#include <format>
#include <optional>

namespace GroupHits
{
	namespace
	{
		// One per RE::DifficultyLevel (0 Very Easy to 6 Survival), as Session.cpp reads the ones the other way.
		constexpr std::array<const char*, 7> SETTINGS{ "fDiffMultHPByPCVE", "fDiffMultHPByPCE", "fDiffMultHPByPCN", "fDiffMultHPByPCH",
			"fDiffMultHPByPCVH", "fDiffMultHPByPCSV", "fDiffMultHPByPCTSV" };

		struct Held
		{
			std::optional<float> original;  // the game's own, read before our first write and again whenever the value isn't ours
			std::optional<float> written;   // the last value we wrote
		};

		std::array<Held, 7> held;
		float               mult = 1.0f;  // what the settings hold now, of their originals
		std::size_t         players = 1;
		std::uint32_t       writes = 0;
		std::uint32_t       rereads = 0;  // originals read again: a mod or setgs changed one under us

		RE::Setting* Find(const char* a_name)
		{
			const auto settings = RE::GameSettingCollection::GetSingleton();
			const auto setting = settings ? settings->GetSetting(a_name) : nullptr;
			return setting && setting->GetType() == RE::Setting::SETTING_TYPE::kFloat ? setting : nullptr;
		}

		void Write(float a_mult)
		{
			for (std::size_t i = 0; i < SETTINGS.size(); ++i) {
				const auto setting = Find(SETTINGS[i]);
				if (!setting) {
					continue;
				}
				auto&       slot = held[i];
				const float now = setting->GetFloat();
				// The value is the game's own (never written, or changed since by a mod or the console's setgs): that
				// is the original from here on, so a changed setting is scaled, not overwritten with a stale one.
				if (!slot.original || !slot.written || std::fabs(now - *slot.written) > 0.0001f) {
					rereads += slot.original.has_value();
					slot.original = now;
				}
				const float value = a_mult == 1.0f ? *slot.original : *slot.original * a_mult;
				setting->SetFloat(value);
				slot.written = value;
			}
			mult = a_mult;
			++writes;
		}
	}

	void Update(std::uint8_t a_percent, std::size_t a_players)
	{
		players = a_players;
		const float wanted = GroupDifficulty::DamageMult(a_percent, a_players);
		if (std::fabs(wanted - mult) < 0.0005f) {
			return;
		}
		Write(wanted);
		REX::INFO("GroupHits: {} players, {}% a player: our hits take x{:.2f} off NPCs", a_players, a_percent, wanted);
	}

	void Restore()
	{
		players = 1;
		if (mult != 1.0f) {
			Write(1.0f);
			REX::INFO("GroupHits: the damage multipliers are the game's own again");
		}
	}

	float FriendlyFireMult()
	{
		return mult > 0.0f ? 1.0f / mult : 1.0f;
	}

	std::string Describe()
	{
		return std::format("group=x{:.2f} players={} writes={} rereads={}", mult, players, writes, rereads);
	}
}
