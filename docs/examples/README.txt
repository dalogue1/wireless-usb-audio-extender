USB Audio Extender - EXTENDER drive

config.txt  the settings of both boards (mic number, channel, feedback,
            noise filter). Edit and save; delete it to return to the defaults.
status.txt  what was in effect when the drive was opened.
adapter.txt the remote's latest audio adapter: its details and capabilities.
events.txt  the event log: starts, USB and link drops, restarts (kept through power cuts).
noise.svg   the noise filter's learned background noise (when the filter has learned it).
report.html all of the above at a glance: open it in any browser.
voice       the announcements.

The local board speaks the files in 'voice'. Replace any of them with
your own recording or another language: keep the file names, save as
WAV, 16-bit PCM, mono or stereo, 8000-48000 Hz, about 1-2 s each.
Silence at the ends is trimmed and the level is evened out automatically.
Ready-made sets (other languages, beeps) are in the project's voice-packs folder.
A missing or unusable file plays the built-in voice instead.

When done, eject the drive (or press RESET): the local board restarts as the microphone
and passes the remote's settings on over the radio.
