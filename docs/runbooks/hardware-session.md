# Hardware session: what to run on the appliance, and what it decides

Three tickets are blocked on a machine rather than on work — **#3** (clock prototype), **#4**
(aes67-daemon at 64 channels) and the delay-growth half of **#8/#17** — and one roadmap question
(Opus density) has just joined them. This runbook turns all of that into one session at the hardware.

## The session

On the appliance, from a clone of this repository:

```sh
# once: the tools the measurements need
sudo apt install -y libsrt-openssl-dev libopus-dev libsamplerate0-dev alsa-utils

# build our own tests, so section 3 can run the loopback on real hardware
cmake -S . -B build && cmake --build build --parallel

# measure
bash scripts/measure-hardware.sh
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

**§8 Resampling.** This is the number that decides between continuously resampling (no sample
corrections, but every sample processed) and slipping the playout buffer (no continuous cost, but
~1700 corrections an hour, per `docs/research/clock-recovery.md`). Under ~20% of a core, continuous
resampling is affordable and is the simpler design. Over ~50%, slips are the only affordable route
and the work moves to making each one inaudible. **Between those, the audibility test decides, not
the CPU number** — and that test needs ears, not a script.

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
