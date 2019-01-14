set(osdc_files
  Filer.cc
  ObjectCacher.cc
  Objecter.cc
  Striper.cc)
add_library(osdc STATIC ${osdc_files})
if(WITH_LTTNG AND WITH_EVENTTRACE)
  add_dependencies(osdc eventtrace_tp)
endif()
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/osdc/Objecter.cc
  APPEND APPEND_STRING
  PROPERTY COMPILE_FLAGS "-pthreads")
