# Copyright (c), ETH Zurich and UNC Chapel Hill.
# All rights reserved.

set(FOLDER_NAME "colmapkit")

set(COLMAPKIT_PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/colmapkit.h")
set(COLMAPKIT_PUBLIC_LINK_LIBS
    colmap_controllers
    colmap_exe
    colmap_feature
    colmap_scene
    colmap_sensor
    colmap_sfm
    colmap_util
)
set(COLMAPKIT_PRIVATE_LINK_LIBS
    Boost::boost
)

COLMAP_ADD_LIBRARY(
    NAME colmap_colmapkit
    SRCS
        colmapkit.h colmapkit.cc colmapkit_v2.cc model_postprocessing.cc
    PUBLIC_LINK_LIBS
        ${COLMAPKIT_PUBLIC_LINK_LIBS}
    PRIVATE_LINK_LIBS
        ${COLMAPKIT_PRIVATE_LINK_LIBS}
)
target_compile_definitions(colmap_colmapkit PRIVATE COLMAPKIT_BUILDING_LIBRARY)

if(COLMAPKIT_FRAMEWORK_ENABLED)
    if(NOT APPLE)
        message(FATAL_ERROR "COLMAPKIT_FRAMEWORK_ENABLED requires an Apple platform")
    endif()

    add_library(ColmapKit SHARED
        colmapkit.cc
        colmapkit_v2.cc
        model_postprocessing.cc
        ${COLMAPKIT_PUBLIC_HEADER}
    )
    set_target_properties(ColmapKit PROPERTIES
        FRAMEWORK TRUE
        FRAMEWORK_VERSION A
        MACOSX_FRAMEWORK_IDENTIFIER org.colmap.ColmapKit
        OUTPUT_NAME ColmapKit
        PUBLIC_HEADER "${COLMAPKIT_PUBLIC_HEADER}"
        FOLDER ${COLMAP_TARGETS_ROOT_FOLDER}/${FOLDER_NAME}
    )
    target_compile_definitions(ColmapKit PRIVATE
        COLMAPKIT_BUILDING_LIBRARY
        ${COLMAP_COMPILE_DEFINITIONS}
    )
    target_include_directories(ColmapKit PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(ColmapKit PRIVATE
        ${COLMAPKIT_PUBLIC_LINK_LIBS}
        ${COLMAPKIT_PRIVATE_LINK_LIBS}
    )
    add_custom_command(TARGET ColmapKit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:ColmapKit>/Modules"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap"
            "$<TARGET_BUNDLE_DIR:ColmapKit>/Modules/module.modulemap"
    )
    install(TARGETS ColmapKit
        FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

COLMAP_ADD_EXECUTABLE(
    NAME colmapkit_sparse_reconstruct
    SRCS
        sparse_reconstruct.cc
    LINK_LIBS
        colmap_colmapkit
)

COLMAP_ADD_EXECUTABLE(
    NAME colmapkit_v2_fixture_runner
    SRCS
        v2_fixture_runner.cc
    LINK_LIBS
        colmap_colmapkit
)

COLMAP_ADD_TEST(
    NAME model_postprocessing_test
    SRCS
        model_postprocessing_test.cc
    LINK_LIBS
        colmap_colmapkit
)

COLMAP_ADD_TEST(
    NAME sparse_reconstruction_progress_test
    SRCS
        sparse_reconstruction_progress_test.cc
    LINK_LIBS
        colmap_colmapkit
)

COLMAP_ADD_TEST(
    NAME colmapkit_v2_test
    SRCS
        colmapkit_v2_test.cc
    LINK_LIBS
        colmap_colmapkit
)
