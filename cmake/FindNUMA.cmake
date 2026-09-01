# FindNUMA.cmake — locate libnuma (no upstream CMake package config exists).
#
# Defines the imported target NUMA::NUMA and the standard
# NUMA_FOUND / NUMA_INCLUDE_DIR / NUMA_LIBRARY variables.

find_path(NUMA_INCLUDE_DIR numa.h)
find_library(NUMA_LIBRARY NAMES numa)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NUMA
  REQUIRED_VARS NUMA_LIBRARY NUMA_INCLUDE_DIR
)

mark_as_advanced(NUMA_INCLUDE_DIR NUMA_LIBRARY)

if(NUMA_FOUND AND NOT TARGET NUMA::NUMA)
  add_library(NUMA::NUMA UNKNOWN IMPORTED)
  set_target_properties(NUMA::NUMA PROPERTIES
    IMPORTED_LOCATION "${NUMA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NUMA_INCLUDE_DIR}"
  )
endif()
