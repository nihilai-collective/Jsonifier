vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nihilai-collective/jsonifier
    REF "v${VERSION}"
    SHA512 5e30cab6599bf98a36835a9de4174ec1729bf0733d2dcfe6feaf453c7a17111d3c9d361d3bcb9b40809d23c59de52edd590dc216300e79392122b98517d3da0b
    HEAD_REF main
)

set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.md" "${SOURCE_PATH}/Third_Party_Licenses.md")
