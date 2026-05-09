// Bit-level encoders/decoders for Product Quantization codes.
//
// Ported from openGauss src/include/access/annvector/distance/pq/pq_endecode.h.
// Only change: drop postgres.h, route Assert through <cassert> so the file is
// backend-neutral. The bit-packing logic itself is untouched.
//
// Three encoder/decoder pairs:
//   - PQEncoder8 / PQDecoder8   — fast path for nbits == 8 (one byte per code)
//   - PQEncoder16 / PQDecoder16 — fast path for nbits == 16 (two bytes per code)
//   - PQEncoderGeneric / PQDecoderGeneric — arbitrary nbits (bit-packed)
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace vex {
namespace quantizer {

struct PQEncoderGeneric {
    uint8_t *code;
    uint8_t  offset;
    const int nbits;
    uint8_t  reg;

    inline PQEncoderGeneric(uint8_t *code_in, int nbits_in, uint8_t offset_in = 0)
        : code(code_in), offset(offset_in), nbits(nbits_in), reg(0) {
        assert(nbits <= 64);
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
    uint8_t *code;
    inline PQEncoder8(uint8_t *code_in, int nbits_in) : code(code_in) {
        assert(nbits_in == 8);
    }

    inline void encode(uint64_t x) {
        *code++ = (uint8_t)x;
    }
    inline void restore_code() {}
};

struct PQEncoder16 {
    uint16_t *code;
    inline PQEncoder16(uint8_t *code_in, int nbits_in) : code((uint16_t *)code_in) {
        assert(nbits_in == 16);
    }

    inline void encode(uint64_t x) {
        *code++ = (uint16_t)x;
    }
    inline void restore_code() {}
};

struct PQDecoderGeneric {
    const uint8_t *code;
    uint8_t  offset;
    const int nbits;
    const uint64_t mask;
    uint8_t  reg;

    inline PQDecoderGeneric(const uint8_t *code_in, int nbits_in)
        : code(code_in), offset(0), nbits(nbits_in),
          mask((1ull << nbits_in) - 1), reg(0) {
        assert(nbits <= 64);
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
    const uint8_t *code;
    inline PQDecoder8(const uint8_t *code_in, int nbits_in) : code(code_in) {
        assert(nbits_in == 8);
    }
    inline uint64_t decode() { return (uint64_t)(*code++); }
};

struct PQDecoder16 {
    static const int nbits = 16;
    const uint16_t *code;
    inline PQDecoder16(const uint8_t *code_in, int nbits_in) : code((const uint16_t *)code_in) {
        assert(nbits_in == 16);
    }
    inline uint64_t decode() { return (uint64_t)(*code++); }
};

} // namespace quantizer
} // namespace vex
