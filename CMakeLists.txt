cmake_minimum_required(VERSION 3.20)
project(d3pt C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "This build is for 64-bit Windows with Visual Studio 2022")
endif()

include(sources.cmake)

set(RUN_DIR ${CMAKE_SOURCE_DIR}/run)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The path tracer (pt/, MIT, engine independent). It is declared here, before
# the options the 2004 code needs, so that it is built without them. It needs
# the Vulkan SDK: headers, the loader's library and glslc.
find_package(Vulkan REQUIRED COMPONENTS glslc)
set(PT_SHADER_DIR ${CMAKE_BINARY_DIR}/pt_shaders)
file(MAKE_DIRECTORY ${PT_SHADER_DIR})
set(PT_SHADER_INC)
foreach(shader blit.vert blit.frag trace.comp temporal.comp change.comp atrous.comp compose.comp
		bloom.comp grade.comp resolve.comp)
	add_custom_command(
		OUTPUT ${PT_SHADER_DIR}/${shader}.inc
		COMMAND Vulkan::glslc -mfmt=c --target-env=vulkan1.3 -O
			${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} -o ${PT_SHADER_DIR}/${shader}.inc
		DEPENDS ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/scene.glsl
		VERBATIM)
	list(APPEND PT_SHADER_INC ${PT_SHADER_DIR}/${shader}.inc)
endforeach()
add_library(pt_rtx STATIC pt/rtx/pt_rtx.cpp pt/cpu/pt_world.cpp pt/cpu/pt_bvh.cpp ${PT_SHADER_INC})
target_include_directories(pt_rtx PRIVATE ${PT_SHADER_DIR})
target_include_directories(pt_rtx PUBLIC pt/include)
target_link_libraries(pt_rtx PUBLIC Vulkan::Vulkan)
target_compile_features(pt_rtx PUBLIC cxx_std_17)
# the tracer is all small inlined functions; RelWithDebInfo would otherwise hold inlining back
target_compile_options(pt_rtx PRIVATE /Ob2 /Oi /Ot /EHsc)

# the code is C++ of 2004: let it through as it is, and say nothing of its style
add_compile_options(/permissive /Zc:strictStrings- /W1 /GR /EHsc /fp:fast /MP)
# a pointer put through a 32-bit integer is an error, not a warning
add_compile_options(/we4311 /we4312 /we4302 /we4306)
add_compile_definitions(
	WIN32 _WINDOWS
	_CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
	_WINSOCK_DEPRECATED_NO_WARNINGS _ALLOW_KEYWORD_MACROS
	__DOOM__
	ID_NO_TOOLS				# the editors need MFC
	ID_OPENAL=0				# the OpenAL and EAX libraries that came with the source are 32-bit
	ID_ENABLE_CURL=0
)

add_library(idlib STATIC ${IDLIB_SOURCES})
target_compile_definitions(idlib PRIVATE __IDLIB__)

function(game_dll name src dir sources)
	add_library(${name} SHARED ${sources} neo/${src}/Game.def)
	target_compile_definitions(${name} PRIVATE GAME_DLL ${ARGN})
	target_link_libraries(${name} PRIVATE idlib)
	set_target_properties(${name} PROPERTIES OUTPUT_NAME gamex64 PREFIX ""
		RUNTIME_OUTPUT_DIRECTORY ${RUN_DIR}/${dir}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${name}
		PDB_OUTPUT_DIRECTORY ${RUN_DIR}/${dir})
endfunction()

game_dll(game game base "${GAME_SOURCES}")
game_dll(game-d3xp d3xp d3xp "${D3XP_SOURCES}" _D3XP CTF)

add_executable(Doom3 WIN32 ${ENGINE_SOURCES} neo/sys/win32/rc/doom_notools.rc)
target_compile_definitions(Doom3 PRIVATE __DOOM_DLL__)
target_include_directories(Doom3 PRIVATE neo/sound/OggVorbis/vorbissrc)
target_link_libraries(Doom3 PRIVATE idlib
	dbghelp dinput8 dsound dxguid glu32 iphlpapi opengl32 winmm ws2_32)
target_link_options(Doom3 PRIVATE /STACK:16777216,16777216)
# the .pdb files go beside the binaries, where the crash handler looks for them
set_target_properties(Doom3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RUN_DIR}
	PDB_OUTPUT_DIRECTORY ${RUN_DIR})
