cmake_minimum_required(VERSION 3.28)

project(ColmapKitFrameFeatureDeviceHarness LANGUAGES Swift)

foreach(required_variable COLMAPKIT_FRAMEWORK COLMAPKIT_FRAME_FEATURE_FIXTURE_DIR)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

add_executable(ColmapKitFrameFeatureDeviceHarness MACOSX_BUNDLE main.swift)
target_link_libraries(
  ColmapKitFrameFeatureDeviceHarness
  PRIVATE "${COLMAPKIT_FRAMEWORK}" "-framework UIKit" sqlite3)

set_target_properties(
  ColmapKitFrameFeatureDeviceHarness
  PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_EMBED_FRAMEWORKS "${COLMAPKIT_FRAMEWORK}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY YES
    XCODE_EMBED_RESOURCES "${COLMAPKIT_FRAME_FEATURE_FIXTURE_DIR}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${COLMAPKIT_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "18.0"
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
      "org.colmap.ColmapKitFrameFeatureDeviceHarness"
    XCODE_ATTRIBUTE_SWIFT_VERSION "6.0"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS iphoneos
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2")
