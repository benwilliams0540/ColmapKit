set(FOLDER_NAME "thirdparty/SiftMetal")

set(SIFT_METAL_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SIFT_METAL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(SIFT_METAL_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/sift.metallib")
set(SIFT_METAL_MODULE_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal-module-cache")

set(SIFT_METAL_COMPILE_FLAGS)
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        if(CMAKE_OSX_SYSROOT MATCHES "iPhoneSimulator")
            set(SIFT_METAL_AIR_TARGET
                "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator")
        else()
            set(SIFT_METAL_AIR_TARGET
                "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
    else()
        set(SIFT_METAL_AIR_TARGET
            "air64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    list(APPEND SIFT_METAL_COMPILE_FLAGS -target ${SIFT_METAL_AIR_TARGET})
endif()
if(IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}")
    list(APPEND SIFT_METAL_COMPILE_FLAGS -isysroot ${CMAKE_OSX_SYSROOT})
endif()

find_program(XCRUN_EXECUTABLE xcrun)
find_program(METAL_EXECUTABLE metal)
find_program(METALLIB_EXECUTABLE metallib)

if(XCRUN_EXECUTABLE)
    execute_process(
        COMMAND ${XCRUN_EXECUTABLE} --find metal
        RESULT_VARIABLE XCRUN_METAL_RESULT
        OUTPUT_VARIABLE XCRUN_METAL_EXECUTABLE
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(XCRUN_METAL_RESULT EQUAL 0 AND XCRUN_METAL_EXECUTABLE)
        set(METAL_EXECUTABLE "${XCRUN_METAL_EXECUTABLE}")
    endif()

    execute_process(
        COMMAND ${XCRUN_EXECUTABLE} --find metallib
        RESULT_VARIABLE XCRUN_METALLIB_RESULT
        OUTPUT_VARIABLE XCRUN_METALLIB_EXECUTABLE
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(XCRUN_METALLIB_RESULT EQUAL 0 AND XCRUN_METALLIB_EXECUTABLE)
        set(METALLIB_EXECUTABLE "${XCRUN_METALLIB_EXECUTABLE}")
    endif()
endif()

if(NOT METAL_EXECUTABLE)
    message(FATAL_ERROR "SiftMetal requires the Metal shader compiler (`metal`)")
endif()

if(NOT METALLIB_EXECUTABLE)
    set(SIFT_METAL_LINKER_EXECUTABLE "${METAL_EXECUTABLE}")
    set(SIFT_METAL_LINKER_FLAGS
        ${SIFT_METAL_COMPILE_FLAGS}
        "-fmodules-cache-path=${SIFT_METAL_MODULE_CACHE_DIR}"
    )
    message(STATUS
        "SiftMetal: `metallib` not found; using the Metal driver to link `sift.metallib`"
    )
else()
    set(SIFT_METAL_LINKER_EXECUTABLE "${METALLIB_EXECUTABLE}")
    set(SIFT_METAL_LINKER_FLAGS)
endif()

set(METAL_SHADERS
    ${SIFT_METAL_SHADER_DIR}/BilinearUpScale.metal
    ${SIFT_METAL_SHADER_DIR}/ConvertSRGBToGrayscale.metal
    ${SIFT_METAL_SHADER_DIR}/Convolution.metal
    ${SIFT_METAL_SHADER_DIR}/ConvolutionSeries.metal
    ${SIFT_METAL_SHADER_DIR}/NearestNeighborDownScale.metal
    ${SIFT_METAL_SHADER_DIR}/NearestNeighborUpScale.metal
    ${SIFT_METAL_SHADER_DIR}/SIFTDescriptor.metal
    ${SIFT_METAL_SHADER_DIR}/SIFTExtrema.metal
    ${SIFT_METAL_SHADER_DIR}/SIFTGradient.metal
    ${SIFT_METAL_SHADER_DIR}/SIFTInterpolate.metal
    ${SIFT_METAL_SHADER_DIR}/SIFTOrientation.metal
    ${SIFT_METAL_SHADER_DIR}/Subtract.metal
)

set(METAL_SHADER_HEADERS
    ${SIFT_METAL_SHADER_DIR}/Common.hpp
    ${SIFT_METAL_INCLUDE_DIR}/ConvolutionSeries.h
    ${SIFT_METAL_INCLUDE_DIR}/NearestNeighbor.h
    ${SIFT_METAL_INCLUDE_DIR}/SIFTDescriptor.h
    ${SIFT_METAL_INCLUDE_DIR}/SIFTExtrema.h
    ${SIFT_METAL_INCLUDE_DIR}/SIFTInterpolate.h
    ${SIFT_METAL_INCLUDE_DIR}/SIFTOrientation.h
)

set(AIR_FILES)
foreach(SHADER ${METAL_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
    set(AIR_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.air")
    add_custom_command(
        OUTPUT ${AIR_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SIFT_METAL_MODULE_CACHE_DIR}
        COMMAND ${METAL_EXECUTABLE}
            ${SIFT_METAL_COMPILE_FLAGS}
            -c ${SHADER}
            -o ${AIR_FILE}
            -I ${SIFT_METAL_INCLUDE_DIR}
            -I ${SIFT_METAL_SHADER_DIR}
            -fmodules-cache-path=${SIFT_METAL_MODULE_CACHE_DIR}
        DEPENDS ${SHADER} ${METAL_SHADER_HEADERS}
        COMMENT "Compiling Metal shader ${SHADER_NAME}.metal"
        VERBATIM
    )
    list(APPEND AIR_FILES ${AIR_FILE})
endforeach()

add_custom_command(
    OUTPUT ${SIFT_METAL_METALLIB}
    COMMAND ${SIFT_METAL_LINKER_EXECUTABLE}
        ${AIR_FILES}
        -o ${SIFT_METAL_METALLIB}
        ${SIFT_METAL_LINKER_FLAGS}
    DEPENDS ${AIR_FILES}
    COMMENT "Linking Metal library sift.metallib"
    VERBATIM
)

add_custom_target(colmap_sift_metal_metallib DEPENDS ${SIFT_METAL_METALLIB})
set_property(GLOBAL PROPERTY COLMAP_SIFT_METAL_METALLIB
             "${SIFT_METAL_METALLIB}")

COLMAP_ADD_LIBRARY(
    NAME colmap_sift_metal
    SRCS
        SiftMetal.mm
        SiftMetal.h
    PUBLIC_LINK_LIBS
        ${COLMAP_METAL_LIBRARIES}
)

add_dependencies(colmap_sift_metal colmap_sift_metal_metallib)

target_include_directories(colmap_sift_metal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${SIFT_METAL_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(colmap_sift_metal PRIVATE
    SIFT_METAL_BUILD_METALLIB_PATH="${SIFT_METAL_METALLIB}"
)

set_source_files_properties(SiftMetal.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc"
)

install(FILES ${SIFT_METAL_METALLIB}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/colmap/metal)
