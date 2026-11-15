# Building your own microphone

[← Back to the main page](../README.md)

Any ready-made microphone with the right plug works with the extender. If you like soldering, a microphone capsule on a cable is easy to make and sounds better than most: the BirdNET-Go example on the [main page](../README.md) uses an AOM5024 capsule (signal-to-noise ratio 80 dB).

## Parts

* **Capsule:** [AOM-5024P-HD-MB-R](https://www.digikey.com/en/products/detail/pui-audio-inc/AOM-5024P-HD-MB-R/16585482), the version with pins. It is easier to solder than the solder-pad version, and the pins keep the soldering heat away from the capsule, which can damage it.
* **Cable:** shielded, with two inner wires, of a reasonable length. 6 ft (1.8 m) of 24 AWG two-conductor shielded security alarm wire is tested; longer may work.
* **Plug:** 3.5 mm, matching the adapter's jack: **TRRS** for one combined headset jack, **TRS** for a separate pink mic jack ([Microphone plugs](troubleshooting.md#microphone-plugs)).
* **Foam cover** (optional): a small windscreen over the capsule, for example [this one](https://www.amazon.com/dp/B01EJ0N5FS).

## The simple way

Cut the end off an existing cable that has the right plug, find which wire goes to the Mic contact and which to Ground with a meter (continuity from each wire to the plug contacts in the table below), and solder the capsule to them. Add a foam cover if you like.

## Wiring

The same on a TRRS or a TRS plug:

| Capsule | Cable | Plug contact |
|:---|:---|:---|
| **+** | inner wire 1 | **Mic** |
| **−** | inner wire 2 | **Ground** |
| (nothing) | shield | **Ground**, at the plug end only |

Why the shield is connected at the plug end only: it is there to catch interference and drain it to ground. Connected at both ends, it would become a second ground wire alongside inner wire 2, and the interference it picks up would flow through the microphone's own ground and into the audio as hum or noise. Connected at one end, it carries no current and the capsule's signal stays on the two inner wires.

Where Mic and Ground are on the plug:

| Plug | Tip | Ring 1 | Ring 2 | Sleeve |
|:---|:---|:---|:---|:---|
| **TRRS** | left (not used) | right (not used) | **Ground** | **Mic** |
| **TRS** (pink mic jack) | **Mic** | (not used) | – | **Ground** |

This TRRS order (also called CTIA) is the one USB audio adapters use. Wiring diagrams online sometimes show an older order with Mic and Ground swapped (OMTP, from old Nokia phones): don't use it, it gives silence on these adapters.

## Examples

* [Soldering the AOM 5024 Microphone Capsule](https://syllepsis.live/2022/10/27/soldering-the-aom-5024-microphone-capsule/): a simple mono build, the capsule soldered to a TRRS cable and plugged into a USB audio adapter, for BirdNET-Pi.
* [Microphone jack connections](https://www.instagram.com/p/DHVruFYzuSa/) (Instagram).
