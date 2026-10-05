# Your own receiver, step by step

This sets up a FernSDR receiver from nothing: a small computer, a USB radio
and an antenna, and at the end a web page that you, your family or the
whole internet can listen with. It assumes you have never used Linux. Where
there is more to know, a *Technical detail* says where to read on; you can
skip every one of them.

If something here is unclear, or goes differently for you, that is worth
fixing: [tell us](#asking-for-help), in your own words.

## What you need

### A radio

The radio is a small box or stick on USB that turns radio waves into numbers
a computer can work with, an SDR (software-defined radio). FernSDR works
with these by itself, and fetches what it needs for them:

| Radio | Good for | Keep in mind |
|---|---|---|
| **RTL-SDR Blog V4** (or another RTL-SDR stick) | a first receiver: cheapest, 500 kHz to 1.7 GHz, shortwave included | shows about 2 MHz at a time, one amateur band such as 40 m |
| **SDRplay RSP** (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx) | better reception on shortwave, up to 8 MHz at a time | needs a PC (x86_64), not a Raspberry Pi, and SDRplay's own software, which the setup explains |
| **RX-888 MkII** | the whole of shortwave, 0 to 30 MHz, or 0 to 60 MHz, all at once | needs USB 3; a PC is the safe choice (0 to 30 MHz should take a little over one of an i5-6500T's four cores, by estimate), and ARM boards have not been measured with it |

The SDRplay and RX-888 support is new and has not been tried with real
radios yet; the RTL-SDR is the tested one. If yours gives trouble,
[say so](#asking-for-help).

Any other SDR with a program of its own, an Airspy, a HackRF, a LimeSDR or a
Pluto among them, works too, through a pipe: see
[A radio FernSDR has no module for](#a-radio-fernsdr-has-no-module-for).

### A computer that stays switched on

- **For an RTL-SDR:** a Raspberry Pi 4 or 5 is a good choice: small, silent
  and a few watts. A Pi 2, 3 or Zero 2 W, or any old PC, works too.
- **For an SDRplay or an RX-888:** a small PC. A used office mini PC, a
  Lenovo ThinkCentre Tiny, Dell OptiPlex Micro or HP EliteDesk Mini with an
  Intel Core i5 of the 6th generation or later (such as the i5-6500T), costs
  little, is quiet, draws 10 to 30 watts and has USB 3 ports (usually blue
  inside), which the RX-888 needs. A newer Intel N100 mini
  PC or an old laptop or desktop works too.
- **Not** a Pi 1 or the first Pi Zero: their processor is too old.

### An antenna, power and a network

- **An antenna.** The telescopic one that comes with an RTL-SDR is for VHF
  and UHF, above about 30 MHz. Shortwave wants a long wire outdoors, or a
  small active loop, which takes its power over the cable (see the bias tee
  below). Outdoors, away from the house, hears far more than indoors, where
  every charger and screen makes noise.
- **Power.** A radio takes its power from the computer's USB port. On a Pi,
  use the official power supply; for several radios or an active antenna, a
  USB hub with a power supply of its own.
- **A network connection,** by cable if you can, or Wi-Fi.

*Technical detail:* FernSDR runs on x86_64, on 64-bit ARM and on 32-bit
ARMv7 with NEON, under any Linux. The released programs bring everything
they need with them, so the distribution and its version do not matter. See
[Installing](DEPLOYMENT.md#installing).

## 1. Put Linux on the computer

Linux is the operating system, like Windows, that the computer runs. You
install it once, then control the computer from your own one over the
network.

### A Raspberry Pi

1. On your own computer, install **Raspberry Pi Imager** from
   raspberrypi.com and start it.
2. Choose your Pi, then *Raspberry Pi OS Lite (64-bit)*, then the SD card.
3. When it offers to change settings, give a user name and password, your
   Wi-Fi if the Pi is to use it, and switch **SSH** on.
4. Write the card, put it into the Pi and switch the Pi on. Give it a minute.

### A PC

1. On your own computer, download **Debian** (the "netinst" image from
   debian.org) or **Ubuntu Server** (from ubuntu.com).
2. Write it to a USB stick of 4 GB or more with **balenaEtcher** (Windows,
   Mac, Linux) or **Rufus** (Windows). Everything on the stick is erased.
3. Plug the stick into the PC, switch it on and choose to start from USB;
   the key for that is usually F12, F11 or Esc, and the screen says which.
4. Install. Everything on the PC's disk is erased. On Debian, leave the
   password for `root` empty when it asks: your own user then gets `sudo`,
   which the next steps use. When it asks about software, choose **SSH
   server** (Debian) or **Install OpenSSH server** (Ubuntu); a desktop is not
   needed. Note the user name and password.

### Log in to it from your own computer

On Windows open *Terminal* (or *PowerShell*), on a Mac *Terminal*, and type,
with your user name:

```sh
ssh yourname@raspberrypi.local
```

For a PC, use the name you gave it, or its address: your router's page
lists the devices on your network, or type `hostname -I` on the PC itself.
Say `yes` to the question about the key, and type the password (it shows
nothing while you type). You are now typing on that computer.

*Technical detail:* a 64-bit system gets the `aarch64` program, which a Pi 3,
4 or 5 should run. A Pi 2, or a 32-bit system on a newer Pi, gets `armhf`.

## 2. Install FernSDR

Logged in, paste this line and press Enter:

```sh
curl -fsSL https://github.com/Steven9101/FernSDR/releases/latest/download/install.sh | sudo sh
```

If it answers `curl: command not found`, use the same line with
`wget -qO-` in place of `curl -fsSL`.

It asks for your password once (that is `sudo`), then where the receiver
runs:

- **1, at home.** Choose this for a receiver in your house.
- **2, on a server with a domain name**, such as `radio.example.org`: type
  `2 radio.example.org`. Where the computer runs systemd, as most do, and
  its distribution has Caddy, it sets up HTTPS for you; the certificate
  arrives once the domain points at the server and ports 80 and
  443 are open.
- **3, on a server without a domain name.** It then asks whether the admin
  panel may answer plain HTTP from anywhere, at your own risk. If not, you
  reach it through an SSH tunnel (see [Open it](#3-open-it)).

On a computer where Docker already runs and FernSDR is not installed yet, it
first asks whether to run FernSDR as a service or in a Docker container. The
service is the simpler choice, updates from the admin panel and works with
every radio; a container is updated by running the install line again, and
cannot use an SDRplay. Then it downloads FernSDR, checks the download, sets it up to start
whenever the computer starts, and prints two things. **Write both down:**
the address to open and a password for the admin panel.

*Technical detail:* where it puts what, how it checks the download, and how
to answer without a keyboard, as from a script: [Installing](DEPLOYMENT.md#installing).

## 3. Open it

On any computer or phone at home, open the address it printed, such as
`http://192.168.1.20:8073`, or `http://raspberrypi.local:8073`. You see a
receiver with a test band FernSDR makes up itself, so that everything can
be tried before the radio is connected. Press *Start audio*.

The **admin panel**, where you run the receiver, is at the same address
with `/admin` on the end: `http://raspberrypi.local:8073/admin`. Sign in
with the password the installer printed. With answer 2 it is
`https://radio.example.org/admin`. With answer 3, unless you allowed plain
HTTP, connect from your own computer with
`ssh -L 8073:localhost:8073 yourname@server` and open
`http://localhost:8073/admin` while that runs.

## 4. The setup

The first time you sign in, the panel walks you through five short steps.
Each is saved as you go; you can leave and come back.

1. **Your password.** Keep the one the installer made up, or choose your
   own. It never crosses the network; the receiver only sees proof that you
   know it.
2. **Your station.** A name for the receiver, your callsign if you have one,
   the town, and its place on a map. The place decides the band plan for
   your region and when sunrise and sunset are.
3. **Your radio.** Plug the radio in now. The panel finds it, says what it
   is, and with one button fetches the small program, the *module*, that
   runs it. For an SDRplay it then shows how to install SDRplay's own
   software, which SDRplay does not let anyone pass on.
4. **What to listen to.** The panel offers the bands your radio can show:
   amateur bands, broadcast, aviation and more. Pick one, or two: one radio
   shows one band at a time, so two take turns, the lower one at night,
   when it carries far, and the higher one by day. The receiver restarts to
   start them, which takes a few seconds.
5. **Who can listen.** *Only at home*, or *Everyone on the internet*, with
   the address people will use and a switch to show the receiver in the
   public list at sdr-list.xyz.

The *Overview* page then shows each band and whether it is receiving, and
anyone who opens the address hears it.

**Moving from another FernSDR?** In the first step, choose *Restore its
backup* instead, with the backup file from the old receiver's *Updates* page:
see [Moving to a new computer](#moving-to-a-new-computer).

*Technical detail:* everything the setup does can also be done by hand, in
the configuration file or on the other pages; every setting of a band:
[Feeding it](DEPLOYMENT.md#feeding-it).

## 5. Make it sound right

Most of this is on the band's page: *Bands*, then the band.

- **Gain** is how much the radio amplifies. Left at *auto*, the module sets
  it itself: as high as it can go without overdriving the radio, which would
  spoil the whole band for everyone. The band's page shows the gain it chose
  and whether anything is overdriven, which it calls clipping.
- **The spike in the middle** of the waterfall is the RTL-SDR's own.
  *Remove the centre spike* takes it out.
- **The frequency.** A radio's crystal, the part that keeps time, can be off
  by a little: about one part in a million on an RTL-SDR Blog V3 or V4, 7 Hz
  at 7 MHz. *Measure against a known carrier* corrects it with a station
  whose frequency is known exactly, such as the time signal WWV on 10 MHz,
  where it can be heard. The correction applies once the receiver restarts.
- **The bias tee** sends power up the antenna cable for antennas and
  amplifiers that need it, such as active loops. Leave it off for any other
  antenna. The switch among the module's settings on the band's page turns
  it on until the band restarts; to keep it on, add `module.bias_tee = yes`
  to the band on the *Configuration* page.

*Technical detail:* how the gain control decides, and the other gain
choices: [RTL dongle](DEPLOYMENT.md#rtl-dongle).

## 6. Let others listen

At home, others open the same address as you.

From the internet there are three ways in:

1. **Your router forwards port 8073** to the receiver. Routers call this port
   forwarding or port sharing; your router's manual says where. Listeners
   then open your internet address with `:8073` on the end. The admin panel
   stays reachable from your home network only.
2. **A tunnel** such as cloudflared gives the receiver an `https://` address
   without opening anything on the router: [Tunnels](DEPLOYMENT.md#tunnels).
3. **A server on the internet** with a domain name: install there and answer
   `2` with the domain. The installer sets up HTTPS where it can, and
   otherwise prints what to do.

To be found, switch on *List on sdr-list.xyz* on the *Station* page, or in
the last step of the setup. It needs the receiver's place on the map and
the address listeners use.

*Technical detail:* why the admin panel takes plain HTTP only from home, and
what HTTPS changes: [HTTPS](DEPLOYMENT.md#https).

## 7. Keep it up to date

When a newer FernSDR is out, the admin panel says so after you sign in, with
what it changes, and *Update now* installs it; the *Updates* page does the
same at any time. Listeners drop out for a few seconds. If the new version
does not work within five minutes, the one before comes back by itself.
Nothing updates without you pressing the button. Running the install line
again and answering `1` does the same, and is how a receiver in a Docker
container is updated.

The same page has *Start with the computer*. On, FernSDR starts by itself
whenever the computer does, after a power cut too. Off, it keeps running
now, but after a restart stays off until you start it.

*Technical detail:* what an update does, step by step: [Updates](DEPLOYMENT.md#updates).

### Moving to a new computer

On the old one, *Download* on the *Updates* page saves a backup file. Install
FernSDR on the new computer, sign in, and in the first step of the setup
choose *Restore its backup*. The new receiver gets your bands, station
details, look and pictures, fetches the modules the old one used and
restarts. Your password on the new computer stays the one it was installed
with. Keep the backup file somewhere safe; it is also a way back after a bad
day.

*Technical detail:* what is in the file and what stays with the machine:
[Moving to another machine](DEPLOYMENT.md#moving-to-another-machine).

## 8. More, when you want it

- **FT8 decoding.** On *Decoders*, one button installs the FT8 decoder and
  turns it on for the FT8 frequencies your bands cover. Listeners then see
  what it heard, on a list and a map, and another switch reports it to PSK
  Reporter, the map of who hears whom.
- **Hours on the air.** On a band's page, *On the air* sets when it
  receives: always, in daylight, at night, or at times you choose.
- **The page's look.** *Appearance* sets the colours and a background
  picture; *Widgets* adds a clock, space weather and band conditions, maps,
  links and notices beside the waterfall.

## A radio FernSDR has no module for

FernSDR can read samples from any program that writes them out: most SDRs
have one, such as `airspy_rx`, `hackrf_transfer` or `rx_sdr` for SoapySDR
devices. The command and the band's settings for the common ones are in
[Radios without a module](DEPLOYMENT.md#radios-without-a-module). If you
would rather have it work like the RTL-SDR, with the setup finding it,
[ask for a module](#asking-for-help): that is how the next one gets chosen.

## When something is wrong

- A band that does not receive says why on the *Overview*: *No RTL-SDR is
  plugged in*, *In use by another program*, and so on, with what to do.
- The *Log* page shows what the receiver has been doing; on the computer,
  `journalctl -u fernsdr -f` does too.
- **Lost the admin password?** Log in to the computer, run the install line
  again and answer `2`: it makes a new one and prints it.
- [Troubleshooting](DEPLOYMENT.md#troubleshooting) goes through the usual
  problems.

## Asking for help

Open an issue on GitHub:
[github.com/Steven9101/FernSDR/issues](https://github.com/Steven9101/FernSDR/issues/new/choose).
You need a free GitHub account, and nothing else: no programming, no
knowledge of how FernSDR works inside. Say what you did, what you expected
and what happened, in your own words; a screenshot or the lines from the
*Log* page help. The same goes for wishes: a radio that is not supported, a
decoder you would like, something that gets in your way. Questions from
people who are just starting are welcome; they show where this guide needs
to be better.

A security problem goes to the
[private report form](https://github.com/Steven9101/FernSDR/security/advisories/new)
instead, not into a public issue.

## On a Raspberry Pi

- **Which one.** A Pi 4 or 5 is much faster than a Pi 2, 3 or Zero 2 W, and
  the one to choose for more than one band. How many listeners each
  carries has not been measured yet; the load test in
  [PERFORMANCE.md](PERFORMANCE.md#size-a-receiver) measures it on yours.
- **The internet connection** is often the limit before the Pi is: every
  listener gets about 85 kbit/s of audio and waterfall, so 20 listeners need
  1.7 Mbit/s of upload.
- **Power.** The official power supply, and for several radios or an active
  antenna on the bias tee, a USB hub with a power supply of its own.
- **The SD card.** FernSDR writes little to it, unless the waterfall archive
  is switched on, which it is not by default.
- **Heat.** Under load a Pi 4 or 5 wants a heatsink or the official case with
  a fan.
