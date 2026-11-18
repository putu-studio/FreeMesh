// app_ble.c: Bluetooth LE tunnel between the FreeMesh phone app and this box.
//
// How it works:
//   1. The phone writes a request to REQ in small pieces, for example "GET /api/messages?since=7\n0\n".
//   2. A worker task replays that request to the box's own web server at http://127.0.0.1,
//      so the /api code in app_web.c serves the web page and the app alike.
//   3. The answer returns to the phone in small pieces on RSP. EVT carries a "something changed" tick.
//
// Piece format (both directions): byte 0 = request number (0..255), byte 1 = flags, then the data.
//   Flag bit 0 (0x01) marks the first piece of a request or answer, bit 1 (0x02) the last piece.
// Request data = "METHOD PATH\nBODYLENGTH\n" followed by the body bytes.
// Answer data  = "STATUS\n" followed by the answer body (JSON, text or a .wav file).

#include "app_ble.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_random.h"
#include "esp_system.h"                // For esp_restart().
#include "esp_http_client.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "app_settings.h"
#include "app_chat.h"

void ble_store_config_init(void);            // Defined in NimBLE's store/config; saves paired phones in NVS.

#define MAX_PHONES   3                       // Phones connected at the same time (CONFIG_BT_NIMBLE_MAX_CONNECTIONS).
#define PIECE_MAX    514                     // Largest write a phone can send (MTU 517 - 3).
#define BODY_BUF     1024                    // Bodies up to this size stay in RAM; larger ones (voice) are streamed.
#define IDLE_MS      15000                   // A half-sent request older than this is dropped.
#define F_FIRST      0x01
#define F_LAST       0x02

// ---------- 128-bit IDs (the app uses the same ones) ----------
// Service 7e1a0000-3c5b-4f2e-9d61-2b8f4a6c0d91, REQ ...0001, RSP ...0002, EVT ...0003.
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0x91, 0x0d, 0x6c, 0x4a, 0x8f, 0x2b, 0x61, 0x9d, 0x2e, 0x4f, 0x5b, 0x3c, 0x00, 0x00, 0x1a, 0x7e);
static const ble_uuid128_t REQ_UUID = BLE_UUID128_INIT(0x91, 0x0d, 0x6c, 0x4a, 0x8f, 0x2b, 0x61, 0x9d, 0x2e, 0x4f, 0x5b, 0x3c, 0x01, 0x00, 0x1a, 0x7e);
static const ble_uuid128_t RSP_UUID = BLE_UUID128_INIT(0x91, 0x0d, 0x6c, 0x4a, 0x8f, 0x2b, 0x61, 0x9d, 0x2e, 0x4f, 0x5b, 0x3c, 0x02, 0x00, 0x1a, 0x7e);
static const ble_uuid128_t EVT_UUID = BLE_UUID128_INIT(0x91, 0x0d, 0x6c, 0x4a, 0x8f, 0x2b, 0x61, 0x9d, 0x2e, 0x4f, 0x5b, 0x3c, 0x03, 0x00, 0x1a, 0x7e);

static uint16_t s_rsp_h, s_evt_h;            // Handles NimBLE assigns to the RSP and EVT values.
static uint8_t  s_own_addr_type;
static uint32_t s_pin;                       // 6-digit pairing PIN.
static bool     s_on;                        // Bluetooth is turned on in Settings (read at start).
static volatile uint32_t s_evt_ver;          // Last chat_version() reported to the phones.

// ---------- Connected phones (written by the NimBLE task, read by the worker) ----------
typedef struct {
    bool     used;
    uint16_t conn;                           // NimBLE connection handle.
    uint16_t mtu;                            // ATT MTU: the largest piece plus 3 header bytes.
    bool     secure;                         // Paired with the PIN and encrypted.
    bool     evt_on;                         // The phone subscribed to EVT.
} phone_t;
static phone_t s_phones[MAX_PHONES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // Short lock for s_phones.

// One piece handed from the NimBLE task to the worker.
typedef struct {
    uint16_t conn;
    uint16_t len;                            // 0 means "this phone left".
    uint8_t  data[PIECE_MAX];
} piece_t;
static QueueHandle_t s_q;

static void advertise(void);

static phone_t *phone_find(uint16_t conn)    // Call inside the lock.
{
    for (int i = 0; i < MAX_PHONES; i++) if (s_phones[i].used && s_phones[i].conn == conn) return &s_phones[i];
    return NULL;
}

int ble_phones_connected(void)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_PHONES; i++) n += s_phones[i].used;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

