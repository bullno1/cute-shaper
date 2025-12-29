add_executable(cute-shaper "main.c")
target_link_libraries(cute-shaper PRIVATE cute)

if (EMSCRIPTEN)
	add_custom_target(copy-emscripten-shell
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/emscripten ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/emscripten
	)
	add_library(cute-emscripten-shell INTERFACE)
	add_dependencies(cute-emscripten-shell copy-emscripten-shell)
	target_link_options(cute-emscripten-shell INTERFACE --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/emscripten/shell.html")

	set_target_properties(cute-shaper PROPERTIES OUTPUT_NAME "cute-shaper" SUFFIX ".html")
	target_compile_options(cute-shaper PRIVATE
		-fno-rtti
		-fno-exceptions
		-gsplit-dwarf
	)
	target_link_options(cute-shaper PRIVATE
		"--js-library=${CMAKE_CURRENT_LIST_DIR}/web.js"
		-sASYNCIFY=1
		-sASYNCIFY_IMPORTS=web_open_file
		-sMALLOC=emmalloc
		-sALLOW_MEMORY_GROWTH=1
		-gseparate-dwarf
	)
	target_link_libraries(cute-shaper PRIVATE cute-emscripten-shell)
else ()
	target_link_libraries(cute-shaper PRIVATE nfd)
endif ()
