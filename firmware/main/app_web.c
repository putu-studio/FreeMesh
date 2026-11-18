#include "esp_http_server.h"       // ESP-IDF web server.
#include "app_web.h"
#include "app_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_chat.h"
#include "app_mode.h"
#include "app_crypto.h"
#include "app_friends.h"
#include "app_groups.h"
#include "app_voice.h"
#include "app_tls.h"
#include "app_sos.h"
#include "app_gw.h"
#include "app_people.h"
#include "app_ble.h"
#include "app_ota.h"
#include "app_batt.h"
#ifdef FREEMESH_BRIDGE
#include "app_bridge.h"       // The optional gateway add-on (see app_gw.h), not included in this repository: its own page and API.
#endif
#include "esp_https_server.h"
#include "mbedtls/ssl.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_system.h"          // esp_restart.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"     // getpeername: address of a connecting phone (diagnostic log).


extern const uint8_t index_html_start[] asm("_binary_index_html_start");   // First byte of the embedded page.
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");     // One past its last byte.



static void url_decode(char *s)
{
    char *out = s;                                         // Write position: the text is decoded in place.
    for (char *in = s; *in; in++) {
        if (*in == '+') *out++ = ' ';                      // '+' means a space in form data.
        else if (*in == '%' && in[1] && in[2]) {           // "%41" is the byte 0x41.
            char hex[3] = { in[1], in[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        } else *out++ = *in;
    }
    *out = '\0';
}


static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}


static esp_err_t settings_post(httpd_req_t *req){
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    settings_t s = device_settings; 

    char v[40]; 
    if (httpd_query_key_value(body, "sf", v, sizeof(v)) == ESP_OK) s.sf = atoi(v);
    if (httpd_query_key_value(body, "bw", v, sizeof(v)) == ESP_OK) s.bw_khz = atoi(v);
    if (httpd_query_key_value(body, "tx", v, sizeof(v)) == ESP_OK) s.tx_dbm = atoi(v);
    if (httpd_query_key_value(body, "freq", v, sizeof(v)) == ESP_OK) s.freq_hz = strtoul(v, NULL, 10);
    if (httpd_query_key_value(body, "devicename", v, sizeof(v)) == ESP_OK) {
        url_decode(v);
        strlcpy(s.devicename, v, sizeof(s.devicename));
    }
    const char *check = settings_check(&s);
    if (check !=NULL){
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, check);
    } else {
        device_settings = s;
        if (settings_save() != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't save to flash");
        }
        else{
            chat_apply_settings();
            httpd_resp_sendstr(req, "ok");
        }
    }
    return ESP_OK;
}

static esp_err_t send_post(httpd_req_t *req)
{
    // URL encoding can triple the size: each byte of an emoji becomes three characters such as "%F0".
    // So 150 bytes of text can take 450 characters, plus the name and the field names.
    char body[768];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    char from[40] = "", text[480] = "", to[8] = "", group[8] = "";
    httpd_query_key_value(body, "from", from, sizeof(from));
    httpd_query_key_value(body, "text", text, sizeof(text));
    httpd_query_key_value(body, "to", to, sizeof(to));        // Optional: a friend box id makes the message private.
    httpd_query_key_value(body, "group", group, sizeof(group)); // Optional: a group id.
    url_decode(from); 
    url_decode(text);
    if (from[0] == '\0' || text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "from and text are needed");
        return ESP_OK;  
    }
    if (to[0] && !friends_is_friend(to)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That box isn't your friend (yet)");
        return ESP_OK;
    }
    if (group[0] && !groups_get(group, NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "You're not in that group");
        return ESP_OK;
    }
    if (group[0]) chat_send_group(from, text, group);
    else          chat_send_to(from, text, to);
    httpd_resp_sendstr(req, "ok"); 
    return ESP_OK;


}

static esp_err_t messages_get(httpd_req_t *req)
{
    uint32_t since = 0;                               // Without "since" in the query, send everything.
    char q[96], v[16], pid[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&   // "pid=a3f0": the phone that asks.
        httpd_query_key_value(q, "pid", pid, sizeof(pid)) == ESP_OK) people_seen(pid);
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&   // "since=7": only what came after 7.
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&   // "t=1763000000": the phone's clock (Unix time).
        httpd_query_key_value(q, "t", v, sizeof(v)) == ESP_OK) sos_set_time(strtoul(v, NULL, 10));
    chat_write_json(req, since, pid);
    return ESP_OK;
}


