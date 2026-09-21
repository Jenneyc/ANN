# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT license.

if(MSVC)
	cmake_minimum_required(VERSION 3.15)
else()
	cmake_minimum_required(VERSION 3.0)
endif()
set(CMAKE_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# add_subdirectory(graph_partition)

#set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/CompilerOptions.cmake")
if(MSVC)
	set(CMAKE_CXX_COMPILER $ENV{VCToolsInstallDir}/bin/Hostx64/x64/cl.exe)
	set(CMAKE_CXX_LINK_EXECUTABLE $ENV{VCToolsInstallDir}/bin/Hostx64/x64/link.exe)
else()
	set(CMAKE_CXX_COMPILER g++)
endif()

project(diskann)
include_directories(
	${PROJECT_SOURCE_DIR}/include 
	${PROJECT_SOURCE_DIR}/include/tsl/include 
	${PROJECT_SOURCE_DIR}/include/dll 
	${PROJECT_SOURCE_DIR}/include/ols
)

add_compile_options(
	-Wno-class-memaccess
	-Wno-unused-variable
	-Wno-unused-but-set-variable
)

# default build type set to release
# if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE Release)
add_compile_options(-g -O3 -march=native)

message("Additional definitions: " $ENV{ADDITIONAL_DEFINITIONS})

# check for AVX512 and AVX2, add the corresponding definitions.
# First, for the compiler.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx512f" COMPILER_SUPPORTS_AVX512F)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)

# Then, for the runtime.
set(CPU_SUPPORTS_AVX512 FALSE)
set(CPU_SUPPORTS_AVX2 FALSE)
try_run(
	RUN_RESULT
	COMPILE_RESULT
	"${CMAKE_BINARY_DIR}"
	SOURCES "${PROJECT_SOURCE_DIR}/tests/test_cpu.cpp"
	COMPILE_OUTPUT_VARIABLE COMPILE_OUT
	RUN_OUTPUT_VARIABLE RUN_OUT
)

if(COMPILE_RESULT AND RUN_RESULT EQUAL 3) # AVX512 + AVX2
	set(CPU_SUPPORTS_AVX512 TRUE)
	message(STATUS "CPU supports AVX512")
elseif(COMPILE_RESULT AND RUN_RESULT EQUAL 1) # AVX2
	set(CPU_SUPPORTS_AVX2 TRUE)
	message(STATUS "CPU supports AVX2")
else()
	message(STATUS "CPU does not support AVX512 or detection failed")
endif()

if(COMPILER_SUPPORTS_AVX512F AND CPU_SUPPORTS_AVX512)
    message(STATUS "Compiler and CPU supports -mavx512f. Enabling AVX512F.")
	add_definitions(-DUSE_AVX512)
elseif(COMPILER_SUPPORTS_AVX2 AND CPU_SUPPORTS_AVX2)
	message(STATUS "Compiler and CPU supports -mavx2. Enabling AVX2.")
	add_definitions(-DUSE_AVX2)
else()
    message(STATUS "Compiler or CPU do not support -mavx512f and -mavx2.")
endif()


add_definitions(
	$ENV{ADDITIONAL_DEFINITIONS}
	# -DUSE_AIO # use AIO instead of io uring (better compatibility, worse performance)
	# optimizations.
	-DOVERLAP_INIT
	# -DDYN_PIPE_WIDTH
	
	# policy.
	# -DSTATIC_POLICY

	# unoptimizations.
	# -DNAIVE_PIPE
)

include_directories(
	${PROJECT_SOURCE_DIR}/third_party/liburing/src/include
)
link_directories(
	${PROJECT_SOURCE_DIR}/third_party/liburing/src
)

link_libraries(
	aio
	uring
)
# add_compile_options(-fsanitize=address,undefined -lasan -lubsan)
# add_link_options(-fsanitize=address,undefined)
# add_compile_options(-fsanitize=undefined -lubsan)
# add_link_options(-fsanitize=undefined)
# endif()


#OpenMP
find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
else()
    message(FATAL_ERROR "No OpenMP support")
endif()


function(checkEnvAndSetLocalVar env_var msg local_var)
	if (NOT EXISTS "$ENV{${env_var}}" )
		message (FATAL_ERROR ${msg})
	else()
		if ($ENV{${env_var}} MATCHES "\\$"  OR $ENV{${env_var}} MATCHES "/$" )
			set(${local_var} $ENV{${env_var}} PARENT_SCOPE)
		else()
			message(STATUS "Appending trailing backslash to ${env_var}")
			set(${local_var} "$ENV{${env_var}}\\" PARENT_SCOPE)
		endif()
	endif()
endfunction()

	

#MKL Config
if (MSVC)
	checkEnvAndSetLocalVar("INTEL_ROOT" "Please install Intel MKL libraries and set the env variable INTEL_ROOT to the intel software directory. Should be similar to: C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\. " "INTEL_ROOT")
	set(MKL_ROOT ${INTEL_ROOT}/mkl)
	add_compile_options(/arch:AVX2 /Qpar)
	link_libraries("${INTEL_ROOT}/mkl/lib/intel64/mkl_core_dll.lib" "${INTEL_ROOT}/mkl/lib/intel64/mkl_rt.lib"  "${INTEL_ROOT}/mkl/lib/intel64/mkl_intel_thread_dll.lib" "${INTEL_ROOT}/compiler/lib/intel64/libiomp5md.lib"  "${INTEL_ROOT}/mkl/lib/intel64/mkl_intel_ilp64_dll.lib" "${INTEL_ROOT}/mkl/lib/intel64/mkl_sequential_dll.lib")
	checkEnvAndSetLocalVar("BOOST_ROOT" "Please install Boost (1.71 or greater) from www.boost.org and set the env var BOOST_ROOT to the boost directory." "BOOST_ROOT")

else()
	find_package(MKL)
        add_compile_options(-m64 -Wl,--no-as-needed)
	link_libraries(mkl_intel_ilp64 mkl_intel_thread mkl_core iomp5 pthread m dl rt)
	link_directories(${INTEL_ROOT}/lib/intel64 ${MKL_ROOT}/lib/intel64)
endif()

add_definitions(-DMKL_ILP64)
include_directories(include ${INTEL_ROOT}/include ${MKL_ROOT}/include ${BOOST_ROOT})


#Main compiler/linker settings 
if(MSVC)
	#language options
	add_compile_options(/permissive- /openmp:experimental /Zc:wchar_t /Zc:twoPhase- /Zc:forScope /Zc:inline /WX- /std:c++14 /Gd /W3 /MP /Zi /FC /nologo /diagnostics:classic) 
	#code generation options
	add_compile_options(/Qpar /fp:fast /Zp8 /fp:except- /EHsc /GS- /Gm- /Gy )
	#optimization options
	add_compile_options(/Ot /Oy /Oi)
	#path options
	#add_compile_options(/Fdx64/Release/vc141.pdb /Fox64/Release/)
	add_definitions(-DUSE_ACCELERATED_PQ -D_WINDOWS -DNOMINMAX -DUNICODE)
	
	set(CMAKE_SHARED_LIBRARY_CXX_LINK_FLAGS "/MANIFEST /MACHINE:X64 /DEBUG:FULL /LTCG:incremental /NXCOMPAT /DYNAMICBASE /OPT:REF /SUBSYSTEM:CONSOLE /MANIFESTUAC:\"level='asInvoker' uiAccess='false'\"")
	set(CMAKE_EXECUTABLE_CXX_LINK_FLAGS "/MANIFEST /MACHINE:X64 /DEBUG:FULL /LTCG:incremental /NXCOMPAT /DYNAMICBASE /OPT:REF /SUBSYSTEM:CONSOLE /MANIFESTUAC:\"level='asInvoker' uiAccess='false'\"")

	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_DEBUG")
	set(CMAKE_SHARED_LIBRARY_CXX_LINK_FLAGS_DEBUG "${CMAKE_SHARED_LIBRARY_CXX_LINK_FLAGS_DEBUG} /DEBUG")

	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${PROJECT_SOURCE_DIR}/x64/Debug)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${PROJECT_SOURCE_DIR}/x64/Debug)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${PROJECT_SOURCE_DIR}/x64/Debug)

	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR}/x64/Release)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR}/x64/Release)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR}/x64/Release)
