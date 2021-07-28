BUILD:
protobuf-3.13.0

ON:
x86 Native Tools Command Prompt for VS 2019

GUIDE:
https://github.com/protocolbuffers/protobuf/blob/master/cmake/README.md

COMMAND:
cmake -G "NMake Makefiles" -Dprotobuf_BUILD_TESTS=OFF  -DCMAKE_BUILD_TYPE=Release -Dprotobuf_MSVC_STATIC_RUNTIME=OFF  -DCMAKE_INSTALL_PREFIX=../../../../install  ../..
nmake