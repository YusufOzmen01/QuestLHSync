# QuestLHSync for Linux (Monado / WiVRn). Windows builds with build.bat.
#   libquestlhsync_shim.so   the lighthouse shim: the runtime loads it as SteamVR's lighthouse driver (questlhsync --setup)
#   questlhsync              the app: alignment, headset link, levelling, recording
#   questlhsync-dashboard    the status page (GTK 3)
#   qlhs_replay, qlhs_nettest  the offline tools
cmake_minimum_required(VERSION 3.16)
project(QuestLHSync CXX C)

if(WIN32)
  message(FATAL_ERROR "On Windows, build with build.bat")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)

set(CORE src/driver/sync.cpp src/driver/net.cpp src/driver/gravity.cpp)

add_library(questlhsync_shim SHARED
  src/linux/shim/shim.cpp src/linux/shim/inline_hook.cpp src/driver/relations.cpp
  third_party/minhook/src/hde/hde64.c)
target_include_directories(questlhsync_shim PRIVATE third_party/openvr/headers third_party/minhook/src)
target_compile_options(questlhsync_shim PRIVATE -fvisibility=hidden -Wall)
target_link_libraries(questlhsync_shim PRIVATE dl rt Threads::Threads)
set_target_properties(questlhsync_shim PROPERTIES OUTPUT_NAME questlhsync_shim)
target_link_options(questlhsync_shim PRIVATE -static-libstdc++ -static-libgcc)

add_executable(questlhsync src/linux/app/main.cpp src/linux/app/monado_link.cpp src/linux/app/steamvr_tree.cpp ${CORE})
target_include_directories(questlhsync PRIVATE third_party/monado)
target_compile_options(questlhsync PRIVATE -Wall)
target_link_libraries(questlhsync PRIVATE PkgConfig::OPENXR dl rt Threads::Threads)

if(GTK3_FOUND)
  add_executable(questlhsync-dashboard src/linux/dashboard/dashboard.cpp)
  target_compile_options(questlhsync-dashboard PRIVATE -Wall)
  target_link_libraries(questlhsync-dashboard PRIVATE PkgConfig::GTK3 rt)
else()
  message(WARNING "GTK 3 not found: no questlhsync-dashboard")
endif()

add_executable(qlhs_replay src/tools/replay.cpp src/driver/sync.cpp)
target_link_libraries(qlhs_replay PRIVATE Threads::Threads)
add_executable(qlhs_nettest src/tools/nettest.cpp ${CORE})
target_link_libraries(qlhs_nettest PRIVATE Threads::Threads)

include(GNUInstallDirs)
set(QLHS_LIBDIR ${CMAKE_INSTALL_LIBDIR}/questlhsync)
install(TARGETS questlhsync_shim LIBRARY DESTINATION ${QLHS_LIBDIR})
install(TARGETS questlhsync qlhs_replay qlhs_nettest RUNTIME DESTINATION ${QLHS_LIBDIR})
if(GTK3_FOUND)
  install(TARGETS questlhsync-dashboard RUNTIME DESTINATION ${QLHS_LIBDIR})
endif()
