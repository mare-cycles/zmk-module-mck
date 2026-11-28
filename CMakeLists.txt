zephyr_include_directories(include)

if(CONFIG_MCK_AAA_BATTERY)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/battery/aaa_battery.c)
endif()

if(CONFIG_MCK_IQS5XX)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/trackpad/iqs5xx.c)
endif()

if(CONFIG_MCK_TRACKPAD_SETTINGS)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/trackpad/tp_settings.c)
endif()

if(CONFIG_MCK_BEHAVIOR_TP_SCROLL)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/behaviors/behavior_tp_scroll.c)
endif()

if(CONFIG_MCK_SETTINGS)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/settings/mck_settings.c)
endif()

# Custom ZMK Studio RPC: mck__device and (2026-11-28, D-136 T5) mck__trackpad.
if(CONFIG_MCK_DEVICE_STUDIO_RPC)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/device_handler.c)
endif()
if(CONFIG_MCK_TRACKPAD_STUDIO_RPC)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/trackpad_handler.c)
endif()

if(CONFIG_MCK_DEVICE_STUDIO_RPC OR CONFIG_MCK_TRACKPAD_STUDIO_RPC)
    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    # NOTE: adding to app target directly causes build issues, so we create a library instead
    zephyr_library()
    file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

    nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

    # app should depend on the generated proto files
    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()
