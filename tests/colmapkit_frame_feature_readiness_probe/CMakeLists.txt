cmake_minimum_required(VERSION 3.28)

project(ColmapKitFrameFeatureReadinessProbe LANGUAGES Swift)

add_executable(ColmapKitFrameFeatureReadinessProbe MACOSX_BUNDLE main.swift)
target_link_libraries(
  ColmapKitFrameFeatureReadinessProbe
  PRIVATE "-framework UIKit")

set_target_properties(
  ColmapKitFrameFeatureReadinessProbe
  PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${COLMAPKIT_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "18.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
      "org.colmap.ColmapKitFrameFeatureReadinessProbe"
    XCODE_ATTRIBUTE_SWIFT_VERSION "6.0"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS iphoneos
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2")
