# 432 Hz Bit-Perfect ASIO Synthesiser

A minimal C++17 application that generates **pure integer-frequency sine waves** through an ASIO driver at **192 000 Hz**, following the 432 Hz natural-tuning philosophy described in *A Dissonância da Medida*.

---

## Philosophy

| Principle | Implementation |
|---|---|
| No logarithmic scales | All frequencies are exact integers (no `2^(n/12)` multipliers) |
| Bit-perfect output | 64-bit `double` samples sent directly to the ASIO driver |
| No runtime `sin()` | A 65 536-entry double-precision LUT, built **once at startup** |
| Integer phase arithmetic | Q32.32 fixed-point accumulator — zero floating-point drift |
| A4 = 432 Hz | Rooted at C4 = 256 Hz (= 2⁸), A4 = 432 Hz (= 2⁴·3³) |

---

## Integer Scale (C4 – C5)

| Note | Hz | Integer factorisation |
|------|----|-----------------------|
| C4   | 256 | 2⁸ — root |
| C#4  | 272 | 16 × 17 |
| D4   | 288 | 2⁵ × 3² |
| D#4  | 304 | 16 × 19 |
| E4   | 320 | 2⁶ × 5 |
| F4   | 341 | integer approx. of just fourth (4/3 × 256) |
| F#4  | 360 | 2³ × 3² × 5 — geometric circle |
| G4   | 384 | 2⁷ × 3 — just fifth (3:2 above C4) |
| G#4  | 405 | 3⁴ × 5 |
| A4   | 432 | 2⁴ × 3³ — natural resonance |
| A#4  | 450 | 2 × 3² × 5² |
| B4   | 480 | 2⁵ × 3 × 5 |
| C5   | 512 | 2⁹ — octave |

Each octave doubles every integer exactly (no rounding).

---

## Requirements

| Dependency | Notes |
|---|---|
| **RtAudio** ≥ 5.2 | Must be compiled with `__WINDOWS_ASIO__` |
| **ASIO SDK** | Download from Steinberg (free registration) |
| **ASIO driver** | ASIO4ALL (free) or a native ASIO card driver |
| **CMake** ≥ 3.16 | Build system |
| **MSVC** ≥ 2019 or **MinGW-w64** | C++17 required |

### Obtaining RtAudio

```powershell
git clone https://github.com/thestk/rtaudio.git C:/libs/rtaudio
```

### Obtaining the ASIO SDK

1. Go to https://www.steinberg.net/developers/
2. Download **ASIO SDK** (free, requires account)
3. Extract to `C:/libs/rtaudio/asiosdk`  (or set `-DASIO_SDK_DIR=...`)

The directory layout expected:

```
C:/libs/rtaudio/
  RtAudio.cpp
  RtAudio.h
  asiosdk/
    common/
    host/
      pc/
```

---

## Build

```powershell
cd d:\Tootega\Source\432
mkdir build ; cd build
cmake .. -DRTAUDIO_DIR=C:/libs/rtaudio -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Or with **Ninja + MSVC**:

```powershell
cmake .. -DRTAUDIO_DIR=C:/libs/rtaudio -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

---

## Usage

```
432hz_player.exe
```

The application auto-selects the first ASIO device that supports 192 000 Hz.

### Interactive commands

```
A4              — play 432 Hz
C4              — play 256 Hz
chord C4 E4 G4  — play a just-intonation major triad (256+320+384 Hz)
x 300           — play any integer Hz (e.g. 300)
stop            — silence all voices
list            — print the full note table (C2–C8)
quit            — exit
```

---

## File Structure

```
432/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp        — ASIO stream + interactive console
    ├── lut.h           — PhaseAccum + LUT declarations
    ├── lut.cpp         — LUT initialisation (only sin() call in the program)
    └── notes_432.h     — Integer-frequency note table (C2–C8)
```

---

## Technical notes

### Why 192 000 Hz?
Higher sample rates give the DAC more headroom for its own internal oversampling and noise-shaping, preserving the bit-perfect integer-sample relationship.

### Why Q32.32 fixed-point?
For an integer frequency `F` Hz and sample rate `SR` Hz, the phase increment is:

```
delta (Q32.32) = (F × LUT_SIZE × 2³²) / SR
```

All arithmetic is integer. The 32 fractional bits give a phase error below
`1 / 2³² ≈ 2.3 × 10⁻¹⁰` cycles per sample — well below any audible threshold.

### Why 64-bit doubles for the LUT?
`double` has 53 mantissa bits, giving ~15–16 significant decimal digits per sample.
The quantisation noise floor is around −318 dBFS, physically irrelevant.

### Polyphony
Up to 16 simultaneous voices (configurable via `MAX_VOICES` in `main.cpp`).
The `chord` command divides gain equally across voices.
