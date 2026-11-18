// HTTPS certificate. A phone's microphone only works on an https:// page.
// On the first start the box makes a P-256 key pair and a self-signed certificate and keeps both in NVS.
// Each box has its own key, so the firmware contains no shared private key.
// A phone shows one "not private" warning per box, because no certificate authority signed it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "psa/crypto.h"
#include "mbedtls/version.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "esp_random.h"
#include "nvs.h"
#include "app_tls.h"
#include "app_settings.h"

#define PEM_MAX 1200
#define NVS_CERT "tlscert5"     // Certificate, version 5. A new version gets a new name, so each box makes a fresh one.
#define NVS_PEM2 "tlspem5"   // NVS name of the private key PEM. The name itself is not a secret.

static char s_cert[PEM_MAX], s_key[PEM_MAX];

#if MBEDTLS_VERSION_MAJOR < 4                   // ESP-IDF 5.x (mbedTLS 3): the writer still needs a random function.
static int rng(void *p, unsigned char *out, size_t n) { esp_fill_random(out, n); return 0; }
#endif

static bool load(void)
{
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return false;
    size_t a = sizeof(s_cert), b = sizeof(s_key);
    bool ok = nvs_get_str(h, NVS_CERT, s_cert, &a) == ESP_OK && nvs_get_str(h, NVS_PEM2, s_key, &b) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool make(void)
{
#if !defined(MBEDTLS_X509_CRT_WRITE_C)       // Turned off in menuconfig: no https, http still works.
    return false;
#else
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t kid = 0;
    if (psa_generate_key(&a, &kid) != PSA_SUCCESS) return false;

    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    mbedtls_x509write_cert crt; mbedtls_x509write_crt_init(&crt);
    bool ok = false;
    if (mbedtls_pk_copy_from_psa(kid, &pk) != 0) goto out;

    char subject[48];
    snprintf(subject, sizeof(subject), "CN=FreeMesh %s,O=FreeMesh", node_id_str());
    unsigned char serial[8]; esp_fill_random(serial, sizeof(serial)); serial[0] &= 0x7F;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);         // Self-signed: the certificate signs itself.
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) goto out;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0) goto out;
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) goto out;
    // Apple devices (iOS and iPadOS 13 or later) refuse a certificate that is valid for more than 825 days.
    // The box has no clock, so the dates are fixed and span 823 days.
    // Phones report the certificate as expired after 2029-02-01. The box needs a new certificate then.
    if (mbedtls_x509write_crt_set_validity(&crt, "20261101000000", "20290201000000") != 0) goto out;
    // Extensions. With none set, mbedTLS writes an empty extensions block, which RFC 5280 forbids and
    // Chrome rejects. Browsers also need the box's IP address in the subject alternative name.
    {
        static const unsigned char ip[4] = { 192, 168, 4, 1 };
        mbedtls_x509_san_list san = { 0 };
        san.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san.node.san.unstructured_name.p = (unsigned char *)ip;
        san.node.san.unstructured_name.len = sizeof(ip);
        if (mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san) != 0) goto out;
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) goto out;    // Not a certificate authority.
        if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) goto out;
        // Apple devices also require the extended key usage "serverAuth": a certificate for a web server.
        mbedtls_asn1_sequence eku = { 0 };
        eku.buf.tag = MBEDTLS_ASN1_OID;
        eku.buf.p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH;
        eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
        if (mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku) != 0) goto out;
    }
#if MBEDTLS_VERSION_MAJOR < 4
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)s_cert, sizeof(s_cert), rng, NULL) != 0) goto out;
#else
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)s_cert, sizeof(s_cert)) != 0) goto out;
#endif
    if (mbedtls_pk_write_key_pem(&pk, (unsigned char *)s_key, sizeof(s_key)) != 0) goto out;

    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "tlscert"); nvs_erase_key(h, "tlskey");     // Remove certificates of older versions, if any.
        nvs_erase_key(h, "tlscert2"); nvs_erase_key(h, "tlspem2");
        nvs_erase_key(h, "tlscert3"); nvs_erase_key(h, "tlspem3");
        nvs_erase_key(h, "tlscert4"); nvs_erase_key(h, "tlspem4");
        if (nvs_set_str(h, NVS_CERT, s_cert) == ESP_OK && nvs_set_str(h, NVS_PEM2, s_key) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    ok = true;
out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    psa_destroy_key(kid);
    return ok;
#endif
}

bool tls_get(const char **cert_pem, const char **key_pem)
{
    if (!s_cert[0] && !load()) {
        printf("HTTPS: making this box's certificate (once)...\n");
        if (!make()) { printf("HTTPS: could not make a certificate, staying on http only\n"); s_cert[0] = 0; return false; }
    }
    *cert_pem = s_cert; *key_pem = s_key;
    return true;
}
