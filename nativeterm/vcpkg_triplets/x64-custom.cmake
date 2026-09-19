set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_ENV_PASSTHROUGH __VCVARSALL_SPECTRE)
set(VCPKG_CXX_FLAGS "/Qspectre /guard:cf /W3")
set(VCPKG_C_FLAGS "/Qspectre /guard:cf /W3")
set(VCPKG_LINKER_FLAGS "/guard:cf /DYNAMICBASE /CETCOMPAT")

if(${PORT} MATCHES "libressl")
	set(VCPKG_CRT_LINKAGE dynamic)
	set(VCPKG_LIBRARY_LINKAGE dynamic)
else()
	set(VCPKG_CRT_LINKAGE static)
	set(VCPKG_LIBRARY_LINKAGE static)
endif()

if(${PORT} MATCHES "zlib")
	set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS} /sdl")
	set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} /sdl")
endif()

# NativeTerm: build the dependencies with the same toolset as the OpenSSH
# projects (v143). With VS 2026 the default is v145, whose newest MSVC may
# lack the Spectre-mitigated libraries this triplet asks for (MSB8040).
set(VCPKG_PLATFORM_TOOLSET v143)
