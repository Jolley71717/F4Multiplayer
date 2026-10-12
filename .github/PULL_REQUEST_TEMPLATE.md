<!-- Thanks. Read CONTRIBUTING.md once if you have not; the short version is below. -->

## What this changes

<!-- What a player or the maintainer sees differently, and why. Link the issue if there is one. -->

## How it was tested

<!-- Tick what applies and say what you saw. "Not tested" is an acceptable answer, just say it. -->

- [ ] Unit tests pass (`xmake build -y F4MPTests`, then run `build\windows\x64\releasedbg\F4MPTests.exe`)
- [ ] New or changed tests cover the change
- [ ] Two-game test (host and joiner, say which transport and what you did):
- [ ] Not tested in a game

## Protocol

- [ ] This does not change what goes over the wire
- [ ] This changes the wire protocol (a message layout, a message type, or what a side expects) and bumps `Protocol::VERSION` in `common/Protocol.h`

## Personal data

- [ ] No personal data in the diff or this description: no Steam names or ids, Windows user names, machine names, emails, absolute paths with a user name, or pasted logs
