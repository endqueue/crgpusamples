# cmake -DIN=<binary> -DOUT=<file.inc> -DNAME=<c name> -P spudgpusamples_embed.cmake
#
# Writes IN as a C array of 32-bit words, little-endian, zero padded to a
# whole word, and NAME_size, its exact byte count (a macro, so it can
# initialize a static struct in C). A word array keeps the 4-byte alignment
# spudgpu_create_shader_module takes. The same approach as spudlib's own
# cmake/spud_embed.cmake, kept here so the samples' build reads nothing
# from spudlib's.

file(READ ${IN} hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
math(EXPR pad "(4 - ${size} % 4) % 4")
if(pad GREATER 0)
    foreach(i RANGE 1 ${pad})
        string(APPEND hex "00")
    endforeach()
endif()
# Each 4 bytes, in file order, become one word with its bytes reversed.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u," words "${hex}")
string(REGEX REPLACE "((0x[0-9a-f]+u,){8})" "\\1\n\t" words "${words}")
math(EXPR word_count "(${size} + 3) / 4")
file(WRITE ${OUT}
    "// Written at build time from ${IN}: not by hand.\n"
    "static const uint32_t ${NAME}[${word_count}] = {\n\t${words}\n};\n"
    "#define ${NAME}_size UINT64_C(${size})\n")
