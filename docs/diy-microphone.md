# Building your own microphone

[← Back to the main page](../README.md)

Any ready-made microphone with the right plug works with the extender. If you like soldering, a microphone capsule on a cable is easy to make and sounds better than most: the BirdNET-Go example on the [main page](../README.md) uses an AOM5024 capsule (signal-to-noise ratio 80 dB).

## Parts

* **Capsule:** [AOM-5024P-HD-MB-R](https://www.digikey.com/en/products/detail/pui-audio-inc/AOM-5024P-HD-MB-R/16585482), the version with pins. It is easier to solder than the solder-pad version, and the pins keep the soldering heat away from the capsule, which can damage it.
* **Cable:** shielded, with two inner wires.
* **Plug:** 3.5 mm, matching the adapter's jack: **TRRS** for one combined headset jack, **TRS** for a separate pink mic jack ([Microphone plugs](troubleshooting.md#microphone-plugs)).

## Wiring

The same on a TRRS or a TRS plug:

| Capsule | Cable | Plug contact |
|:---|:---|:---|
| **+** | inner wire 1 | **Mic** |
| **−** | inner wire 2 | **Ground** |
| (nothing) | shield | **Ground**, at the plug end only |

Connect the shield at the plug end only, and leave it unconnected at the capsule. It then keeps interference off the two inner wires without carrying any of the microphone's own signal.

Where Mic and Ground are on the plug:

| Plug | Tip | Ring 1 | Ring 2 | Sleeve |
|:---|:---|:---|:---|:---|
| **TRRS** (the usual standard, also called CTIA) | left (not used) | right (not used) | **Ground** | **Mic** |
| **TRRS**, older OMTP standard | left (not used) | right (not used) | **Mic** | **Ground** |
| **TRS** (pink mic jack) | **Mic** | (not used) | – | **Ground** |

Nearly all current USB audio adapters use the usual (CTIA) TRRS standard. If a TRRS build gives silence, the adapter may be one of the older OMTP ones: swap Mic and Ground.

## Examples

* [Soldering the AOM 5024 Microphone Capsule](https://syllepsis.live/2022/10/27/soldering-the-aom-5024-microphone-capsule/): a simple mono build, the capsule soldered to a TRRS cable and plugged into a USB audio adapter, for BirdNET-Pi.
* [Another example](https://www.instagram.com/p/DHVruFYzuSa/) (Instagram).
