vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nihilai-collective/jsonifier
    REF "v${VERSION}"
    SHA512 ce3695ee3ffda05c3f6876229d7643506f47297d9cf622b9cc1e2fb458d04ae2106f3f2fa931b12c713ee60f143367f982a441b95fa456cd20e5009b614f4640
    HEAD_REF main
)

set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.md")
