# aasrouterbake: the program that bakes the lists of the AAS router ( README.txt ). The map compiler starts it
# after an AAS compile ( tools/compilers/aas/AASBuild.cpp, BakeRouterLists ) and looks for it in the base path.
#
# One translation unit: aasrouterbake.cpp includes every file of src/. Nothing in src/ is compiled by itself,
# and nothing there is edited or formatted in this tree ( SOURCES.txt says where the files come from ).

if(NOT MSVC)
	# the sources use OpenMP; without it the engine still builds, only this program is left out
	find_package(OpenMP)
	if(NOT OpenMP_CXX_FOUND)
		message(WARNING "aasrouterbake is not built: no OpenMP for ${CMAKE_CXX_COMPILER}. Without the program an AAS compile writes no router lists.")
		return()
	endif()
endif()

file(GLOB AASROUTERBAKE_SRC src/*.h src/*.cpp)

add_executable(aasrouterbake aasrouterbake.cpp ${AASROUTERBAKE_SRC})

# src/*.cpp are included by aasrouterbake.cpp like the headers: listed for the IDE, not compiled
set_source_files_properties(${AASROUTERBAKE_SRC} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("src" FILES ${AASROUTERBAKE_SRC})

if(MSVC)
	# /openmp:llvm: the sources use OpenMP 3.1, and plain /openmp is OpenMP 2.0
	# /STACK: the stack of the standalone builds the lists were proven with
	# /wd4267: size_t put into the sources' 32-bit counts; the sources are not edited in this tree
	target_compile_options(aasrouterbake PRIVATE /openmp:llvm /EHsc /wd4267)
	set_target_properties(aasrouterbake PROPERTIES LINK_FLAGS "/STACK:268435456")
	# the debug library's checked iterators make the bake about 40 times slower; this tree's Debug and
	# RelWithDebInfo builds both use that library, and their program replaces the Release one in the base path
	target_compile_definitions(aasrouterbake PRIVATE _ITERATOR_DEBUG_LEVEL=0)
else()
	# the warnings the standalone builds had switched off, and unused functions of headers written for several programs
	target_compile_options(aasrouterbake PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-function)
	target_link_libraries(aasrouterbake OpenMP::OpenMP_CXX)
	if(WIN32)
		set_target_properties(aasrouterbake PROPERTIES LINK_FLAGS "-Wl,--stack,268435456")
	endif()
endif()

# compat/ stands in for <bits/stdc++.h>, which two of the sources include, where the C++ library has no such
# header ( Visual Studio, libc++ )
include(CheckIncludeFileCXX)
check_include_file_cxx("bits/stdc++.h" AASROUTERBAKE_HAS_BITS_STDCPP)
if(NOT AASROUTERBAKE_HAS_BITS_STDCPP)
	target_include_directories(aasrouterbake PRIVATE compat)
endif()

# The program goes to the root of the tree: the base path of a game or a map compiler that is started there.
# A build of another configuration replaces it.
add_custom_command(TARGET aasrouterbake POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:aasrouterbake>" "${CMAKE_SOURCE_DIR}/../$<TARGET_FILE_NAME:aasrouterbake>"
		COMMENT "copy aasrouterbake to the root of the tree ( the base path )"
		)
