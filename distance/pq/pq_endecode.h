#ifndef PQ_ENDECODE_H
#define PQ_ENDECODE_H

#include <stddef.h>
#include <cstdint>

// Assert: PG defines it via postgres.h before this header is reached on the
// PG build target; on duck/standalone builds we fall back to <cassert>.
#ifndef Assert
#include <cassert>
#define Assert(x) assert(x)
#endif

/*************************************************
 * Objects to encode / decode strings of bits
 *************************************************/

struct PQEncoderGeneric {
    uint8_t* code; ///< code for this vector
    uint8_t offset;
    const int nbits; ///< number of bits per subquantizer index

    uint8_t reg;

    inline PQEncoderGeneric(
        uint8_t* code,
        int nbits,
        uint8_t offset = 0)
        : code(code), offset(offset), nbits(nbits), reg(0) {
        Assert(nbits <= 64);
        if (offset > 0) {
            reg = (*code & ((1 << offset) - 1));
        }
    }

    inline void encode(uint64_t x) {
        reg |= (uint8_t)(x << offset);
        x >>= (8 - offset);
        if (offset + nbits >= 8) {
            *code++ = reg;

            for (int i = 0; i < (nbits - (8 - offset)) / 8; ++i) {
                *code++ = (uint8_t)x;
                x >>= 8;
            }

            offset += nbits;
            offset &= 7;
            reg = (uint8_t)x;
        } else {
            offset += nbits;
        }
    }

    inline void restore_code() {
    if (offset > 0) {
        *code = reg;
    }
}
};

struct PQEncoder8 {
    uint8_t* code;
    inline PQEncoder8(uint8_t* code, int nbits) : code(code) {
        Assert(8 == nbits);
    }

    inline void encode(uint64_t x) {
        *code++ = (uint8_t)x;
    }
    inline void restore_code() {}
};

struct PQEncoder16 {
    uint16_t* code;

    inline PQEncoder16(uint8_t* code, int nbits)
        : code((uint16_t*)code) {
        Assert(16 == nbits);
    }

    inline void encode(uint64_t x) {
        *code++ = (uint16_t)x;
    }
    inline void restore_code() {}
};

struct PQDecoderGeneric {
    const uint8_t* code;
    uint8_t offset;
    const int nbits;
    const uint64_t mask;
    uint8_t reg;
    inline PQDecoderGeneric(const uint8_t* code, int nbits)
            : code(code),
            offset(0),
            nbits(nbits),
            mask((1ull << nbits) - 1),
            reg(0) {
        Assert(nbits <= 64);
    }

    inline uint64_t decode() {
        if (offset == 0) {
            reg = *code;
        }
        uint64_t c = (reg >> offset);

        if (offset + nbits >= 8) {
            uint64_t e = 8 - offset;
            ++code;
            for (int i = 0; i < (nbits - (8 - offset)) / 8; ++i) {
                c |= ((uint64_t)(*code++) << e);
                e += 8;
            }

            offset += nbits;
            offset &= 7;
            if (offset > 0) {
                reg = *code;
                c |= ((uint64_t)reg << e);
            }
        } else {
            offset += nbits;
        }

        return c & mask;
    }
};

struct PQDecoder8 {
    static const int nbits = 8;
    const uint8_t* code;

    inline PQDecoder8(const uint8_t* code, int nbits_in) : code(code) {
        Assert(8 == nbits_in);
    }

    inline uint64_t decode() {
        return (uint64_t)(*code++);
    }
};

struct PQDecoder16 {
    static const int nbits = 16;
    const uint16_t* code;
    inline PQDecoder16(const uint8_t* code, int nbits_in)
        : code((uint16_t*)code) {
        Assert(16 == nbits_in);
    }

    inline uint64_t decode() {
        return (uint64_t)(*code++);
    }
};

#endif