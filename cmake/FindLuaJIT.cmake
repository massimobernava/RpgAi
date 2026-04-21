# Locate LuaJIT library
# Defines:
#   LUAJIT_FOUND
#   LUAJIT_LIBRARIES
#   LUAJIT_INCLUDE_DIR

#=============================================================================
# Copyright 2007-2009 Kitware, Inc.  BSD License.
# Modified 2010 for LuaJIT; 2024 updated to prefer dylib on macOS to avoid
# a crash in release builds caused by LuaJIT static linking on Apple Silicon.
#=============================================================================

FIND_PATH(LUAJIT_INCLUDE_DIR lua.h
  HINTS
  $ENV{LUAJIT_DIR}
  PATH_SUFFIXES luajit-2.1 luajit-2.0 luajit2.0 luajit
  PATHS
  ~/Library/Frameworks
  /Library/Frameworks
  /usr/local
  /usr
  /sw
  /opt/local
  /opt/csw
  /opt
  /opt/homebrew/include
)

# On macOS, prefer the dynamic library: statically linking LuaJIT under
# Release (-O3 -DNDEBUG) crashes on Apple Silicon due to memory layout
# constraints of the JIT allocator.  The dylib has the correct flags baked in.
if(APPLE)
  FIND_LIBRARY(LUAJIT_LIBRARY
    NAMES luajit-5.1 luajit
    HINTS $ENV{LUAJIT_DIR}
    PATH_SUFFIXES lib64 lib
    PATHS
    ~/Library/Frameworks /Library/Frameworks
    /usr/local /usr /sw /opt/local /opt/csw /opt /opt/homebrew
  )
else()
  FIND_LIBRARY(LUAJIT_LIBRARY
    NAMES libluajit-51.a libluajit-5.1.a libluajit.a libluajit-5.1.so luajit-5.1 luajit
    HINTS $ENV{LUAJIT_DIR}
    PATH_SUFFIXES lib64 lib
    PATHS
    ~/Library/Frameworks /Library/Frameworks
    /usr/local /usr /sw /opt/local /opt/csw /opt
  )
endif()

IF(LUAJIT_LIBRARY)
  IF(UNIX AND NOT APPLE)
    FIND_LIBRARY(LUAJIT_MATH_LIBRARY m)
    FIND_LIBRARY(LUAJIT_DL_LIBRARY dl)
    SET(LUAJIT_LIBRARIES "${LUAJIT_LIBRARY};${LUAJIT_DL_LIBRARY};${LUAJIT_MATH_LIBRARY}" CACHE STRING "LuaJIT Libraries")
  ELSE()
    SET(LUAJIT_LIBRARIES "${LUAJIT_LIBRARY}" CACHE STRING "LuaJIT Libraries")
  ENDIF()
ENDIF()

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LuaJIT DEFAULT_MSG LUAJIT_LIBRARIES LUAJIT_INCLUDE_DIR)
MARK_AS_ADVANCED(LUAJIT_INCLUDE_DIR LUAJIT_LIBRARIES LUAJIT_LIBRARY LUAJIT_MATH_LIBRARY)
