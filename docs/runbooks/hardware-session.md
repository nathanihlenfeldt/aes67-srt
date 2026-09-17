# Hardware session: what to run on the appliance, and what it decides

Three tickets are blocked on a machine rather than on work — **#3** (clock prototype), **#4**
(aes67-daemon at 64 channels) and the delay-growth half of **#8/#17** — and one roadmap question
(Opus density) has just joined them. This runbook turns all of that into one session at the hardware.

## The session

On the appliance, from a clone of this repository:

```sh
# once: the tools the measurements need
sudo apt install -y libsrt-openssl-dev libopus-dev libsamplerate0-dev alsa-utils

# build our own tests and the appliance, so sections 3 and 9 can run on real hardware
cmake -S . -B build && cmake --build build --parallel

# measure
bash scripts/measure-hardware.sh

# and then the one section that writes to the daemon, which is off by default
bash scripts/measure-hardware.sh --commission
```

**If the appliance cannot clone this repository** (it is private), the same script is published as a
public gist and needs no clone:

```sh
curl -fsSL https://gist.githubusercontent.com/nathanihlenfeldt/35d40add04d9449038f1071c2359bb5b/raw/measure-hardware.sh -o /tmp/measure.sh
bash /tmp/measure.sh
```

It needs **no root**, installs nothing, and writes one report file in the directory you run it from.
The only sections it cannot cover without a clone are §3 and §9, and it says so — those two need our
own binary. **`--commission` is the only thing the script does that writes anywhere**: it asks our
binary to publish one AES67 source per block and to subscribe at most one sink, which is two REST
documents per block for the sources and one for the sink. Everything else is read-only.
`--subscribe <name|self|auto>` chooses what that sink takes; `auto` means the first
eight-channel L24 sender it finds, and the binary says which one it picked.

### The three things that matter most, in this order

1. **§4's ten-second capture.** The earlier session proved the device *opens* at 64 channels; it did
   not prove it streams. Watch for overruns and for the wall clock against the audio duration.
