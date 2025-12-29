add_executable(cute-shaper "main.c")
target_link_libraries(cute-shaper PRIVATE cute)

if (EMSCRIPTEN)
	target_link_options(cute-shaper PRIVATE
		"--js-library=${CMAKE_CURRENT_LIST_DIR}/web.js"
	)
	set_target_properties(cute-shaper PROPERTIES OUTPUT_NAME "cute-shaper" SUFFIX ".html")
	target_compile_options(cute-shaper PRIVATE
		-fno-rtti
		-fno-exceptions
		-gsplit-dwarf
	)
	target_link_options(cute-shaper PRIVATE
		-sASYNCIFY=1
		-sASYNCIFY_IMPORTS=web_open_file
		-sMALLOC=emmalloc
		-sALLOW_MEMORY_GROWTH=1
		-gseparate-dwarf
	)

	# Force recompile on JS change
	target_sources(cute-shaper PRIVATE web.js)
	set_source_files_properties(web.js PROPERTIES HEADER_FILE_ONLY TRUE)
else ()
	target_link_libraries(cute-shaper PRIVATE nfd)
endif ()
