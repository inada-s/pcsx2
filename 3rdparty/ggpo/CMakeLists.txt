add_library(pcsx2-ggpo STATIC
	include/ggponet.h
	include/ggpo_log.h
	lib/ggpo/backends/backend.h
	lib/ggpo/backends/p2p.cpp
	lib/ggpo/backends/p2p.h
	lib/ggpo/backends/spectator.cpp
	lib/ggpo/backends/spectator.h
	lib/ggpo/backends/synctest.cpp
	lib/ggpo/backends/synctest.h
	lib/ggpo/bitvector.cpp
	lib/ggpo/bitvector.h
	lib/ggpo/game_input.cpp
	lib/ggpo/game_input.h
	lib/ggpo/ggpo_poll.h
	lib/ggpo/ggpo_types.h
	lib/ggpo/input_queue.cpp
	lib/ggpo/input_queue.h
	lib/ggpo/log/Log.h
	lib/ggpo/log/log.cpp
	lib/ggpo/main.cpp
	lib/ggpo/network/udp.cpp
	lib/ggpo/network/udp.h
	lib/ggpo/network/udp_msg.h
	lib/ggpo/network/udp_proto.cpp
	lib/ggpo/network/udp_proto.h
	lib/ggpo/platform_linux.cpp
	lib/ggpo/platform_linux.h
	lib/ggpo/platform_windows.cpp
	lib/ggpo/platform_windows.h
	lib/ggpo/poll.cpp
	lib/ggpo/ring_buffer.h
	lib/ggpo/sleep.h
	lib/ggpo/static_buffer.h
	lib/ggpo/sync.cpp
	lib/ggpo/sync.h
	lib/ggpo/timesync.cpp
	lib/ggpo/timesync.h
)

target_include_directories(pcsx2-ggpo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(pcsx2-ggpo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/ggpo")

# GGPO throws internally and catches at its API (main.cpp); PCSX2 builds without exceptions.
if(MSVC)
	target_compile_options(pcsx2-ggpo PRIVATE /EHsc)
else()
	target_compile_options(pcsx2-ggpo PRIVATE -fexceptions)
endif()

if(WIN32)
	target_compile_definitions(pcsx2-ggpo PRIVATE NOMINMAX)
	target_link_libraries(pcsx2-ggpo PRIVATE ws2_32 winmm)
endif()

add_library(GGPO::GGPO ALIAS pcsx2-ggpo)
