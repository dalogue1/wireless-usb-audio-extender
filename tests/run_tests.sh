#!/bin/sh
# Host tests (g++ on Linux/macOS). TINYUSB_SRC must point at TinyUSB's src/
# folder for the descriptor test; by default the copy inside the installed
# Arduino-ESP32 3.3.12 core is used.
set -e
cd "$(dirname "$0")"
TU=${TINYUSB_SRC:-$HOME/.arduino15/packages/esp32/tools/esp32s3-libs/3.3.12/include/arduino_tinyusb/tinyusb/src}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
../tools/sync_shared.sh --check
g++ -std=c++17 -O2 -Wall -I../AudioExtender_Local jitter_buffer_test.cpp -o "$B/jb" && "$B/jb" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -Wall -I../AudioExtender_Local link_protocol_test.cpp -o "$B/lp" && "$B/lp" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -Wall -I../AudioExtender_Remote usb_mic_descriptor_test.cpp -o "$B/um" && "$B/um" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -w -Ishim -I"$TU" -I../AudioExtender_Local -I../AudioExtender_Remote usb_device_descriptor_test.cpp -o "$B/ud" && "$B/ud" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -O1 -Wall -I../AudioExtender_Local feature_tests.cpp -o "$B/ft" && "$B/ft" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -O1 -Wall -Wextra -I../AudioExtender_Local adapter_info_test.cpp -o "$B/ai" && "$B/ai" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -O1 -Wall -Wextra -I../AudioExtender_Local event_log_test.cpp -o "$B/el" && "$B/el" || { echo "TEST FAILED"; exit 1; }
g++ -std=c++17 -O2 -I../AudioExtender_Remote noise_signature_test.cpp -o "$B/ns" && "$B/ns" | tail -1 || { echo "TEST FAILED"; exit 1; }
echo "ALL TESTS PASSED"
