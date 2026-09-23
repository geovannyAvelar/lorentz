# Lorentz: A black hole for Internet advertisements
# (c) 2026 Pi-hole, LLC (https://pi-hole.net)
# Network-wide ad blocking via your own hardware.
#
# Lorentz Engine
# /src/api/webui/CMakeLists.txt
#
# Embeds the web UI (../../../web) into the lorentz binary: builds its static
# export with Node.js/npm, then folds it into one generated C source file
# served from memory by webui_handler() (webui.c). See README.md, "Web UI".
#
# This file is copyright under the latest version of the EUPL.
# Please see LICENSE file for your rights under this license.

option(EMBED_WEBUI "Embed the web UI (web/) into the binary, needs Node.js/npm" OFF)

if(EMBED_WEBUI)
	find_program(NPM_EXECUTABLE npm REQUIRED)

	set(WEBUI_SOURCE_DIR ${PROJECT_SOURCE_DIR}/web)
	set(WEBUI_OUT_DIR ${WEBUI_SOURCE_DIR}/out)
	set(WEBUI_BUILD_STAMP ${CMAKE_CURRENT_BINARY_DIR}/webui_npm_build.stamp)
	set(WEBUI_GENERATED_C ${CMAKE_CURRENT_BINARY_DIR}/webui_generated.c)

	# Rebuild the static export whenever any of its sources change. Globbing
	# at configure time is fine here (unlike web/out below): these are
	# checked-in files whose set is known when CMake runs.
	file(GLOB_RECURSE WEBUI_SOURCES LIST_DIRECTORIES false
			${WEBUI_SOURCE_DIR}/app/*
			${WEBUI_SOURCE_DIR}/components/*
			${WEBUI_SOURCE_DIR}/hooks/*
			${WEBUI_SOURCE_DIR}/lib/*
			${WEBUI_SOURCE_DIR}/public/*
			)
	list(APPEND WEBUI_SOURCES
			${WEBUI_SOURCE_DIR}/package.json
			${WEBUI_SOURCE_DIR}/package-lock.json
			${WEBUI_SOURCE_DIR}/next.config.ts
			${WEBUI_SOURCE_DIR}/tsconfig.json
			)

	add_custom_command(
			OUTPUT ${WEBUI_BUILD_STAMP}
			COMMAND ${NPM_EXECUTABLE} ci
			COMMAND ${NPM_EXECUTABLE} run build:embed
			COMMAND ${CMAKE_COMMAND} -E touch ${WEBUI_BUILD_STAMP}
			WORKING_DIRECTORY ${WEBUI_SOURCE_DIR}
			DEPENDS ${WEBUI_SOURCES}
			COMMENT "Building the web UI static export (web/out)"
			VERBATIM)

	# web/out itself is not a CMake OUTPUT (its file set isn't known until the
	# command above has run), so this step depends on the stamp instead.
	add_custom_command(
			OUTPUT ${WEBUI_GENERATED_C}
			COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/generate.sh ${WEBUI_OUT_DIR} ${WEBUI_GENERATED_C}
			DEPENDS ${WEBUI_BUILD_STAMP} ${CMAKE_CURRENT_SOURCE_DIR}/generate.sh
			COMMENT "Embedding the web UI static export into a C source file"
			VERBATIM)

	set(sources ${WEBUI_GENERATED_C} webui.c webui.h)
else()
	# webui_stub.c provides an empty webui_files[] so webui_handler() (always
	# built-in) and the rest of the build need no #ifdef when EMBED_WEBUI is off.
	set(sources webui_stub.c webui.c webui.h)
endif()

add_library(api_webui OBJECT ${sources})
target_compile_options(api_webui PRIVATE ${EXTRAWARN})
target_include_directories(api_webui PRIVATE ${PROJECT_SOURCE_DIR}/src)