// ---------- PIN in flash (NVS namespace "lora", key "blepin") ----------
static void pin_load(void)
{
    nvs_handle_t h;
    s_pin = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) { s_pin = 123456; return; }
    if (nvs_get_u32(h, "blepin", &s_pin) != ESP_OK || s_pin < 100000 || s_pin > 999999) {
        s_pin = 100000 + esp_random() % 900000;          // First start: pick a random PIN.
        nvs_set_u32(h, "blepin", s_pin);
        nvs_commit(h);
    }
    nvs_close(h);
}

uint32_t ble_pin(void) { return s_pin; }

esp_err_t ble_set_pin(uint32_t pin)
{
    if (pin < 100000 || pin > 999999) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open("lora", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u32(h, "blepin", pin);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) s_pin = pin;                        // Phones already paired keep working.
    return e;
}

void ble_forget_phones(void) { if (s_on) ble_store_clear(); }

// ---------- Bluetooth on/off (NVS "bleon", default off) ----------
// Bluetooth uses about 65 KB of RAM and only the FreeMesh app needs it, so a new box starts with it off
// and https gets that RAM. App users turn it on in Settings > Bluetooth. Changing it restarts the box.
static bool on_load(void)
{
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "bleon", &v); nvs_close(h); }
    return v != 0;
}

static void restart_soon(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }   // Lets the page get its answer.

static esp_err_t on_save(bool on)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("lora", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "bleon", on ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// ---------- GATT: what the phone can read and write ----------
static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {      // A piece arrived on REQ.
        static piece_t p;                                // Static: only the NimBLE task runs this.
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, p.data, sizeof(p.data), &len) != 0 || len < 2)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        p.conn = conn;
        p.len = len;
        // Wait up to 2 s if the worker is busy. The phone waits for the write to be confirmed, so it slows down too.
        if (xQueueSend(s_q, &p, pdMS_TO_TICKS(2000)) != pdTRUE) return BLE_ATT_ERR_UNLIKELY;
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {       // A read of EVT returns "m:<version>".
        char t[16];
        int n = snprintf(t, sizeof(t), "m:%lu", (unsigned long)s_evt_ver);
        return os_mbuf_append(ctxt->om, t, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   // REQ: only a phone paired with the PIN (encrypted and authenticated) may write.
                .uuid = &REQ_UUID.u, .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {   // RSP: answers.
                .uuid = &RSP_UUID.u, .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_rsp_h,
            },
            {   // EVT: "m:<n>" when messages or ticks change.
                .uuid = &EVT_UUID.u, .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_evt_h,
            },
            { 0 }                                        // End of the list.
        },
    },
    { 0 }
};

