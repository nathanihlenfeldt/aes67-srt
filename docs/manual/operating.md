# Operating it

The honest answer to "how do I start and stop this?" is that there are **three things** that can be
started and stopped, at three levels — and knowing which one you want is most of it.

## The three levels

| Level | What it is | What stopping it does | Kept alive by |
|---|---|---|---|
| **The program** (the service) | the `aes67-srt` / `aes67-srt-mac` process | the audio **and the web page** stop | starts on boot/login; restarts if it exits |
| **The audio engine** | the part inside the program that moves audio | the audio stops; **the page stays up** | the page's Start/Stop |
| **The AES67 daemon** (appliance only) | `aes67-daemon`, a separate service | the site's AES67 fabric no longer reaches us | its own systemd unit |

**After installing, it is already running.** There is nothing to start. It comes back on its own at boot
or login, and if it crashes the supervisor restarts it. In the ordinary case — installed and working —
the operator does nothing.

## The everyday controls

### The web page

Every end serves a page at `http://<address>:<port>/` — **8082** on the appliance, and 8082 by default
on the Mac (the installer can change it). This is the whole control surface:

- **Service card — Start / Stop / Restart** operate the **audio engine**. Stop silences the audio but
  leaves the page answering, so you can Stop, change a setting, and Start again without losing the page.
  **Restart** is what applies a saved settings change. **Restart process** restarts the whole program —
  the page reconnects a few seconds later — which is what you use to pick up a **new binary**.
- **Configuration card** — the settings, with *"restart to apply"* when needed (see
  [Configuring it](configuring.md)).
- **Preflight** — the checks that decide whether audio can flow at all, led by **PTP**.
- **Live** — the delay, the clock correction, the frames received and refused.
- **AES67 daemon** — the sources and sinks, and **Start / Stop / Restart daemon** (appliance only).

### The studio Mac's menu bar

The Mac endpoint shows a small **menu-bar icon**, installed with it:

| Icon | Meaning |
|---|---|
| **● aes67** | running |
| **◐ aes67** | starting, stopped, or failed |
| **○ aes67** | the endpoint is not answering |

Hover it for the rate, delay and refused count. Its menu has **Open control page**, **Start / Stop /
Restart audio**, **Restart process**, and **Quit the menu bar**. Quitting the menu bar does **not** stop
the audio — it is only a face on the service.

### The appliance has no icon

It is a headless box; its web page is its face. From a terminal, the everyday view is:

```sh
sudo systemctl status aes67-srt          # is it up?
sudo journalctl -u aes67-srt -f          # what is it doing?
```

## From a terminal

**Mac** — a per-user LaunchAgent, no `sudo`:

```sh
L=com.aes67-srt.endpoint
launchctl print gui/$(id -u)/$L         # status: state, pid, last exit
launchctl kickstart -k gui/$(id -u)/$L  # restart it
launchctl bootout gui/$(id -u)/$L       # STOP it (and keep it stopped)
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/$L.plist   # start again
tail -f ~/Library/Logs/aes67-srt/endpoint.log
```

`KeepAlive` means **`launchctl stop` does not keep it stopped** — the system brings it straight back, by
design. Use `bootout` to stop it and `bootstrap` to start it. (Older macOS: `launchctl load`/`unload`.)
For the menu bar, the same with `com.aes67-srt.menubar`.

**Appliance** — a systemd service, needs `sudo`:

```sh
sudo systemctl status aes67-srt
sudo systemctl stop aes67-srt        # start / restart likewise
sudo systemctl disable aes67-srt     # do not start on boot
sudo journalctl -u aes67-srt -f

sudo systemctl status aes67-daemon   # the AES67 daemon, separate
```

## Monitoring

- **The page's Preflight** leads with PTP, because an unlocked PTP slave is the most common way the
  appliance looks healthy while producing silence.
- **The link line** shows the negotiated latency, the round trip, and — the numbers that matter under
  strain — packets lost, retransmitted and **dropped** (dropped means audio that arrived too late to
  play).
- **`scripts/soak-link.sh <page-url> <seconds>`** samples a running link and reports the trend, failing
  if it stops delivering or the delay trips the alarm. This is the tool for "keep an eye on it".
- **`scripts/collect-diagnostics.sh`** gathers everything needed to diagnose an appliance into one file,
  read-only, with the passphrase redacted — the thing to send when asking for help.

## Updating

- **Appliance:** re-run the installer. It is idempotent — it updates the checkout, rebuilds, and
  reinstalls, leaving your configuration alone.
- **Mac:** re-run `scripts/install-mac.sh` (same options). It rebuilds the menu bar too.

Picking up a **new binary** without reinstalling: replace it and use **Restart process** on the page, or
`sudo systemctl restart aes67-srt` / `launchctl kickstart -k`.

> Do not copy a new binary over the Mac's with `cp`: macOS tags it and refuses to run it. Re-run the
> installer, which installs it correctly.

## Removing it

- **Mac:** `./scripts/uninstall-mac.sh` stops the endpoint and its menu bar and removes their binaries
  and agents. It **keeps your configuration** (it holds the site's address and passphrase); `--purge`
  removes that too.
- **Appliance:** `sudo ./scripts/uninstall.sh` stops and disables the service and removes the unit,
  binary, service user, the daemon-control rule and the source checkout. It keeps the configuration
  unless `--purge`, and leaves `aes67-daemon` and the RAVENNA kernel module alone unless `--with-daemon`.
  Removing a kernel module is a kernel concern — `dkms remove` is the operator's to run knowingly.

## What to expect

- **A listener shows "starting" until a caller connects.** That is honest, not stuck: the state is
  `starting` while the link is not up, `running` once it is.
- **An Opus link takes a minute to settle** to the target latency, because the level converges one 20 ms
  period at a time. A fresh Opus link looks idle for a little while before it looks right.
- **The Mac needs a device to play into** — BlackHole — and a DAW or monitoring app to hear it.