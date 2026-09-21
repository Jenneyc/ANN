# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT license.

set(CMAKE_CXX_STANDARD 17)

if(MSVC)
	add_subdirectory(dll)
else()
	#file(GLOB CPP_SOURCES *.cpp)
	file(GLOB CPP_SOURCES *.cpp v2/*.cpp)
	add_library(${PROJECT_NAME} ${CPP_SOURCES})
	# add_library(${PROJECT_NAME}_s STATIC ${CPP_SOURCES})
	target_link_libraries(${PROJECT_NAME} aio)
	if(PIPEANN_WITH_SIMSSD)
		# SimpleSSD <-> DRAMPower have circular symbol references.
		target_link_libraries(${PROJECT_NAME}
			-Wl,--start-group simplessd_standalone simplessd mcpat -Wl,--end-group)
	endif()
	# target_link_libraries(${PROJECT_NAME}_s aio)
endif()
install()
