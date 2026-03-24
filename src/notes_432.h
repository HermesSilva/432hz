#pragma once
// =============================================================================
//  notes_432.h  —  Integer-frequency note table (432 Hz tuning, A=432)
//
//  Philosophy
//  ----------
//  Every frequency is an exact integer in Hz.  No logarithmic scale,
//  no irrational multipliers.  Intervals are built from pure integer ratios
//  rooted at C4 = 256 Hz (= 2^8) and A4 = 432 Hz.
//
//  Scale pattern (one octave, C4–C5)
//  -----------------------------------
//   C4   256 Hz   2^8          root
//   C#4  272 Hz   16 * 17
//   D4   288 Hz   2^5 * 3^2
//   D#4  304 Hz   16 * 19
//   E4   320 Hz   2^6 * 5
//   F4   341 Hz   (integer approximation of just fourth 4/3 * 256 ≈ 341)
//   F#4  360 Hz   2^3 * 3^2 * 5  (geometric-circle reference)
//   G4   384 Hz   2^7 * 3          (just fifth 3:2 above C4)
//   G#4  405 Hz   3^4 * 5
//   A4   432 Hz   2^4 * 3^3        (natural resonance, concert pitch)
//   A#4  450 Hz   2 * 3^2 * 5^2
//   B4   480 Hz   2^5 * 3 * 5
//   C5   512 Hz   2^9              (octave)
// =============================================================================

#include <cstdint>
#include <array>
#include <string_view>

struct Note {
    std::string_view name;
    uint32_t         hz;       // exact integer frequency in Hz
};

// Full chromatic table from C2 (64 Hz) up to C8 (16384 Hz).
// Each octave doubles the integers — pure integer octave relationships.
// clang-format off
inline constexpr std::array<Note, 85> NOTE_TABLE = {{
    // Octave 2  (C2 = 64)
    { "C2",   64  }, { "C#2",  68  }, { "D2",   72  }, { "D#2",  76  },
    { "E2",   80  }, { "F2",   85  }, { "F#2",  90  }, { "G2",   96  },
    { "G#2", 101  }, { "A2",  108  }, { "A#2", 112  }, { "B2",  120  },
    // Octave 3  (C3 = 128)
    { "C3",  128  }, { "C#3", 136  }, { "D3",  144  }, { "D#3", 152  },
    { "E3",  160  }, { "F3",  171  }, { "F#3", 180  }, { "G3",  192  },
    { "G#3", 202  }, { "A3",  216  }, { "A#3", 225  }, { "B3",  240  },
    // Octave 4  (C4 = 256)  — reference octave
    { "C4",  256  }, { "C#4", 272  }, { "D4",  288  }, { "D#4", 304  },
    { "E4",  320  }, { "F4",  341  }, { "F#4", 360  }, { "G4",  384  },
    { "G#4", 405  }, { "A4",  432  }, { "A#4", 450  }, { "B4",  480  },
    // Octave 5  (C5 = 512)
    { "C5",  512  }, { "C#5", 544  }, { "D5",  576  }, { "D#5", 608  },
    { "E5",  640  }, { "F5",  682  }, { "F#5", 720  }, { "G5",  768  },
    { "G#5", 810  }, { "A5",  864  }, { "A#5", 900  }, { "B5",  960  },
    // Octave 6  (C6 = 1024)
    { "C6", 1024  }, { "C#6",1088  }, { "D6", 1152  }, { "D#6",1216  },
    { "E6", 1280  }, { "F6", 1364  }, { "F#6",1440  }, { "G6", 1536  },
    { "G#6",1620  }, { "A6", 1728  }, { "A#6",1800  }, { "B6", 1920  },
    // Octave 7  (C7 = 2048)
    { "C7", 2048  }, { "C#7",2176  }, { "D7", 2304  }, { "D#7",2432  },
    { "E7", 2560  }, { "F7", 2728  }, { "F#7",2880  }, { "G7", 3072  },
    { "G#7",3240  }, { "A7", 3456  }, { "A#7",3600  }, { "B7", 3840  },
    // Octave 8  (C8 = 4096)
    { "C8", 4096  },
}};
// clang-format on

// Convenience: find a note by name (returns nullptr if not found)
inline const Note* findNote(std::string_view name) {
    for (const auto& n : NOTE_TABLE)
        if (n.name == name) return &n;
    return nullptr;
}
