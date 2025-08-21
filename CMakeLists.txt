cmake_minimum_required(VERSION 3.25.0)
project(flash_tool VERSION 0.2.0 LANGUAGES C)

set(PROJECT_SOURCES
            src/mtk_da.c
            src/mtk_device.c
            src/mtk_preloader.c
            src/util.h

            include/mtk_da.h
            include/mtk_device.h
            include/mtk_preloader.h

            flash_tool/args.c
            flash_tool/args.h
            flash_tool/io_handler.c
            flash_tool/io_handler.h
            flash_tool/main.c
            flash_tool/util.c
            flash_tool/util.h
)

set(LIBUSB_NAME usb)
include(FetchContent)
set(LIBUSB_WINDOWS_HOTPLUG yes CACHE INTERNAL "enable windows hotplug")

# this is a fork of libusb with windows hotplug support (not merged yet at the time of commit)
# TODO replace with regular libusb version
FetchContent_Declare(
        ${LIBUSB_NAME}
        GIT_REPOSITORY https://github.com/unknown321/libusb-cmake.git
        GIT_TAG 786b87593c527f2427862a1ff99157283d9f8b0e
)
FetchContent_MakeAvailable(${LIBUSB_NAME})
set(LIBUSB_FOUND ON)
set(LIBUSB_INCLUDE_DIR "")
set(LIBUSB_LIBRARY ${LIBUSB_NAME})
SET(LIBUSB_BUILD_SHARED_LIBS OFF)
FIND_PATH(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        PATH_SUFFIXES libusb
)

include_directories(
        include
        ${LIBUSB_INCLUDE_DIR}
)

add_executable(flash_tool ${PROJECT_SOURCES})

target_include_directories(flash_tool PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(flash_tool PRIVATE usb-1.0)