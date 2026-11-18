// Codec2 at 1200 bit/s for the web page (WebAssembly), so that a phone can compress and decompress
// voice itself. This lets voice to one person be encrypted end to end: the phone encrypts the
// 1500-byte clip and the boxes carry only encrypted bytes. Same Codec2 source as the box (components/codec2).
// Build: tools/codec2-wasm/build.sh  ->  main/webapp/codec2.wasm.gz
#include <stdlib.h>
#include "codec2.h"

#define EXPORT(n) __attribute__((export_name(n)))

EXPORT("c2_new")    struct CODEC2 *c2_new(void)                { return codec2_create(CODEC2_MODE_1200); }
EXPORT("c2_free")   void c2_free(struct CODEC2 *c)             { codec2_destroy(c); }
EXPORT("c2_samples") int c2_samples(struct CODEC2 *c)          { return codec2_samples_per_frame(c); }   // 320 = 40 ms at 8 kHz.
EXPORT("c2_bytes")  int c2_bytes(struct CODEC2 *c)             { return (codec2_bits_per_frame(c) + 7) / 8; }  // 6 bytes.
EXPORT("c2_enc")    void c2_enc(struct CODEC2 *c, unsigned char *bits, short *pcm) { codec2_encode(c, bits, pcm); }
EXPORT("c2_dec")    void c2_dec(struct CODEC2 *c, short *pcm, const unsigned char *bits) { codec2_decode(c, pcm, bits); }
EXPORT("buf_new")   void *buf_new(int n)                       { return malloc(n); }
EXPORT("buf_free")  void buf_free(void *p)                     { free(p); }
