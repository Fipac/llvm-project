from conans import ConanFile, CMake, tools
from conans.errors import ConanException
import os
import re


def get_version():
    git = tools.Git()
    try:
        return git.get_revision()
    except:
        return None


class SceTcConan(ConanFile):
    name = "sce.bare_tc"
    version = os.environ.get("SCE_VERSION", get_version())
    license = "LLVM Release License"
    url = "https://extgit.iaik.tugraz.at/sce/llvm-project.git"
    description = "Custom toolchain configuration for secure code execution."
    settings = "os", "compiler", "build_type", "arch"
    # options = {"shared": [True, False], "split_shared_libs": [True, False]}
    # default_options = {"shared": True, "split_shared_libs": True}
    no_copy_source = True
    scm = {
        "type": "git",
        "url": "https://extgit.iaik.tugraz.at/sce/llvm-project.git",
        "revision": "auto"
    }

    def configure(self):
        # Since conan_basic_setup() is not called currently only the default libcxx is supported.
        # see https://github.com/conan-io/conan/issues/2115#issuecomment-353020236
        if self.settings.compiler == 'gcc' and float(self.settings.compiler.version.value) >= 5.1:
            if self.settings.compiler.libcxx != 'libstdc++11':
                raise ConanException("You must use the setting compiler.libcxx=libstdc++11")

    def configure_cmake(self):
        cmake = CMake(self)

        # Build LLVM as static or shared library.
        # (see https://llvm.org/docs/BuildingADistribution.html#shared-libs )
        # Individual shared libraries are only supported for development
        # due to faster incremental builds.
        development = True # self.options.shared and self.options.split_shared_libs

        cmake.definitions["BUILD_SHARED_LIBS"] = development
        # cmake.definitions["LLVM_BUILD_LLVM_DYLIB"] = self.options.shared and not development
        # cmake.definitions["LLVM_LINK_LLVM_DYLIB"] = self.options.shared and not development

        # Do not install the development and testing tools except for development builds.
        # (Building the pp tool requires currently the full install including C++ headers.)
        cmake.definitions["LLVM_INSTALL_TOOLCHAIN_ONLY"] = False  # not development

        # Configure what targets to include into the build.
        cmake.definitions["LLVM_TARGETS_TO_BUILD"] = "X86;ARM;RISCV"

        # Configure the version information.
        cmake.definitions["LLVM_VERSION_SUFFIX"] = "git"
        version = re.match("([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)", self.version)
        if version:
            cmake.definitions["LLVM_VERSION_MAJOR"] = version.group(1)
            cmake.definitions["LLVM_VERSION_MINOR"] = version.group(2)
            cmake.definitions["LLVM_VERSION_PATCH"] = version.group(3)
            cmake.definitions["LLVM_VERSION_SUFFIX"] = version.group(4)

        # Restrict link jobs because they consume a lot of memory
        # (works only with the Ninja generator).
        cmake.definitions["LLVM_PARALLEL_LINK_JOBS"] = "1"

        # The base toolchain consists of clang, and lld but will be extended
        # with compiler-rt, musl as libc, libc++, and libunwind in the future.
        cmake.definitions["LLVM_ENABLE_PROJECTS"] = "clang;lld"
        cmake.definitions["CLANG_DEFAULT_LINKER"] = "lld"
        cmake.definitions["CLANG_DEFAULT_RTLIB"] = "compiler-rt"
        # cmake.definitions["CLANG_DEFAULT_CXX_STDLIB"] = "libc++"
        # cmake.definitions["CLANG_DEFAULT_UNWINDLIB"] = "libunwind"

        cmake.configure(source_folder="llvm")
        return cmake

    def build(self):
        cmake = self.configure_cmake()
        cmake.build()

    def package(self):
        cmake = self.configure_cmake()
        cmake.install()

    def package_info(self):
        # define LLVM_DIR for cmake finding
        self.env_info.LLVM_DIR = self.package_folder
        # make built applications usable by appending the bin directory to PATH
        self.env_info.PATH = [os.path.join(self.package_folder, "bin")]
        if self.options.shared:
            self.env_info.LD_LIBRARY_PATH = [os.path.join(self.package_folder, "lib")]
