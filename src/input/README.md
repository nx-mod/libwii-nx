# input

The Wii Remote: buttons, motion, its pointer, and the extensions that plug into
it. A game reads its own SDK's structures, so a Switch controller arrives as a
remote rather than as something the game can notice.

`wpad.cpp` is the raw interface, `kpad.cpp` the one most games use, and
`wii_remote_input.cpp` turns what the host reports into both.

A Switch pad has no IR camera, so the pointer is emulated: the right stick
moves it, and in handheld a touch on the screen puts it there and holds A (a
tap on a button presses it). It appears at first use and goes off the screen
after 15 s unused, so a game that reads only buttons still sees a remote
pointed away. KPAD gets the position, its motion, and both sensor-bar marks;
the raw WPAD IR objects stay invalid.
