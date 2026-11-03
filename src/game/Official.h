#pragma once

// Whether a form comes from the game's own files (Fallout4.esm and the official add-ons). Quest
// stages and spoken lines are shared only for those: a stage set on a quest from somebody's mod runs
// that mod's scripts in a game that never reached it (a report had two players with a story mod lose
// their Pip-Boy and weapons, and crash in its dialogue), and a mod's topic said aloud does the same.
namespace Official
{
	inline bool IsOfficialFile(const RE::TESFile* a_file)
	{
		if (!a_file) {
			return false;
		}
		static constexpr std::array NAMES{
			"Fallout4.esm"sv, "DLCRobot.esm"sv, "DLCworkshop01.esm"sv, "DLCCoast.esm"sv,
			"DLCworkshop02.esm"sv, "DLCworkshop03.esm"sv, "DLCNukaWorld.esm"sv, "DLCUltraHighResolution.esm"sv
		};
		const auto name = a_file->GetFilename();
		return std::ranges::any_of(NAMES, [&](std::string_view a_official) {
			return std::ranges::equal(name, a_official, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
		});
	}

	// The plugin that made the form is one of the game's own (a later plugin may have changed it).
	inline bool IsOfficialForm(const RE::TESForm* a_form)
	{
		if (!a_form || (a_form->GetFormID() >> 24) == 0xFF) {
			return false;
		}
		return IsOfficialFile(a_form->GetFile(0));
	}

	// The plugin that made the form and every plugin that changes it are the game's own: what the record says (an
	// enable parent, a flag) is the game's, as it is in every game with those files.
	inline bool IsOfficialThroughout(const RE::TESForm* a_form)
	{
		if (!IsOfficialForm(a_form)) {
			return false;
		}
		const auto files = a_form->sourceFiles.array;
		if (!files) {
			return false;
		}
		for (std::uint32_t i = 0; i < files->size(); ++i) {
			if (!IsOfficialFile((*files)[i])) {
				return false;
			}
		}
		return true;
	}
}