// ---------- GAP: connect, disconnect, pairing ----------
static void check_secure(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    bool ok = ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated;
    taskENTER_CRITICAL(&s_mux);
    phone_t *p = phone_find(conn);
    if (p) p->secure = ok;
    taskEXIT_CRITICAL(&s_mux);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            taskENTER_CRITICAL(&s_mux);
            for (int i = 0; i < MAX_PHONES; i++) if (!s_phones[i].used) {
                s_phones[i] = (phone_t){ .used = true, .conn = ev->connect.conn_handle, .mtu = 23 };
                break;
            }
            taskEXIT_CRITICAL(&s_mux);
            printf("BLE: phone connected (%d now)\n", ble_phones_connected());
            ble_gap_security_initiate(ev->connect.conn_handle);   // Ask for the PIN, or use the saved pairing.
        }
        if (ble_phones_connected() < MAX_PHONES) advertise();      // Let more phones find the box.
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t c = ev->disconnect.conn.conn_handle;
        taskENTER_CRITICAL(&s_mux);
        phone_t *p = phone_find(c);
        if (p) p->used = false;
        taskEXIT_CRITICAL(&s_mux);
        piece_t *gone = calloc(1, sizeof(piece_t));               // Tell the worker to drop this phone's request.
        if (gone) { gone->conn = c; gone->len = 0; xQueueSend(s_q, gone, pdMS_TO_TICKS(100)); free(gone); }
        printf("BLE: phone left (reason %d)\n", ev->disconnect.reason);
        advertise();
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        taskENTER_CRITICAL(&s_mux);
        { phone_t *p = phone_find(ev->mtu.conn_handle); if (p) p->mtu = ev->mtu.value; }
        taskEXIT_CRITICAL(&s_mux);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_evt_h) {
            taskENTER_CRITICAL(&s_mux);
            phone_t *p = phone_find(ev->subscribe.conn_handle);
            if (p) p->evt_on = ev->subscribe.cur_notify;
            taskEXIT_CRITICAL(&s_mux);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        check_secure(ev->enc_change.conn_handle);
        printf("BLE: pairing %s\n", ev->enc_change.status == 0 ? "OK" : "failed (wrong PIN?)");
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {    // The phone asks for the PIN: supply ours.
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = s_pin };
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {                         // The phone lost its pairing: forget ours, pair again.
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (ble_gap_adv_active()) return;
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID;                     // The app scans for this ID.
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;

    struct ble_hs_adv_fields r = { 0 };                          // Scan response: the box name.
    const char *name = ble_svc_gap_device_name();
    r.name = (const uint8_t *)name;
    r.name_len = strlen(name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params a = { 0 };
    a.conn_mode = BLE_GAP_CONN_MODE_UND;
    a.disc_mode = BLE_GAP_DISC_MODE_GEN;
    a.itvl_min = BLE_GAP_ADV_ITVL_MS(200);                       // 200-300 ms saves power; still found in about 1 s.
    a.itvl_max = BLE_GAP_ADV_ITVL_MS(300);
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &a, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    printf("BLE: on, name \"%s\", PIN %06lu\n", ble_svc_gap_device_name(), (unsigned long)s_pin);
}

static void on_reset(int reason) { printf("BLE: reset, reason %d\n", reason); }

static void host_task(void *arg)
{
    nimble_port_run();                                           // Runs until nimble_port_stop().
    nimble_port_freertos_deinit();
}

// ---------- Worker: turns pieces into web requests ----------
enum { ST_IDLE, ST_HEAD, ST_BODY, ST_DROP };
typedef struct {
    bool     used;
    uint16_t conn;
    uint8_t  rid;                            // Request number from the phone.
    int      stage;
    char     head[300]; int head_len;        // "METHOD PATH\nLEN\n" while it arrives.
    char     method[6], path[256];
    uint32_t body_len, got;
    uint8_t  body[BODY_BUF];                 // Small bodies wait here.
    esp_http_client_handle_t stream;         // Large bodies (voice) go straight to the web server.
    TickType_t last;
    // Answer being built.
    uint8_t  out[PIECE_MAX]; int out_len; bool out_first;
} req_t;
static req_t s_req[MAX_PHONES];
static req_t *s_streaming;                   // Only one large upload runs at a time.

static req_t *req_for(uint16_t conn, bool make)
{
    for (int i = 0; i < MAX_PHONES; i++) if (s_req[i].used && s_req[i].conn == conn) return &s_req[i];
    if (!make) return NULL;
    for (int i = 0; i < MAX_PHONES; i++) if (!s_req[i].used) {
        memset(&s_req[i], 0, sizeof(req_t));
        s_req[i].used = true; s_req[i].conn = conn;
        return &s_req[i];
    }
    return NULL;
}

static void req_reset(req_t *r)
{
    if (r->stream) { esp_http_client_cleanup(r->stream); r->stream = NULL; }
    if (s_streaming == r) s_streaming = NULL;
    r->stage = ST_IDLE; r->head_len = 0; r->got = 0; r->body_len = 0;
}

static int notify(uint16_t conn, uint16_t h, const uint8_t *p, size_t n)
{
    for (int tries = 0; tries < 300; tries++) {                  // Retry for about 3 s if the radio queue is full.
        struct os_mbuf *om = ble_hs_mbuf_from_flat(p, n);
        if (om) {
            int rc = ble_gatts_notify_custom(conn, h, om);      // Takes ownership of om, even on error.
            if (rc == 0) return 0;
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return rc;   // Any other error, such as the phone leaving.
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return -1;
}

static int piece_size(uint16_t conn)
{
    uint16_t mtu = 23;
    taskENTER_CRITICAL(&s_mux);
    phone_t *p = phone_find(conn);
    if (p) mtu = p->mtu;
    taskEXIT_CRITICAL(&s_mux);
    if (mtu > PIECE_MAX + 3) mtu = PIECE_MAX + 3;
    return mtu - 3;                                              // The ATT header takes 3 bytes.
}

static int out_flush(req_t *r, bool last)
{
    r->out[0] = r->rid;
    r->out[1] = (r->out_first ? F_FIRST : 0) | (last ? F_LAST : 0);
    r->out_first = false;
    int rc = notify(r->conn, s_rsp_h, r->out, r->out_len);
    r->out_len = 2;
    return rc;
}

static int out_add(req_t *r, const void *data, size_t n)         // Add answer bytes and send each full piece.
{
    const uint8_t *d = data;
    int max = piece_size(r->conn);
    while (n > 0) {
        size_t room = max - r->out_len;
        size_t k = n < room ? n : room;
        memcpy(r->out + r->out_len, d, k);
        r->out_len += k; d += k; n -= k;
        if (r->out_len == max && out_flush(r, false) != 0) return -1;
    }
    return 0;
}

static void answer_begin(req_t *r, int status)
{
    r->out_len = 2; r->out_first = true;
    char s[8];
    int n = snprintf(s, sizeof(s), "%d\n", status);
    out_add(r, s, n);
}

static void answer_text(req_t *r, int status, const char *text)
{
    answer_begin(r, status);
    out_add(r, text, strlen(text));
    out_flush(r, true);
}

static esp_http_client_handle_t open_local(req_t *r)             // Start "METHOD http://127.0.0.1PATH".
{
    char url[300];
    snprintf(url, sizeof(url), "http://127.0.0.1%s", r->path);
    esp_http_client_config_t c = {
        .url = url,
        .method = strcmp(r->method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    if (c.method == HTTP_METHOD_POST)
        esp_http_client_set_header(h, "Content-Type",
            strncmp(r->path, "/api/voice", 10) == 0 ? "application/octet-stream" : "application/x-www-form-urlencoded");
    if (esp_http_client_open(h, r->body_len) != ESP_OK) { esp_http_client_cleanup(h); return NULL; }
    return h;
}

static void finish(req_t *r, esp_http_client_handle_t h)        // Read the web answer and send it to the phone.
{
    if (esp_http_client_fetch_headers(h) < 0) {                  // The web server did not answer.
        esp_http_client_cleanup(h); answer_text(r, 502, "The box didn't answer"); return;
    }
    int status = esp_http_client_get_status_code(h);
    answer_begin(r, status);
    static char buf[1024];                                       // Static: only the worker runs this.
    int n, ok = 0;
    while ((n = esp_http_client_read(h, buf, sizeof(buf))) > 0) {
        if (out_add(r, buf, n) != 0) { ok = -1; break; }         // The phone left: stop.
    }
    if (ok == 0) out_flush(r, true);
    esp_http_client_cleanup(h);
}

static void run_small(req_t *r)                                  // The whole body is in r->body.
{
    esp_http_client_handle_t h = open_local(r);
    if (!h) { answer_text(r, 500, "The box couldn't run that"); return; }
    if (r->body_len && esp_http_client_write(h, (const char *)r->body, r->body_len) != (int)r->body_len) {
        esp_http_client_cleanup(h); answer_text(r, 500, "The box couldn't run that"); return;
    }
    finish(r, h);
}

// Looks for "METHOD PATH\nLEN\n". Returns where the body starts, or 0 while the header is incomplete.
static int head_done(req_t *r)
{
    int nl1 = -1, end = 0;
    for (int i = 0; i < r->head_len; i++) {
        if (r->head[i] != '\n') continue;
        if (nl1 < 0) nl1 = i;                                   // End of line 1.
        else { end = i + 1; break; }                            // End of line 2, which ends the header.
    }
    if (!end) return 0;
    r->head[nl1] = 0;                                           // Cut the lines into C strings.
    r->head[end - 1] = 0;
    r->method[0] = 0; r->path[0] = 0;
    char *sp = strchr(r->head, ' ');                            // For example "POST /api/send".
    if (sp && sp - r->head < (int)sizeof(r->method)) {
        *sp = 0;
        strlcpy(r->method, r->head, sizeof(r->method));
        strlcpy(r->path, sp + 1, sizeof(r->path));
    }
    r->body_len = strtoul(r->head + nl1 + 1, NULL, 10);
    return end;
}

static void feed(req_t *r, const uint8_t *d, size_t n);

static void start_body(req_t *r, const uint8_t *rest, size_t rest_n)
{
    bool ok_method = strcmp(r->method, "GET") == 0 || strcmp(r->method, "POST") == 0;
    bool ok_path = strncmp(r->path, "/api/", 5) == 0 && !strchr(r->path, ' ');
    if (!ok_method || !ok_path) { answer_text(r, 400, "Bad request"); r->stage = ST_DROP; }
    else if (s_streaming && s_streaming != r) {                  // A voice upload is running: the phone retries.
        answer_text(r, 503, "Busy, try again in a moment"); r->stage = ST_DROP;
    }
    else if (r->body_len > BODY_BUF) {                           // Large body: stream it.
        if (r->body_len > 200000) { answer_text(r, 413, "Too big"); r->stage = ST_DROP; }
        else {
            r->stream = open_local(r);
            if (!r->stream) { answer_text(r, 500, "The box couldn't run that"); r->stage = ST_DROP; }
            else { s_streaming = r; r->stage = ST_BODY; }
        }
    }
    else r->stage = ST_BODY;
    r->got = 0;
    if (r->body_len == 0) {                                      // Nothing more to wait for.
        if (r->stage == ST_BODY) run_small(r);
        req_reset(r);
        return;
    }
    if (rest_n) feed(r, rest, rest_n);
}

static void feed(req_t *r, const uint8_t *d, size_t n)          // Body bytes arrived.
{
    size_t want = r->body_len - r->got;
    if (n > want) n = want;
    if (r->stage == ST_BODY) {
        if (r->stream) {
            if (esp_http_client_write(r->stream, (const char *)d, n) != (int)n) {
                answer_text(r, 500, "Upload failed"); req_reset(r); return;
            }
        } else memcpy(r->body + r->got, d, n);
    }
    r->got += n;
    if (r->got < r->body_len) return;
    if (r->stage == ST_BODY) {                                   // The body is complete: run it and answer.
        if (r->stream) { esp_http_client_handle_t h = r->stream; r->stream = NULL; s_streaming = NULL; finish(r, h); }
        else run_small(r);
    }
    req_reset(r);
}

static void on_piece(const piece_t *p)
{
    req_t *r = req_for(p->conn, p->len > 0);
    if (!r) return;
    if (p->len == 0) { req_reset(r); r->used = false; return; }  // The phone left.
    r->last = xTaskGetTickCount();
    uint8_t rid = p->data[0], flags = p->data[1];
    const uint8_t *d = p->data + 2;
    size_t n = p->len - 2;

    if (flags & F_FIRST) {                                       // A new request starts.
        req_reset(r);
        r->rid = rid; r->stage = ST_HEAD;
    } else if (r->stage == ST_IDLE || rid != r->rid) return;     // Ignore a stray piece.

    if (r->stage == ST_HEAD) {
        size_t room = sizeof(r->head) - 1 - r->head_len;
        size_t k = n < room ? n : room;
        int before = r->head_len;                               // Header bytes from earlier pieces.
        memcpy(r->head + r->head_len, d, k);
        r->head_len += k;
        int end = head_done(r);
        if (!end) {
            if (r->head_len >= (int)sizeof(r->head) - 1) { answer_text(r, 400, "Bad request"); req_reset(r); }
            return;
        }
        size_t rest_from = (size_t)(end - before);              // Where the body starts inside this piece.
        start_body(r, d + rest_from, n - rest_from);
        return;
    }
    if (r->stage == ST_BODY || r->stage == ST_DROP) feed(r, d, n);
}

static void tell_phones(void)                                   // Send EVT "m:<n>" to every paired, subscribed phone.
{
    char t[16];
    int n = snprintf(t, sizeof(t), "m:%lu", (unsigned long)s_evt_ver);
    uint16_t list[MAX_PHONES]; int k = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_PHONES; i++)
        if (s_phones[i].used && s_phones[i].secure && s_phones[i].evt_on) list[k++] = s_phones[i].conn;
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < k; i++) notify(list[i], s_evt_h, (const uint8_t *)t, n);
}

static void worker(void *arg)
{
    static piece_t p;                                            // Static: large, and only this task uses it.
    while (1) {
        if (xQueueReceive(s_q, &p, pdMS_TO_TICKS(300)) == pdTRUE) on_piece(&p);
        uint32_t v = chat_version();                             // Changes on a new message or tick.
        if (v != s_evt_ver) { s_evt_ver = v; tell_phones(); }
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_PHONES; i++)                     // Drop requests a phone never finished.
            if (s_req[i].used && s_req[i].stage != ST_IDLE && now - s_req[i].last > pdMS_TO_TICKS(IDLE_MS))
                req_reset(&s_req[i]);
    }
}

// ---------- Start ----------
void ble_start(void)
{
    pin_load();
    s_on = on_load();
    if (!s_on) { printf("BLE: off (Settings > Bluetooth turns it on)\n"); return; }
    s_q = xQueueCreate(8, sizeof(piece_t));                      // 8 x 518 B = about 4 KB.
    if (nimble_port_init() != ESP_OK) { printf("BLE: init failed\n"); return; }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;              // The box supplies the PIN; the phone user types it.
    ble_hs_cfg.sm_bonding = 1;                                   // Remember the phone, so the PIN is needed only once.
    ble_hs_cfg.sm_mitm = 1;                                      // Require the PIN: no "just works" pairing.
    ble_hs_cfg.sm_sc = 1;                                        // LE Secure Connections (ECDH keys).
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_svcs) != 0 || ble_gatts_add_svcs(s_svcs) != 0) { printf("BLE: service failed\n"); return; }
    ble_svc_gap_device_name_set(device_settings.devicename);     // Phones see the box name, for example "LoRa A1B2".
    ble_store_config_init();

    s_evt_ver = chat_version();
    xTaskCreate(worker, "ble_work", 8192, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}

// ---------- Web page: GET/POST /api/ble ----------
esp_err_t ble_http_get(httpd_req_t *req)
{
    char j[80];
    snprintf(j, sizeof(j), "{\"on\":%s,\"pin\":\"%06lu\",\"phones\":%d}", s_on ? "true" : "false", (unsigned long)s_pin, ble_phones_connected());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

esp_err_t ble_http_post(httpd_req_t *req)
{
    char body[64], act[10] = "", v[10] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = 0;
    httpd_query_key_value(body, "do", act, sizeof(act));
    httpd_query_key_value(body, "pin", v, sizeof(v));
    if (strcmp(act, "forget") == 0) { ble_forget_phones(); return ble_http_get(req); }
    if (strcmp(act, "on") == 0 || strcmp(act, "off") == 0) {
        bool want = strcmp(act, "on") == 0;
        if (want != s_on) {
            if (on_save(want) != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save"); return ESP_OK; }
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"restarting\":true}");
            printf("BLE: turned %s, restarting\n", want ? "on" : "off");
            xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
            return ESP_OK;
        }
        return ble_http_get(req);
    }
    if (strcmp(act, "pin") == 0) {
        bool digits = strlen(v) == 6;
        for (int i = 0; digits && i < 6; i++) if (v[i] < '0' || v[i] > '9') digits = false;
        if (digits && ble_set_pin(strtoul(v, NULL, 10)) == ESP_OK) return ble_http_get(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use 6 digits, not starting with 0");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    return ESP_OK;
}
