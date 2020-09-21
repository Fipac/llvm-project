from conans import ConanFile, CMake, tools
import os


def get_revision():
    git = tools.Git()
    try:
        return git.get_revision()
    except Exception:
        return None


class CxxLibsConan(ConanFile):
    name = "fipac.libunwind"
    version = os.environ.get("FIPAC_VERSION", get_revision())
    license = "LLVM Release License"
    url = "https://extgit.iaik.tugraz.at/fipac/llvm-project.git"
    description = "C++ unwinding library for the LLVM configuration with FIPAC."
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake"
    no_copy_source = True
    scm = {
        "type": "git",
        "url": "auto",
        "revision": "auto"
    }

    def configure_cmake(self):
        cmake = CMake(self)

        cmake.definitions["LLVM_PATH"] = os.path.join(self.source_folder, "llvm")

        # Skip linking during compile checks because it will fail until
        # the runtime libraries are built and installed.
        cmake.definitions["CMAKE_TRY_COMPILE_TARGET_TYPE"] = "STATIC_LIBRARY"

        cmake.definitions["LIBUNWIND_USE_COMPILER_RT"] = True

        # Suppress the libstdc++ version check in CheckCompilerVersion.cmake
        # since we do not use it anyway.
        cmake.definitions["LLVM_ENABLE_LIBCXX"] = True

        # Configure the build.
        cmake.configure(source_folder="libunwind")
        return cmake

    def build(self):
        cmake = self.configure_cmake()
        cmake.build()

    def package(self):
        cmake = self.configure_cmake()
        cmake.install()
