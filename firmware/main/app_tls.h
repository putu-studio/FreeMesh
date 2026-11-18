#pragma once
#include <stdbool.h>


// This box's HTTPS certificate and private key as PEM text. Made once, then kept in NVS.
// Returns false when there is none: the box then serves http only.
bool tls_get(const char **cert_pem, const char **key_pem);
