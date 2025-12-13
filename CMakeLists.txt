cmake_minimum_required(VERSION 3.20)
project(kio-afp VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(ECM 6.0.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMSetupVersion)

find_package(Qt6 6.5.0 REQUIRED COMPONENTS Core Widgets)
find_package(KF6 REQUIRED COMPONENTS KIO I18n Service)

qt_standard_project_setup()

add_subdirectory(src)

# Set translation domain for extraction/build
add_definitions(-DTRANSLATION_DOMAIN=\"kio_afp\")

# Install translations from po/ using KF6 i18n helpers
ki18n_install(po)

# Create a helper target `update-po` that regenerates the POT file from source
find_program(XGETTEXT_EXECUTABLE xgettext)
find_program(EXTRACTRC_EXECUTABLE extractrc)
find_program(MSGMERGE_EXECUTABLE msgmerge)

if(XGETTEXT_EXECUTABLE)
	# Collect source files to extract translatable strings from
	file(GLOB_RECURSE I18N_SOURCES
		"${CMAKE_SOURCE_DIR}/src/*.cpp"
		"${CMAKE_SOURCE_DIR}/src/*.h"
		"${CMAKE_SOURCE_DIR}/src/*.qml"
	)
	file(GLOB_RECURSE I18N_UI_FILES
		"${CMAKE_SOURCE_DIR}/src/*.ui"
		"${CMAKE_SOURCE_DIR}/src/*.kcfg"
		"${CMAKE_SOURCE_DIR}/*.rc"
	)
	set(PO_DIR "${CMAKE_SOURCE_DIR}/po")
	set(POTFILE "${PO_DIR}/kio_afp.pot")

	# Create a newline-separated list of source files at configure time
	string(REPLACE ";" "\n" I18N_SOURCES_TEXT "${I18N_SOURCES}")
	file(WRITE ${CMAKE_BINARY_DIR}/i18n_sources.list "${I18N_SOURCES_TEXT}")

	add_custom_target(update-po
		COMMENT "Regenerate PO/POT template with xgettext and extractrc"
		COMMAND ${CMAKE_COMMAND} -E make_directory ${PO_DIR}
		# Note: rc.cpp will be created by a PRE_BUILD command depending on extractrc availability
		# Run xgettext over the source files list and the generated rc.cpp file
		COMMAND ${XGETTEXT_EXECUTABLE} --from-code=UTF-8 --output=${POTFILE}
			--keyword=i18n:1
			--keyword=i18nc:1c,2
			--keyword=i18np:1,2
			--keyword=i18ncp:1c,2,3
			--files-from=${CMAKE_BINARY_DIR}/i18n_sources.list
			${CMAKE_BINARY_DIR}/rc.cpp
		COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_BINARY_DIR}/rc.cpp
		# Merge updated POT into existent PO files if msgmerge is available
		COMMAND ${CMAKE_COMMAND} -E echo "Merging POT into existing PO files..."
		COMMAND /bin/sh -c "if (${MSGMERGE_EXECUTABLE} --version >/dev/null 2>&1); then for p in ${PO_DIR}/*.po; do if [ -f \"$p\" ]; then ${MSGMERGE_EXECUTABLE} --update --backup=none \"$p\" ${POTFILE}; fi; done; else echo 'msgmerge not found, skip merging'; fi"
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		VERBATIM
	)
	if(NOT EXTRACTRC_EXECUTABLE)
		add_custom_command(TARGET update-po PRE_BUILD
			COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/rc.cpp
			COMMENT "extractrc not found: creating empty rc.cpp"
		)
	else()
		add_custom_command(TARGET update-po PRE_BUILD
			COMMAND ${EXTRACTRC_EXECUTABLE} ${I18N_UI_FILES} > ${CMAKE_BINARY_DIR}/rc.cpp
		)
	endif()
else()
	add_custom_target(update-po
		COMMAND ${CMAKE_COMMAND} -E echo "xgettext not found; update-po target is a no-op"
		COMMENT "xgettext not found: update-po skipped"
	)
endif()

# Translation support: ensure the translation domain is defined and install translations from po/
