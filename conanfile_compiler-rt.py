from conans import ConanFile, CMake, tools
import os


def get_revision():
    git = tools.Git()
    try:
        return git.get_revision()
    except Exception:
        return None


class CompilerRtConan(ConanFile):
    name = "fipac.compiler-rt"
    version = os.environ.get("FIPAC_VERSION", get_revision())
    license = "LLVM Release License"
    url = "https://extgit.iaik.tugraz.at/fipac/llvm-project.git"
    description = "RT library for LLVM configuration with FIPAC."
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake"
    no_copy_source = True
    scm = {
        "type": "git",
        "url": "auto",
        "revision": "auto"
    }

    def configure_cmake(self):
        helper = CMake(self)

        # Skip linking during compile checks because it will fail until
        # the runtime libraries are built and installed.
        helper.definitions["CMAKE_TRY_COMPILE_TARGET_TYPE"] = "STATIC_LIBRARY"

        helper.definitions["COMPILER_RT_DEFAULT_TARGET_ONLY"] = True

        helper.definitions["COMPILER_RT_BUILD_LIBFUZZER"] = False
        helper.definitions["COMPILER_RT_BUILD_SANITIZERS"] = False
        helper.definitions["COMPILER_RT_BUILD_XRAY"] = False
        helper.definitions["COMPILER_RT_BUILD_PROFILE"] = False

        helper.definitions["COMPILER_RT_EXCLUDE_ATOMIC_BUILTIN"] = False

        # Configure the build.
        helper.configure(source_folder="compiler-rt")
        return helper

    def build(self):
        helper = self.configure_cmake()
        helper.build()

    def package(self):
        helper = self.configure_cmake()
        helper.install()
