cmake_minimum_required(VERSION 3.20)
project(d3pt C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "This build is for 64 bits")
endif()
if(MSVC)
	set(D3_WINDOWS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
		AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(D3_LINUX TRUE)
else()
	message(FATAL_ERROR "This build is for 64-bit Windows with Visual Studio 2022, or x86_64 Linux with GCC")
endif()
if(D3_LINUX AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(sources.cmake)

set(RUN_DIR ${CMAKE_SOURCE_DIR}/run)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The path tracer (pt/, MIT, engine independent). It is declared here, before
# the options the 2004 code needs, so that it is built without them. It needs
# the Vulkan SDK: headers, the loader's library and glslc.
if(D3_WINDOWS)
	find_package(Vulkan REQUIRED COMPONENTS glslc)
else()
	# Linux: glslc if it is there, or else glslangValidator, which distributions more often have
	find_package(Vulkan REQUIRED)
	find_program(PT_GLSLC glslc)
	if(NOT PT_GLSLC)
		find_program(PT_GLSLANG glslangValidator REQUIRED)
	endif()
endif()
set(PT_SHADER_DIR ${CMAKE_BINARY_DIR}/pt_shaders)
file(MAKE_DIRECTORY ${PT_SHADER_DIR})
# parts of the tracer this game has no use for are left out of its shaders: see scene.glsl
set(PT_SHADER_DEFINES -DPT_NO_WATER -DPT_NO_FOG -DPT_NO_BLUR -DPT_NO_EXPORT)
set(PT_SHADER_INC)
foreach(shader blit.vert blit.frag trace.comp temporal.comp change.comp atrous.comp compose.comp
		bloom.comp grade.comp resolve.comp)
	if(D3_WINDOWS OR PT_GLSLC)
		if(D3_WINDOWS)
			set(PT_GLSLC_COMMAND Vulkan::glslc)
		else()
			set(PT_GLSLC_COMMAND ${PT_GLSLC})
		endif()
		add_custom_command(
			OUTPUT ${PT_SHADER_DIR}/${shader}.inc
			COMMAND ${PT_GLSLC_COMMAND} -mfmt=c --target-env=vulkan1.3 -O ${PT_SHADER_DEFINES}
				${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} -o ${PT_SHADER_DIR}/${shader}.inc
			DEPENDS ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/scene.glsl
			VERBATIM)
	else()
		# glslangValidator writes the numbers alone; pt/glslang_inc.cmake puts the braces around them
		add_custom_command(
			OUTPUT ${PT_SHADER_DIR}/${shader}.inc
			COMMAND ${CMAKE_COMMAND} -DGLSLANG=${PT_GLSLANG} "-DDEFINES=${PT_SHADER_DEFINES}"
				-DIN=${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} -DOUT=${PT_SHADER_DIR}/${shader}.inc
				-P ${CMAKE_SOURCE_DIR}/pt/glslang_inc.cmake
			DEPENDS ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/${shader} ${CMAKE_SOURCE_DIR}/pt/rtx/shaders/scene.glsl
				${CMAKE_SOURCE_DIR}/pt/glslang_inc.cmake
			VERBATIM)
	endif()
	list(APPEND PT_SHADER_INC ${PT_SHADER_DIR}/${shader}.inc)
endforeach()
add_library(pt_rtx STATIC pt/rtx/pt_rtx.cpp pt/cpu/pt_world.cpp pt/cpu/pt_bvh.cpp ${PT_SHADER_INC})
target_include_directories(pt_rtx PRIVATE ${PT_SHADER_DIR})
target_include_directories(pt_rtx PUBLIC pt/include)
target_link_libraries(pt_rtx PUBLIC Vulkan::Vulkan)
target_compile_features(pt_rtx PUBLIC cxx_std_17)
# -DPT_PERF=ON: the tracer times each part of its work, for r_ptShowTimes
option(PT_PERF "time each part of the path tracer's work" OFF)
if(PT_PERF)
	target_compile_definitions(pt_rtx PUBLIC PT_PERF)
endif()
# the tracer is all small inlined functions; RelWithDebInfo would otherwise hold inlining back
if(D3_WINDOWS)
	target_compile_options(pt_rtx PRIVATE /Ob2 /Oi /Ot /EHsc)
else()
	target_compile_options(pt_rtx PRIVATE -O2)
	find_package(X11 REQUIRED)
	target_link_libraries(pt_rtx PUBLIC X11::X11)
endif()

if(D3_WINDOWS)
	# the code is C++ of 2004: let it through as it is, and say nothing of its style
	add_compile_options(/permissive /Zc:strictStrings- /W1 /GR /EHsc /fp:fast /MP)
	# a pointer put through a 32-bit integer is an error, not a warning
	add_compile_options(/we4311 /we4312 /we4302 /we4306)
	add_compile_definitions(
		WIN32 _WINDOWS
		_CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
		_WINSOCK_DEPRECATED_NO_WARNINGS _ALLOW_KEYWORD_MACROS
	)
else()
	# the same, for GCC: no warnings, and none of the liberties a compiler of today
	# takes with code that was written before it could
	add_compile_options(-w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
		-fvisibility=hidden -pipe)
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	string(APPEND CMAKE_CXX_FLAGS " -fpermissive")
	# a pointer put through a 32-bit integer is an error, not a warning
	string(APPEND CMAKE_CXX_FLAGS " -Werror=int-to-pointer-cast")
endif()
add_compile_definitions(
	__DOOM__
	ID_NO_TOOLS				# the editors need MFC
	ID_OPENAL=0				# the OpenAL and EAX libraries that came with the source are 32-bit
	ID_ENABLE_CURL=0
	ID_ENFORCE_KEY=0		# a game starts without the doomkey and xpkey files
)

if(D3_LINUX)
	list(APPEND IDLIB_SOURCES ${IDLIB_GCC_SOURCES})
endif()
add_library(idlib STATIC ${IDLIB_SOURCES})
target_compile_definitions(idlib PRIVATE __IDLIB__)

if(D3_WINDOWS)
function(game_dll name src dir sources)
	add_library(${name} SHARED ${sources} neo/${src}/Game.def)
	target_compile_definitions(${name} PRIVATE GAME_DLL ${ARGN})
	target_link_libraries(${name} PRIVATE idlib)
	set_target_properties(${name} PROPERTIES OUTPUT_NAME gamex64 PREFIX ""
		RUNTIME_OUTPUT_DIRECTORY ${RUN_DIR}/${dir}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${name}
		PDB_OUTPUT_DIRECTORY ${RUN_DIR}/${dir})
endfunction()
else()
function(game_dll name src dir sources)
	add_library(${name} MODULE ${sources})
	target_compile_definitions(${name} PRIVATE GAME_DLL ${ARGN})
	target_link_libraries(${name} PRIVATE idlib)
	target_link_options(${name} PRIVATE -Wl,--no-undefined)
	set_target_properties(${name} PROPERTIES OUTPUT_NAME gamex64 PREFIX ""
		LIBRARY_OUTPUT_DIRECTORY ${RUN_DIR}/${dir})
endfunction()
endif()

game_dll(game game base "${GAME_SOURCES}")
game_dll(game-d3xp d3xp d3xp "${D3XP_SOURCES}" _D3XP CTF)

if(D3_WINDOWS)
	add_executable(Doom3 WIN32 ${ENGINE_SOURCES} ${SYS_WIN32_SOURCES} neo/sys/win32/rc/doom_notools.rc)
	target_link_libraries(Doom3 PRIVATE idlib pt_rtx
		dbghelp dinput8 dsound dxguid glu32 iphlpapi opengl32 winmm ws2_32)
	target_link_options(Doom3 PRIVATE /STACK:16777216,16777216)
else()
	find_package(Threads REQUIRED)
	# a test program of the Vorbis library: all it holds is a main() of its own
	list(REMOVE_ITEM ENGINE_SOURCES neo/sound/OggVorbis/vorbissrc/barkmel.c)
	add_executable(Doom3 ${ENGINE_SOURCES} ${SYS_LINUX_SOURCES})
	target_link_libraries(Doom3 PRIVATE idlib pt_rtx
		X11::X11 X11::Xext X11::Xxf86vm Threads::Threads ${CMAKE_DL_LIBS})
	# the game libraries find the engine's symbols by themselves: nothing is exported to them
endif()
target_compile_definitions(Doom3 PRIVATE __DOOM_DLL__)
target_include_directories(Doom3 PRIVATE neo/sound/OggVorbis/vorbissrc)
# the .pdb files go beside the binaries, where the crash handler looks for them
set_target_properties(Doom3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RUN_DIR}
	PDB_OUTPUT_DIRECTORY ${RUN_DIR})
