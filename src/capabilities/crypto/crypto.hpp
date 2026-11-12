#ifndef PIME_CAPABILITIES_CRYPTO_HPP
#define PIME_CAPABILITIES_CRYPTO_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pime::cap::crypto {

bool available();
bool have_openssl();

pime_status digest(const std::string& alg, const uint8_t* data, size_t len,
                   std::vector<uint8_t>* out);
pime_status mac(const std::string& alg, const std::vector<uint8_t>& key,
                const uint8_t* data, size_t len, std::vector<uint8_t>* out);
pime_status aead_encrypt(const std::string& alg,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& iv,
                         const std::vector<uint8_t>& aad,
                         const uint8_t* plaintext, size_t len,
                         std::vector<uint8_t>* ciphertext,
                         std::vector<uint8_t>* tag);
pime_status aead_decrypt(const std::string& alg,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& iv,
                         const std::vector<uint8_t>& aad,
                         const uint8_t* ciphertext, size_t len,
                         const std::vector<uint8_t>& tag,
                         std::vector<uint8_t>* plaintext);
pime_status random_bytes(uint8_t* out, size_t len);

/* Verifies a detached signature over data with a PEM public key. */
pime_status verify_signature(const std::string& public_key_pem,
                             const std::string& algorithm,
                             const uint8_t* data, size_t len,
                             const uint8_t* signature, size_t signature_len);
pime_status sign_detached(const std::string& private_key_pem,
                          const std::string& algorithm, const uint8_t* data,
                          size_t len, std::vector<uint8_t>* signature);

pime_status x509_verify(const std::string& cert_pem, const std::string& ca_pem);

pime_status kdf_hkdf(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& salt,
                     const std::vector<uint8_t>& info, size_t out_len,
                     std::vector<uint8_t>* out);

}  // namespace pime::cap::crypto

#endif
