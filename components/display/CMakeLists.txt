# the JPEG library is in ROM but seems to fail randomly (PSRAM issue?)
set(TJPGD tjpgd)

idf_component_register(SRC_DIRS . core core/ifaces  fonts
						INCLUDE_DIRS . fonts core
						REQUIRES platform_config tools esp_common
						PRIV_REQUIRES services freertos driver lwip ${TJPGD}
						EMBED_FILES note.jpg )		

if (NOT TJPGD)
add_compile_definitions(TJPGD_ROM)												
endif()

# Enable large fonts (incl. Font_Tarable7Seg_32x64) used by the native
# full-height standby clock (bigclock.c). Without this, GDS_FONT_SEGMENT/LARGE
# fall back to a small font (see gds_text.c #ifdef USE_LARGE_FONTS).
add_compile_definitions(USE_LARGE_FONTS)

set_source_files_properties(display.c
    PROPERTIES COMPILE_FLAGS
   -Wno-format-overflow )
