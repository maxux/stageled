# Stage LED

Stage LED is a project for stage lighting. Initial and current project is controling
`24x 2 meters` LED bar. Each bar of LED is a stripe of 120x `WS2811` LED (12v).

This make a total of `2,880 LEDs`. To control all of this, a `Teensy 4.1` control
`3 lanes` of `960 LEDs` at more than `30 FPS`. Teensy receives frame from a client over `Ethernet (UDP)`.

# Controller

Controller is a `Teensy 4.1`  with Ethernet.

# Client Control

Remote control is done via a custom console software (under Linux) with a `MIDI Surface` (last version
uses a `AKAI APC mini mk2`.

# Physical Segments

They are made of aluminium bars, painted in black, with LED sticked on it. Bar's ends are covered
with 3D printed corners and connectors. Data signal is transmitted via a `XLR` cable and connector
(like DMX but it's not DMX over the cable, it's plain WS2812 protocol). To manage power, PowerCON
is used.

# More soon
