#pragma once
// Voice-files mode: the local board restarts as a small USB drive ("EXTENDER")
// holding the /voice WAV files, instead of being a microphone. Copy your own
// recordings over them, eject the drive (or press RESET) and the local board
// restarts as the microphone, now speaking the new files.
//
// The drive is the local board's FAT partition (see partitions.csv), shared with the
// PC through the same wear-levelling layer the firmware's FAT driver uses.
#include <Arduino.h>

namespace voicedrive {

// Call before USB.begin(). Returns false if the FAT partition is missing.
bool begin(void (*log)(const char* fmt, ...));
// Call from loop(): true once the PC ejected the drive (time to restart).
bool ejected();

}  // namespace voicedrive
