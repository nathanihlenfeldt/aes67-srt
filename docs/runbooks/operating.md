# Runbook: running it, starting it, stopping it

The question this answers: **how does someone start and stop this?** The honest answer is that there
are three things that can be "started and stopped", at three levels, and knowing which one you want is
most of it.

## The three levels

| Level | What it is | What stopping it does | How the OS keeps it alive |
|---|---|---|---|
| **The program** (the service) | `aes67-srt` / `aes67-srt-mac`, the process | the audio **and the web page** stop | starts on boot/login, restarts if it exits |
| **The audio engine** | the part inside the program that moves audio | the audio stops; **the page stays up** | started/stopped from the page |
| **The AES67 daemon** (appliance only) | `aes67-daemon`, a separate service | the site's AES67 fabric stops reaching us | its own systemd unit |

**After installing, it is already running.** There is nothing to start. It comes back on its own at
boot or login, and if it crashes the supervisor restarts it. For the ordinary case — it is installed,
it is working — the operator does nothing.

## The web page is the everyday control

`http://<host>:<http_port>/` (8082 on the appliance, 8082 by default on the Mac).

- **Start / Stop / Restart** (the **Service** card) are the *audio engine*: Stop silences the audio but
  leaves the page answering, so you can Stop, change a setting, and Start again without losing the
  page.
- **Restart** applies a saved change. The configuration card says *"restart to apply"* when a change
  needs it, and the Restart button is how it lands.
- **Restart process** is different: it restarts the whole program (to pick up a **new binary**) and the
  page reconnects a few seconds later. Both supervisors relaunch on exit.
- **Start / Stop / Restart daemon** (the AES67 daemon card, appliance only) manage `aes67-daemon`. On
  the appliance these need the polkit rule `install-daemon.sh` installs, scoped to that one unit; a
  refusal in the page names the missing privilege.

## The program, from a terminal

**macOS** — a per-user LaunchAgent, no sudo:

```bash
L=com.aes67-srt.endpoint

launchctl print gui/$(id -u)/$L        # status: state, pid, last exit code
launchctl kickstart -k gui/$(id -u)/$L # restart it
launchctl bootout gui/$(id -u)/$L      # STOP it (and keep it stopped)
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/$L.plist   # start it again

tail -f ~/Library/Logs/aes67-srt/endpoint.log
```

`KeepAlive` means **`launchctl stop` does not keep it stopped** — launchd brings it straight back, by
design. Use `bootout` to stop it, `bootstrap` to start it. (Older macOS: `launchctl load`/`unload`.)
It is unsigned; if Gatekeeper blocks it, see `docs/runbooks/macos-endpoint.md`.

**Appliance** — a systemd unit, needs sudo:

```bash
sudo systemctl status aes67-srt
sudo systemctl stop aes67-srt        / start / restart
sudo systemctl disable aes67-srt     # do not start on boot
sudo journalctl -u aes67-srt -f

sudo systemctl status aes67-daemon   # the AES67 daemon, separate
```

## Installing, updating, removing

- **Appliance:** `scripts/install.sh` (or the `curl | bash` line in the spec). Idempotent — re-running
  it updates and reinstalls. It installs the daemon and the polkit rule too, unless `--skip-daemon`.
- **macOS:** `scripts/install-mac.sh` — re-run it to update the binary. `scripts/uninstall-mac.sh`
  removes the agent and binary (keeps the config; `--purge` removes that too).

## Why it looks like nothing is there

Both are **background services**: no Dock icon, no window. That is normal for a service, and it is
also the thing people find disconcerting — the software is running and invisible.

**On the Mac there is now a menu bar app**, installed by `install-mac.sh` (skip it with
`--no-menubar`). It shows the state — **● aes67** running,  starting/stopped, ○ offline — with the
rate and delay in the tooltip, and its menu has **Open control page**, **Start / Stop / Restart
audio**, **Restart process** and **Quit the menu bar**. It is a separate LaunchAgent
(`com.aes67-srt.menubar`) that starts at login and polls the endpoint's own `/api/status`; it owns
nothing, so quitting it does not stop the audio.

On the appliance there is no icon — it is a headless box, reached by its web page — so the page is the
only face it has.

## What to expect

- **The Mac needs a device to play into.** BlackHole 64ch, installed separately (one sudo step), then
  `sudo killall coreaudiod`. A DAW records from it.
- **A listener shows "starting" until a caller connects** — that is honest, not stuck. The page's state
  is `starting` while the link is not yet up, `running` once it is.
- **An Opus link takes a minute to settle** to the target latency: the level converges one 20 ms period
  at a time, so a fresh link looks idle before it looks right.