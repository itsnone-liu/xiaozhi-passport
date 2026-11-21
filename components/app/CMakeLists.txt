# components/app/CMakeLists.txt
idf_component_register(
    SRCS "src/app.c" "src/app_plays.c" "src/tls_probe.c"
    INCLUDE_DIRS "include"
    REQUIRES xiaozhi ports services presentation platform_esp32 esp_timer esp_app_format esp_hw_support esp-tls mbedtls)
