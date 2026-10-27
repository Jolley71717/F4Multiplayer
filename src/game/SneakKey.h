#pragma once

#include <cstdint>
#include <string>

// What holds the player's sneak key back, for the `controls` dev command: a key that does nothing is then a look at
// the line, not a guess.
//
// The key goes through the game's controls, then the game's sneak action, then the behaviour graph, and each has its
// say (Fallout4.exe 1.11.240):
//  - an input layer with its sneaking flag off drops the key's event, as a DisablePlayerControls with sneaking does;
//  - the controls' sneak handler with its input switched off, or the dialogue camera, never reads it;
//  - the controls read nothing while the player is not alive (down, dying or dead);
//  - knocked over, the sneak action is refused;
//  - the action's idles want a leg that is not crippled, no melee attack going on, and, in first person, no swim;
//  - the stance says which of sneakStart and sneakStop is asked, and a graph already in that state refuses it. That one
//    cannot be read from here (the graph's iIsInSneak is set with the stance): a graph left in sneak by a stance-only
//    stand is the down's to put right (DownRules::Step::graphSneakStop).
//
// Pure, so it is unit tested; DevCommands.cpp reads the game and sets the facts.
namespace SneakKey
{
	struct Facts
	{
		bool          layersLetSneak = true;  // every input layer in use lets the sneaking event through
		bool          handlerOn = true;       // the controls' sneak handler reads input
		bool          dialogueCamera = false;
		std::uint32_t lifeState = 0;          // 0: alive
		std::uint32_t knockState = 0;         // 0: on their feet
		std::uint32_t meleeAttackState = 0;   // 0: no melee attack going on
		float         leftLeg = 100.0f;       // the legs' condition; 0: crippled
		float         rightLeg = 100.0f;
		bool          firstPerson = false;
		bool          swimming = false;
	};

	// The names of what holds the key back, space-separated, or "none".
	[[nodiscard]] inline std::string Blocking(const Facts& a_facts)
	{
		std::string out;
		const auto  add = [&](const char* a_name) {
			if (!out.empty()) {
				out += ' ';
			}
			out += a_name;
		};
		if (!a_facts.layersLetSneak) {
			add("layer");
		}
		if (!a_facts.handlerOn) {
			add("handler-off");
		}
		if (a_facts.dialogueCamera) {
			add("dialogue");
		}
		if (a_facts.lifeState != 0) {
			add("not-alive");
		}
		if (a_facts.knockState != 0) {
			add("knocked");
		}
		if (a_facts.meleeAttackState != 0) {
			add("melee-attack");
		}
		if (a_facts.leftLeg <= 0.0f && a_facts.rightLeg <= 0.0f) {
			add("legs-crippled");
		}
		if (a_facts.firstPerson && a_facts.swimming) {
			add("swimming");
		}
		return out.empty() ? std::string{ "none" } : out;
	}
}
