# Codec2 speech codec (LGPL-2.1), only the parts the 1200 bit/s mode needs.
# Source: github.com/drowe67/codec2 @ 310777b. Codebook .c files made by tools/gen_codebooks.py
file(GLOB C2_SRCS "src/*.c")
idf_component_register(SRCS ${C2_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -std=gnu17 -Wno-error -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-maybe-uninitialized -Wno-stringop-overflow -Wno-array-parameter -DNDEBUG -D__EMBEDDED__)   # __EMBEDDED__: tables stay in flash, not RAM
