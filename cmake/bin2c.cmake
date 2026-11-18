# bin2c.cmake - Converte arquivo binário em array C++
if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE e OUTPUT_FILE precisam ser definidos!")
endif()

file(READ "${INPUT_FILE}" HEX_DATA HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " FORMATTED "${HEX_DATA}")

file(WRITE "${OUTPUT_FILE}" "// Gerado automaticamente pelo CMake a partir de ${INPUT_FILE}\n#pragma once\n#include <cstdint>\n\nalignas(4) const uint8_t chroma_shader_spv[] = {\n${FORMATTED}\n};\n")
