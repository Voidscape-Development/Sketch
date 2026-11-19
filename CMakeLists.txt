cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_BUILD_LEGACY "Enable legacy build" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Svg)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg
)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  # Xcode script phases export several deployment targets that break moc's clang predefines probe
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON AUTOMOC_COMPILER_PREDEFINES OFF
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/canvas.cpp src/sources.cpp src/panel.cpp src/remote.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
