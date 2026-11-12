#include "crypto.hpp"

#include "platform/platform.hpp"
#include "util/sha256.hpp"
#include "util/string_util.hpp"

#if defined(PIME_HAVE_OPENSSL)
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#endif

namespace pime::cap::crypto {

namespace {

std::string normalize_alg(const std::string& alg) {
    std::string out = to_lower(alg);
    if (out.rfind("hmac-", 0) == 0) out = out.substr(5);
    return out;
}

#if defined(PIME_HAVE_OPENSSL)
const EVP_MD* digest_for(const std::string& alg) {
    std::string name = normalize_alg(alg);
    const EVP_MD* md = EVP_get_digestbyname(name.c_str());
    if (md) return md;
    if (name.find("sha256") != std::string::npos) return EVP_sha256();
    if (name.find("sha384") != std::string::npos) return EVP_sha384();
    if (name.find("sha512") != std::string::npos) return EVP_sha512();
    if (name.find("sha1") != std::string::npos) return EVP_sha1();
    return nullptr;
}
#endif

}  // namespace

bool have_openssl() {
#if defined(PIME_HAVE_OPENSSL)
    return true;
#else
    return false;
#endif
}

bool available() { return true; }

pime_status digest(const std::string& alg, const uint8_t* data, size_t len,
                   std::vector<uint8_t>* out) {
    if (!out || (!data && len != 0)) return PIME_INVALID;
    std::string name = normalize_alg(alg);
    if (name == "sha256") {
        auto hash = crypto_hash::sha256(data, len);
        out->assign(hash.begin(), hash.end());
        return PIME_OK;
    }
#if defined(PIME_HAVE_OPENSSL)
    const EVP_MD* md = digest_for(name);
    if (!md) return PIME_UNSUPPORTED;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return PIME_NO_MEMORY;
    unsigned char buffer[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    pime_status status = PIME_OK;
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, buffer, &size) != 1) {
        status = PIME_RESOURCE;
    } else {
        out->assign(buffer, buffer + size);
    }
    EVP_MD_CTX_free(ctx);
    return status;
#else
    (void)name;
    return PIME_UNSUPPORTED;
#endif
}

pime_status mac(const std::string& alg, const std::vector<uint8_t>& key,
                const uint8_t* data, size_t len, std::vector<uint8_t>* out) {
    if (!out || (!data && len != 0)) return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    const EVP_MD* md = digest_for(alg);
    if (!md) return PIME_UNSUPPORTED;
    unsigned char buffer[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (!HMAC(md, key.data(), static_cast<int>(key.size()), data, len, buffer, &size))
        return PIME_RESOURCE;
    out->assign(buffer, buffer + size);
    return PIME_OK;
#else
    (void)alg;
    (void)key;
    return PIME_UNSUPPORTED;
#endif
}

pime_status aead_encrypt(const std::string& alg,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& iv,
                         const std::vector<uint8_t>& aad,
                         const uint8_t* plaintext, size_t len,
                         std::vector<uint8_t>* ciphertext,
                         std::vector<uint8_t>* tag) {
    if (!ciphertext || !tag || (!plaintext && len != 0)) return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    const EVP_CIPHER* cipher = EVP_get_cipherbyname(normalize_alg(alg).c_str());
    if (!cipher) return PIME_UNSUPPORTED;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return PIME_NO_MEMORY;
    pime_status status = PIME_OK;
    ciphertext->resize(len + 16);
    int produced = 0;
    int final_len = 0;
    unsigned char tag_buf[16];
    if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(iv.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        status = PIME_RESOURCE;
    } else {
        if (!aad.empty() &&
            EVP_EncryptUpdate(ctx, nullptr, &produced, aad.data(),
                              static_cast<int>(aad.size())) != 1) {
            status = PIME_RESOURCE;
        }
        if (status == PIME_OK) {
            if (EVP_EncryptUpdate(ctx, ciphertext->data(), &produced, plaintext,
                                  static_cast<int>(len)) != 1 ||
                EVP_EncryptFinal_ex(ctx, ciphertext->data() + produced,
                                    &final_len) != 1) {
                status = PIME_RESOURCE;
            } else {
                ciphertext->resize(static_cast<size_t>(produced + final_len));
                if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                                        tag_buf) != 1) {
                    status = PIME_RESOURCE;
                } else {
                    tag->assign(tag_buf, tag_buf + 16);
                }
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return status;
#else
    (void)alg; (void)key; (void)iv; (void)aad; (void)plaintext; (void)len;
    return PIME_UNSUPPORTED;
#endif
}

pime_status aead_decrypt(const std::string& alg,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& iv,
                         const std::vector<uint8_t>& aad,
                         const uint8_t* ciphertext, size_t len,
                         const std::vector<uint8_t>& tag,
                         std::vector<uint8_t>* plaintext) {
    if (!plaintext || (!ciphertext && len != 0)) return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    const EVP_CIPHER* cipher = EVP_get_cipherbyname(normalize_alg(alg).c_str());
    if (!cipher || tag.size() != 16) return PIME_UNSUPPORTED;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return PIME_NO_MEMORY;
    pime_status status = PIME_OK;
    plaintext->resize(len);
    int produced = 0;
    int final_len = 0;
    if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(iv.size()),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        status = PIME_RESOURCE;
    } else {
        if (!aad.empty() &&
            EVP_DecryptUpdate(ctx, nullptr, &produced, aad.data(),
                              static_cast<int>(aad.size())) != 1) {
            status = PIME_RESOURCE;
        }
        if (status == PIME_OK) {
            if (EVP_DecryptUpdate(ctx, plaintext->data(), &produced, ciphertext,
                                  static_cast<int>(len)) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                    const_cast<uint8_t*>(tag.data())) != 1 ||
                EVP_DecryptFinal_ex(ctx, plaintext->data() + produced,
                                    &final_len) != 1) {
                status = PIME_CORRUPT;
            } else {
                plaintext->resize(static_cast<size_t>(produced + final_len));
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return status;
#else
    (void)alg; (void)key; (void)iv; (void)aad; (void)ciphertext; (void)len;
    (void)tag;
    return PIME_UNSUPPORTED;
#endif
}

pime_status random_bytes(uint8_t* out, size_t len) {
    return platform::random_bytes(out, len);
}

pime_status verify_signature(const std::string& public_key_pem,
                             const std::string& algorithm,
                             const uint8_t* data, size_t len,
                             const uint8_t* signature, size_t signature_len) {
    if ((!data && len != 0) || (!signature && signature_len != 0))
        return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(),
                               static_cast<int>(public_key_pem.size()));
    if (!bio) return PIME_NO_MEMORY;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) return PIME_INVALID;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return PIME_NO_MEMORY;
    }
    const std::string alg = to_lower(algorithm);
    const EVP_MD* md = nullptr;
    if (alg.find("ed25519") == std::string::npos) md = digest_for(alg);
    pime_status status = PIME_OK;
    if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, key) != 1 ||
        EVP_DigestVerify(ctx, signature, signature_len, data, len) != 1) {
        status = PIME_DENIED;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return status;
#else
    (void)public_key_pem; (void)algorithm;
    return PIME_UNSUPPORTED;
#endif
}

