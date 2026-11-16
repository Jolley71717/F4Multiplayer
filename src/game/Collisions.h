#pragma once

#include "game/HavokNan.h"

#include <cstddef>
#include <type_traits>
#include <vector>

// The physics bodies under a reference's 3D, walked as MovedObjects and RackDisplays walk them.
namespace Collisions
{
	// Every collision object under a_root (a walk of at most HavokNan::NODE_LIMIT nodes), depth first. a_each may return
	// false to stop the walk. a_stack is the walk's own (emptied first): a caller that walks every frame keeps one, so the
	// walk allocates nothing once it has grown.
	template <class F>
	void ForEach(RE::NiAVObject* a_root, F&& a_each, std::vector<RE::NiAVObject*>& a_stack)
	{
		a_stack.clear();
		if (a_root) {
			a_stack.push_back(a_root);
		}
		std::size_t walked = 0;
		while (!a_stack.empty() && walked++ < HavokNan::NODE_LIMIT) {
			const auto node = a_stack.back();
			a_stack.pop_back();
			if (const auto object = node->collisionObject.get()) {
				if (const auto collision = object->IsbhkNPCollisionObject()) {
					if constexpr (std::is_void_v<std::invoke_result_t<F&, RE::bhkNPCollisionObject*>>) {
						a_each(collision);
					} else if (!a_each(collision)) {
						return;
					}
				}
			}
			if (const auto parent = node->IsNode()) {
				for (const auto& child : parent->children) {
					if (child.get()) {
						a_stack.push_back(child.get());
					}
				}
			}
		}
	}

	template <class F>
	void ForEach(RE::NiAVObject* a_root, F&& a_each)
	{
		std::vector<RE::NiAVObject*> stack;
		ForEach(a_root, a_each, stack);
	}
}
