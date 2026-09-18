if(NOT DEFINED OPENH264_PRIVATE_PREFIX OR NOT DEFINED OPENH264_INSTALL_PREFIX)
    message(FATAL_ERROR "OpenH264 install requires private and qualified prefixes")
endif()

file(GLOB openh264_libraries
     "${OPENH264_PRIVATE_PREFIX}/lib/libopenh264.so.*")
list(FILTER openh264_libraries EXCLUDE REGEX "\.so\.8$")
list(LENGTH openh264_libraries openh264_library_count)
if(NOT openh264_library_count EQUAL 1)
    message(FATAL_ERROR "OpenH264 private install must contain one versioned library")
endif()
list(GET openh264_libraries 0 openh264_library)

file(MAKE_DIRECTORY "${OPENH264_INSTALL_PREFIX}/include")
file(MAKE_DIRECTORY "${OPENH264_INSTALL_PREFIX}/lib/pkgconfig")
file(COPY "${OPENH264_PRIVATE_PREFIX}/include/wels"
     DESTINATION "${OPENH264_INSTALL_PREFIX}/include")
file(READ "${OPENH264_PRIVATE_PREFIX}/lib/pkgconfig/openh264.pc" openh264_pc)
string(REPLACE "${OPENH264_PRIVATE_PREFIX}" "${OPENH264_INSTALL_PREFIX}" openh264_pc
               "${openh264_pc}")
file(WRITE "${OPENH264_INSTALL_PREFIX}/lib/pkgconfig/openh264.pc" "${openh264_pc}")

# This is deliberately a text import script, not a runtime library. It makes FFmpeg's configure
# and link step resolve the source-built private SONAME while keeping all source-built ELF bytes
# outside the qualified prefix.
file(WRITE "${OPENH264_INSTALL_PREFIX}/lib/libopenh264.so"
     "/* Bloom OpenH264 link-time import stub; never load this file at runtime. */\n"
     "INPUT(\"${openh264_library}\")\n")
