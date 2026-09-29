# Your own receiver, step by step

This sets up a FernSDR receiver from nothing: a small computer, a USB radio
and an antenna, and at the end a web page that you, your family or the
whole internet can listen with. It assumes no Linux experience. Where there
is more to know, a *Technical detail* says where to read on.

## What you need

- **A computer that runs Linux and stays switched on.** A Raspberry Pi 4 or 5
  is a good choice: small, silent and a few watts. A Pi 2, a Pi 3, a Pi Zero
  2 W or any old PC works too. Not a Pi 1 or the first Pi Zero: their
  processor is too old for FernSDR's released programs.
- **An RTL-SDR dongle,** a USB stick that turns radio waves into numbers a
  computer can work with. The RTL-SDR Blog V4 receives from 500 kHz to about
  1.7 GHz, the shortwave bands included.
- **An antenna.** The telescopic one that comes with the dongle is for VHF
  and UHF, above about 30 MHz. Shortwave wants a long wire outdoors, or a
  small active loop, which takes its power over the cable (see the bias tee
  below).
- **A good power supply.** The dongle takes its power from the computer's USB
  port. On a Pi, use the official power supply.
- **A network connection,** by cable or Wi-Fi.

*Technical detail:* FernSDR runs on x86_64, on 64-bit ARM and on 32-bit
ARMv7 with NEON, under any Linux. The released programs bring everything
they need with them, so the distribution and its version do not matter; the
installer sets it up with systemd, OpenRC, runit or SysV init, whichever the
machine uses. See [Installing](DEPLOYMENT.md#installing).

## 1. Put Linux on the computer

On a Raspberry Pi, use Raspberry Pi Imager on another computer. Choose
*Raspberry Pi OS Lite (64-bit)* and your SD card, and in the imager's
settings give a user name and password, your Wi-Fi if the Pi is to use it,
and switch SSH on. Put the card into the Pi and switch it on. A minute later
you can log in to it from the other computer:

```sh
ssh yourname@raspberrypi.local
```

On a PC, install Debian or Ubuntu; any recent one works.

*Technical detail:* a 64-bit system gets the `aarch64` program, which a Pi 3,
4 or 5 should run. A Pi 2, or a 32-bit system on a newer Pi, gets `armhf`.

## 2. Install FernSDR

Logged in, paste this line and press Enter:

```sh
curl -fsSL https://github.com/Steven9101/FernSDR/releases/latest/download/install.sh | sudo sh
```

It asks one question: is the receiver at home, or on a server on the
internet? For a receiver at home, answer `1`. It then downloads FernSDR,
checks the download, sets it up to start by itself whenever the computer
starts, and prints two things to write down: the address to open and a
password for the admin panel.

*Technical detail:* where it puts what, how it checks the download, and how
to answer without a keyboard, as from a script: [Installing](DEPLOYMENT.md#installing).

## 3. Open it

On any computer or phone at home, open the address it printed, such as
`http://192.168.1.20:8073`, or `http://raspberrypi.local:8073`. You see a
receiver with a band FernSDR makes up itself, a test signal, so that
everything can be tried before the radio is connected. Press *Start audio*.

The admin panel is at the same address with `/admin` on the end, and takes
the password the installer printed. Lost it? On the computer:

```sh
sudo cat /opt/fernsdr/admin-password
```

## 4. Connect the radio

Plug the dongle in, and in the admin panel:

1. Open *Modules*, press *Check for updates* and install `rtlsdr`. A module
   is a small program of its own that talks to one kind of hardware;
   FernSDR itself talks to none, which keeps it simple and safe.
2. Press *Find devices*. The dongle appears with its serial number.

*Technical detail:* the installer already let the receiver use USB devices,
and kept Linux's television driver away from the dongle. How modules work
and how to write one: [MODULES.md](MODULES.md).

## 5. Tell it what to listen to

A band is a slice of the radio spectrum that the receiver watches all at
once. The dongle watches about 2 MHz at a time: enough for a whole amateur
band such as 40 m, which runs from 7.0 to 7.2 MHz.

Open *Configuration* in the admin panel. Replace the made-up band, the line
`[band:demo]` and everything under it down to the first empty line, with:

```ini
[band:40m]
name        = 40 m
source      = module
module      = rtlsdr
sample_rate = 2400k
center      = 7.1M
```

Save, then restart the receiver on the computer:

```sh
sudo systemctl restart fernsdr
```

`center` is the middle of the band, and `sample_rate` how wide it is:
2.4 million samples a second cover 2.4 MHz, of which the middle 1.9 MHz are
clean enough to listen to. Some other bands, with the same five lines
otherwise:

| band | center |
|---|---|
| 80 m | `3.65M` |
| 20 m | `14.175M` |
| 10 m, the CW and SSB part | `28.5M` |
| 2 m | `145M` |

One dongle gives one band. A second dongle gives a second band: give each
band `module.device = serial:...` with the serial *Find devices* showed.

*Technical detail:* the dongle runs at 225 to 300 kHz or 900 kHz to
3.2 MHz; above 2.4 MHz many computers lose samples without saying so. Every
setting of a module band: [RTL dongle](DEPLOYMENT.md#rtl-dongle).

## 6. Make it sound right

- **Gain** is how much the radio amplifies. Left at the default, `auto`, the
  module sets it itself: as high as it can go without overdriving the
  dongle's converter, which would spoil the whole band for everyone. The
  band's page in the admin panel shows the gain it chose and whether
  anything is overdriven, which it calls clipping.
- **The spike in the middle** of the waterfall is the dongle's own. Add
  `dc_remove = yes` to the band to take it out.
- **The frequency.** The module tunes the dongle to within a few hertz, but
  its crystal, the part that keeps time, can be off by a little: by about one
  part in a million on the RTL-SDR Blog V3 and V4, 7 Hz at 7 MHz. To correct
  it, open the band's page and use *Measure against a known carrier* with a
  station whose frequency is known exactly, such as the time signal WWV on
  10 MHz, where it can be heard.
- **The bias tee**, `module.bias_tee = yes`, sends 4.5 V up the antenna cable
  for antennas and amplifiers that need it, such as active loops. Leave it
  off for any other antenna.

*Technical detail:* how the gain control decides, and the other gain
choices: [RTL dongle](DEPLOYMENT.md#rtl-dongle).

## 7. Let others listen

At home, others open the same address as you.

From the internet there are three ways in:

1. **Your router forwards port 8073** to the receiver. Routers call this port
   forwarding or port sharing. Listeners then open your internet address
   with `:8073` on the end. The admin panel stays reachable from your home
   network only.
2. **A tunnel** such as cloudflared gives the receiver an `https://` address
   without opening anything on the router: [Tunnels](DEPLOYMENT.md#tunnels).
3. **A server on the internet** with a domain name: install there, answer
   `2` with the domain, and put Caddy in front, as the installer explains.

To be found, switch on *List on sdr-list.xyz* on the *Station* page. It needs
the receiver's grid square and the address listeners use.

*Technical detail:* why the admin panel takes plain HTTP only from home, and
what HTTPS changes: [HTTPS](DEPLOYMENT.md#https).

## 8. Keep it up to date

On the *Updates* page, *Check for updates* shows whether a newer FernSDR is
out and what it changes, and *Update* installs it. Listeners drop out for a
few seconds. If the new version does not work within five minutes, the one
before comes back by itself. Running the install line again does the same.

*Technical detail:* what an update does, step by step: [Updates](DEPLOYMENT.md#updates).

## When something is wrong

- A band that does not receive says why on the *Overview*: *No RTL-SDR is
  plugged in*, *In use by another program*, and so on, with what to do.
- The *Log* page shows what the receiver has been doing; on the computer,
  `journalctl -u fernsdr -f` does too.
- [Troubleshooting](DEPLOYMENT.md#troubleshooting) goes through the usual
  problems.

## On a Raspberry Pi

- **Which one.** A Pi 4 or 5 is much faster than a Pi 2, 3 or Zero 2 W, and
  the one to choose for more than one band. How many listeners each
  carries has not been measured yet; the load test in
  [PERFORMANCE.md](PERFORMANCE.md#size-a-receiver) measures it on yours.
- **The internet connection** is often the limit before the Pi is: every
  listener gets about 85 kbit/s of audio and waterfall, so 20 listeners need
  1.7 Mbit/s of upload.
- **Power.** The official power supply, and for several dongles or an active
  antenna on the bias tee, a USB hub with a power supply of its own.
- **The SD card.** FernSDR writes little to it, unless the waterfall archive
  is switched on, which it is not by default.
- **Heat.** Under load a Pi 4 or 5 wants a heatsink or the official case with
  a fan.
