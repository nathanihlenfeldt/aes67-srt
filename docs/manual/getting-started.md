# Getting started

Two ends to install: the **appliance** at the production site, and the **endpoint** in the studio.
Neither takes long; the appliance's installer does the heavy lifting.

## 1. The appliance (at the site)

On a Raspberry Pi 5 running 64-bit Raspberry Pi OS, with wired Ethernet and the AES67 network
connected:

```sh
curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-srt/main/scripts/install.sh \
  | sudo bash
```

This one command, run once, does everything and is safe to re-run:

- installs the build dependencies, the **RAVENNA kernel module** and **`aes67-daemon`** (20–40 minutes
  the first time — the kernel module builds for your kernel);
- builds and installs this appliance, its **systemd service**, and a **preflight report**;
- sets the CPU governor, the real-time limits, and disables PulseAudio (it disturbs the audio device).

At the end it prints a **preflight report**. That report is the thing to read:

```
==> preflight report
  configuration          ok: /etc/aes67-srt.conf (8 blocks, 64 channels, role duplex)
  daemon                 active
  ptp                    { "status": "locked", "gmid": "...", "jitter": 10 }
  device                 plughw:RAVENNA present
  service                active / enabled
```

**PTP locked** and **device present** are the two that matter. If PTP is not locked there will be no
audio, however healthy everything else looks.

The appliance is now **running**, and comes back on every reboot. It serves a web page at
`http://<its address>:8082/`. Open it and check the **Preflight** section is green.

## 2. The studio Mac

Build the endpoint, then run the installer:

```sh
cmake -S . -B build && cmake --build build --target aes67-srt-mac
./scripts/install-mac.sh --peer <appliance address>:9000 --passphrase <the shared secret> --role rx
```

The installer (no `sudo`) copies the endpoint and its menu-bar app into place, writes the configuration
with your appliance's address and the shared passphrase, and starts both at login.

**One step needs your password: BlackHole.** It is a CoreAudio driver, and the installer cannot place
it for you:

1. Download **BlackHole 64ch** from <https://existential.audio/blackhole/> and install it.
2. Run `sudo killall coreaudiod` so the device appears.

Check the menu bar: an **● aes67** icon means the endpoint is running (see
[Operating it](operating.md)). Open its menu and **Open control page**.

## 3. First audio

1. **Check the link.** The appliance's page and the Mac's page both show a **link_open** state and, once
   connected, a rate and delay. The studio is the **caller** and the site the **listener** — the Mac
   dials the appliance. Once connected, both show a rate of a few Mbit/s (Opus) and 0 refused.
2. **Confirm the site is publishing audio.** On the appliance's page, the **AES67 daemon** card lists
   the sources and their state. The site's console should be sending AES67 to the appliance.
3. **Record or monitor.** In your DAW, choose **BlackHole 64ch** as the input and record. **Stream
   channel 1 is DAW input 1**; the first eight channels are the first block.
4. **Listen.** BlackHole is a device, not a speaker — record it in the DAW, or route BlackHole to an
   output through a monitoring path, to hear it.

If you get silence but the link is up, jump to [Troubleshooting](troubleshooting.md) — the usual cause
is PTP, or the far end not subscribed to the site's streams.

## If something is wrong at this stage

The two things people hit first:

- **The Mac has no BlackHole device.** `coreaudiod` was not restarted after installing it. `sudo
  killall coreaudiod`.
- **The appliance shows PTP not locked.** The site's AES67 network has no PTP grandmaster reachable, or
  the daemon is not seeing it. No PTP, no audio — this is a network problem, not an appliance one.

Everything else is in [Troubleshooting](troubleshooting.md).