## 0.7.6

The host believes less of what a joined game says, so one broken or hostile client can do less to everyone else (docs/AUDIT-LIFETIME.md, network trust):

- A death report is taken only from the player whose game runs the NPC (or from anyone when nobody does).
- A hit on a player is capped at 500 and at 1,500 a second per reporter.
- A face change reaches the others at most every five seconds; a newcomer always gets the latest.
- An explosion is relayed only within ten seconds of the reporter's own throw and near where they are.
- One player's game may run at most 4,096 NPCs and claim at most four companions.
- Protocol unchanged: 0.7.0 to 0.7.6 play together; the limits apply on the host.