static esp_err_t settings_get(httpd_req_t *req)
{
    char json[300];
    char dn[34];
    json_escape(dn, sizeof(dn), device_settings.devicename);
    batt_t b; batt_get(&b);                 // "batt" is the percent, or -1 when this board cannot measure its battery.
    snprintf(json, sizeof(json),
        "{\"id\":\"%s\",\"devicename\":\"%s\",\"freq\":%lu,\"sf\":%u,\"bw\":%u,\"tx\":%d,\"batt\":%d,\"batt_mv\":%d%s}",
        node_id_str(), dn, (unsigned long)device_settings.freq_hz,
        device_settings.sf, device_settings.bw_khz, device_settings.tx_dbm, b.present ? b.percent : -1, b.mv,
#ifdef FREEMESH_BRIDGE
        ",\"extras\":[\"/bridge.js\"]"      // Screens of the optional gateway add-on: the page loads these scripts.
#else
        ""
#endif
        );
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// GET /api/mode -> {"current":"freemesh","modes":[{"id":"freemesh","name":"FreeMesh","installed":true},...]}
static esp_err_t mode_get(httpd_req_t *req)
{
    char json[320];
    int n = snprintf(json, sizeof(json), "{\"current\":\"%s\",\"modes\":[", mode_id(mode_current()));
    for (int i = 0; i < MODE_COUNT; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"installed\":%s}",
                      i ? "," : "", mode_id(i), mode_name(i), mode_installed(i) ? "true" : "false");
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /api/mode  body "mode=meshtastic" -> answers "ok", then the box restarts in that mode.
static esp_err_t mode_post(httpd_req_t *req)
{
    char body[64], v[24];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    int m = -1;
    if (httpd_query_key_value(body, "mode", v, sizeof(v)) == ESP_OK) m = mode_from_id(v);
    if (m < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown mode"); return ESP_OK; }
    if (m == (int)mode_current()) { httpd_resp_sendstr(req, "ok"); return ESP_OK; }
    if (ota_busy()) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The box is updating. Try again when it is done"); return ESP_OK; }
    if (!mode_installed(m)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That mode isn't installed on this box yet");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "ok");
    mode_switch(m);                                   // Restarts the box, so it normally does not return.
    return ESP_OK;
}

// GET /api/lock -> {"locked":true,"code":"ED"}   Never returns the key or the passphrase.
static esp_err_t lock_get(httpd_req_t *req)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"locked\":%s,\"code\":\"%s\"}",
             crypto_locked() ? "true" : "false", crypto_check_code());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /api/lock  body "pass=..." (empty = unlock) -> answers like GET.
static esp_err_t lock_post(httpd_req_t *req)
{
    char body[256], pass[200] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(pass);
    memset(body, 0, sizeof(body));                    // Do not leave the passphrase in memory.
    esp_err_t err = crypto_set_passphrase(pass);
    memset(pass, 0, sizeof(pass));
    if (err == ESP_ERR_INVALID_ARG) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use 8 to 64 letters");
    else if (err != ESP_OK)         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't save the lock");
    else {
        chat_apply_settings();                        // The lock changes the length of the ACK slots.
        lock_get(req);                                // Answer with the new state.
    }
    return ESP_OK;
}

// GET /api/friends -> {"friends":[{"id":"A1B2","devicename":"LoRa A1B2","code":"4821","state":"friend"},...]}
static esp_err_t friends_get(httpd_req_t *req)
{
    static const char *ST[] = { "", "asks_you", "asked", "friend" };
    friend_info_t l[MAX_FRIENDS + MAX_REQUESTS];
    int n = friends_list(l, MAX_FRIENDS + MAX_REQUESTS);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"friends\":[");
    for (int i = 0; i < n; i++) {
        char d[34], one[128];
        json_escape(d, sizeof(d), l[i].devicename[0] ? l[i].devicename : l[i].id);
        snprintf(one, sizeof(one), "%s{\"id\":\"%s\",\"devicename\":\"%s\",\"code\":\"%s\",\"state\":\"%s\"}",
                 i ? "," : "", l[i].id, d, l[i].code, ST[l[i].state & 3]);
        httpd_resp_sendstr_chunk(req, one);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /api/friends  body "do=ask|approve|remove&id=A1B2"
static esp_err_t friends_post(httpd_req_t *req)
{
    char body[64], act[12] = "", id[8] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "do", act, sizeof(act));
    httpd_query_key_value(body, "id", id, sizeof(id));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(act, "ask") == 0)          err = friends_ask(id);
    else if (strcmp(act, "approve") == 0) err = friends_approve(id);
    else if (strcmp(act, "remove") == 0)  err = friends_remove(id);
    if (err == ESP_OK) return friends_get(req);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
        err == ESP_ERR_NO_MEM ? "Too many open requests" : err == ESP_ERR_NOT_FOUND ? "No such request" : "Can't do that");
    return ESP_OK;
}

// GET /api/groups -> {"me":"BBBB","groups":[{"gid":"3fa9","name":"Hikers","owner":"BBBB","members":["AAAA","A1B2"]},...]}
static esp_err_t groups_get_h(httpd_req_t *req)
{
    group_info_t l[MAX_GROUPS];
    int n = groups_list(l, MAX_GROUPS);
    httpd_resp_set_type(req, "application/json");
    char one[160];
    snprintf(one, sizeof(one), "{\"me\":\"%s\",\"groups\":[", node_id_str());
    httpd_resp_sendstr_chunk(req, one);
    for (int i = 0; i < n; i++) {
        char nm[34];
        json_escape(nm, sizeof(nm), l[i].name);
        int o = snprintf(one, sizeof(one), "%s{\"gid\":\"%s\",\"name\":\"%s\",\"owner\":\"%s\",\"members\":[",
                         i ? "," : "", l[i].gid, nm, l[i].owner);
        for (int k = 0; k < l[i].count; k++)
            o += snprintf(one + o, sizeof(one) - o, "%s\"%s\"", k ? "," : "", l[i].members[k]);
        snprintf(one + o, sizeof(one) - o, "]}");
        httpd_resp_sendstr_chunk(req, one);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /api/groups  body "do=create&name=Hikers&members=AAAA,A1B2"  or  "do=delete|resend&gid=3fa9"
static esp_err_t groups_post(httpd_req_t *req)
{
    char body[200], act[12] = "", name[60] = "", mem[GROUP_MAX_MEMBERS * 5 + 8] = "", gid[8] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "do", act, sizeof(act));
    httpd_query_key_value(body, "name", name, sizeof(name));
    httpd_query_key_value(body, "members", mem, sizeof(mem));
    httpd_query_key_value(body, "gid", gid, sizeof(gid));
    url_decode(name); url_decode(mem);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(act, "create") == 0) {
        char ids[GROUP_MAX_MEMBERS][5]; int k = 0;
        for (char *m = strtok(mem, ","); m && k < GROUP_MAX_MEMBERS; m = strtok(NULL, ","))
            if (strlen(m) == 4) strlcpy(ids[k++], m, 5);
        err = groups_create(name, ids, k, NULL);
    }
    else if (strcmp(act, "delete") == 0) err = groups_delete(gid);
    else if (strcmp(act, "resend") == 0) err = groups_resend(gid);
    if (err == ESP_OK) return groups_get_h(req);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
        err == ESP_ERR_NO_MEM ? "Too many groups" : err == ESP_ERR_NOT_FOUND ? "Every member must be a friend box" : "Give it a name and pick 1 to 8 boxes");
    return ESP_OK;
}

