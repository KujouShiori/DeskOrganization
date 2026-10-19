cmake_minimum_required(VERSION 3.25)
project(DeskOrganization LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DeskOrganization WIN32
  src/main.cpp
  src/util.cpp
  src/layout.cpp
  src/render.cpp
  src/rename.cpp
  src/geometry.cpp
  src/shellmenu.cpp
  src/box.cpp
  src/category.cpp
  src/content.cpp
  src/theme.cpp
  src/desklayout.cpp
  src/iconview.cpp
  src/droptarget.cpp
  src/fileop.cpp
  src/iconlayer.cpp
  src/organize.cpp
  src/app.cpp
)

target_include_directories(DeskOrganization PRIVATE src)

target_compile_definitions(DeskOrganization PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
)

target_compile_options(DeskOrganization PRIVATE /utf-8 /W4 /EHsc /permissive-)

target_link_libraries(DeskOrganization PRIVATE
  user32 gdi32 shell32 ole32 advapi32 comctl32 d2d1 dwrite windowscodecs shlwapi dwmapi
)
