# Included after RPCS3's project() (build.bat passes -DCMAKE_PROJECT_INCLUDE). RPCS3's CMakeLists (the port's patch)
# looks for the optional Intel XeSS / NVIDIA Streamline SDKs in tools\ when it is configured. build.bat configures
# every time, but a plain "cmake --build build_rpcs3" / ninja would not notice an SDK copied in later: this makes the
# build configure again when tools\ or an SDK folder changes. Only folders and files that exist are listed (a missing
# one would make Ninja configure on every build).
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  get_filename_component(_port_root "${CMAKE_SOURCE_DIR}/../../.." ABSOLUTE)
  set(_port_sdk_deps "")
  foreach(_p IN ITEMS
      "tools"
      "tools/xess" "tools/xess/inc/xess" "tools/xess/inc/xess/xess_vk.h"
      "tools/streamline" "tools/streamline/include" "tools/streamline/include/sl.h")
    if(EXISTS "${_port_root}/${_p}")
      list(APPEND _port_sdk_deps "${_port_root}/${_p}")
    endif()
  endforeach()
  if(_port_sdk_deps)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_port_sdk_deps})
  endif()
  unset(_port_sdk_deps)
  unset(_port_root)
endif()