// POST /api/voice?from=Alex&to=AAAA (or &group=3fa9)  body = the sound: 8000 Hz, 16-bit, mono, 10 s at most.
static esp_err_t voice_post(httpd_req_t *req)
{
    char q[160] = "", from[40] = "", to[8] = "", group[8] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "from", from, sizeof(from));
    httpd_query_key_value(q, "to", to, sizeof(to));
    httpd_query_key_value(q, "group", group, sizeof(group));
    url_decode(from);
    const char *why = NULL;
    if (!from[0]) why = "from is needed";
    else if (to[0] && !friends_is_friend(to)) why = "That box isn't your friend (yet)";
    else if (group[0] && !groups_get(group, NULL)) why = "You're not in that group";
    // The encoder keeps the first 10 s, so only much longer uploads are refused.
    else if (req->content_len > (VOICE_MAX_S + 5) * 16000) why = "Too long: 10 s at most";
    else if (voice_wait_s()) why = "Wait a bit: one voice message every 30 s";
    if (why) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why); return ESP_OK; }

    // codec2=1: the page already compressed the clip with codec2.wasm. The raw path below, where the
    // box compresses the clip, stays for the Android app.
    char c2f[4] = "";
    httpd_query_key_value(q, "codec2", c2f, sizeof(c2f));
    if (c2f[0] == '1') {
        static uint8_t c2[1600];                       // Static: the server handles one request at a time.
        int total = req->content_len;
        if (total <= 0 || total > (int)sizeof(c2)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too long: 10 s at most"); return ESP_OK; }
        for (int got = 0; got < total; ) {
            int n = httpd_req_recv(req, (char *)c2 + got, total - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) return ESP_FAIL;
            got += n;
        }
        uint32_t id = voice_send_codec2(from, to, group, c2, total, &why);
        if (!id) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why ? why : "Not sent"); return ESP_OK; }
        return httpd_resp_sendstr(req, "ok");
    }

    voice_rec_t *r = voice_rec_start();
    if (!r) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); return ESP_OK; }
    static uint8_t buf[1024];                      // Static: the server handles one request at a time.
    size_t left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, (char *)buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { voice_rec_drop(r); return ESP_FAIL; }
        voice_rec_feed(r, buf, n);                 // Codec2 compresses the sound while it arrives.
        left -= n;
    }
    uint32_t id = voice_rec_finish(r, from, to, group, &why);
    voice_rec_drop(r);
    if (!id) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why ? why : "Not sent"); return ESP_OK; }
    return httpd_resp_sendstr(req, "ok");
}

