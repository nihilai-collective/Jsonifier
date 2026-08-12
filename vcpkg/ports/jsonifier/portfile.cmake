vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nihilai-collective/jsonifier
    REF "v${VERSION}"
    SHA512 e98fe95d35262a0455b3b14b248c5398d23a71a92000d8d1ae349db0827a34446d5b22eb24ae200c5acfe93f54ecfa942dc787392aca77539e6e271f866bba00
    HEAD_REF main
)

set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.md" "${SOURCE_PATH}/Third_Party_Licenses.md")
