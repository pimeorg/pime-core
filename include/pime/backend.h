/*
 * Inference backend ABI. A backend module statically links its engine and
 * exposes only this C ABI.
 */
#ifndef PIME_BACKEND_H
#define PIME_BACKEND_H

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIME_ML_MAX_DIMS 4

typedef enum {
    PIME_ML_F32 = 0,
    PIME_ML_F16 = 1,
    PIME_ML_I8 = 2,
    PIME_ML_U8 = 3,
    PIME_ML_I32 = 4
} pime_ml_dtype;

typedef struct {
    uint32_t size, version;
    uint32_t dtype;
    uint32_t rank;
    uint32_t shape[PIME_ML_MAX_DIMS];
    const void* data;
    uint32_t data_len;
} pime_ml_tensor;

typedef struct {
    uint32_t size, version;
    const uint8_t* desc; uint32_t desc_len; /* CBOR: model + options */
    const pime_ml_tensor* inputs; uint32_t input_count;
    pime_ml_tensor* outputs; uint32_t output_cap; uint32_t* output_count;
} pime_ml_request;

typedef struct {
    uint32_t size, version;
    pime_status (*load)(const uint8_t* desc, uint32_t len, uint64_t* session);
    pime_status (*run)(uint64_t session, const uint8_t* in, uint32_t in_len,
                       uint8_t* out, uint32_t cap, uint32_t* out_len);
    pime_status (*cancel)(uint64_t job);
    pime_status (*free)(uint64_t session);
} pime_ml_backend;

PIME_API pime_status pime_ml_backend_query(uint32_t abi_major,
                                           pime_ml_backend* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIME_BACKEND_H */
