/*
 * 门户密码加密：RSA-512 (PKCS#1 v1.5) + 双重 Base64
 *
 * 与门户 script.js 完全一致：
 *     $("#Wp_password").val(encodeBase64(encrypt(mypassword)));
 * 其中 encrypt() 返回的是 base64(密文)，再套一层 base64。
 *
 * 明文密码最长 64 - 11 = 53 字节。
 */
#ifndef PWD_ENC_H
#define PWD_ENC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWD_ENC_MAX_LEN 120   /* 双重 base64 后的固定长度 */

/**
 * 把明文密码加密成门户要求的 userPwd。
 *
 * @param plain     明文密码
 * @param out       输出缓冲区（建议 PWD_ENC_MAX_LEN + 1）
 * @param out_size  缓冲区大小
 * @return          0 成功，负数失败
 */
int portal_encrypt_password(const char *plain, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PWD_ENC_H */