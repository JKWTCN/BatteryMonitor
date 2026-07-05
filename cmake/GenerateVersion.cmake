if(NOT DEFINED APP_VERSION_MAJOR)
    message(FATAL_ERROR "APP_VERSION_MAJOR is required")
endif()
if(NOT DEFINED APP_VERSION_MINOR)
    message(FATAL_ERROR "APP_VERSION_MINOR is required")
endif()
if(NOT DEFINED APP_VERSION_HEADER)
    message(FATAL_ERROR "APP_VERSION_HEADER is required")
endif()
if(NOT DEFINED APP_VERSION_HEADER_IN)
    message(FATAL_ERROR "APP_VERSION_HEADER_IN is required")
endif()
if(NOT DEFINED APP_VERSION_RC)
    message(FATAL_ERROR "APP_VERSION_RC is required")
endif()
if(NOT DEFINED APP_VERSION_RC_IN)
    message(FATAL_ERROR "APP_VERSION_RC_IN is required")
endif()

string(TIMESTAMP APP_VERSION_BUILD "%y%m%d")
set(APP_VERSION_STRING "${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_VERSION_BUILD}")

# Windows VERSIONINFO stores four 16-bit integers. Split yyMMdd into yyMM/dd
# while keeping the visible product version as major.minor.yyMMdd.
math(EXPR APP_VERSION_BUILD_HIGH "${APP_VERSION_BUILD} / 100")
math(EXPR APP_VERSION_BUILD_LOW "${APP_VERSION_BUILD} % 100")

get_filename_component(APP_VERSION_HEADER_DIR "${APP_VERSION_HEADER}" DIRECTORY)
get_filename_component(APP_VERSION_RC_DIR "${APP_VERSION_RC}" DIRECTORY)
file(MAKE_DIRECTORY "${APP_VERSION_HEADER_DIR}")
file(MAKE_DIRECTORY "${APP_VERSION_RC_DIR}")

configure_file("${APP_VERSION_HEADER_IN}" "${APP_VERSION_HEADER}" @ONLY)
configure_file("${APP_VERSION_RC_IN}" "${APP_VERSION_RC}" @ONLY)
