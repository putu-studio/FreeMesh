#!/bin/sh
# Compiles Codec2 to WebAssembly for the page. Run from firmware/.
# Needs Zig: uses "zig" from the PATH, or the program named in the ZIG variable.
Z=${ZIG:-zig}
C=components/codec2
SRCS=$(ls $C/src/*.c)     # esp_alloc.c is plain malloc wrappers, fine in the browser too
"$Z" cc -target wasm32-wasi -mexec-model=reactor -Oz -flto -DNDEBUG -D__EMBEDDED__ \
  -I$C/include -I$C/src -w tools/codec2-wasm/c2w.c $SRCS -lm \
  -Wl,--strip-all -Wl,--export=malloc -o main/webapp/codec2.wasm
# The box serves the file gzipped and the browser unpacks it: 181 KB -> about 139 KB of flash.
gzip -9 -n -c main/webapp/codec2.wasm > main/webapp/codec2.wasm.gz && rm main/webapp/codec2.wasm
ls -l main/webapp/codec2.wasm.gz
