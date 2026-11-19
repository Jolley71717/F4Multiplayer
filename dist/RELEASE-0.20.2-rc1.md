## 0.20.2-rc1

A test build of 0.20.2. It uses the same protocol as 0.20.0 and 0.20.1 (84), so it can play with them. Install it over 0.20.1.

Fixed:

- **A friend can open the workbench at the host's settlements.** With shared building on, a friend pressing E at the workbench of a settlement the host owns (but the friend's own game doesn't) got "You need to clear the enemies from the location" (or "not allied") and nothing opened. It now opens workshop mode there, and everything the friend builds is the host's, as at any of the host's settlements. The workshop key works there too. The friend's own game claims nothing: the settlement stays unowned in their save.
