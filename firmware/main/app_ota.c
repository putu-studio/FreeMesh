// Update over the internet ("OTA").
//
// The flash holds three programs and has no spare slot: freemesh | meshtastic | meshcore.
// A running program cannot overwrite itself, so FreeMesh updates itself by borrowing the MeshCore slot:
//
//   1. The old FreeMesh, in its own slot, downloads the new FreeMesh into the MeshCore slot and starts it there.
//   2. The new FreeMesh runs from the MeshCore slot. Once radio, Wi-Fi and web have come up and stayed up,
//      it copies itself into the FreeMesh slot and restarts from there.
//   3. Back in its own slot, it downloads MeshCore into the MeshCore slot again.
//
// One complete FreeMesh program is in the flash at every step, so the box still starts after a power loss.
// If the new version fails to come up in 3 tries, the box returns to the old one. Holding the D1 button
// for 5 s while the box starts (bootloader) also returns to the FreeMesh slot.
// Meshtastic and MeshCore are written straight into their own slots.
//
// A small signed text list (see tools/make-update.py) says what to install:
//   freemesh-update 1 <bundle number>
//   <program> <version> <size> <sha256> <file name>      (freemesh, meshtastic, meshcore)
//   sig <128 hex digits>
// The box checks the signature against the public key in app_ota_key.h, and each file's sha256 as it
// downloads. A program counts as installed when the sha256 of its slot matches the list.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "app_ota.h"
#include "app_ota_key.h"
#include "app_gw.h"
#include "app_io.h"
#include "app_web.h"

#define STAGE_NONE    0       // Nothing in progress.
#define STAGE_TRIAL   1       // A new FreeMesh is in the MeshCore slot and is being tried.
#define STAGE_COPY    2       // The new FreeMesh works and is copying itself home.
#define STAGE_FINISH  3       // A slot may be missing its program: one more download round is needed.

#define MAX_TRIES     3       // Starts a new version gets before the box goes back.
#define TRIAL_OK_S    20      // A new version must stay up this long to count as working.
#define TRIAL_MAX_S   120     // If it has not confirmed by then, the box restarts, which uses up one try.
#define JOIN_USER_S   600     // Wait for the internet Wi-Fi after "Update" is tapped: time to turn on a phone hotspot.
#define JOIN_BOOT_S   180     // Wait when the box continues by itself after a restart.
#define CHUNK         4096
#define OTA_DEFAULT_URL ""    // Default address of the update list. Empty: the owner sets it on the Update page.

typedef struct {              // Kept in NVS "ota" / "st".
    uint8_t  stage, tries;
    uint32_t bundle;          // Number of the newest bundle fully installed; older ones are refused.
    uint32_t try_bundle;      // Bundle being tried now.
    uint32_t bad_bundle;      // Bundle whose FreeMesh did not start on this box: that FreeMesh is skipped.
} ota_nv_t;

typedef struct { char name[12], version[24], sha[65], file[64]; uint32_t size; bool present; } entry_t;
typedef struct { uint32_t bundle; entry_t freemesh, meshtastic, meshcore; } list_t;

static ota_nv_t s_nv;
static char     s_url[160];
static char     s_msg[112];
static char     s_base[160];                    // List address up to the last '/': the files sit next to the list.
static const char *volatile s_state = "idle";
static const char *volatile s_what = "";
static volatile int  s_percent;
static volatile bool s_running;
static int      s_join_s = JOIN_USER_S;
static const esp_partition_t *s_home, *s_mt, *s_spare, *s_run;

// ---------- Small helpers ----------
static const esp_partition_t *part(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

static void nv_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "st", &s_nv, sizeof(s_nv));
    nvs_set_str(h, "url", s_url);
    nvs_set_str(h, "msg", s_msg);
    nvs_commit(h);
    nvs_close(h);
}