// GET /api/voice?id=12 -> the clip as a .wav file, or with &raw=1 as Codec2 bytes.
static esp_err_t voice_get(httpd_req_t *req)
{
    char q[48], v[16], rw[4] = "";
    uint32_t id = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) id = strtoul(v, NULL, 10);
        httpd_query_key_value(q, "raw", rw, sizeof(rw));
    }
    return voice_play(req, id, rw[0] == '1');      // raw=1: Codec2 bytes for the page. Otherwise a WAV (Android app).
}

// ---------- Update over the internet (app_ota.c) ----------
// GET /api/update -> {"version":"0.10.0","state":"idle","what":"","percent":0,"msg":"","unfinished":false,
//                     "url":"","ssid":"","has_pass":false}
// state is one of: idle, joining, checking, downloading, restarting, finishing, done, failed.
static esp_err_t update_get(httpd_req_t *req)
{
    ota_status_t s; ota_get(&s);
    gw_status_t g; gw_get(&g);
    char ver[50], msg[230], url[330], ss[70], out[900];
    json_escape(ver, sizeof(ver), s.version); json_escape(msg, sizeof(msg), s.msg);
    json_escape(url, sizeof(url), s.url); json_escape(ss, sizeof(ss), g.ssid);
    snprintf(out, sizeof(out), "{\"version\":\"%s\",\"state\":\"%s\",\"what\":\"%s\",\"percent\":%d,\"msg\":\"%s\",\"unfinished\":%s,\"url\":\"%s\",\"ssid\":\"%s\",\"has_pass\":%s}",
             ver, s.state, s.what, s.percent, msg, s.unfinished ? "true" : "false", url, ss, g.has_pass ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// POST /api/update  form fields ssid, pass, url (empty = keep the saved value) -> starts the update, answers like GET.
static esp_err_t update_post(httpd_req_t *req)
{
    char body[800], ssid[100] = "", pass[200] = "", url[480] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "url", url, sizeof(url));
    url_decode(ssid); url_decode(pass); url_decode(url);
    esp_err_t err = (strlen(ssid) > 32 || strlen(pass) > 64) ? ESP_ERR_NOT_FOUND : ota_begin(ssid, pass, url);
    memset(pass, 0, sizeof(pass)); memset(body, 0, sizeof(body));
    if (err == ESP_OK) return update_get(req);
    const char *why = err == ESP_ERR_INVALID_STATE ? "An update is already running"
                    : err == ESP_ERR_NOT_FOUND     ? "Type the internet Wi-Fi name (and password) first"
                    : err == ESP_ERR_INVALID_SIZE  ? "The update address must start with http:// or https://"
                    : err == ESP_ERR_INVALID_ARG   ? "No update address is set"
                    : err == ESP_ERR_NO_MEM        ? "Not enough memory right now. Restart the box and try again"
                    :                                "This box can't update itself";
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
    return ESP_OK;
}

// ---------- People ----------
// GET /api/people -> {"me":"BBBB","people":[{"box":"BBBB","pid":"a3f0","name":"Alex","pub":"64 hex",
//                     "online":1,"local":1,"age_s":3},...]}
static esp_err_t people_get(httpd_req_t *req)
{
    person_info_t *l = malloc(sizeof(person_info_t) * (PEOPLE_LOCAL_MAX + PEOPLE_REMOTE_MAX));
    if (!l) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    int n = people_list(l, PEOPLE_LOCAL_MAX + PEOPLE_REMOTE_MAX);
    httpd_resp_set_type(req, "application/json");
    char one[200];
    snprintf(one, sizeof(one), "{\"me\":\"%s\",\"people\":[", node_id_str());
    httpd_resp_sendstr_chunk(req, one);
    for (int i = 0; i < n; i++) {
        char nm[26];
        json_escape(nm, sizeof(nm), l[i].name);
        snprintf(one, sizeof(one), "%s{\"box\":\"%.4s\",\"pid\":\"%.4s\",\"name\":\"%s\",\"pub\":\"%.64s\",\"online\":%u,\"local\":%u,\"age_s\":%lu}",
                 i ? "," : "", l[i].box, l[i].pid, nm, l[i].pub, l[i].online, l[i].local, (unsigned long)l[i].age_s);
        httpd_resp_sendstr_chunk(req, one);
    }
    free(l);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /api/me  body "pid=a3f0&name=Liam&pub=<64 hex>": registers this phone here, answers like GET /api/people.
static esp_err_t me_post(httpd_req_t *req)
{
    char body[200], pid[8] = "", name[40] = "", pub[72] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "pid", pid, sizeof(pid));
    httpd_query_key_value(body, "name", name, sizeof(name));
    httpd_query_key_value(body, "pub", pub, sizeof(pub));
    url_decode(name);
    if (people_register(pid, name, pub) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad phone id, name or key");
    return people_get(req);
}

// POST /api/psend  body "to_box=AAAA&to_pid=7c21&from_pid=a3f0&blob=<base64url>": an encrypted message to one person.
#define BLOB_MAX ((int)sizeof(((chat_msg_t *)0)->text))
static esp_err_t psend_post(httpd_req_t *req)
{
    char body[320], tb[8] = "", tp[8] = "", fp[8] = "", blob[BLOB_MAX + 4];
    blob[0] = '\0';
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "to_box", tb, sizeof(tb));
    httpd_query_key_value(body, "to_pid", tp, sizeof(tp));
    httpd_query_key_value(body, "from_pid", fp, sizeof(fp));
    if (httpd_query_key_value(body, "blob", blob, sizeof(blob)) != ESP_OK || (int)strlen(blob) >= BLOB_MAX)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message too long");
    uint8_t raw[160];
    int bl = b64_decode(blob, raw, sizeof(raw));
    if (bl < 28 || bl > 12 + PERSON_TEXT_MAX + 16) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message too long or broken");
    bool here = strcmp(tb, node_id_str()) == 0;
    if (!here && !friends_is_friend(tb)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That person's box isn't your friend (yet)");
    if (!people_known(tb, tp) || !people_known(node_id_str(), fp)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Don't know that person (yet)");
    people_seen(fp);
    if (!chat_send_e2e(tb, tp, fp, blob)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Radio not ready");
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/pvoice?to_box=AAAA&to_pid=a3f0&from_pid=77c1&secs=4  body = the clip, compressed and encrypted
// on the phone (nonce + Codec2 bytes + tag). End-to-end: this box only stores and forwards it.
static esp_err_t pvoice_post(httpd_req_t *req)
{
    char q[96], tb[8] = "", tp[8] = "", fp[8] = "", sv[6] = "1";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "to_box", tb, sizeof(tb));
        httpd_query_key_value(q, "to_pid", tp, sizeof(tp));
        httpd_query_key_value(q, "from_pid", fp, sizeof(fp));
        httpd_query_key_value(q, "secs", sv, sizeof(sv));
    }
    static uint8_t blob[1600];                         // Static: the server handles one request at a time.
    int total = req->content_len;
    if (total < 28 || total > (int)sizeof(blob)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Voice message too long");
    for (int got = 0; got < total; ) {
        int n = httpd_req_recv(req, (char *)blob + got, total - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    bool here = strcmp(tb, node_id_str()) == 0;
    if (!here && !friends_is_friend(tb)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That person's box isn't your friend (yet)");
    if (!people_known(tb, tp) || !people_known(node_id_str(), fp)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Don't know that person (yet)");
    people_seen(fp);
    const char *why = NULL;
    if (!voice_send_sealed(tb, tp, fp, atoi(sv), blob, total, &why)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why ? why : "Not sent");
    return httpd_resp_sendstr(req, "ok");
}

// GET /codec2.wasm -> Codec2 for the page (built by tools/codec2-wasm/build.sh), stored gzipped.
static esp_err_t wasm_get(httpd_req_t *req)
{
    extern const uint8_t wasm_start[] asm("_binary_codec2_wasm_gz_start");
    extern const uint8_t wasm_end[]   asm("_binary_codec2_wasm_gz_end");
    httpd_resp_set_type(req, "application/wasm");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");      // One week: phones download it once.
    return httpd_resp_send(req, (const char *)wasm_start, wasm_end - wasm_start);
}

// POST /api/unsend  body "ids=12,13,14&from=NAME&pid=PHONE" -> {"n":how many}
// Takes back messages this phone sent: they are removed from this box, and the other boxes get an X packet.
// Error 500 = the radio is still sending the previous unsend: try again in a few seconds.
static esp_err_t unsend_post(httpd_req_t *req)
{
    char body[256], ids[160] = "", from[40] = "", pid[8] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "ids", ids, sizeof(ids));
    httpd_query_key_value(body, "from", from, sizeof(from));
    httpd_query_key_value(body, "pid", pid, sizeof(pid));
    url_decode(ids); url_decode(from);
    uint32_t list[16]; int k = 0;
    for (char *p = ids; *p && k < 16; ) {
        uint32_t v = strtoul(p, &p, 10);
        if (v) list[k++] = v;
        while (*p && (*p < '0' || *p > '9')) p++;
    }
    if (!k) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ids are needed");
    int done = chat_unsend(list, k, from, pid);
    if (done < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy, try again in a few seconds");
    char out[24]; snprintf(out, sizeof(out), "{\"n\":%d}", done);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// POST /api/forgetme  body "pid=PHONE" -> "ok". "Forget me on this box": the phone also erases its own id.
static esp_err_t forgetme_post(httpd_req_t *req)
{
    char body[64], pid[8] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "pid", pid, sizeof(pid));
    people_forget(pid);                                  // An unknown id (already forgotten) is fine too.
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/factory  body "confirm=yes" -> answers "ok", then the box restarts. "Factory reset".
// The box forgets everything and restarts like new: people, friend boxes, groups, messages, its name,
// radio settings, network lock, Bluetooth PIN and phones, the internet Wi-Fi and password, the settings of
// the optional gateway add-on, its own key and https certificate, and the settings of Meshtastic and MeshCore.
// The three programs stay. The erase runs at the start of the next boot (settings_factory_apply).
static void factory_restart(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }   // Lets the answer go out first.
static esp_err_t factory_post(httpd_req_t *req)
{
    char body[32], c[8] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    httpd_query_key_value(body, "confirm", c, sizeof(c));
    if (strcmp(c, "yes") != 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm=yes is needed");
    if (ota_busy()) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The box is updating. Try again when it is done");
    if (settings_factory_request() != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't save to flash");
    printf("FACTORY RESET asked from the page, restarting\n");
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(factory_restart, "factory", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Every page and API route. Registered on both the http server and the https server.
static void register_all(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = root_get      },
        { .uri = "/api/settings", .method = HTTP_GET,  .handler = settings_get  },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/send",     .method = HTTP_POST, .handler = send_post     },
        { .uri = "/api/messages", .method = HTTP_GET,  .handler = messages_get  },
        { .uri = "/api/mode",     .method = HTTP_GET,  .handler = mode_get      },
        { .uri = "/api/mode",     .method = HTTP_POST, .handler = mode_post     },
        { .uri = "/api/lock",     .method = HTTP_GET,  .handler = lock_get      },
        { .uri = "/api/lock",     .method = HTTP_POST, .handler = lock_post     },
        { .uri = "/api/friends",  .method = HTTP_GET,  .handler = friends_get   },
        { .uri = "/api/friends",  .method = HTTP_POST, .handler = friends_post  },
        { .uri = "/api/groups",   .method = HTTP_GET,  .handler = groups_get_h  },
        { .uri = "/api/groups",   .method = HTTP_POST, .handler = groups_post   },
        { .uri = "/api/voice",    .method = HTTP_GET,  .handler = voice_get     },
        { .uri = "/api/voice",    .method = HTTP_POST, .handler = voice_post    },
        { .uri = "/api/people",   .method = HTTP_GET,  .handler = people_get    },
        { .uri = "/api/me",       .method = HTTP_POST, .handler = me_post       },
        { .uri = "/api/psend",    .method = HTTP_POST, .handler = psend_post    },
        { .uri = "/api/unsend",   .method = HTTP_POST, .handler = unsend_post   },
        { .uri = "/api/forgetme", .method = HTTP_POST, .handler = forgetme_post },
        { .uri = "/api/factory",  .method = HTTP_POST, .handler = factory_post  },
        { .uri = "/api/pvoice",   .method = HTTP_POST, .handler = pvoice_post   },
        { .uri = "/codec2.wasm",  .method = HTTP_GET,  .handler = wasm_get      },
        { .uri = "/api/ble",      .method = HTTP_GET,  .handler = ble_http_get  },
        { .uri = "/api/ble",      .method = HTTP_POST, .handler = ble_http_post },
        { .uri = "/api/update",   .method = HTTP_GET,  .handler = update_get    },
        { .uri = "/api/update",   .method = HTTP_POST, .handler = update_post   },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) httpd_register_uri_handler(server, &uris[i]);
#ifdef FREEMESH_BRIDGE
    bridge_web_register(server);        // The optional gateway add-on registers 3 more.
#endif
}

// Internet test pages that phones request on a new Wi-Fi (see keep_phones_connected in app_wifi.c).
// Each answer is exactly what the phone expects, so the phone keeps the box's Wi-Fi instead of dropping
// it when the screen sleeps.
static esp_err_t probe_204(httpd_req_t *req)       // Android, Chrome OS.
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}
static esp_err_t probe_text(httpd_req_t *req)      // Apple, Windows, Firefox: a fixed text.
{
    const char *u = req->uri, *t = "success\n";
    if (strstr(u, "hotspot-detect") || strstr(u, "/library/test/success"))
        t = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    else if (strstr(u, "connecttest")) t = "Microsoft Connect Test";
    else if (strstr(u, "ncsi"))        t = "Microsoft NCSI";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, t);
}
static void register_probes(httpd_handle_t server)
{
    static const httpd_uri_t p[] = {
        { .uri = "/generate_204",               .method = HTTP_GET, .handler = probe_204  },
        { .uri = "/gen_204",                    .method = HTTP_GET, .handler = probe_204  },
        { .uri = "/hotspot-detect.html",        .method = HTTP_GET, .handler = probe_text },
        { .uri = "/library/test/success.html",  .method = HTTP_GET, .handler = probe_text },
        { .uri = "/connecttest.txt",            .method = HTTP_GET, .handler = probe_text },
        { .uri = "/ncsi.txt",                   .method = HTTP_GET, .handler = probe_text },
        { .uri = "/success.txt",                .method = HTTP_GET, .handler = probe_text },
    };
    for (int i = 0; i < (int)(sizeof(p) / sizeof(p[0])); i++) httpd_register_uri_handler(server, &p[i]);
}

// Phones that sleep or walk away never close their connections, and each open connection holds RAM.
//  - Keep-alive: after 10 s idle, probe every 5 s. 3 missed answers drop the connection (about 25 s).
//  - LRU purge: when all sockets are busy, a new visitor replaces the oldest connection instead of being refused.
static void dead_conn_cleanup(httpd_config_t *c)
{
    c->lru_purge_enable = true;
    c->keep_alive_enable = true;
    c->keep_alive_idle = 10;
    c->keep_alive_interval = 5;
    c->keep_alive_count = 3;
}

// The box answers every web name with its own address, so apps on a phone also try https to the box,
// and each handshake keeps the single https worker busy. The page is opened by IP (https://192.168.4.1),
// and browsers send no server name (SNI) for an IP, while apps send the name they want. This callback
// runs right after the client hello and closes a connection that carries a name. A name made only of
// digits and dots is an IP address (some phones send "192.168.4.1" as the name) and is accepted.
static int only_our_page(mbedtls_ssl_context *ssl)
{
    size_t n = 0;
    const unsigned char *name = mbedtls_ssl_get_hs_sni(ssl, &n);
    if (!name || !n) return 0;                           // No name: a browser on https://192.168.4.1.
    bool ip = true;
    for (size_t i = 0; i < n; i++) if ((name[i] < '0' || name[i] > '9') && name[i] != '.') ip = false;
    if (ip) return 0;
    static char last[40];                               // Logs each refused name once, not on every retry.
    char now[40]; size_t k = n < sizeof(now) - 1 ? n : sizeof(now) - 1;
    memcpy(now, name, k); now[k] = '\0';
    if (strcmp(now, last)) { strlcpy(last, now, sizeof(last)); printf("HTTPS: refused a connection for \"%s\" (not our page)\n", now); }
    return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
}

// Diagnostic log: one line per phone connection. It shows whether a phone reaches the box at all and,
// on https, whether the handshake worked.
static void peer_ip(int fd, char *out, size_t n)
{
    struct sockaddr_storage a; socklen_t l = sizeof(a);
    out[0] = '\0';
    if (getpeername(fd, (struct sockaddr *)&a, &l) != 0) return;
    if (a.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, out, n);
    else if (a.ss_family == AF_INET6) {               // lwIP reports IPv4 phones as ::ffff:a.b.c.d.
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        const uint8_t *b = (const uint8_t *)&a6->sin6_addr;
        snprintf(out, n, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    }
}
static esp_err_t http_opened(httpd_handle_t hd, int fd)
{
    char ip[48]; peer_ip(fd, ip, sizeof(ip));
    printf("HTTP: %s connected\n", ip);
    return ESP_OK;
}
static void https_event(esp_https_server_user_cb_arg_t *a)
{
    if (a->user_cb_state == HTTPD_SSL_USER_CB_SESS_CLOSE) return;
    int fd = -1; esp_tls_get_conn_sockfd(a->tls, &fd);
    char ip[48]; peer_ip(fd, ip, sizeof(ip));
    if (a->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)esp_tls_get_ssl_context(a->tls);
        printf("HTTPS: %s ok (%s, %s)\n", ip, ssl ? mbedtls_ssl_get_version(ssl) : "?", ssl ? mbedtls_ssl_get_ciphersuite(ssl) : "?");
    } else {
        printf("HTTPS: %s handshake failed\n", ip);
    }
}

static void https_start(void);

void web_start(void)
{
    // The http server at http://192.168.4.1 shows no certificate warning, but offers no voice recording (phones need https for the microphone).
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 24576;         // Codec2 decoding (FFT arrays) plus http overflows a 16 KB stack.
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 40;      // 27 pages and APIs (+ 3 with the optional gateway add-on) + 7 internet test pages.
    dead_conn_cleanup(&cfg);
    cfg.open_fn = http_opened;      // Diagnostic log.
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    register_all(server);
    register_probes(server);        // Phones test on plain http only.
    https_start();
}

// The https server at https://192.168.4.1 serves the same page, and the microphone works. Each phone shows one certificate warning.
static httpd_handle_t s_secure;

static void https_start(void)
{
    if (s_secure) return;
    const char *cert, *key;
    if (!tls_get(&cert, &key)) return;
    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
    ssl.httpd.stack_size = 24576;    // Codec2 decoding plus TLS overflows a 16 KB stack.
    ssl.httpd.max_open_sockets = 3;  // Each TLS connection takes about 40 KB of RAM.
    ssl.httpd.max_uri_handlers = 32;    // 27 pages and APIs + 3 of the optional gateway add-on = 30 used. Only 2 are free.
    ssl.httpd.ctrl_port = 32769;     // The http server already uses 32768.
    dead_conn_cleanup(&ssl.httpd);
    ssl.cert_select_cb = only_our_page;      // Refuses https meant for other apps before the slow handshake math.
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_WARN);   // At the default level it prints a line for every connection.
    ssl.session_tickets = true;              // A returning phone skips the slow handshake math (about 0.5 s).
    ssl.tls_handshake_timeout_ms = 4000;     // The default is much longer, and one stuck phone blocks everyone else.
    ssl.user_cb = https_event;               // Diagnostic log.
    ssl.servercert = (const uint8_t *)cert; ssl.servercert_len = strlen(cert) + 1;
    ssl.prvtkey_pem = (const uint8_t *)key; ssl.prvtkey_len = strlen(key) + 1;
    if (httpd_ssl_start(&s_secure, &ssl) == ESP_OK) { register_all(s_secure); printf("HTTPS: on, https://192.168.4.1\n"); }
    else { s_secure = NULL; printf("HTTPS: failed to start (not enough memory?), http still works\n"); }
}

// An update download and the phones' https connections (about 40 KB each) do not fit in RAM together.
// So https stops while the box downloads. Plain http stays up for the progress screen.
void web_https_pause(bool pause)
{
    if (pause && s_secure) {
        httpd_ssl_stop(s_secure);
        s_secure = NULL;
        printf("HTTPS: paused for the update\n");
    } else if (!pause) https_start();
}
