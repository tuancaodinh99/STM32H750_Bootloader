/*
 * sha256.h
 *
 *  Created on: Aug 19, 2026
 *      Author: dinhtuan.cao
 */

#ifndef INC_SHA256_H_
#define INC_SHA256_H_

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE  32U
#define SHA256_BLOCK_SIZE   64U

typedef struct {
    uint32_t state[8];                  /* Current hash state   */
    uint32_t bitcount;                  /* Total bits processed (8MB max = 67M bits, fits uint32_t) */

    uint8_t  buffer[SHA256_BLOCK_SIZE]; /* Partial block buffer */
    uint32_t buflen;                    /* Bytes in buffer      */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/* Convenience: hash entire buffer at once */
void sha256_hash(const uint8_t *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif /* INC_SHA256_H_ */
