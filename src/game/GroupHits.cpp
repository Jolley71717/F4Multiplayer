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

		std::array<std::optional<float>, 7> originals;  // read the first time a change is made
		float                               mult = 1.0f;  // what the settings hold now, of their originals
		std::size_t                         players = 1;
		std::uint32_t                       writes = 0;

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
				if (!originals[i]) {
					originals[i] = setting->GetFloat();
				}
				setting->SetFloat(*originals[i] * a_mult);
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

	std::string Describe()
	{
		return std::format("group=x{:.2f} players={} writes={}", mult, players, writes);
	}
}