static void nv_load(void)
{
    memset(&s_nv, 0, sizeof(s_nv)); s_msg[0] = 0;
    strlcpy(s_url, OTA_DEFAULT_URL, sizeof(s_url));
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(s_nv);
    if (nvs_get_blob(h, "st", &s_nv, &l) != ESP_OK || l != sizeof(s_nv)) memset(&s_nv, 0, sizeof(s_nv));
    l = sizeof(s_url); if (nvs_get_str(h, "url", s_url, &l) != ESP_OK || !s_url[0]) strlcpy(s_url, OTA_DEFAULT_URL, sizeof(s_url));
    l = sizeof(s_msg); if (nvs_get_str(h, "msg", s_msg, &l) != ESP_OK) s_msg[0] = 0;
    nvs_close(h);
}

static void say(const char *text)               // Sets the result line on the Update page and logs it.
{
    strlcpy(s_msg, text, sizeof(s_msg));
    printf("Update: %s\n", s_msg);
}

static void ram(const char *when)
{
    printf("Update: RAM %s: %u KB free, biggest block %u KB\n", when,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

bool ota_is_freemesh_image(const esp_partition_t *p)
{
    esp_app_desc_t d;
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) return false;
    return strncmp(d.project_name, esp_app_get_description()->project_name, sizeof(d.project_name)) == 0;
}

bool ota_busy(void) { return s_running; }

// True when the update server is a computer or phone app on the box's own Wi-Fi, so no internet Wi-Fi is needed.
// The list must still carry a valid signature.
static bool url_local(const char *url)
{
    const char *h = strstr(url, "://");
    return h && strncmp(h + 3, "192.168.4.", 10) == 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex(const char *in, uint8_t *out, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        int a = hexval(in[2 * i]), b = a < 0 ? -1 : hexval(in[2 * i + 1]);
        if (b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}

// ---------- Signed list ----------
static bool sig_ok(const uint8_t *msg, size_t len, const uint8_t sig[64])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t k = 0;
    if (psa_import_key(&a, OTA_PUBKEY, sizeof(OTA_PUBKEY), &k) != PSA_SUCCESS) return false;
    psa_status_t st = psa_verify_message(k, PSA_ALG_ECDSA(PSA_ALG_SHA_256), msg, len, sig, 64);
    psa_destroy_key(k);
    return st == PSA_SUCCESS;
}

static bool safe_name(const char *s)             // File names: letters, digits, '.', '_' and '-' only, so no folders.
{
    if (!s[0]) return false;
    for (; *s; s++) {
        bool ok = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-';
        if (!ok) return false;
    }
    return true;
}

// Parses the whole downloaded list (0-terminated). Returns NULL when it is valid, otherwise what is wrong.
static const char *list_parse(char *text, list_t *out)
{
    memset(out, 0, sizeof(*out));
    char *sig = strstr(text, "\nsig ");
    if (!sig) return "The update list has no signature";
    size_t signed_len = (size_t)(sig + 1 - text);            // Up to and including the line break before "sig".
    uint8_t raw[64];
    if (strlen(sig + 5) < 128 || !unhex(sig + 5, raw, 64)) return "The update list's signature is damaged";
    if (!sig_ok((const uint8_t *)text, signed_len, raw)) return "The update list is not signed by FreeMesh. Nothing was changed";
    sig[1] = 0;                                              // Cut off the signature line, then read line by line.
    unsigned long fmt = 0, bundle = 0;
    char *save = NULL, *line = strtok_r(text, "\n", &save);
    if (!line || sscanf(line, "freemesh-update %lu %lu", &fmt, &bundle) != 2 || fmt != 1) return "The update list is in a format this version can't read";
    out->bundle = (uint32_t)bundle;
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        entry_t e = { 0 };
        unsigned long size = 0;
        if (sscanf(line, "%11s %23s %lu %64s %63s", e.name, e.version, &size, e.sha, e.file) != 5) continue;
        if (strlen(e.sha) != 64 || !safe_name(e.file) || size < 1024) return "The update list has a bad line";
        e.size = (uint32_t)size; e.present = true;
        if (strcmp(e.name, "freemesh") == 0) out->freemesh = e;
        else if (strcmp(e.name, "meshtastic") == 0) out->meshtastic = e;
        else if (strcmp(e.name, "meshcore") == 0) out->meshcore = e;
    }
    return NULL;
}

// ---------- Download ----------
// Opens the address and follows up to 3 redirects. Returns NULL on failure.
static esp_http_client_handle_t http_open(const char *url, int64_t *len)
{
    esp_http_client_config_t c = {
        .url = url, .timeout_ms = 15000, .buffer_size = 2048, .buffer_size_tx = 1536,
        .crt_bundle_attach = esp_crt_bundle_attach,          // For https: verify the server's certificate.
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    for (int hop = 0; hop < 4; hop++) {
        esp_err_t err = esp_http_client_open(h, 0);
        if (err != ESP_OK) { printf("Update: can't reach the server (%s)\n", esp_err_to_name(err)); break; }
        int64_t n = esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h);
        if (st == 200) { *len = n; return h; }
        if (st / 100 == 3 && esp_http_client_set_redirection(h) == ESP_OK) { esp_http_client_close(h); continue; }
        printf("Update: the server answered %d\n", st);
        break;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return NULL;
}

static void http_done(esp_http_client_handle_t h) { esp_http_client_close(h); esp_http_client_cleanup(h); }

// True when the sha256 of the slot's first e->size bytes matches the list: that exact program is installed.
static bool slot_matches(const esp_partition_t *p, const entry_t *e, uint8_t *buf)
{
    uint8_t want[32], got[32];
    if (!p || e->size > p->size || !unhex(e->sha, want, 32)) return false;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    for (uint32_t off = 0; off < e->size; off += CHUNK) {
        uint32_t n = e->size - off < CHUNK ? e->size - off : CHUNK;
        if (esp_partition_read(p, off, buf, n) != ESP_OK || psa_hash_update(&op, buf, n) != PSA_SUCCESS) { psa_hash_abort(&op); return false; }
        if ((off / CHUNK) % 64 == 0) vTaskDelay(1);          // Hashing 2 MB takes a while: let other tasks run.
    }
    size_t gl = 0;
    if (psa_hash_finish(&op, got, sizeof(got), &gl) != PSA_SUCCESS) return false;
    return memcmp(want, got, 32) == 0;
}

// Downloads one program into a slot. The slot counts as installed only when the size, the sha256 and the
// program's own checksum are all correct; otherwise its start is erased so it reads as "not installed".
static bool download(const esp_partition_t *p, const entry_t *e, uint8_t *buf)
{
    char url[240];
    if (snprintf(url, sizeof(url), "%s%s", s_base, e->file) >= (int)sizeof(url)) { say("The update address is too long"); return false; }
    uint8_t want[32], got[32];
    if (!p || e->size > p->size || !unhex(e->sha, want, 32)) { say("That program does not fit this box"); return false; }
    s_what = e->name; s_percent = 0; s_state = "downloading";
    printf("Update: downloading %s %s (%lu bytes)\n", e->name, e->version, (unsigned long)e->size);
    int64_t len = 0;
    esp_http_client_handle_t h = http_open(url, &len);
    if (!h) { say("Could not download the update. Is the internet working on that Wi-Fi?"); return false; }
    if (len > 0 && (uint32_t)len != e->size) { http_done(h); say("The download has the wrong size"); return false; }
    esp_ota_handle_t oh = 0;
    if (esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &oh) != ESP_OK) { http_done(h); say("Could not write to the flash"); return false; }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    uint32_t total = 0;
    const char *bad = NULL;
    while (total < e->size) {
        int n = esp_http_client_read(h, (char *)buf, CHUNK);
        if (n <= 0) { bad = "The download stopped half way. Try again"; break; }
        if (total + n > e->size) { bad = "The download has the wrong size"; break; }
        psa_hash_update(&op, buf, n);
        if (esp_ota_write(oh, buf, n) != ESP_OK) { bad = "Could not write to the flash"; break; }
        total += n;
        s_percent = (int)((uint64_t)total * 100 / e->size);
    }
    http_done(h);
    size_t gl = 0;
    if (bad) psa_hash_abort(&op);
    else if (psa_hash_finish(&op, got, sizeof(got), &gl) != PSA_SUCCESS || memcmp(want, got, 32) != 0) bad = "The download was damaged on the way. Try again";
    if (bad) esp_ota_abort(oh);
    else if (esp_ota_end(oh) != ESP_OK) bad = "The downloaded file is not a working program";
    if (bad) {
        esp_partition_erase_range(p, 0, 4096);               // A partly written program must not look installed.
        say(bad);
        return false;
    }
    printf("Update: %s %s is in its slot\n", e->name, e->version);
    return true;
}

// ---------- The new FreeMesh copies itself home ----------
static bool copy_home(uint8_t *buf)
{
    esp_partition_pos_t pos = { .offset = s_run->address, .size = s_run->size };
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len == 0 || meta.image_len > s_home->size) return false;
    esp_ota_handle_t oh = 0;
    if (esp_ota_begin(s_home, OTA_WITH_SEQUENTIAL_WRITES, &oh) != ESP_OK) return false;
    for (uint32_t off = 0; off < meta.image_len; off += CHUNK) {
        uint32_t n = meta.image_len - off < CHUNK ? meta.image_len - off : CHUNK;
        if (esp_partition_read(s_run, off, buf, n) != ESP_OK || esp_ota_write(oh, buf, n) != ESP_OK) { esp_ota_abort(oh); return false; }
        s_percent = (int)((uint64_t)off * 100 / meta.image_len);
    }
    return esp_ota_end(oh) == ESP_OK;                        // Also verifies the copy's checksum.
}

