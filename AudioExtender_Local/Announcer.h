#pragma once
// Spoken announcements mixed into the USB microphone stream.
//
// Voice: each announcement plays /voice/<name>.wav from the local board's own small
// FAT drive (replaceable in voice-files mode, see VoiceDrive.h), or the
// built-in clip from voice_clips.h when that file is missing or unusable.
// Other languages, or beeps instead of words, are voice packs: sets of WAV files
// copied into the drive's voice folder (voice-packs/ in the project).
// Announcements only play while the PC is recording; they are mixed in at
// about -12 dBFS with the microphone turned down underneath (VoiceMixer).
#include <Arduino.h>

class VoiceMixer;

namespace announcer {

// Order matches CLIPS in tools/make_voice.py and kBuiltinClips.
enum class Event : uint8_t {
    Mic1, Mic2, Mic3, Mic4, Mic5, Mic6, Mic7, Mic8, Mic9,  // contiguous: Mic1 + pair ID
    LinkUp, LinkLost,
    AdapterConnected, AdapterRemoved, AdapterUnsupported,
    FilterOn, FilterStrong, FilterOff,
    NoSignal,
    Count
};

enum class Mode : uint8_t { Voice = 0, Off = 1 };  // config.txt order

// Mounts the voice drive (formatting and filling it with the built-in clips
// on first use), checks the files and starts the playback task.
// log: printf-style logger; recording: true while the PC records.
void begin(VoiceMixer* mixer, bool (*recording)(), void (*log)(const char* fmt, ...));
void setMode(Mode m);
Mode mode();
// Queues an announcement (only while the PC records) and reports it to the event log.
void say(Event e);
// Event logger for "said: ..." lines (called from the task that calls say()).
void setEventLog(void (*log)(const char* fmt, ...));
const char* modeText(Mode m);
// Factory reset: erase the voice drive (refilled with the built-in clips at the next boot).
void eraseVoiceFiles();
// Voice files on the drive that passed the checks (the rest use the built-in clips).
int usableFiles();
// The ones that did not, with why: "mic_4.wav (empty file), ..." or "none".
void unusableText(char* out, size_t cap);

}  // namespace announcer
