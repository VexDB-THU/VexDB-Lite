cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "VexDBLiteDemo must be generated with CMake -G Xcode")
endif()

project(VexDBLiteDemo LANGUAGES C CXX OBJCXX Swift)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(VEXDB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(VEXDB_XCFRAMEWORK "${VEXDB_ROOT}/dist/sqlite/vexdb_lite.xcframework")
if(NOT EXISTS "${VEXDB_XCFRAMEWORK}/Info.plist")
    message(FATAL_ERROR
        "Missing ${VEXDB_XCFRAMEWORK}. Run: bash build_sqlite.sh ios")
endif()

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(SWIFTUI_FRAMEWORK SwiftUI REQUIRED)
find_library(SQLITE3_LIBRARY sqlite3 REQUIRED)
find_library(SECURITY_FRAMEWORK Security REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
file(GLOB GENERAL_IMAGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/image-test-img*.jpg"
)
list(SORT GENERAL_IMAGE_SOURCES)
list(LENGTH GENERAL_IMAGE_SOURCES GENERAL_IMAGE_COUNT)
if(NOT GENERAL_IMAGE_COUNT EQUAL 20)
    message(FATAL_ERROR "Expected 20 general demo images, found ${GENERAL_IMAGE_COUNT}")
endif()
set(BUNDLED_IMAGE_RESOURCES ${GENERAL_IMAGE_SOURCES})
set(STALE_GENERATED_IMAGE_RESOURCES)
foreach(IMAGE_SOURCE IN LISTS GENERAL_IMAGE_SOURCES)
    get_filename_component(IMAGE_STEM "${IMAGE_SOURCE}" NAME_WE)
    list(APPEND STALE_GENERATED_IMAGE_RESOURCES
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/${IMAGE_STEM}.webp"
    )
endforeach()

add_executable(VexDBLiteDemo MACOSX_BUNDLE
    App/VexDBLiteDemoApp.swift
    App/ContentView.swift
    App/ExampleDataView.swift
    App/ExpandableTextBlock.swift
    App/DemoViewModel.swift
    App/EmbeddingService.swift
    App/DemoDocument.swift
    App/BundledExample.swift
    App/UserKnowledgeViewModel.swift
    App/SettingsView.swift
    App/UserDataView.swift
    App/ImageEmbeddingService.swift
    App/ImageAssetCache.swift
    App/ImageAssetSmoke.swift
    App/ImageIndexIsolationSmoke.swift
    App/BundledImageSearchSmoke.swift
    App/BundledTextSmoke.swift
    App/TextVectorImportSmoke.swift
    App/BundledImageExample.swift
    App/ImageExampleView.swift
    App/CustomImageViewModel.swift
    App/CustomImageView.swift
    Bridge/VexDBBridge.h
    Bridge/VexDBBridge.mm
    Resources/embedding-demo.txt
    Resources/embedding-demo-vectors.json
    ${BUNDLED_IMAGE_RESOURCES}
    Resources/image-demo-vectors.json
    Resources/Assets.xcassets
)

set_source_files_properties(
    Resources/embedding-demo.txt
    Resources/embedding-demo-vectors.json
    ${BUNDLED_IMAGE_RESOURCES}
    Resources/image-demo-vectors.json
    Resources/Assets.xcassets
    PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)

target_include_directories(VexDBLiteDemo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Bridge"
    "${VEXDB_ROOT}/vexdb_sqlite/include"
)
target_compile_definitions(VexDBLiteDemo PRIVATE VEXDB_SQLITE_CORE=1)
target_link_libraries(VexDBLiteDemo PRIVATE
    "${VEXDB_XCFRAMEWORK}"
    "${SQLITE3_LIBRARY}"
    "${FOUNDATION_FRAMEWORK}"
    "${SWIFTUI_FRAMEWORK}"
    "${SECURITY_FRAMEWORK}"
    "${IMAGEIO_FRAMEWORK}"
)

set_target_properties(VexDBLiteDemo PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.vexdb.lite.demo"
    XCODE_ATTRIBUTE_PRODUCT_NAME "VexDB Lite"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/VexDBLiteDemo-Bridging-Header.h"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_ENABLE_PREVIEWS "YES"
)

# Existing local build directories may still contain the previous demo resources.
add_custom_command(TARGET VexDBLiteDemo POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-sheep-car-2.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-sheep-car-3.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-2.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-llm-2.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-llm-3.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-3-real.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-agent-2.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-agent-3.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-1.png"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-sheep-car-2.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-sheep-car-3.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-2.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-llm-2.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-llm-3.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-3-real.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-agent-2.webp"
        "$<TARGET_BUNDLE_DIR:VexDBLiteDemo>/image-demo-agent-3.webp"
        ${STALE_GENERATED_IMAGE_RESOURCES}
    VERBATIM
)
