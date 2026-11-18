#pragma once
#include <stdbool.h>
void web_start(void);               // Start the http server and, if a certificate exists, the https server.
void web_https_pause(bool pause);   // true stops the https server to free RAM for an update download; false restarts it.
