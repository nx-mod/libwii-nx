# input

The Wii Remote: buttons, motion, its pointer, and the extensions that plug into
it. A game reads its own SDK's structures, so a Switch controller arrives as a
remote rather than as something the game can notice.

`wpad.cpp` is the raw interface, `kpad.cpp` the one most games use, and
`wii_remote_input.cpp` turns what the host reports into both.
