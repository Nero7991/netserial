# netserial ESP-IDF component. The portable core lives in core/ and is also built by test/ on the host.
idf_component_register(
    SRCS "core/ns_ring.c"
    INCLUDE_DIRS "core")