2. **§9, the commissioning.** This is issue #9's hardware criterion. It runs the appliance with
   `config/aes67-srt.commissioning.conf` — the production daemon and device, but a **loopback link**,
   because the AES67 wiring does not involve the link and a config pointing at a WAN peer would fail to
   open seconds after commissioning succeeded. What answers the criterion is the counting line —
   *"N sources published, N sinks subscribed, **N receiving**"*.

   **A sink pointed at this box's own source is refused by the RAVENNA driver** (`failed to add sink 0 :
   (driver) command failed`), which is why the default is a *discovered* sender instead: the network
   already carries an eight-channel L24 source, and subscribing to it is both the real product
   behaviour and the thing that proves the receive side. **If everything is green except "receiving",
   check `streamer_enabled` in §9's settings list**: provisioning sets it false, and whether a source
   handed over REST needs it true is the open question this run answers.

   **`--commission --subscribe <name|self|auto>` is what to run**, and `auto` is the useful default: it
   subscribes block 0 to the first discovered announcement carrying eight channels of L24 — on this
   bench, `AES67-TX-1`. `self` is the spec's commissioning loopback, and **the RAVENNA driver refuses
   it** (`failed to add sink 0 : (driver) command failed`), so it is not the first thing to try.
   Measured 2026-09-17: our sources *are* accepted by the daemon; the sink is the open half.
3. **§5's PTP state.** Locked or not is the difference between audio and no audio, and it is the
   commonest reason a healthy-looking appliance is silent.

### Provisioning the daemon, without which sections 4 and 5 cannot be measured

A bare Pi has no RAVENNA kernel module and no `aes67-daemon`, so §4 and §5 have nothing to measure.
The same gist carries a provisioner:

```sh
curl -fsSL https://gist.githubusercontent.com/nathanihlenfeldt/35d40add04d9449038f1071c2359bb5b/raw/install-daemon.sh -o /tmp/install-daemon.sh
sudo bash /tmp/install-daemon.sh          # add --dry-run to print every command first
```

It installs the RAVENNA module through DKMS, builds `aes67-daemon`, installs its service and config,
and pins the CPU governor. **Expect 20–40 minutes**, almost all of it Boost compiling. It is modelled
closely on `aes67-sip`'s tested installer, including the two details that make it work — DKMS has to
be told the module lands in `driver/`, and the daemon's config has to be pointed away from its
default `lo`. If it fails, that installer is the fallback:

```sh
git clone --depth 1 https://github.com/nathanihlenfeldt/aes67-sip /tmp/aes67-sip
sudo bash /tmp/aes67-sip/scripts/install.sh --skip-gateway
```

It writes `hardware-report-<host>-<date>.txt`. **Send that whole file back** — including the parts
that could not be measured, because a missing measurement says what the machine lacks.

## What it measures, and what each one decides

| § | Measurement | Decides | Ticket |
|---|---|---|---|
| 1 | The machine, its cores, its governor | The CPU budget every other number is read against | — |
| 2 | Which libsrt and libopus are really installed | Whether the researched APIs are the ones present | #3, #4 |
| 3 | Our own loopback tests on real hardware | That the transport works somewhere other than a laptop | #7 |
| 4 | The RAVENNA device opened at 64ch S24_3LE 48 kHz | Whether the audio module's central assumption is true | #8 |
| 5 | `aes67-daemon`'s REST surface and PTP state | The black box's real behaviour, and whether PTP is locked | #3 |
| 6 | AES-CTR throughput | Whether a passphrase link is affordable at 148 Mbit/s | #7 |
| 7 | Opus lookahead and 64-channel encode/decode CPU | **Whether phase 2 fits on a Pi at all** | #4 |
| 8 | Resampling CPU at 64ch, +10 ppm | **The clock module's deciding number** | #3 |
| 9 | Our binary + the daemon + the device, on one box | **Whether the AES67 half works at all** — the one criterion no runner can reach | #9 |

## How to read the numbers

These thresholds are judgement, not fact — they are where I would change my mind, written down so
the decision does not depend on remembering the reasoning.

**§6 Encryption.** SRT uses AES-CTR, and 148 Mbit/s is 18.6 MB/s. Take the MB/s `openssl` reports and
divide: at 200 MB/s the cipher costs ~9% of one core and a passphrase link is clearly affordable; at
20 MB/s it is the whole appliance and encryption stops being free.

**§7 Opus.** The MacBook measurement was **39% of one core to encode 64 channels** (see
`docs/research/opus.md`). Under ~30% on the Pi, 64 encoded channels fit and phase 2 is a bandwidth
story. Over ~80%, phase 2 is a different product — fewer encoded channels, a faster appliance class,
or encoding only some blocks, which the per-block payload type already allows. The **lookahead**
number should say 312 samples / 6.50 ms; anything else is worth knowing before the delay line is
built against it.

**§8 Resampling — measured, and it decided the design.** On the Pi 5 it costs **11.27% of one core**
at 64 channels and +10 ppm, against a slipping design that needs ~1700 corrections an hour
(`docs/research/clock-recovery.md`). **The decision is ADR 0003: continuous resampling.** Anything
under ~20% was affordable; anything over ~50% would have forced slips and an audibility test. The
converter used here is `SINC_FASTEST`, the cheapest, and the quality-versus-CPU choice among
converters belongs with the clock module's implementation.

**§7 Opus — measured, and it fits.** Encoding 64 channels costs **43.32% of one Pi 5 core**
single-threaded (2.3× realtime), decoding 14.90%. Under ~30% would have made phase 2 a bandwidth
story in the simplest sense; over ~80% would have made it a different product. At 43% the answer is
"fits, but the codec *is* the CPU budget, so the encoders must be threaded per block" — a design
requirement rather than an optimisation. The lookahead measured 312 samples / 6.50 ms.

## What this session does *not* cover

- **The real ppm offset between two appliances.** That needs two of them on a real link, and it is
  five minutes of work once they exist: ticket 09. Until it is measured, every drift figure in
  `docs/research/clock-recovery.md` rests on a plausible 10 ppm rather than on your hardware.
- **Whether a sample correction is audible.** A listening test, not a measurement, and it is the
  deciding fact for the clock design.
- **Loss on a real WAN**, as opposed to the induced backpressure on loopback. Tickets 09 and 10.
- **Rendezvous through a real NAT.** Documentation cannot answer it and neither can a laptop;
  ticket 09.

## If something is missing

The report says so rather than guessing, which is deliberate. The two most likely gaps are
`libsamplerate0-dev` (section 8 then prints the two ways to get it) and the RAVENNA device itself
(section 4 needs `alsa-utils`, and the device only exists once the daemon's kernel module is loaded).
