/**
 * @file shim.h
 * @brief Compatibility shim between the Rust FFI bindings and the C connector.
 *
 * This shim layer exists because the Rust bindgen tool generates slightly
 * different type names than what the C library declares. Specifically,
 * bindgen mangles some struct tags and enum variants in ways that are
 * technically correct C but don't match the original declarations.
 *
 * Instead of fixing the bindgen configuration (which would require
 * updating the bindgen version and re-validating all generated bindings),
 * we created this shim layer that provides C-callable wrappers with
 * predictable names that bindgen can digest correctly.
 *
 * When the connector library is compiled for the Rust integration, the
 * build system defines the TENT_CONNECTOR_SHIM macro. When compiled for
 * standalone use, the shim is not compiled and the direct API (api.h)
 * is used instead.
 *
 * The shim functions are thin wrappers that delegate to the real API.
 * They exist only to provide stable symbol names for the FFI bindings.
 * If you add a new function to the API, you MUST add a corresponding
 * shim wrapper here, or the Rust code won't be able to call it.
 *
 * TODO: Remove this shim layer when bindgen is upgraded or when we
 * switch to a different FFI generation tool (like cbindgen or diplomat).
 * The evaluation of alternatives was done in 2022 and the recommendation
 * was to use cbindgen. The migration to cbindgen was started but never
 * completed because the person leading the migration left the company.
 * The migration branch is at `experiment/cbindgen-migration` if anyone
 * wants to pick it up.
 */

#ifndef TENT_CONNECTOR_SHIM_H
#define TENT_CONNECTOR_SHIM_H

#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* INITIALIZATION SHIMS                                               */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_init().
 * Provides a stable ABI for the Rust FFI bindings.
 * Directly delegates to connector_init().
 */
connector_result_t tot_connector_init(const connector_config_t *config);

/**
 * Shim for connector_shutdown().
 */
connector_result_t tot_connector_shutdown(void);

/**
 * Shim for connector_drain().
 */
connector_result_t tot_connector_drain(void);

/**
 * Shim for connector_get_config().
 */
connector_result_t tot_connector_get_config(connector_config_t *config);

/**
 * Shim for connector_set_config().
 */
connector_result_t tot_connector_set_config(const connector_config_t *config);

/* ------------------------------------------------------------------ */
/* STATISTICS SHIMS                                                   */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_get_stats().
 */
connector_result_t tot_connector_get_stats(connector_stats_t *stats);

/**
 * Shim for connector_reset_stats().
 */
connector_result_t tot_connector_reset_stats(void);

/* ------------------------------------------------------------------ */
/* OPERATION SHIMS                                                    */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_send().
 */
connector_result_t tot_connector_send(const connector_buffer_t *buffer);

/**
 * Shim for connector_receive().
 */
connector_result_t tot_connector_receive(connector_buffer_t *buffer);

/**
 * Shim for connector_submit().
 */
connector_result_t tot_connector_submit(connector_operation_t *operation);

/**
 * Shim for connector_cancel().
 */
connector_result_t tot_connector_cancel(uint64_t operation_id);

/**
 * Shim for connector_wait_all().
 */
connector_result_t tot_connector_wait_all(uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* BUFFER SHIMS                                                       */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_buffer_alloc().
 */
connector_buffer_t *tot_connector_buffer_alloc(uint64_t size);

/**
 * Shim for connector_buffer_free().
 */
connector_result_t tot_connector_buffer_free(connector_buffer_t *buffer);

/**
 * Shim for connector_buffer_resize().
 */
connector_result_t tot_connector_buffer_resize(connector_buffer_t *buffer, uint64_t new_size);

/**
 * Shim for connector_buffer_reset().
 */
connector_result_t tot_connector_buffer_reset(connector_buffer_t *buffer);

/* ------------------------------------------------------------------ */
/* VERSION SHIMS                                                      */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_version().
 */
const char *tot_connector_version(void);

/**
 * Shim for connector_has_feature().
 */
int tot_connector_has_feature(connector_feature_t feature);

/**
 * Shim for connector_supported_features().
 */
uint32_t tot_connector_supported_features(void);

/* ------------------------------------------------------------------ */
/* LEGACY V1 SHIMS                                                    */
/* ------------------------------------------------------------------ */

/**
 * Shim for connector_init_v1().
 */
connector_result_t tot_connector_init_v1(
    connector_mode_t mode,
    uint32_t timeout_ms,
    uint32_t max_connections
);

/**
 * Shim for connector_send_v1().
 */
connector_result_t tot_connector_send_v1(
    const void *data,
    uint64_t size,
    uint32_t timeout_ms
);

/**
 * Shim for connector_receive_v1().
 */
connector_result_t tot_connector_receive_v1(
    void *buffer,
    uint64_t *size,
    uint32_t timeout_ms
);

/**
 * Shim for connector_get_stats_v1().
 */
connector_result_t tot_connector_get_stats_v1(
    uint64_t *uptime,
    uint64_t *operations,
    uint64_t *errors,
    uint64_t *bytes
);

#ifdef __cplusplus
}
#endif

#endif /* TENT_CONNECTOR_SHIM_H */
