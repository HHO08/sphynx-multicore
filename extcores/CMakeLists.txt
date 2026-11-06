# ============================================================================
#  Sphynx — extcores/CMakeLists.txt
#
#  Builds the external-core host as its own shared library so it can be loaded,
#  replaced or dropped without touching the PSP build. Included by the root
#  CMakeLists.txt through the SPHYNX_EXTCORE option (see the SPHYNX block at the
#  bottom of that file).
#
#  NOTHING here links against, or is linked into, the PSP emulator core.
#
#  SPDX-License-Identifier: GPL-2.0-or-later
# ============================================================================

set(SPHYNX_EXTCORE_SOURCES
	src/system_db.cpp
	src/core_loader.cpp
	src/env_callback.cpp
	src/session.cpp
	src/png_write.cpp
	src/ui_api.cpp
	# Android URI -> file resolution. Pure string work, so it is compiled for
	# every target and covered by the host tests; only the JNI fallback inside
	# it is Android-specific.
	src/android_paths.cpp
	# PSP/SYSTEMS_BIOS: the per-system BIOS folders (plain C++ so the host test
	# covers the exact code that runs on the device).
	src/bios_layout.cpp
	# Save states on disk: header + core tag + checksum.
	src/state_file.cpp
)

# JNI bridge is only meaningful on Android; it compiles to stubs elsewhere.
if(ANDROID)
	list(APPEND SPHYNX_EXTCORE_SOURCES
		jni/sphynx_extcore_jni.cpp
		src/player_android.cpp)
	set(SPHYNX_JNI TRUE)
endif()

add_library(sphynx_extcore SHARED ${SPHYNX_EXTCORE_SOURCES})

target_include_directories(sphynx_extcore PRIVATE include)
target_compile_features(sphynx_extcore PRIVATE cxx_std_17)
set_target_properties(sphynx_extcore PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	# Ship without debug symbols in release builds (size rule of the project).
	COMPILE_FLAGS "-fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections"
	LINK_FLAGS "-Wl,--gc-sections"
)

if(SPHYNX_JNI)
	target_compile_definitions(sphynx_extcore PRIVATE SPHYNX_ANDROID=1)
	# The player screen renders with raw GLES2 into its own surface.
	find_library(SPHYNX_GLES_LIB GLESv2)
	find_library(SPHYNX_EGL_LIB EGL)
	if(SPHYNX_GLES_LIB)
		target_link_libraries(sphynx_extcore PRIVATE ${SPHYNX_GLES_LIB})
	endif()
	if(SPHYNX_EGL_LIB)
		target_link_libraries(sphynx_extcore PRIVATE ${SPHYNX_EGL_LIB})
	endif()
	# liblog for the Android log, libdl for dlopen() of the cores.
	find_library(SPHYNX_LOG_LIB log)
	find_library(SPHYNX_DL_LIB dl)
	if(SPHYNX_LOG_LIB)
		target_link_libraries(sphynx_extcore PRIVATE ${SPHYNX_LOG_LIB})
	endif()
	if(SPHYNX_DL_LIB)
		target_link_libraries(sphynx_extcore PRIVATE ${SPHYNX_DL_LIB})
	endif()
endif()

# ---------------------------------------------------------------------------
# Developer/CI test: a self-contained, host-toolchain build of the same code
# plus a stub libretro core. Does not need the Android SDK, so CI can run it on
# every push in under a second.
#
#   cmake -DSPHYNX_BUILD_TESTS=ON ..
#   ctest -R sphynx
# ---------------------------------------------------------------------------
option(SPHYNX_BUILD_TESTS "Build the Sphynx host smoke test" OFF)

if(SPHYNX_BUILD_TESTS)
	add_library(sphynx_stub_core SHARED tests/stub_core.c)
	target_include_directories(sphynx_stub_core PRIVATE include)
	set_target_properties(sphynx_stub_core PROPERTIES PREFIX "lib" OUTPUT_NAME "stub_core")

	add_executable(sphynx_host_smoke
		tests/host_smoke.cpp
		src/system_db.cpp
		src/core_loader.cpp
		src/env_callback.cpp
		src/session.cpp
	)
	target_include_directories(sphynx_host_smoke PRIVATE include)
	target_compile_features(sphynx_host_smoke PRIVATE cxx_std_17)
	target_link_libraries(sphynx_host_smoke PRIVATE ${CMAKE_DL_LIBS})

	add_test(NAME sphynx_host_smoke
		COMMAND sphynx_host_smoke $<TARGET_FILE:sphynx_stub_core>
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

	# "press the game and it plays": URI resolution, list classification,
	# thumbnail rendering through a real dlopen()ed core, and the player
	# session (frames, audio, input, save state).
	add_executable(sphynx_host_play
		tests/host_play_test.cpp
		src/system_db.cpp
		src/core_loader.cpp
		src/env_callback.cpp
		src/session.cpp
		src/png_write.cpp
		src/ui_api.cpp
		src/android_paths.cpp
		src/bios_layout.cpp
		src/state_file.cpp
	)
	target_include_directories(sphynx_host_play PRIVATE include src)
	target_compile_features(sphynx_host_play PRIVATE cxx_std_17)
	target_link_libraries(sphynx_host_play PRIVATE ${CMAKE_DL_LIBS})

	add_test(NAME sphynx_host_play
		COMMAND sphynx_host_play $<TARGET_FILE:sphynx_stub_core>
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

	# BIOS tree (PSP/SYSTEMS_BIOS) + save state files.
	add_executable(sphynx_host_bios
		tests/host_bios_test.cpp
		src/system_db.cpp
		src/core_loader.cpp
		src/env_callback.cpp
		src/session.cpp
		src/png_write.cpp
		src/ui_api.cpp
		src/android_paths.cpp
		src/bios_layout.cpp
		src/state_file.cpp
	)
	target_include_directories(sphynx_host_bios PRIVATE include src)
	target_compile_features(sphynx_host_bios PRIVATE cxx_std_17)
	target_link_libraries(sphynx_host_bios PRIVATE ${CMAKE_DL_LIBS})

	add_test(NAME sphynx_host_bios
		COMMAND sphynx_host_bios
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
