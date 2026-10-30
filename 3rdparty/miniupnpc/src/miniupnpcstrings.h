#ifndef MINIUPNPCSTRINGS_H_INCLUDED
#define MINIUPNPCSTRINGS_H_INCLUDED

#if defined(_WIN32)
#define OS_STRING "Windows"
#elif defined(__APPLE__)
#define OS_STRING "macOS"
#else
#define OS_STRING "Linux"
#endif
#define MINIUPNPC_VERSION_STRING "2.2.5"
#define UPNP_VERSION_STRING "UPnP/1.1"

#endif
