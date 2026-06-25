cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME pdf)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies: poppler-cpp (text + rendering), tesseract + leptonica (OCR).
# In CI / on POSIX these resolve via pkg-config (brew install poppler tesseract
# leptonica, or vcpkg-provided .pc files). On Windows + vcpkg we use the CONFIG
# packages and find_path/find_library instead, since pkgconf static resolution
# is unreliable there.
include_directories(src/include)

set(EXTENSION_SOURCES src/pdf_extension.cpp src/pdf_cli.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(NOT WIN32)
  # ---- POSIX path: pkg-config ----
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
  pkg_check_modules(TESSERACT   REQUIRED IMPORTED_TARGET tesseract)
  pkg_check_modules(LEPTONICA   REQUIRED IMPORTED_TARGET lept)

  include_directories(${POPPLER_CPP_INCLUDE_DIRS} ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS})

  # Link poppler-cpp + tesseract + leptonica into both the static lib and the loadable extension
  target_link_libraries(${EXTENSION_NAME}
    PkgConfig::POPPLER_CPP PkgConfig::TESSERACT PkgConfig::LEPTONICA)
  target_link_libraries(${LOADABLE_EXTENSION_NAME}
    PkgConfig::POPPLER_CPP PkgConfig::TESSERACT PkgConfig::LEPTONICA)
else()
  # ---- Windows path: vcpkg CONFIG packages + find_path/find_library ----
  find_package(Tesseract CONFIG REQUIRED)
  find_package(Leptonica CONFIG REQUIRED)

  # poppler-cpp has no CMake config export; locate it manually. vcpkg installs
  # headers under <prefix>/include/poppler/cpp and the import lib as poppler-cpp.lib.
  find_path(POPPLER_CPP_INCLUDE_DIR
    NAMES poppler-document.h
    PATH_SUFFIXES poppler/cpp)
  find_library(POPPLER_CPP_LIBRARY NAMES poppler-cpp)
  find_library(POPPLER_LIBRARY NAMES poppler)

  if(NOT POPPLER_CPP_INCLUDE_DIR OR NOT POPPLER_CPP_LIBRARY)
    message(FATAL_ERROR "poppler-cpp not found (include=${POPPLER_CPP_INCLUDE_DIR} lib=${POPPLER_CPP_LIBRARY})")
  endif()

  # Include both the cpp subdir (for poppler-document.h etc.) and its parent,
  # plus the top-level vcpkg include dir (for tesseract/baseapi.h,
  # leptonica/allheaders.h). POPPLER_INCLUDE_PARENT is <prefix>/include/poppler;
  # its parent is <prefix>/include.
  get_filename_component(POPPLER_INCLUDE_PARENT "${POPPLER_CPP_INCLUDE_DIR}" DIRECTORY)
  get_filename_component(VCPKG_INCLUDE_DIR "${POPPLER_INCLUDE_PARENT}" DIRECTORY)
  include_directories(${POPPLER_CPP_INCLUDE_DIR} ${POPPLER_INCLUDE_PARENT} ${VCPKG_INCLUDE_DIR})

  # poppler-cpp/poppler/tesseract/leptonica are static libs; on Windows the
  # transitive dependency closure (freetype, libpng, jpeg, iconv/charset, zlib,
  # lzma, openjpeg, brotli, bz2, gif, tiff, webp, AND tesseract's libcurl +
  # libarchive + openssl for model download/traineddata reading) is NOT resolved
  # automatically the way pkg-config does on POSIX. Link the whole vcpkg static
  # lib directory: over-linking is harmless for a static link (the linker only
  # pulls in referenced objects) and guarantees every transitive symbol resolves.
  get_filename_component(VCPKG_LIB_DIR "${POPPLER_CPP_LIBRARY}" DIRECTORY)
  file(GLOB VCPKG_ALL_LIBS "${VCPKG_LIB_DIR}/*.lib")

  # Order the primaries first so their references drive symbol resolution.
  set(WIN_PDF_LIBS ${POPPLER_CPP_LIBRARY})
  if(POPPLER_LIBRARY)
    list(APPEND WIN_PDF_LIBS ${POPPLER_LIBRARY})
  endif()
  list(APPEND WIN_PDF_LIBS ${VCPKG_ALL_LIBS})

  # legacy_stdio_definitions resolves __stdio_common_* / __imp_* stdio symbols
  # that the older-CRT-compiled vcpkg deps reference. The remaining libs are
  # Windows SDK import libraries needed by libcurl/openssl/libarchive (which
  # tesseract pulls in): crypt32/secur32/bcrypt (TLS/crypto), ws2_32/iphlpapi
  # (sockets), xmllite (libarchive XAR), wldap32/normaliz (curl), plus the
  # common advapi32/user32/crypt helpers.
  list(APPEND WIN_PDF_LIBS
    legacy_stdio_definitions.lib
    crypt32.lib
    secur32.lib
    bcrypt.lib
    ws2_32.lib
    iphlpapi.lib
    xmllite.lib
    wldap32.lib
    normaliz.lib
    advapi32.lib
    user32.lib
    gdi32.lib)

  # CRITICAL: the vcpkg triplet is x64-windows-static-md => deps use the DYNAMIC
  # CRT (/MD). DuckDB's extension build defaults extension objects to the STATIC
  # CRT (/MT), which causes LNK2038 RuntimeLibrary mismatch (MD vs MT) and the
  # cascade of unresolved __imp_* CRT symbols. Force our extension objects to /MD
  # so they match the vcpkg libs. Requires policy CMP0091=NEW (abstraction via
  # MSVC_RUNTIME_LIBRARY), which DuckDB's build already sets.
  set_property(TARGET ${EXTENSION_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

  target_link_libraries(${EXTENSION_NAME} ${WIN_PDF_LIBS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${WIN_PDF_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