static void trial_task(void *arg)
{
    s_running = true; s_state = "finishing"; s_what = "freemesh"; s_percent = 0;
    vTaskDelay(pdMS_TO_TICKS(TRIAL_OK_S * 1000));            // Everything started and stayed up: the version works.
    s_nv.stage = STAGE_COPY; s_nv.tries = 0; nv_save();
    printf("Update: the new version works. Copying it into the FreeMesh slot\n");
    uint8_t *buf = malloc(CHUNK);
    bool ok = false;
    for (int t = 0; buf && t < 3 && !ok; t++) ok = copy_home(buf);
    free(buf);
    if (!ok) {                                               // Still runs from the MeshCore slot; next start retries.
        say("The new version runs, but copying it into place failed. Restart the box to try again");
        s_state = "failed"; s_running = false; io_notify(IO_EVT_ERROR);
        vTaskDelete(NULL);
    }
    s_nv.stage = STAGE_FINISH; s_nv.bundle = s_nv.try_bundle;
    say("FreeMesh is updated. Putting MeshCore back needs the internet Wi-Fi once more");
    nv_save();
    esp_ota_set_boot_partition(s_home);
    s_state = "restarting";
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void trial_too_slow(void *arg)                        // Fires TRIAL_MAX_S after start if still not confirmed.
{
    if (s_nv.stage == STAGE_TRIAL) { printf("Update: the new version did not come up in time, restarting\n"); esp_restart(); }
}

// ---------- Download what is missing ----------
static bool wait_online(int seconds)
{
    s_state = "joining";
    gw_wifi_need(true);
    for (int s = 0; s < seconds; s++) {
        if (gw_online()) return true;
        if (s % 10 == 9) gw_wifi_retry();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return gw_online();
}

static bool fetch_list(char *text, int max)
{
    int64_t len = 0;
    esp_http_client_handle_t h = http_open(s_url, &len);
    if (!h) return false;
    int total = 0;
    while (total < max - 1) {
        int n = esp_http_client_read(h, text + total, max - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    http_done(h);
    text[total] = 0;
    return total > 0;
}

static void run_task(void *arg)
{
    bool ok = false, restart = false;
    uint8_t *buf = malloc(CHUNK);
    char *text = malloc(1536);
    list_t *list = malloc(sizeof(list_t));
    vTaskDelay(pdMS_TO_TICKS(700));                          // Let the page get its answer first.
    web_https_pause(true);                                   // Https to phones and a download do not both fit in RAM.
    ram("before the update");
    if (!buf || !text || !list) { say("Not enough memory for the update right now. Restart the box and try again"); goto out; }

    bool local = url_local(s_url);
    if (local) printf("Update: the server is on the box's own Wi-Fi, no internet Wi-Fi needed\n");
    else {
        gw_status_t gs; gw_get(&gs);
        printf("Update: joining \"%s\" (waiting up to %d s)\n", gs.ssid, s_join_s);
        if (!wait_online(s_join_s)) {
            char m[112]; snprintf(m, sizeof(m), "Could not join the Wi-Fi \"%.32s\". Check the name and password", gs.ssid);
            say(m); goto out;
        }
    }
    s_state = "checking";
    strlcpy(s_base, s_url, sizeof(s_base));
    char *slash = strrchr(s_base, '/');
    if (slash) slash[1] = 0;
    bool got = false;                                        // Retry: the Wi-Fi may still be settling after a restart.
    for (int t = 0; t < (local ? 12 : 3) && !got; t++) {
        if (t) vTaskDelay(pdMS_TO_TICKS(5000));
        got = fetch_list(text, 1536);
    }
    if (!got) { say("Could not get the update list. Is the internet working on that Wi-Fi?"); goto out; }
    ram("after the list");
    const char *bad = list_parse(text, list);
    if (bad) { say(bad); goto out; }
    printf("Update: list number %lu (this box has %lu)\n", (unsigned long)list->bundle, (unsigned long)s_nv.bundle);
    if (list->bundle < s_nv.bundle) { say("That update is older than what this box already has"); goto out; }

    bool changed = false, skipped_bad = false;
    // Meshtastic first: its slot is not needed for anything else.
    if (list->meshtastic.present && !slot_matches(s_mt, &list->meshtastic, buf)) {
        s_nv.stage = STAGE_FINISH; nv_save();                // From here a slot may be half written until done.
        if (!download(s_mt, &list->meshtastic, buf)) goto out;
        changed = true;
    }
    // FreeMesh itself: into the MeshCore slot, then restart into it (see the top of this file).
    if (list->freemesh.present && !slot_matches(s_home, &list->freemesh, buf)) {
        if (list->bundle == s_nv.bad_bundle) skipped_bad = true;
        else {
            s_nv.stage = STAGE_FINISH; nv_save();
            if (!download(s_spare, &list->freemesh, buf)) goto out;
            if (!ota_is_freemesh_image(s_spare)) { esp_partition_erase_range(s_spare, 0, 4096); say("The downloaded FreeMesh is not a FreeMesh program"); goto out; }
            s_nv.stage = STAGE_TRIAL; s_nv.tries = 0; s_nv.try_bundle = list->bundle;
            char m[112]; snprintf(m, sizeof(m), "Trying FreeMesh %.23s. The box restarts twice", list->freemesh.version);
            say(m); nv_save();
            if (esp_ota_set_boot_partition(s_spare) != ESP_OK) { s_nv.stage = STAGE_FINISH; say("Could not start the new version"); nv_save(); goto out; }
            restart = true; ok = true; goto out;
        }
    }
    // MeshCore last, and always after a FreeMesh update because its slot was borrowed.
    if (list->meshcore.present && !slot_matches(s_spare, &list->meshcore, buf)) {
        s_nv.stage = STAGE_FINISH; nv_save();
        if (!download(s_spare, &list->meshcore, buf)) goto out;
        changed = true;
    }
    s_nv.stage = STAGE_NONE;
    if (!skipped_bad) s_nv.bundle = list->bundle;
    char m[112];
    if (skipped_bad) snprintf(m, sizeof(m), "FreeMesh %.23s did not start on this box and was skipped. The rest is up to date", list->freemesh.version);
    else snprintf(m, sizeof(m), "%s FreeMesh %.23s, Meshtastic %.12s, MeshCore %.12s", changed ? "Updated." : "Up to date.",
                  esp_app_get_description()->version, list->meshtastic.version, list->meshcore.version);
    say(m); nv_save();
    ok = true;

out:
    ram("at the end");
    free(buf); free(text); free(list);
    if (restart) { s_state = "restarting"; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }
    if (!ok) { nv_save(); io_notify(IO_EVT_ERROR); }
    gw_wifi_need(false);                                     // Leave the internet Wi-Fi unless the optional gateway add-on uses it.
    web_https_pause(false);                                  // Turn https back on.
    s_state = ok ? "done" : "failed"; s_what = ""; s_running = false;
    vTaskDelete(NULL);
}

static esp_err_t start_run(int join_seconds)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_join_s = join_seconds;
    s_running = true;
    // 12 KB stack: enough for TLS to the update server and the signature check.
    if (xTaskCreate(run_task, "update", 12288, NULL, 4, NULL) != pdPASS) { s_running = false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

// ---------- For the rest of the box ----------
void ota_boot_check(void)
{
    nv_load();
    s_home = part("freemesh"); s_mt = part("meshtastic"); s_spare = part("meshcore");
    s_run = esp_ota_get_running_partition();
    if (!s_home || !s_spare || !s_run) return;               // Old single-slot layout: no updates.
    bool at_home = s_run->address == s_home->address;
    if (!at_home && s_nv.stage == STAGE_TRIAL) {
        s_nv.tries++;
        if (s_nv.tries > MAX_TRIES) {
            s_nv.bad_bundle = s_nv.try_bundle; s_nv.stage = STAGE_FINISH; s_nv.tries = 0;
            say("The new FreeMesh did not start. The box went back to the old one");
            nv_save();
            esp_ota_set_boot_partition(s_home);
            esp_restart();
        }
        nv_save();
        printf("Update: trying the new version, start %u of %u\n", s_nv.tries, MAX_TRIES);
        const esp_timer_create_args_t t = { .callback = trial_too_slow, .name = "trial" };
        esp_timer_handle_t th;
        if (esp_timer_create(&t, &th) == ESP_OK) esp_timer_start_once(th, (uint64_t)TRIAL_MAX_S * 1000000);
    } else if (at_home && (s_nv.stage == STAGE_TRIAL || s_nv.stage == STAGE_COPY)) {
        // Back in the FreeMesh slot before the new version finished: D1 was held, or the bootloader refused it.
        if (s_nv.stage == STAGE_TRIAL) { s_nv.bad_bundle = s_nv.try_bundle; say("The new FreeMesh did not start. The box went back to the old one"); }
        s_nv.stage = STAGE_FINISH; s_nv.tries = 0;
        nv_save();
    }
}

void ota_start(void)
{
    if (!s_home || !s_spare || !s_run) return;
    printf("Update: FreeMesh %s, bundle %lu%s\n", esp_app_get_description()->version, (unsigned long)s_nv.bundle,
           s_nv.stage == STAGE_NONE ? "" : " (an update is not finished)");
    if (s_run->address != s_home->address) {                 // This is the new version, running from the MeshCore slot.
        xTaskCreate(trial_task, "update", 6144, NULL, 4, NULL);
        return;
    }
    if (s_nv.stage == STAGE_FINISH && s_url[0]) {
        gw_status_t gs; gw_get(&gs);
        // Continue without the user. If the Wi-Fi is not there, the page offers "Update" again.
        if (gs.ssid[0] || url_local(s_url)) start_run(JOIN_BOOT_S);
    }
}

esp_err_t ota_begin(const char *ssid, const char *pass, const char *url)
{
    if (!s_home || !s_spare || !s_mt) return ESP_ERR_NOT_SUPPORTED;
    // Refuse while an update runs, or while FreeMesh still runs from the MeshCore slot.
    if (s_running || s_run->address != s_home->address) return ESP_ERR_INVALID_STATE;
    if (url[0]) {
        if ((strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) || strlen(url) >= sizeof(s_url)) return ESP_ERR_INVALID_SIZE;
        strlcpy(s_url, url, sizeof(s_url));
        nv_save();
    }
    if (!s_url[0]) return ESP_ERR_INVALID_ARG;
    if (ssid[0]) gw_wifi_set(ssid, pass);
    gw_status_t gs; gw_get(&gs);
    if (!gs.ssid[0] && !url_local(s_url)) return ESP_ERR_NOT_FOUND;
    return start_run(JOIN_USER_S);
}

void ota_get(ota_status_t *st)
{
    memset(st, 0, sizeof(*st));
    strlcpy(st->state, s_state, sizeof(st->state));
    strlcpy(st->what, s_what, sizeof(st->what));
    st->percent = s_percent;
    strlcpy(st->msg, s_msg, sizeof(st->msg));
    strlcpy(st->version, esp_app_get_description()->version, sizeof(st->version));
    strlcpy(st->url, s_url, sizeof(st->url));
    st->unfinished = s_nv.stage != STAGE_NONE;
}
