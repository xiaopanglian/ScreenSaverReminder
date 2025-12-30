cmake_minimum_required(VERSION 3.21)
project(ScreenSaverReminderCPP LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ScreenSaverReminderCPP WIN32
  src/main.cpp
  resource.rc
)

target_compile_definitions(ScreenSaverReminderCPP PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

if (MSVC)
  target_compile_options(ScreenSaverReminderCPP PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET ScreenSaverReminderCPP PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ScreenSaverReminderCPP PRIVATE
  user32
  gdi32
  shell32
  comctl32
  comdlg32
  ole32
  advapi32
)