pime_status sign_detached(const std::string& private_key_pem,
                          const std::string& algorithm, const uint8_t* data,
                          size_t len, std::vector<uint8_t>* signature) {
    if (!signature || (!data && len != 0)) return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    BIO* bio = BIO_new_mem_buf(private_key_pem.data(),
                               static_cast<int>(private_key_pem.size()));
    if (!bio) return PIME_NO_MEMORY;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) return PIME_INVALID;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return PIME_NO_MEMORY;
    }
    const std::string alg = to_lower(algorithm);
    const EVP_MD* md = nullptr;
    if (alg.find("ed25519") == std::string::npos) md = digest_for(alg);
    pime_status status = PIME_OK;
    if (EVP_DigestSignInit(ctx, nullptr, md, nullptr, key) != 1 ||
        EVP_DigestSignUpdate(ctx, data, len) != 1) {
        status = PIME_RESOURCE;
    } else {
        size_t sig_len = 0;
        if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) != 1) {
            status = PIME_RESOURCE;
        } else {
            signature->resize(sig_len);
            if (EVP_DigestSignFinal(ctx, signature->data(), &sig_len) != 1) {
                status = PIME_RESOURCE;
            } else {
                signature->resize(sig_len);
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return status;
#else
    (void)private_key_pem; (void)algorithm;
    return PIME_UNSUPPORTED;
#endif
}

pime_status x509_verify(const std::string& cert_pem, const std::string& ca_pem) {
#if defined(PIME_HAVE_OPENSSL)
    BIO* cbio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    BIO* abio = BIO_new_mem_buf(ca_pem.data(), static_cast<int>(ca_pem.size()));
    if (!cbio || !abio) {
        if (cbio) BIO_free(cbio);
        if (abio) BIO_free(abio);
        return PIME_NO_MEMORY;
    }
    X509* cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
    X509* ca = PEM_read_bio_X509(abio, nullptr, nullptr, nullptr);
    BIO_free(cbio);
    BIO_free(abio);
    if (!cert || !ca) {
        if (cert) X509_free(cert);
        if (ca) X509_free(ca);
        return PIME_INVALID;
    }
    X509_STORE* store = X509_STORE_new();
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    pime_status status = PIME_DENIED;
    if (store && ctx && X509_STORE_add_cert(store, ca) == 1 &&
        X509_STORE_CTX_init(ctx, store, cert, nullptr) == 1) {
        if (X509_verify_cert(ctx) == 1) status = PIME_OK;
    }
    if (ctx) X509_STORE_CTX_free(ctx);
    if (store) X509_STORE_free(store);
    X509_free(cert);
    X509_free(ca);
    return status == PIME_OK ? PIME_OK : PIME_RESOURCE;
#else
    (void)cert_pem; (void)ca_pem;
    return PIME_UNSUPPORTED;
#endif
}

pime_status kdf_hkdf(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& salt,
                     const std::vector<uint8_t>& info, size_t out_len,
                     std::vector<uint8_t>* out) {
    if (!out) return PIME_INVALID;
#if defined(PIME_HAVE_OPENSSL)
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return PIME_NO_MEMORY;
    pime_status status = PIME_OK;
    size_t len = out_len;
    out->resize(out_len);
    if (EVP_PKEY_derive_init(ctx) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt.data(),
                                    static_cast<int>(salt.size())) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(ctx, key.data(),
                                   static_cast<int>(key.size())) != 1 ||
        (info.empty() ? 1
                      : EVP_PKEY_CTX_add1_hkdf_info(
                            ctx, info.data(), static_cast<int>(info.size()))) != 1 ||
        EVP_PKEY_derive(ctx, out->data(), &len) != 1) {
        status = PIME_RESOURCE;
    } else {
        out->resize(len);
    }
    EVP_PKEY_CTX_free(ctx);
    return status;
#else
    (void)key; (void)salt; (void)info; (void)out_len;
    return PIME_UNSUPPORTED;
#endif
}

}  // namespace pime::cap::crypto
