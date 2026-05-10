project(rime-typeanything)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src typeanything_src)

set(typeanything_library rime-typeanything)
set(typeanything_deps ${rime_library} winhttp.lib shell32.lib)
set(typeanything_modules "typeanything")

# Standard rime plugin layout — produces rime-typeanything.dll merged into rime
add_library(rime-typeanything-objs OBJECT ${typeanything_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-typeanything-objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name ${typeanything_library} PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-typeanything-objs> PARENT_SCOPE)
set(plugin_deps ${typeanything_deps} PARENT_SCOPE)
set(plugin_modules ${typeanything_modules} PARENT_SCOPE)
