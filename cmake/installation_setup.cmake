# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/installation_setup.cmake

set(CONFIG_FILE_NAME ${PROJECT_NAME}Config.cmake)
set(EXPORTED_TARGETS_NAME ${PROJECT_NAME}Targets)
set(EXPORTED_TARGETS_FILE_NAME ${EXPORTED_TARGETS_NAME}.cmake)
set(EXPORTED_TARGETS_FILE_PATH share/jsonifier/${EXPORTED_TARGETS_FILE_NAME})

include(CMakePackageConfigHelpers)
configure_package_config_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/${CONFIG_FILE_NAME}.in
	${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_FILE_NAME}
	INSTALL_DESTINATION share/jsonifier
	PATH_VARS
	EXPORTED_TARGETS_FILE_PATH
)

set(VERSION_FILE_NAME ${PROJECT_NAME}ConfigVersion.cmake)

write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/${VERSION_FILE_NAME}
	VERSION ${PRODUCT_VERSION}
	COMPATIBILITY AnyNewerVersion
)

install(
	FILES
	${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_FILE_NAME}
	${CMAKE_CURRENT_BINARY_DIR}/${VERSION_FILE_NAME}
	DESTINATION share/jsonifier
)

install(
	DIRECTORY
	${CMAKE_CURRENT_SOURCE_DIR}/include/
	DESTINATION include
)

install(
	TARGETS ${PROJECT_NAME}
	EXPORT ${EXPORTED_TARGETS_NAME}
)

install(
	EXPORT ${EXPORTED_TARGETS_NAME}
	FILE ${EXPORTED_TARGETS_FILE_NAME}
	NAMESPACE ${PROJECT_NAME}::
	DESTINATION share/jsonifier
)
