cmake_minimum_required(VERSION 3.25)
project(DeskOrganization LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 发布版本号的唯一来源，格式 a.b.c。src\app.rc 用它生成 exe 的版本资源，
# scripts\package.cmd 从构建目录里的 version.txt 读同一个值来命名发布产物。
# 这里故意不用 CACHE：cache 会记住上一次配置时的值，改了这一行也会被旧值盖掉，
# 出现"版本号改了、包名还是上一个"这种事。
set(DESKORG_VERSION "1.2.0")
string(REPLACE "." ";" DESKORG_VERSION_PARTS "${DESKORG_VERSION}")
list(LENGTH DESKORG_VERSION_PARTS DESKORG_VERSION_PARTS_COUNT)
if(NOT DESKORG_VERSION_PARTS_COUNT EQUAL 3)
  message(FATAL_ERROR "DESKORG_VERSION 要写成 a.b.c，现在是 ${DESKORG_VERSION}")
endif()
list(GET DESKORG_VERSION_PARTS 0 DESKORG_VER_MAJOR)
list(GET DESKORG_VERSION_PARTS 1 DESKORG_VER_MINOR)
list(GET DESKORG_VERSION_PARTS 2 DESKORG_VER_PATCH)
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${DESKORG_VERSION}")

# rc.exe 解析 ICON 的路径是相对它自己的工作目录的（也就是构建目录），所以这里把
# 图标拷成构建目录里的同名文件，src\app.rc 只写文件名。走构建步骤而不是配置时拷贝，
# 是为了改过 resources 里的图标后能自动重新编译资源，不需要手动清一次构建目录。
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/DeskOrganization.ico"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/DeskOrganization.ico"
          "${CMAKE_BINARY_DIR}/DeskOrganization.ico"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/DeskOrganization.ico"
)
set_source_files_properties(src/app.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/DeskOrganization.ico"
)

add_executable(DeskOrganization WIN32
  src/main.cpp
  src/util.cpp
  src/layout.cpp
  src/render.cpp
  src/rename.cpp
  src/merge.cpp
  src/geometry.cpp
  src/shellmenu.cpp
  src/box.cpp
  src/category.cpp
  src/content.cpp
  src/theme.cpp
  src/iconres.cpp
  src/glyphs.cpp
  src/menuicons.cpp
  src/desklayout.cpp
  src/iconview.cpp
  src/droptarget.cpp
  src/fileop.cpp
  src/iconlayer.cpp
  src/organize.cpp
  src/app.cpp
  src/app.rc
)

target_include_directories(DeskOrganization PRIVATE src)

target_compile_definitions(DeskOrganization PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
  VER_MAJOR=${DESKORG_VER_MAJOR} VER_MINOR=${DESKORG_VER_MINOR} VER_PATCH=${DESKORG_VER_PATCH}
)

target_compile_options(DeskOrganization PRIVATE /utf-8 /W4 /EHsc /permissive-)

target_link_libraries(DeskOrganization PRIVATE
  user32 gdi32 shell32 ole32 advapi32 comctl32 d2d1 dwrite windowscodecs shlwapi dwmapi
)
