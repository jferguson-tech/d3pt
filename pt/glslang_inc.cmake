# SPDX-License-Identifier: MIT
# Compiles one shader with glslangValidator to what glslc -mfmt=c writes:
# its words as numbers, in braces, for a C array to be made of.
# cmake -DGLSLANG=... -DDEFINES=... -DIN=... -DOUT=... -P glslang_inc.cmake
execute_process(
	COMMAND ${GLSLANG} -V --target-env vulkan1.3 --quiet ${DEFINES} -x -o ${OUT}.hex ${IN}
	RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "glslangValidator failed on ${IN}")
endif()
file(READ ${OUT}.hex words)
file(WRITE ${OUT} "{\n${words}}\n")
file(REMOVE ${OUT}.hex)
