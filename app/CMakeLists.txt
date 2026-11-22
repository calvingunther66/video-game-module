# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB APP_SOURCES
    "*.c*"
	"cli/*.c*"
	"imu/*.c*"
	"heatshrink/*.c"
)

add_executable(firmware
	${APP_SOURCES}
)

target_compile_definitions(firmware PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
)

# Desktop shell: -DVGM_DESKTOP=ON draws the Flipper Desktop (app/desktop/)
# on HDMI instead of mirroring the Flipper screen full size.
if(VGM_DESKTOP)
	file(GLOB DESKTOP_SOURCES "desktop/*.c")
	target_sources(firmware PRIVATE ${DESKTOP_SOURCES})
	target_compile_definitions(firmware PRIVATE VGM_DESKTOP=1)
endif()

# Link soak test: -DVGM_LINK_PING_TEST=<ms> sends an RPC ping every <ms> while
# the screen streams (see expansion_flush_outbound in uart.c).
if(VGM_LINK_PING_TEST)
	target_compile_definitions(firmware PRIVATE VGM_LINK_PING_TEST=${VGM_LINK_PING_TEST})
endif()

# Make sure our own tusb_config.h (this directory) is found ahead of the
# Pico SDK's stock pico_stdio_usb/include/tusb_config.h - see the comment at
# the top of app/tusb_config.h for why this needs to be "ours" now that USB
# is a composite CDC+HID device instead of CDC-only.
target_include_directories(firmware BEFORE PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(firmware
	pico_stdlib
	pico_multicore
	pico_util
	hardware_spi
	libdvi
	libsprite
	FreeRTOS
	protobuf
	# Marked (not "_unmarked") tinyusb_device: linking this defines
	# LIB_TINYUSB_DEVICE=1 for this target, which is what hands descriptor
	# ownership and the tud_init()/tud_task() lifecycle to us (usb_hid.c,
	# usb_descriptors.c) instead of pico_stdio_usb's own CDC-only versions -
	# see tusb_config.h. pico_stdio_usb (pulled in below via
	# pico_enable_stdio_usb) still supplies the CDC glue the existing CLI
	# runs on.
	tinyusb_device
)

# Increase XOSC startup delay to improve boot reliability
add_definitions(-DPICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# create map/bin/hex file etc.
pico_add_extra_outputs(firmware)

# disable stdio for uart
pico_enable_stdio_uart(firmware 0)
pico_enable_stdio_usb(firmware 1)

# uf2 to board via cp
add_custom_target(flash_uf2
	COMMAND cp "${CMAKE_CURRENT_BINARY_DIR}/firmware.uf2" "/Volumes/RPI-RP2/firmware.uf2"
	DEPENDS firmware
)

# flash board via dap-link
add_custom_target(flash_dap
	COMMAND openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "program ${CMAKE_CURRENT_BINARY_DIR}/firmware.elf verify reset exit"
	DEPENDS firmware
)

# Build date
string(TIMESTAMP FW_BUILD_DATE "%Y-%m-%d")

# Commit
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE FW_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Branch
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE FW_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Tag
execute_process(
	COMMAND git describe --tags --abbrev=0
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE FW_GIT_TAG
	OUTPUT_STRIP_TRAILING_WHITESPACE)

if ("${FW_GIT_TAG}" STREQUAL "")
	set(FW_GIT_TAG "none")
endif()

set(INFO_FLAGS
    -DFW_BUILD_DATE="${FW_BUILD_DATE}"
    -DFW_GIT_COMMIT="${FW_GIT_COMMIT}"
    -DFW_GIT_BRANCH="${FW_GIT_BRANCH}"
    -DFW_GIT_TAG="${FW_GIT_TAG}"
)

message(STATUS "Build date: ${FW_BUILD_DATE}")
message(STATUS "FW commit: ${FW_GIT_COMMIT}")
message(STATUS "FW branch: ${FW_GIT_BRANCH}")
message(STATUS "FW tag: ${FW_GIT_TAG}")

set_property(SOURCE "cli/cli_command_device_info.cpp" APPEND PROPERTY COMPILE_OPTIONS ${INFO_FLAGS})

if(VGM_DESKTOP)
	set(FW_NAME "vgm-fw-desktop")
else()
	set(FW_NAME "vgm-fw")
endif()
set(FW_PATH "${CMAKE_CURRENT_BINARY_DIR}")
set(FW_NEW_PATH "${FW_PATH}/${FW_NAME}-${FW_GIT_TAG}")
set(FW_PATH_UF2 "${FW_NEW_PATH}.uf2")
set(FW_PATH_ELF "${FW_NEW_PATH}.elf")
set(FW_PATH_BIN "${FW_NEW_PATH}.bin")

ADD_CUSTOM_COMMAND(TARGET firmware POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy
                       	"${FW_PATH}/firmware.uf2"
                       	${FW_PATH_UF2}
                   COMMENT "Copying 'firmware.uf2' to '${FW_PATH_UF2}'")

ADD_CUSTOM_COMMAND(TARGET firmware POST_BUILD
				   COMMAND ${CMAKE_COMMAND} -E copy
					   	"${FW_PATH}/firmware.elf"
					   	${FW_PATH_ELF}
				   COMMENT "Copying 'firmware.elf' to '${FW_PATH_ELF}'")

ADD_CUSTOM_COMMAND(TARGET firmware POST_BUILD
				   COMMAND ${CMAKE_COMMAND} -E copy
					   	"${FW_PATH}/firmware.bin"
					   	${FW_PATH_BIN}
				   COMMENT "Copying 'firmware.bin' to '${FW_PATH_BIN}'")