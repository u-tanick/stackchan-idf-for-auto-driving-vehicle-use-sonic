idf_component_register(
    SRCS
        "client.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        tl_expected
    PRIV_REQUIRES
        esp_http_client
        esp-tls
        mbedtls
        json
        esp_event
        nvs_flash
    # No EMBED_TXTFILES for a CA — we use the Mozilla cert bundle
    # (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, already enabled in the build) so
    # api.telegram.org's Go Daddy chain validates without us having to track
    # which root CA Telegram uses. ~12 KiB of flash for the bundle.
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
