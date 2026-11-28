# netserial ESP-IDF component. The portable core lives in core/ and is also built by test/ on the host.
idf_component_register(
    SRCS "core/ns_ring.c" "core/ns_frame.c" "core/ns_tx.c" "core/ns_line.c" "core/ns_lastwords.c" "netserial.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "core"
    REQUIRES log
    PRIV_REQUIRES lwip esp_timer esp_hw_support esp_system freertos
    LDFRAGMENTS "linker.lf")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Last words also record the panic handler's output (see the __wrap_panic_print_* in netserial.c).
if(CONFIG_NETSERIAL_LAST_WORDS_SIZE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=panic_print_char" "-Wl,--wrap=panic_print_str"
        "-Wl,--wrap=panic_print_hex" "-Wl,--wrap=panic_print_dec")
endif()
