## 0.8.4

- Saving right after a friend appeared no longer writes their stand-in, light or power armor frame copy into the save: everything still waiting to be deleted is deleted before the save.
- The host answers a player's requests for the whole session state from a bucket of three (a join, a reload and one more), refilled one every ten seconds, instead of up to four every second. A client asking without pause could make the host upload its whole session several times a second.
- The compass posts nothing to the UI thread when there is nobody to show and no marker showing (main menu, loading, playing alone).
- Protocol 26, as 0.8.0 to 0.8.3. Newer builds are on GitHub first; Nexus follows less often.
