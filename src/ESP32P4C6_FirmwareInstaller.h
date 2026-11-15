/*
 * ESP32P4C6_FirmwareInstaller - helpers for the installer sketches.
 *
 * Writes a firmware blob into NVS, where the board libraries load it from,
 * after checking that it inflates to exactly the image its SHA-256 names; then
 * reads it back and checks again.  The firmware itself is not here - each
 * installer example carries its own, under that firmware's license.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */
#pragma once
#include <Arduino.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "miniz.h"              // ROM inflate (tinfl), esp_rom component

namespace p4c6fw {

enum Result {
    INSTALLED,          // written and read back intact
    ALREADY_INSTALLED,  // NVS already held exactly this blob; nothing written
    BAD_SOURCE,         // the blob in this sketch doesn't match its own hash
    NO_MEMORY,
    NVS_FULL,           // not enough free NVS entries for the blob
    NVS_ERROR,
    VERIFY_FAILED,      // written, but what came back was wrong
};

inline const char *resultText(Result r)
{
    switch (r) {
        case INSTALLED:         return "installed and verified";
        case ALREADY_INSTALLED: return "already installed (identical); nothing written";
        case BAD_SOURCE:        return "the firmware in this sketch failed its own check - rebuild it";
        case NO_MEMORY:         return "out of memory";
        case NVS_FULL:          return "not enough free space in the NVS partition";
        case NVS_ERROR:         return "NVS error";
        case VERIFY_FAILED:     return "written, but reading it back failed the check";
    }
    return "?";
}

// True if `blob` (zlib) inflates to exactly `imageBytes` bytes whose SHA-256
// is `sha`.  The inflater's ~11 KB state goes on the heap, not the stack.
inline bool verifyBlob(const uint8_t *blob, size_t len, size_t imageBytes, const uint8_t sha[32])
{
    uint8_t *img = (uint8_t *)malloc(imageBytes);
    tinfl_decompressor *inf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    bool ok = false;
    if (img && inf) {
        tinfl_init(inf);
        size_t inLen = len, outLen = imageBytes;
        tinfl_status st = tinfl_decompress(inf, blob, &inLen, img, img, &outLen,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (st == TINFL_STATUS_DONE && outLen == imageBytes) {
            uint8_t got[32];
            mbedtls_sha256(img, imageBytes, got, 0);
            ok = memcmp(got, sha, 32) == 0;
        }
    }
    free(img);
    free(inf);
    return ok;
}

// Install `blob` under namespace/key in the default NVS partition.
inline Result storeBlob(const char *ns, const char *key,
                        const uint8_t *blob, size_t len,
                        size_t imageBytes, const uint8_t sha[32], Print &log)
{
    if (!verifyBlob(blob, len, imageBytes, sha)) return BAD_SOURCE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        log.printf("nvs_open: %s\n", esp_err_to_name(err));
        return NVS_ERROR;
    }

    // Already there?
    size_t have = 0;
    if (nvs_get_blob(h, key, nullptr, &have) == ESP_OK && have == len) {
        uint8_t *cur = (uint8_t *)malloc(len);
        if (!cur) { nvs_close(h); return NO_MEMORY; }
        bool same = nvs_get_blob(h, key, cur, &have) == ESP_OK && memcmp(cur, blob, len) == 0;
        free(cur);
        if (same) { nvs_close(h); return ALREADY_INSTALLED; }
    }

    // Room?  An entry is 32 bytes; a blob also takes an index entry, plus a
    // header entry per page it spans (a page holds 126 entries).
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        size_t need = (len + 31) / 32 + 1 + (len / (126 * 32) + 1) * 2;
        log.printf("NVS: %u of %u entries free; this needs about %u\n",
                   (unsigned)st.free_entries, (unsigned)st.total_entries, (unsigned)need);
        if (st.free_entries < need + have / 32) {
            nvs_close(h);
            return NVS_FULL;
        }
    }

    err = nvs_set_blob(h, key, blob, len);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        log.printf("nvs_set_blob: %s\n", esp_err_to_name(err));
        nvs_close(h);
        return err == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? NVS_FULL : NVS_ERROR;
    }

    // Read back and check the way the board library will.
    Result r = VERIFY_FAILED;
    uint8_t *back = (uint8_t *)malloc(len);
    if (back) {
        size_t got = len;
        if (nvs_get_blob(h, key, back, &got) == ESP_OK && got == len &&
            memcmp(back, blob, len) == 0 && verifyBlob(back, got, imageBytes, sha)) {
            r = INSTALLED;
        }
        free(back);
    } else {
        r = NO_MEMORY;
    }
    nvs_close(h);
    return r;
}

// Remove a key (for testing the libraries' "not installed" path).
inline bool eraseBlob(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

}  // namespace p4c6fw
