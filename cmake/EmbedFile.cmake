function(mfa_embed_file input symbol header)
    file(READ "${input}" _contents HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_contents}")
    string(LENGTH "${_contents}" _hexlen)
    math(EXPR _size "${_hexlen} / 2")
    file(WRITE "${header}"
        "#pragma once\n#include <cstddef>\nstatic unsigned char const ${symbol}[] = { ${_bytes} };\nstatic std::size_t const ${symbol}Size = ${_size};\n")
endfunction()
