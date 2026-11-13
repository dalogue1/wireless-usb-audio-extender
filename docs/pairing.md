# Pairing and several mics

[← Back to the main page](../README.md)

## How a pair finds itself

The remote announces itself on its channel five times a second. A local board with the **same mic number** answers, and from then on the two talk only to each other, with acknowledgements and retries. If either hears nothing for 2 s, both start looking again; a short dropout recovers by itself in a second or two. There is no Wi-Fi network, router or IP address involved.

## Several mics

Set a different `mic` (1–9) in each local board's [`config.txt`](configuration.md): the computer sees **Extender Mic 1**, **Extender Mic 2** … and the EXTENDER drives are named EXTENDER1, EXTENDER2 … A remote and a local board only talk when their numbers match.

* Each pair uses about 7–14 % of a channel (depending on [`distance`](distance.md)), so several pairs share one channel comfortably. For more, or at a busy site, spread them over channels **1, 6 and 11** with `channel`.
* Pairs far enough apart not to hear each other don't share airtime at all.
* Linux names the sound card after the mic number (`E1`, `E2` …). After changing `mic`, choose the new **Extender Mic N** in your recording software.

## Changing the mic number or channel

A change of `mic` or `channel` is a *handover*: the local board starts on the old setting, tells the remote, and both restart on the new one.

* The local board **never moves without the remote.** If the remote is off, the local board waits on the old setting (`status.txt` says so) until the remote is switched on and confirms.
* Meanwhile it also listens for the remote on the new setting, and follows it there if it hears it.

## Locking a pair (remote = MAC)

By default (`remote = any`) a local board links to whichever remote it hears on its mic number and channel. Every extender starts as Mic 1 on channel 11, so a **neighbour's extender** in range could take over the link after a dropout, and you'd record the wrong microphone without noticing. Both boards **warn** when that could happen: a second remote on the same mic number, or a second local board trying to link, shows up as a WARNING in the log, `events.txt` and `status.txt`.

To lock a pair:

1. Open the drive and find the remote's **MAC address** in the Remote section of `status.txt`.
2. Put it in `config.txt`, for example `remote = 44:BD:8D:76:CA:44`, and eject.

The local board then links only to that remote, and tells the remote to accept only this local board. The remote saves the lock, and other local boards skip it. `remote = any` unlocks both again. Nothing ever locks by itself.

* **Replaced a local board?** It can't reach a remote locked to the old one. Hold the **remote's** BOOT for 10 s (factory reset unlocks it).
* **Reset the remote with BOOT?** It goes back to Mic 1 / channel 11, unlocked. A locked local board on another mic number or channel finds it again by itself: after 30 s without a link it also listens on Mic 1 / channel 11 **for its own remote's MAC only**, moves there, hands the remote back to the configured setting, and locks it again.

## Remote and local on different settings

If they end up on different mic numbers or channels (a remote replaced or reset while the local board was unlocked and away), hold the **remote's** BOOT for 10 s (back to Mic 1 / channel 11). Then set `mic = 1` and `channel = 11` on the local board (or delete `config.txt`), eject, and once they link, change them again with the remote switched on.
