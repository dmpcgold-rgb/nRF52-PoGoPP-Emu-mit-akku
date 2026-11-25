# Pokémon GO Plus+ on the XIAO 1.47" touch display

![The XIAO 1.47" display running the firmware](images/device.jpg)

This is firmware that turns Seeed's
[XIAO 1.47" touch display](https://www.seeedstudio.com/1-47-Inch-Touch-Display-Powered-by-XIAO-nRF52840-Plus-p-6995.html)
into a stand-in for a Pokémon GO Plus+ that you own. It catches and spins by
itself, and the screen shows what the app just reported and counts what
you've caught, what fled and the stops you've spun since it was switched on.

It's built on kiyoukan's
[nRF52 Plus+ backup firmware](https://github.com/kiyoukan/nRF52-go-plus-plus-backup-),
which does the hard part, the pairing and reconnect protocol. I added the
catch and spin handling from [pgpemu](https://github.com/shortcuts/pgpemu),
the screen, the touch and the two buttons.

There is no device material in this repo. The firmware only works with the
Bluetooth address and the keys of a real Plus+, and you have to read those
from a device you own and put them in yourself. It won't build without them.

## Disclaimer

This is a hobby project and there are probably rough edges. It's meant for
backing up a Plus+ you own, nothing else. I'm not affiliated with Nintendo,
Niantic, The Pokémon Company or Seeed. A device that plays for you may be
against the game's terms of service, and if your account gets in trouble over
it, that is on you. Getting the keys means opening your Plus+ and reading a
1.8 V flash chip, and if that kills it, that is on you too. I've only tested
this on my own board, with the material from my own Plus+.

The firmware and the documentation here were written with help from AI. If
something in the text doesn't match the code, trust the code and let me know.

## What you need

* The [XIAO 1.47" touch display](https://www.seeedstudio.com/1-47-Inch-Touch-Display-Powered-by-XIAO-nRF52840-Plus-p-6995.html)
(the nRF52840 Plus version)
* A LiPo if you want to carry it around. Mine is a 3.7 V 700 mAh cell.
* The [enclosure on Printables](https://www.printables.com/model/1843008-enclosure-for-xiao-147-ips-touch-display-esp32nrf5)
by *add the designer's name here*, if you want a case. It's the one in the
photo.
* Your own Plus+, and your device material (next section)
* `arduino-cli` with the Seeed nRF52 core (`Seeeduino:nrf52`, I use 1.1.13)

## Your device material

Follow kiyoukan's README to read the keys from your Plus+'s flash chip and
turn them into a header with their `flash\_image\_reader`. Then fill in your
Plus+'s public Bluetooth address (little-endian, so `AA:BB:CC:DD:EE:FF`
becomes `{ 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA }`), set
`PP\_DEVICE\_MATERIAL\_CONFIGURED` to `1` and save it as
`PlusPlusXiao/pp\_device\_material.h`. `pp\_device\_material.template.h` shows
what goes in it.

Make sure the address is really yours. Every Plus+ advertises the same
name, and with the wrong address pairing fails partway.

## Building and flashing

```bash
./build.sh
```

That gives you `build/PlusPlusXiao.uf2`. Double-tap the reset button so the
board shows up as a drive and copy the file onto it. That replaces Seeed's
demo firmware. I flashed mine over USB serial instead, so I haven't tried the
drag-and-drop on this board myself.

Button B sits on one of the nRF52840's NFC pins. On the first boot the
firmware switches those pins to normal GPIO, which takes one extra reset.

## Using it

**Never have your real Plus+ and this board switched on at the same time.**
They share one address, and that confuses the phone.

Pair it in Pokémon GO under Settings > Connected Devices and Services, the
same way as a real Plus+.

The board has a little slide switch for the battery. It has to be in the
**down** position, or the board goes dark as soon as you unplug USB.

|Control|What it does|
|-|-|
|A, click|a manual catch or spin press (when connected)|
|A, double-tap|locks or unlocks B and the touch screen|
|A, hold 5 s|wipes the pairing (not while locked)|
|B, click|next mode: AUTO (catch + spin), CATCH, SPIN, MANUAL|
|B, hold 1.5 s|screen brightness low / mid / high|
|Tap the mode card|same as B|
|Tap the top banner|dismisses it, or searches for the phone when not connected|

Double-tap A to lock the controls, so nothing gets changed by accident in a
pocket. While it's locked, B and the touch screen only show LOCKED, and
holding A won't wipe the pairing. A still does its catch and spin presses.
The padlock next to the B on the mode card shows which way it is: orange
and shut when it's locked. Since A waits a moment to see if a second tap is
coming, a single click goes out about a third of a second after you let go.

The screen shows the link and the battery at the top, then the last thing
the app reported, the mode, and counters for what you've caught, what fled
and the stops you've spun since it was switched on. The last row, LINKED,
shows how long the phone has been connected this time.

When a stop won't spin, say it's still on cool-down, the game answers with a
quick red flash. The screen shows NO SPIN for that. BOX FULL only comes up
after a Pokémon.

The screen goes dark after 30 seconds by default to save battery. In the USB console (115200 baud), press `t` to cycle through 15, 30, 60 and 120 seconds. The selected timeout is saved. A tap or a button
wakes it. The buttons work with it dark. On battery it goes to sleep after
15 minutes without a connection, and A or B wakes it. All the settings are
saved, and so is the lock.

## Ball selection

The ball type is selected in Pokémon GO, not on the device. For Great Balls or Ultra Balls, disable the game's auto-throw setting, select the desired ball in the accessory settings and use the appropriate button-press mode. Actual behavior depends on the game version and must be verified on hardware. No ball-type information is inferred from the LED prompts.

## Battery life

I haven't measured it yet, so these are my estimates from the datasheets and
from how the firmware runs, next to my
[ESP32 version](https://github.com/Jackv-makes/ESP32-PoGoPP-Emu) on the
M5Stack Stick S3. To keep it fair, both get a 250 mAh battery, the size the
Stick has built in.

||This board|ESP32 version on the Stick S3|
|-|-|-|
|Battery|250 mAh|250 mAh, built in|
|Screen when idle|off after 30 seconds|off after a minute|
|Between Bluetooth events|the chip sleeps|the chip stays awake, at 80 MHz|
|Draw while connected|about 2 to 4 mA with the screen off, 12 to 20 mA more with it on|about 20 to 35 mA with the screen off|
|Time playing|about 17 to 34 hours if you glance at it now and then, about 10 to 18 hours with the screen always on|about 7 to 12 hours|

The nRF52840 is made for small battery gadgets, and its Bluetooth stack
sleeps between events on its own. The ESP32 version stays awake between
events, even at its slower clock, so on the same battery it runs flat a lot
sooner. This board goes to sleep after 15 minutes without a connection and the
ESP32 version after 5, so neither runs flat sitting in a drawer.

## What works

Working on my board:

* Pairing with Pokémon GO, auto catch and auto spin
* The screen, the counters and charging from USB

## Keeping your keys private

If you fork this or share a build, leave out `pp\_device\_material.h`, your
flash images, everything in `build/` and any `.uf2` or `.hex` (they contain
your keys), and serial logs (they show your address). The `.gitignore` covers
the files.

## License

GNU AGPL v3.0, the same as kiyoukan's firmware that this builds on. See
[LICENSE](LICENSE).

## Credits

* [kiyoukan](https://github.com/kiyoukan/nRF52-go-plus-plus-backup-) for
working out the Plus+ pairing and reconnect protocol and publishing the
nRF52 firmware this runs on.
* [shortcuts](https://github.com/shortcuts/pgpemu) for pgpemu, and yohanes,
spezifisch and paper183 for the pgpemu work it builds on. The LED pattern
reading and the button press timing come from there.
* [Seeed Studio](https://www.seeedstudio.com/) for the board, and their
display library, where the screen's start-up settings come from.
* *the designer's name* for the
[enclosure](https://www.printables.com/model/1843008-enclosure-for-xiao-147-ips-touch-display-esp32nrf5).

