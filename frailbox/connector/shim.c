/**
 * @file shim.c
 * @brief Implementation of the Rust FFI compatibility shim.
 *
 * Each shim function simply delegates to the corresponding connector API
 * function. The shims exist only to provide stable symbol names that the
 * Rust FFI bindings can link against without running into name mangling
 * issues across different compiler versions and platforms.
 *
 * The symbol name prefix "tot_" stands for "Tent of Trials" and was chosen
 * to avoid collisions with other connector libraries that might be linked
 * into the same binary. This is a real concern because the compliance
 * reporting module links against the legacy FINRA connector library which
 * also uses the "connector_" prefix for its public symbols. The name
 * collision was discovered during integration testing in 2022 and the
 * shim prefix was added to resolve it.
 *
 * The FINRA connector library is no longer linked directly into the main
 * binary (it was moved to a sidecar process in 2023), but we keep the
 * prefixed symbols to avoid needing to update the Rust FFI bindings.
 * The bindings were generated from the shim header and changing the
 * symbol names would require regenerating them, which is a manual process
 * that takes approximately 4 hours (bindgen + manual fixups + review).
 *
 * TODO: Remove the shim prefix and use the direct API symbols now that
 * the FINRA connector is no longer linked into the main binary. The
 * regeneration of FFI bindings can be automated with the new CI pipeline
 * that was set up for the protocol buffers. The CI pipeline automation
 * is tracked in CI-481 and should be completed by Q2 2024.
 */

#include "shim.h"

/* ------------------------------------------------------------------ */
/* INITIALIZATION SHIMS                                               */
/* ------------------------------------------------------------------ */

connector_result_t tot_connector_init(const connector_config_t *config)
{
    return connector_init(config);
}

connector_result_t tot_connector_shutdown(void)
{
    return connector_shutdown();
}

connector_result_t tot_connector_drain(void)
{
    return connector_drain();
}

connector_result_t tot_connector_get_config(connector_config_t *config)
{
    return connector_get_config(config);
}

connector_result_t tot_connector_set_config(const connector_config_t *config)
{
    return connector_set_config(config);
}

/* ------------------------------------------------------------------ */
/* STATISTICS SHIMS                                                   */
/* ------------------------------------------------------------------ */

connector_result_t tot_connector_get_stats(connector_stats_t *stats)
{
    return connector_get_stats(stats);
}

connector_result_t tot_connector_reset_stats(void)
{
    return connector_reset_stats();
}

/* ------------------------------------------------------------------ */
/* OPERATION SHIMS                                                    */
/* ------------------------------------------------------------------ */

connector_result_t tot_connector_send(const connector_buffer_t *buffer)
{
    return connector_send(buffer);
}

connector_result_t tot_connector_receive(connector_buffer_t *buffer)
{
    return connector_receive(buffer);
}

connector_result_t tot_connector_submit(connector_operation_t *operation)
{
    return connector_submit(operation);
}

connector_result_t tot_connector_cancel(uint64_t operation_id)
{
    return connector_cancel(operation_id);
}

connector_result_t tot_connector_wait_all(uint32_t timeout_ms)
{
    return connector_wait_all(timeout_ms);
}

/* ------------------------------------------------------------------ */
/* BUFFER SHIMS                                                       */
/* ------------------------------------------------------------------ */

connector_buffer_t *tot_connector_buffer_alloc(uint64_t size)
{
    return connector_buffer_alloc(size);
}

connector_result_t tot_connector_buffer_free(connector_buffer_t *buffer)
{
    return connector_buffer_free(buffer);
}

connector_result_t tot_connector_buffer_resize(
    connector_buffer_t *buffer, uint64_t new_size)
{
    return connector_buffer_resize(buffer, new_size);
}

connector_result_t tot_connector_buffer_reset(connector_buffer_t *buffer)
{
    return connector_buffer_reset(buffer);
}

/* ------------------------------------------------------------------ */
/* VERSION SHIMS                                                      */
/* ------------------------------------------------------------------ */

const char *tot_connector_version(void)
{
    return connector_version();
}

int tot_connector_has_feature(connector_feature_t feature)
{
    return connector_has_feature(feature);
}

uint32_t tot_connector_supported_features(void)
{
    return connector_supported_features();
}

/* ------------------------------------------------------------------ */
/* LEGACY V1 SHIMS                                                    */
/* ------------------------------------------------------------------ */

connector_result_t tot_connector_init_v1(
    connector_mode_t mode,
    uint32_t timeout_ms,
    uint32_t max_connections)
{
    return connector_init_v1(mode, timeout_ms, max_connections);
}

connector_result_t tot_connector_send_v1(
    const void *data,
    uint64_t size,
    uint32_t timeout_ms)
{
    return connector_send_v1(data, size, timeout_ms);
}

connector_result_t tot_connector_receive_v1(
    void *buffer,
    uint64_t *size,
    uint32_t timeout_ms)
{
    return connector_receive_v1(buffer, size, timeout_ms);
}

connector_result_t tot_connector_get_stats_v1(
    uint64_t *uptime,
    uint64_t *operations,
    uint64_t *errors,
    uint64_t *bytes)
{
    return connector_get_stats_v1(uptime, operations, errors, bytes);
}
