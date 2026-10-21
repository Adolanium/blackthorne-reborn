# Writes a binary file as a C++ byte array: cmake -DIN=... -DOUT=... -DNAME=... -P bin2h.cmake
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" len)
math(EXPR count "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "// Generated from ${IN}.\n#pragma once\nstatic const unsigned char ${NAME}[${count}] = {${bytes}};\n")