else()
	set(ENV{TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD} 500000000000)
    #set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG -O0 -fsanitize=address -fsanitize=leak -fsanitize=undefined")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG -Wall -Wextra")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Ofast -DNDEBUG -march=native -mtune=native -ftree-vectorize")
	add_compile_options(-march=native -Wall -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fopenmp -fopenmp-simd -funroll-loops -Wfatal-errors -DUSE_ACCELERATED_PQ)
endif()


# ---------------------------------------------------------------------------
# Optional SSD simulator reader backed by the open-source SimpleSSD simulator
# (third_party/SimpleSSD-Standalone).  Enabled by default on Linux when the
# SimpleSSD sources are present; disable with -DPIPEANN_WITH_SIMSSD=OFF.
# ---------------------------------------------------------------------------
option(PIPEANN_WITH_SIMSSD "Build SimSSDFileReader backed by SimpleSSD" ON)
if(PIPEANN_WITH_SIMSSD AND NOT MSVC)
	set(SIMPLESSD_HOME ${PROJECT_SOURCE_DIR}/third_party/SimpleSSD-Standalone)
	if(EXISTS ${SIMPLESSD_HOME}/simplessd/CMakeLists.txt)
		message(STATUS "SimpleSSD found: building SSD simulator reader support")
		# SimpleSSD/DRAMPower/McPAT are old code bases built with -Werror;
		# relax it so modern compilers do not fail the build.
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")
		set(DRAMPOWER_SOURCE_DIR ${SIMPLESSD_HOME}/lib/drampower/src CACHE PATH "" FORCE)
		add_subdirectory(${SIMPLESSD_HOME}/simplessd ${CMAKE_BINARY_DIR}/simplessd EXCLUDE_FROM_ALL)

		file(GLOB SIMPLESSD_DRAMPOWER_SOURCES
			${SIMPLESSD_HOME}/lib/drampower/src/*.cc
			${SIMPLESSD_HOME}/lib/drampower/src/libdrampower/*.cc)
		add_library(simplessd_standalone STATIC
			${SIMPLESSD_HOME}/bil/entry.cc
			${SIMPLESSD_HOME}/bil/noop_scheduler.cc
			${SIMPLESSD_HOME}/igl/request/request_config.cc
			${SIMPLESSD_HOME}/igl/trace/trace_config.cc
			${SIMPLESSD_HOME}/sil/none/none.cc
			${SIMPLESSD_HOME}/sil/nvme/nvme.cc
			${SIMPLESSD_HOME}/sil/nvme/prp.cc
			${SIMPLESSD_HOME}/sil/nvme/queue.cc
			${SIMPLESSD_HOME}/sim/cfg_reader.cc
			${SIMPLESSD_HOME}/sim/engine.cc
			${SIMPLESSD_HOME}/sim/global_config.cc
			${SIMPLESSD_HOME}/util/convert.cc
			${SIMPLESSD_HOME}/util/print.cc
			${SIMPLESSD_HOME}/util/stopwatch.cc
			${SIMPLESSD_DRAMPOWER_SOURCES}
		)
		target_include_directories(simplessd_standalone PUBLIC
			${SIMPLESSD_HOME}
			${SIMPLESSD_HOME}/simplessd
			${SIMPLESSD_HOME}/simplessd/lib/mcpat
			${SIMPLESSD_HOME}/simplessd/lib/mcpat/cacti
			${SIMPLESSD_HOME}/lib/drampower/src
		)
		set_target_properties(simplessd_standalone PROPERTIES CXX_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
		target_link_libraries(simplessd_standalone PUBLIC simplessd)
		# Silence the (very verbose) legacy SimpleSSD/McPAT code base.
		foreach(tgt simplessd mcpat simplessd_standalone)
			if(TARGET ${tgt})
				target_compile_options(${tgt} PRIVATE -w)
			endif()
		endforeach()
		add_definitions(-DPIPEANN_WITH_SIMSSD)
	else()
		message(WARNING "PIPEANN_WITH_SIMSSD is ON but SimpleSSD is missing. "
			"Run: git clone https://github.com/SimpleSSD/SimpleSSD-Standalone third_party/SimpleSSD-Standalone && "
			"git -C third_party/SimpleSSD-Standalone submodule update --init")
		set(PIPEANN_WITH_SIMSSD OFF)
	endif()
endif()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(tests/utils)
