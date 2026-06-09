# EKey-Jerboa v2
<p align="center">
  <img src="docs%20screenshots/ui.png" alt="EKey-Jerboa" width="800">
</p>

<p align="center">
  <b>GPU-accelerated search tool for the Bitcoin "puzzle" challenges.</b>
</p>
**GPU-accelerated search tool for the Bitcoin "puzzle" challenges.**

Given a target P2PKH address (or compressed public key) and a key range,
EKey-Jerboa sweeps that range on an NVIDIA GPU looking for the matching private
key. The range (a *chunk*) is split into many equal *slots*; depending on the
mode, the program either spreads its search across those slots — hopping between
them on a timer — or reads the whole chunk straight through. Progress is saved,
so a search can be paused and resumed.

---

## Credits

- **Fork author / maintainer:** [egorrushka](https://github.com/egorrushka)
- **Based on:** [VanitySearch](https://github.com/JeanLucPons/VanitySearch) by **Jean Luc PONS** (GPLv3)
- **Programmed by:** **Claude (Anthropic)** under the meticulous guidance and
  direction of **egorrushka** — who designed the engine behaviour, drove the
  architecture and tested every single build.
- **Made in:** Ukraine, under the extreme conditions of war, in the glorious
  city of **Chernihiv**.

---

## How the search works

The engine has **two modes**, selected by the jump interval `-T`. This is the
most important thing to understand, because it decides whether the search is
*exhaustive* or a *spread*:

### 🟢 Linear / no-jump mode — `-T 999999999` — **full sweep, no skips**
Scatter is disabled. Each slot is read from start to end, slot by slot, and the
program stops with `Done!` once the **entire chunk has been read**. Every key in
the range is checked exactly once — **no gaps, guaranteed complete**. Use this
when you must be certain the whole range was examined.

### 🟠 Jump mode — finite `-T` (default `30`) — **broad scatter, not exhaustive**
The chunk is split into slots; the program visits them (sequential, or random
with `-R`) and on each visit jumps to a **golden-ratio-distributed position
inside the slot**, reads forward for `-T` seconds, then hops to the next slot.
This **spreads the search across the whole chunk**, so a key located anywhere is
likely to be reached early instead of grinding linearly from the start.

It runs in multiple passes (up to **10,000**), deepening coverage each pass via
a two-level golden-ratio scatter (`gcd(6181, S) = 1` ⇒ a bijection, up to
**S × S = 100,000,000** unique depth points per slot at `-S10000`). Because it
prioritises *spread* over *completeness*, **if you stop it early the chunk is
covered only partially — there will be gaps.** Use this when you expect a single
hit somewhere in a large range and want the best *average* time-to-find.

> `-b` saves progress and resumes in **both** modes.

---

## Features

- Target match by P2PKH compressed address (`-a`) or compressed public key (`-p`).
- **Two search strategies** — exhaustive linear sweep, or multi-pass
  golden-ratio scatter (see above).
- **Configurable scatter depth (`-S`)** — `100 / 500 / 1000 / 5000 / 10000`
  sections per slot; deeper = finer no-repeat coverage (up to 100M unique
  points/slot).
- **Random slot order (`-R`)** — full-period LCG permutation; every slot is
  visited exactly once per pass, in pseudo-random order, reshuffled each pass.
- **Time jumps (`-T`)** — hop to the next slot every *T* seconds; `-T 999999999`
  reads each slot to the end (linear mode).
- **Resume (`-b`)** — per-puzzle / chunk / mode / sections progress folders; a
  resumed run continues exactly where it stopped.
- **Live console** — current slot, hop counter, completed slots, keys this hop,
  depth/scatter position, speed and time to the next hop.
- **Graphical launcher** (Python / Tkinter) that builds the command line for you.

### Under the hood
- **secp256k1 engine** — 256-bit modular arithmetic, point add/double, **batch
  modular inversion** (Montgomery trick) shared across a whole key group.
- **Hash160 pipeline** — SHA-256 → RIPEMD-160 over the compressed pubkey, on CPU
  (verify) and GPU (search).
- **`SINGLE_TARGET_MODE`** — for one-address puzzle hunting the target Hash160
  sits in GPU constant memory and is compared in registers (near-zero global
  reads).
- **Warp voting** (`__any_sync`) on the multi-target path; `__forceinline__`
  SHA-256 / RIPEMD-160; in-place SHA-256 transform to cut local-memory pressure.
- **Thread-safe RNG** and robust scatter that degrades gracefully on small
  ranges (V2.0.0).

---

## Requirements

- NVIDIA GPU + **CUDA Toolkit** (developed / tested on CUDA 13.1).
- Windows: Visual Studio build tools (MSVC). &nbsp;|&nbsp; Linux: `g++` + `make`.

---

## Build

### Windows
A ready **`compile_modern.bat`** is included.

> ⚠️ **Before running it, open `compile_modern.bat` and set your own project
> path** — edit the `cd /d "..."` line at the top so it points to the folder
> where you unpacked the sources. Then run the script from the *x64 Native Tools
> Command Prompt for VS*. It produces `EKey-Jerboa.exe`.

### Linux
A **`Makefile`** is included. From the project root:

```bash
make                      # multi-arch fat binary (sm_75 / 86 / 89 + PTX)
make CUDA_ARCH=sm_86      # single architecture
make CCAP="75 86"         # custom multi-arch list
make clean
make run ARGS="-a <addr> -s 0x.. -e 0x.."
```

> **GPU architectures:** builds target `sm_75` (RTX 20xx), `sm_86`
> (RTX 30xx / A4000), `sm_89` (RTX 40xx) out of the box, plus `compute_89` PTX
> so newer cards (RTX 50xx+) run via JIT. A legacy build (CUDA 12.3) can target
> `sm_61` (GTX 10xx). The sources are already adapted to current NVIDIA
> architectures.

---

## Usage

```
EKey-Jerboa -a <address> -s 0x<start> -e 0x<end> [options]
```

| Option | Meaning |
|--------|---------|
| `-a <addr>`   | Target P2PKH (compressed) address |
| `-p <pubkey>` | Target compressed public key (alternative to `-a`) |
| `-s 0x<hex>`  | Chunk start |
| `-e 0x<hex>`  | Chunk end |
| `-r <bits>`   | Bit-range shorthand (e.g. `-r 71`) |
| `-T <sec>`    | Jump interval (default `30`). `999999999` = linear full sweep |
| `-G <id>`     | GPU device id (default `0`) |
| `-W <0-7>`    | Grid profile (`0`=auto, `5`=6144x256, `7`=12288x256) |
| `-R`          | Random slot order (LCG permutation) |
| `-S<N>`       | Scatter depth: `100 / 500 / 1000 / 5000 / 10000` |
| `-b`          | Save / resume progress |
| `-faq` / `-inf` | Full in-program manual / version & credits |

Run `EKey-Jerboa -faq` for the full manual, or `-inf` for version & credits.

### Launcher
A graphical launcher lets you pick the puzzle / address, set the chunk on a
visual ruler or in hex, choose grid / GPU / jump interval / scatter depth, toggle
random slot order and progress save-resume, and save/restore sessions. It only
starts `EKey-Jerboa` with the right flags — all GPU work is done by the compiled
program itself. If you prefer the command line, ignore the launcher entirely and
run `EKey-Jerboa` directly with the flags above.

---

## License

Licensed under the **GNU General Public License v3** — see [`LICENSE`](LICENSE).
This is a fork based on VanitySearch (also GPLv3); the GPLv3 notice in every
source file is preserved.

## Contact

Questions / feedback: **egor.gr1@gmail.com**
