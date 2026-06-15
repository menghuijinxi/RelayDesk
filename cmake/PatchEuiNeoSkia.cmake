if(NOT DEFINED RELAYDESK_SOURCE_DIR)
    message(FATAL_ERROR "RELAYDESK_SOURCE_DIR is required.")
endif()

if(NOT DEFINED EUI_NEO_SOURCE_DIR)
    message(FATAL_ERROR "EUI_NEO_SOURCE_DIR is required.")
endif()

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoAppShortcuts.cmake")

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoWindowsImagePaths.cmake")
relaydesk_patch_eui_neo_windows_image_paths("${EUI_NEO_SOURCE_DIR}")

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoSkiaTextMetrics.cmake")
relaydesk_patch_eui_neo_skia_text_metrics("${EUI_NEO_SOURCE_DIR}")

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoSkiaTextSource.cmake")
relaydesk_patch_eui_neo_skia_text_source("${EUI_NEO_SOURCE_DIR}")

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoSkiaBackend.cmake")
relaydesk_patch_eui_neo_skia_backend("${EUI_NEO_SOURCE_DIR}")
