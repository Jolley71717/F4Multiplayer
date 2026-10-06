#pragma once

// Loading screens in Fallout 4 load in lockstep with frame presentation, so vsync makes
// them much slower. This turns vsync off while the loading menu is open and restores it after.
namespace FastLoad
{
	void Install();
}
