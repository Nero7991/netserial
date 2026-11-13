# netserial ESP-IDF component. The portable core lives in core/ and is also built by test/ on the host.
idf_component_register(
    SRCS "core/ns_ring.c" "core/ns_frame.c" "core/ns_tx.c" "core/ns_line.c" "core/ns_lastwords.c" "netserial.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "core"
    REQUIRES log
    PRIV_REQUIRES lwip esp_timer esp_hw_support esp_system freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
