add_library(pcsx2-miniupnpc STATIC
	src/addr_is_reserved.c
	src/connecthostport.c
	src/igd_desc_parse.c
	src/minisoap.c
	src/minissdpc.c
	src/miniupnpc.c
	src/miniwget.c
	src/minixml.c
	src/portlistingparse.c
	src/receivedata.c
	src/upnpcommands.c
	src/upnpdev.c
	src/upnperrors.c
	src/upnpreplyparse.c
)

target_include_directories(pcsx2-miniupnpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(pcsx2-miniupnpc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(pcsx2-miniupnpc PUBLIC MINIUPNP_STATICLIB)

if(WIN32)
	target_compile_definitions(pcsx2-miniupnpc PRIVATE _WIN32_WINNT=0x0601)
	target_link_libraries(pcsx2-miniupnpc PRIVATE ws2_32 iphlpapi)
else()
	target_compile_definitions(pcsx2-miniupnpc PRIVATE _BSD_SOURCE _DEFAULT_SOURCE)
endif()

add_library(MiniUPnPc::MiniUPnPc ALIAS pcsx2-miniupnpc)
