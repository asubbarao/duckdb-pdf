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

# libharu (HPDF) for native in-process write_pdf (no LibreOffice).
# Use find_path (NOT pkg_check_modules) — brew libharu ships no .pc file.
# A single find_path(HPDF_INCLUDE_DIR hpdf.h) works for BOTH branches:
#   * vcpkg/CI (static): vcpkg toolchain + include layout makes find_path locate it;
#     also the GLOB *.a later will auto-include libhpdf.a (and its libpng.a etc).
#   * local POSIX (brew): we seed common paths (homebrew on arm64/x86, macports, etc).
# The include_directories below ensures headers are found for the compile of pdf_extension.cpp.
find_path(HPDF_INCLUDE_DIR
	NAMES hpdf.h
	PATHS
		${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
		/opt/homebrew/opt/libharu/include
		/usr/local/opt/libharu/include
		/opt/local/include
		/usr/local/include
		/usr/include
	PATH_SUFFIXES include
)
if(HPDF_INCLUDE_DIR)
	include_directories(${HPDF_INCLUDE_DIR})
	message(STATUS "pdf: libharu headers at ${HPDF_INCLUDE_DIR}")
else()
	# last resort — compiler may still find it via defaults or prior -I
	find_path(HPDF_INCLUDE_DIR NAMES hpdf.h)
	if(HPDF_INCLUDE_DIR)
		include_directories(${HPDF_INCLUDE_DIR})
	endif()
endif()

set(EXTENSION_SOURCES src/pdf_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(NOT WIN32)
  # ---- POSIX path: pkg-config ----
  find_package(PkgConfig REQUIRED)

  if(VCPKG_TOOLCHAIN)
    # ---- vcpkg (static) path: the community-extensions CI ----
    # vcpkg ships poppler/tesseract/leptonica as STATIC archives. We use
    # pkg-config only to locate headers + the install lib dir; we do NOT trust its
    # link line. vcpkg builds tesseract with CMake, and the generated tesseract.pc
    # does not declare the full transitive static closure: libtesseract.a
    # references libcurl + libarchive (traineddata download / archive reading),
    # which in turn pull in ssl/crypto/zstd/lzma/bz2/..., and libpoppler.a
    # references freetype/harfbuzz/jpeg/png/openjpeg/zlib. `pkg-config --static`
    # silently omits any of these not reachable via Requires.private, so the final
    # link fails with undefined curl_easy_*/archive_* symbols.
    #
    # Rather than hand-maintain a fragile dependency list, link EVERY archive vcpkg
    # installed: over-linking is harmless for a static link (the linker pulls in
    # only objects that resolve a referenced symbol). This mirrors the Windows
    # branch below. Crucially the archives are attached to the *static* extension
    # target so the closure also propagates into DuckDB's own executables
    # (duckdb / unittest / plan_serializer), which link the extension by target
    # name and -- unlike a shared library -- do not tolerate unresolved symbols.
    pkg_check_modules(POPPLER_CPP REQUIRED poppler-cpp)
    pkg_check_modules(TESSERACT   REQUIRED tesseract)
    pkg_check_modules(LEPTONICA   REQUIRED lept)
    # qpdf (pdf_merge/pdf_split/pdf_rotate): pkg-config only for headers —
    # libqpdf.a joins the link via the GLOB over-link below like every other
    # vcpkg archive. Built with default-features OFF, so qpdf uses its
    # built-in native crypto (no openssl/gnutls in the closure).
    pkg_check_modules(LIBQPDF REQUIRED libqpdf)
    include_directories(${POPPLER_CPP_INCLUDE_DIRS} ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS}
                        ${LIBQPDF_INCLUDE_DIRS})

    if(DEFINED TESSERACT_LIBDIR AND EXISTS "${TESSERACT_LIBDIR}")
      set(VCPKG_STATIC_LIBDIR "${TESSERACT_LIBDIR}")
    else()
      list(GET TESSERACT_LIBRARY_DIRS 0 VCPKG_STATIC_LIBDIR)
    endif()
    file(GLOB VCPKG_STATIC_ARCHIVES "${VCPKG_STATIC_LIBDIR}/*.a")
    if(NOT VCPKG_STATIC_ARCHIVES)
      message(FATAL_ERROR "pdf: no static archives found under '${VCPKG_STATIC_LIBDIR}'")
    endif()

    # DROP vcpkg's mbedTLS archives (libmbedcrypto/libmbedtls/libmbedx509) from the
    # over-link set: DuckDB statically bundles its OWN mbedTLS inside
    # libduckdb_static.a (third_party/mbedtls -> cipher_wrap.cpp.o etc.). curl —
    # pulled in transitively by tesseract — references mbedTLS symbols, and the
    # pdf_to_svg PNG-writer code path now reaches that closure, so ld would
    # otherwise pull cipher_wrap.c.o from BOTH vcpkg's libmbedcrypto.a AND DuckDB's
    # bundled copy, yielding `duplicate symbol _mbedtls_cipher_definitions /
    # _mbedtls_cipher_base_lookup_table`. macOS ld64 errors hard on that (Linux's
    # --start-group merely picks the first, which is why only MacOS failed). Letting
    # DuckDB's bundled mbedTLS satisfy those references resolves the collision; the
    # vcpkg curl/poppler objects link against the bundled symbols instead.
    set(VCPKG_STATIC_ARCHIVES_FILTERED "")
    foreach(_lib ${VCPKG_STATIC_ARCHIVES})
      get_filename_component(_libname "${_lib}" NAME)
      if(_libname MATCHES "^libmbed")
        message(STATUS "pdf: excluding vcpkg mbedTLS archive from over-link (DuckDB bundles its own): ${_libname}")
      else()
        list(APPEND VCPKG_STATIC_ARCHIVES_FILTERED "${_lib}")
      endif()
    endforeach()
    set(VCPKG_STATIC_ARCHIVES ${VCPKG_STATIC_ARCHIVES_FILTERED})

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # GNU ld is single-pass; wrap the whole archive set in a group so the cyclic
      # deps among poppler/tesseract/leptonica/curl/archive/crypto/freetype resolve
      # regardless of order. (mach-o ld is multi-pass and neither needs nor
      # supports this, so it is Linux-only.)
      set(PDF_POSIX_LINK -Wl,--start-group ${VCPKG_STATIC_ARCHIVES} -Wl,--end-group)
    else()
      # macOS: ld64 resolves archives order-independently, so no group is needed.
      # The static closure pulls in macOS *system* dependencies that are not vcpkg
      # archives and must be linked explicitly:
      #   * libiconv      — poppler-cpp's ustring::to_utf8 / from_utf8 call iconv.
      #   * CoreFoundation + SystemConfiguration — libcurl (pulled in by tesseract)
      #     references _CFRelease and _SCDynamicStoreCopyProxies to read the macOS
      #     system proxy configuration.
      # (On Linux iconv lives in glibc and curl needs no frameworks.)
      find_library(PDF_COREFOUNDATION CoreFoundation REQUIRED)
      find_library(PDF_SYSTEMCONFIGURATION SystemConfiguration REQUIRED)
      set(PDF_POSIX_LINK ${VCPKG_STATIC_ARCHIVES} -liconv
          ${PDF_COREFOUNDATION} ${PDF_SYSTEMCONFIGURATION})
    endif()

    target_link_libraries(${EXTENSION_NAME} ${PDF_POSIX_LINK})
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ${PDF_POSIX_LINK})
  else()
    # ---- system (shared) path: local dev via Homebrew / apt ----
    # System installs are dynamic (.dylib/.so), so the IMPORTED_TARGET shared
    # link line resolves the full dependency closure at load time. (The static
    # closure is avoided here because keg-only system libs such as gettext's
    # -lintl are not on the default linker search path.)
    pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
    pkg_check_modules(TESSERACT   REQUIRED IMPORTED_TARGET tesseract)
    pkg_check_modules(LEPTONICA   REQUIRED IMPORTED_TARGET lept)
    # qpdf (pdf_merge/pdf_split/pdf_rotate): brew/apt qpdf ships libqpdf.pc.
    pkg_check_modules(LIBQPDF     REQUIRED IMPORTED_TARGET libqpdf)
    include_directories(${POPPLER_CPP_INCLUDE_DIRS} ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS}
                        ${LIBQPDF_INCLUDE_DIRS})

    target_link_libraries(${EXTENSION_NAME}
      PkgConfig::POPPLER_CPP PkgConfig::TESSERACT PkgConfig::LEPTONICA PkgConfig::LIBQPDF)
    target_link_libraries(${LOADABLE_EXTENSION_NAME}
      PkgConfig::POPPLER_CPP PkgConfig::TESSERACT PkgConfig::LEPTONICA PkgConfig::LIBQPDF)

    # libharu for write_pdf (local brew path). Dynamic dylib; find explicitly (no pkg-config).
    # Transitive (libpng, zlib) are satisfied by the dylib itself at runtime.
    find_library(HPDF_LIBRARY
      NAMES hpdf libhpdf HPDF
      PATHS
        /opt/homebrew/opt/libharu/lib
        /usr/local/opt/libharu/lib
        /opt/local/lib
        /usr/local/lib
        /usr/lib
    )
    if(NOT HPDF_LIBRARY)
      message(FATAL_ERROR "pdf: libharu (libhpdf.dylib) not found. Run: brew install libharu")
    endif()
    target_link_libraries(${EXTENSION_NAME} ${HPDF_LIBRARY})
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ${HPDF_LIBRARY})
  endif()
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

  # qpdf (pdf_merge/pdf_split/pdf_rotate): headers live under the same vcpkg
  # include dir added above (<prefix>/include/qpdf/...), and qpdf.lib joins the
  # link via the whole-directory GLOB below. Probe for it here only so a
  # missing vcpkg qpdf install fails at configure time with a clear message
  # instead of at link time.
  find_library(QPDF_LIBRARY NAMES qpdf libqpdf)
  if(NOT QPDF_LIBRARY)
    message(FATAL_ERROR "pdf: qpdf not found in the vcpkg installed tree (is \"qpdf\" in vcpkg.json?)")
  endif()

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

  # CRITICAL: the extension objects' C runtime MUST match the vcpkg deps and
  # duckdb_static, or the link fails — either LNK2038 (RuntimeLibrary MD vs MT
  # mismatch) or LNK2005 duplicate-CRT-symbol errors (e.g. std::codecvt defined
  # in both msvcprt.lib/MSVCP140.dll and the static libs). The CRT is determined
  # by the vcpkg triplet:
  #   x64-windows-static-release  -> static CRT (/MT)   [community-extensions CI]
  #   x64-windows-static-md       -> dynamic CRT (/MD)
  # Pick to match. Requires policy CMP0091=NEW (MSVC_RUNTIME_LIBRARY), which
  # DuckDB's build already sets.
  if(VCPKG_TARGET_TRIPLET MATCHES "-md")
    set(PDF_MSVC_RUNTIME "MultiThreadedDLL")
  else()
    set(PDF_MSVC_RUNTIME "MultiThreaded")
  endif()
  set_property(TARGET ${EXTENSION_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "${PDF_MSVC_RUNTIME}")
  set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "${PDF_MSVC_RUNTIME}")

  target_link_libraries(${EXTENSION_NAME} ${WIN_PDF_LIBS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${WIN_PDF_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
