/**
 * 门户密码加密实现
 *
 * 链路（与 PC 端 portal_login.py 的 portal_password 一致）：
 *   RSA-512 PKCS#1 v1.5 -> 64 字节密文 -> base64 (88 字符) -> 再 base64 (120 字符)
 */

#include "pwd_enc.h"

#include <string.h>
#include <stdio.h>

#include "mbedtls/rsa.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/version.h"
#include "esp_random.h"

/* PKCS#1 v1.5 的随机填充源 */
static int pwd_rng(void *p_rng, unsigned char *out, size_t len)
{
    (void) p_rng;
    esp_fill_random(out, len);
    return 0;
}

/* 门户公钥（从 script.js 的 publicKey 解出来的 DER 里提取） */
static const uint8_t RSA_N[64] = {
    0xC8, 0xC1, 0x69, 0x7D, 0xC1, 0xF7, 0xC9, 0xBE, 0x24, 0xA6, 0x0D, 0x40,
    0x21, 0x29, 0xD4, 0x3F, 0xE8, 0xAA, 0x03, 0xC6, 0xAE, 0xB8, 0x54, 0x31,
    0x6B, 0x93, 0xB9, 0xB6, 0xA6, 0x20, 0xBB, 0x1A, 0x37, 0x13, 0xA4, 0x64,
    0x9B, 0x02, 0xA2, 0xF7, 0xEF, 0xB5, 0x07, 0x7C, 0xBC, 0xB3, 0x40, 0x22,
    0x44, 0x84, 0x70, 0x75, 0x2A, 0xC4, 0x17, 0x5E, 0x3D, 0x39, 0xEC, 0x2F,
    0x11, 0x69, 0xCE, 0x37,
};

static const uint8_t RSA_E[3] = { 0x01, 0x00, 0x01 };   /* 65537 */

#define RSA_LEN 64

int portal_encrypt_password(const char *plain, char *out, size_t out_size)
{
    if (!plain || !out) {
        return -1;
    }
    size_t plen = strlen(plain);
    if (plen == 0 || plen > RSA_LEN - 11) {
        return -2;
    }
    if (out_size < PWD_ENC_MAX_LEN + 1) {
        return -3;
    }

    mbedtls_rsa_context rsa;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    mbedtls_rsa_init(&rsa);
    mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
#else
    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
#endif

    int ret = mbedtls_rsa_import_raw(&rsa,
                                     RSA_N, sizeof(RSA_N),
                                     NULL, 0,   /* P */
                                     NULL, 0,   /* Q */
                                     NULL, 0,   /* D */
                                     RSA_E, sizeof(RSA_E));
    if (ret == 0) {
        ret = mbedtls_rsa_complete(&rsa);
    }
    if (ret != 0) {
        mbedtls_rsa_free(&rsa);
        return -4;
    }

    /* 第一次 base64：RSA 密文 -> 88 字符
     *
     * 必须用 pkcs1_encrypt 而不是 rsa_public：
     * rsa_public 是裸模幂（mbedtls 里直接 mpi_read_binary 64 字节，不看 padding），
     * 而门户 script.js 用的是 RSAES-PKCS1-v1_5。
     * 以前传进去的是 strlen 长度的密码串，剩下的字节是 cfg.password 的残尾，
     * 服务器按 v1.5 解必然失败（而且同一密码每次密文都不同）。 */
    uint8_t cipher[RSA_LEN];
    ret = mbedtls_rsa_pkcs1_encrypt(&rsa, pwd_rng, NULL, plen,
                                    (const unsigned char *) plain, cipher);
    mbedtls_rsa_free(&rsa);
    if (ret != 0) {
        return -5;
    }

    unsigned char b64_first[128];
    size_t first_len = 0;
    ret = mbedtls_base64_encode(b64_first, sizeof(b64_first) - 1, &first_len,
                                cipher, sizeof(cipher));
    if (ret != 0) {
        return -6;
    }
    b64_first[first_len] = '\0';

    /* 第二次 base64：88 字符 -> 120 字符 */
    unsigned char b64_second[192];
    size_t second_len = 0;
    ret = mbedtls_base64_encode(b64_second, sizeof(b64_second) - 1, &second_len,
                                b64_first, first_len);
    if (ret != 0) {
        return -7;
    }
    b64_second[second_len] = '\0';

    if (second_len + 1 > out_size) {
        return -8;
    }
    memcpy(out, b64_second, second_len + 1);
    return 0;
}