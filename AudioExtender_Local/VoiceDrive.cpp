#include "VoiceDrive.h"
#include "USBMSC.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include <atomic>

namespace voicedrive {
namespace {

wl_handle_t s_wl = WL_INVALID_HANDLE;
uint32_t s_sector = 4096;
USBMSC* s_msc = nullptr;
std::atomic<bool> s_ejected{false};
uint8_t s_rmw[4096];  // read-modify-write buffer for partial sector writes

int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    return wl_read(s_wl, size_t(lba) * s_sector + offset, buffer, bufsize) == ESP_OK ? int32_t(bufsize) : -1;
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    const size_t start = size_t(lba) * s_sector + offset;
    uint32_t done = 0;
    while (done < bufsize) {
        const size_t pos = start + done;
        const size_t sectorStart = pos - pos % s_sector;
        const uint32_t inSector = uint32_t(pos - sectorStart);
        const uint32_t n = min<uint32_t>(bufsize - done, s_sector - inSector);
        if (inSector != 0 || n != s_sector) {  // partial sector: merge with what is there
            if (wl_read(s_wl, sectorStart, s_rmw, s_sector) != ESP_OK) return -1;
            memcpy(s_rmw + inSector, buffer + done, n);
            if (wl_erase_range(s_wl, sectorStart, s_sector) != ESP_OK) return -1;
            if (wl_write(s_wl, sectorStart, s_rmw, s_sector) != ESP_OK) return -1;
        } else {
            if (wl_erase_range(s_wl, sectorStart, s_sector) != ESP_OK) return -1;
            if (wl_write(s_wl, sectorStart, buffer + done, s_sector) != ESP_OK) return -1;
        }
        done += n;
    }
    return int32_t(bufsize);
}

bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject && !start) s_ejected = true;  // "eject" / "safely remove"
    return true;
}

}  // namespace

bool begin(void (*log)(const char* fmt, ...)) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
    if (!part) {
        if (log) log("voice files: no FAT partition (flash the full image, or use the sketch's partitions.csv)");
        return false;
    }
    if (wl_mount(part, &s_wl) != ESP_OK) {
        if (log) log("voice files: wear-levelling mount failed");
        return false;
    }
    s_sector = wl_sector_size(s_wl);
    const uint32_t blocks = wl_size(s_wl) / s_sector;
    s_msc = new USBMSC();  // created here only: in microphone mode there is no drive
    s_msc->vendorID("Extender");
    s_msc->productID("Voice files");
    s_msc->productRevision("1.1");
    s_msc->onRead(onRead);
    s_msc->onWrite(onWrite);
    s_msc->onStartStop(onStartStop);
    s_msc->mediaPresent(true);
    s_msc->begin(blocks, uint16_t(s_sector));
    if (log) log("voice files: USB drive with %lu KB (%lu sectors of %lu bytes)", (unsigned long)(blocks * s_sector / 1024),
                 (unsigned long)blocks, (unsigned long)s_sector);
    return true;
}

bool ejected() { return s_ejected.load(); }

}  // namespace voicedrive
