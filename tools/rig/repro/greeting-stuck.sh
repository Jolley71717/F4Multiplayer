#!/bin/bash
# greeting-stuck.sh: a conversation an NPC starts with our player (the 0.23.0-rc1 gate: Codsworth, made Sanctuary's settler by the
# host's console, greeted the VM's player as they walked past, and the VM's dialogue menu stayed up for good: Tab and Esc did not close
# it, so the rig's state guard failed every step after the doors).
#
# Both games joined in Sanctuary (rig.sh join SanctuaryExt). The VM's game is made to run Codsworth (the host leaves for another cell,
# the VM loads Sanctuary alone, the host comes back), the host's console makes him Sanctuary's settler and provisioner as the gate's bench
# step does, and the VM's player walks to him with the pad (real movement). He starts a conversation by himself; nobody presses E.
# After WAIT s the player closes the menu as a player would: Tab, then Esc (Esc opens the pause menu: Esc again).
# REPRO: YES when the VM's dialogue menu is still open WAIT s after it opened and nothing closes it: not Tab, not Esc, not the dev
# channel's `dialogue reset` (the plugin's forced ending); NO when it ended by itself, closed with the keys, or `dialogue reset` closed it
# (Codsworth's greeting waits for a choice from the dialogue wheel, as in 0.22.2; the rig does not choose). VOID when no conversation
# started, or the VM did not run Codsworth.
# The end step closes whatever is left: `dialogue reset`, else the cell is loaded again.
# WAIT=60 bash tools/rig/repro/greeting-stuck.sh
cd "$(dirname "$0")" || exit 2
. ./lib.sh
CODS=0001CA7D; BENCH=000250FE; LINK_BENCH=0006F5C5; LINK_LOCATION=0006B4D0
WAIT=${WAIT:-60}
need_session
rmark host a; rmark vm a
dc host "log level debug" >/dev/null; dc vm "log level debug" >/dev/null
log_back() { dc host "log level info" >/dev/null; dc vm "log level info" >/dev/null; }
on_end log_back
close_talks() {
	menu_open vm DialogueMenu || return 0
	local r n; for n in 1 2 3; do r=$(dc vm "dialogue reset"); sleep 3; menu_open vm DialogueMenu || break; done
	sleep 2
	if menu_open vm DialogueMenu; then
		say "end step: the VM's dialogue menu is still up after 'dialogue reset' ($(cut -c1-120 <<<"$r")): the cell is loaded again (console coc)"
		dc vm "console coc SanctuaryExt" >/dev/null; sleep 3; wait_ingame vm 60
	else say "end step: the VM's dialogue menu closed by 'dialogue reset'"; fi
}
on_end close_talks
vm_id() { field "$(dc vm 'net status')" id; }

say "set-up (console): the host leaves for Red Rocket, the VM loads Sanctuary alone and runs Sanctuary's NPCs, the host comes back"
dc host "console coc RedRocketExt" >/dev/null; sleep 12; wait_ingame host 60
dc vm "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame vm 60; sleep 4
dc host "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame host 60; sleep 5
me=$(vm_id); ow=$(field "$(dc vm "refinfo $CODS")" owner)
say "Codsworth: the VM is player $me and sees his owner as player $ow"
[ "$ow" = "$me" ] || verdict VOID "the VM's game does not run Codsworth (owner $ow, VM is player $me)"
say "set-up (console, the host's game): Sanctuary and Abernathy Farm the host's, Codsworth Sanctuary's settler and provisioner (as the gate's bench step)"
dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; dc host "console $LINK_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
dc host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $CODS $BENCH" >/dev/null; sleep 2
dc host "console cqf WorkshopParent \"WorkshopParentScript.AssignCaravanActorPUBLIC\" $CODS $LINK_LOCATION" >/dev/null; sleep 2
say "host: $(dc host "scriptvars $CODS WorkshopNPCScript workshopID")"
menu_open vm DialogueMenu && verdict VOID "the VM's dialogue menu is open before the run"

rmark vm b
say "the VM's player walks to Codsworth (the pad); he greets them on the way"
rwalk vm "$CODS" 100 || say "the walk ended short ($WALKED)"
dc vm "pad unplug" >/dev/null
opened() { menu_open vm DialogueMenu; }
scene_live "the VM's dialogue menu opened" 25 opened
T0=$SECONDS
say "VM: talkingTo=$(field "$(dc vm 'net status')" talkingTo); $(dc vm dialogue | cut -c1-260)"
while [ $((SECONDS - T0)) -lt "$WAIT" ]; do
	opened || break
	sleep 5
done
if ! opened; then
	say "the conversation ended after $((SECONDS - T0)) s"
	rsince vm b | grep -E "NpcSync: (talking to|not talking to|our dialogue menu|the conversation with)|Dialogue:" | cut -c1-220 | tee -a "$REPRO_OUT/run.log"
	rkeep vm after
	verdict NO "the greeting ended after $((SECONDS - T0)) s ($(menus vm | cut -c1-60))"
fi
say "still open after $WAIT s: $(dc vm dialogue | cut -c1-260)"
rkey vm "$VK_TAB" 100; sleep 2; say "after Tab: $(menus vm | cut -c1-80)"
if opened; then rkey vm 0x1B 100; sleep 2; say "after Esc: $(menus vm | cut -c1-80)"; menu_open vm PauseMenu && { rkey vm 0x1B 100; sleep 2; }; fi
rsince vm b | grep -E "NpcSync: (talking to|not talking to|our dialogue menu|the conversation with)|Dialogue:" | cut -c1-220 | tee -a "$REPRO_OUT/run.log"
rkeep vm after
if opened; then
	say "Tab and Esc did not close it: the dev channel's dialogue reset (the plugin's forced ending, what the rig's state guard uses) is run"
	for n in 1 2 3; do r=$(dc vm "dialogue reset"); sleep 3; menu_open vm PauseMenu && { rkey vm 0x1B 100; sleep 1; }; opened || break; done   # the first one also stops the scene (a Papyrus call, a moment later): the scene may open the menu again once
	say "dialogue reset ($n times): $(cut -c1-200 <<<"$r"); menus now: $(menus vm | cut -c1-80)"
	opened && verdict YES "the VM's dialogue menu is still open $WAIT s after the greeting began, and neither Tab, Esc nor dialogue reset closed it"
	verdict NO "the greeting waits for a choice the rig does not make (as in 0.22.2: the dialogue wheel, a vanilla scene), Tab and Esc did not close it, dialogue reset did"
fi
verdict NO "the greeting closed with the keys ($(menus vm | cut -c1-60))"
