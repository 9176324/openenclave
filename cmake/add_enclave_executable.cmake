# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
#
# Helper function to create enclave binary
#
# Generates a target for an enclave binary
#
# Usage:
#
#	add_enclave_executable(
#		<name> <signconffile> <signkeyfile>
#		source1 [source2...]
#
# Target properties can be set on <name>, see add_executable for details.
#
# Restrictions: A number of subtleties are not handled, such as
# - RUNTIME_OUTPUT_DIRECTORY property is not handled correctly
# - the resulting binary name is not reflected by the target
#   (complicating install rules)
#
if (USE_CLANG)
	message("Here-add_enclave_executable!!!")
	
	# Setup compilers
	set(CMAKE_C_COMPILE_OBJECT
		"clang -target x86_64-pc-linux <DEFINES> <INCLUDES> -g -fPIE -DOE_BUILD_ENCLAVE -o <OBJECT> -c <SOURCE>")
	
	set(CMAKE_CXX_COMPILE_OBJECT
		"clang -target x86_64-pc-linux <DEFINES> <INCLUDES> -g -fPIE -DOE_BUILD_ENCLAVE -o <OBJECT> -c <SOURCE>")

	# Setup linker
	find_program(LD_LLD "ld.lld.exe")
	set(CMAKE_EXECUTABLE_SUFFIX ".so")
	set(CMAKE_C_STANDARD_LIBRARIES "")
	set(CMAKE_C_LINK_EXECUTABLE
    	"clang -target x86_64-pc-linux <OBJECTS> -o <TARGET>  <LINK_LIBRARIES> -fuse-ld=\"${LD_LLD}\"")
	set(CMAKE_CXX_STANDARD_LIBRARIES "")		
	set(CMAKE_CXX_LINK_EXECUTABLE
    	"clang -target x86_64-pc-linux <OBJECTS> -o <TARGET>  <LINK_LIBRARIES> -fuse-ld=\"${LD_LLD}\"")
endif()


function(add_enclave_executable BIN SIGNCONF)
	if (USE_CLANG)
		add_library(${BIN} STATIC)
		set_target_properties(${BIN} PROPERTIES 
			SUFFIX ".so"
		) 	
		set(CMAKE_C_STANDARD_LIBRARIES_INIT "")
		message("Foooo1")	
	else()
		add_executable(${BIN} ${ARGN})
	endif()

	# custom rule to generate signing key
	add_custom_command(OUTPUT ${BIN}-private.pem
		COMMAND openssl genrsa -out ${BIN}-private.pem -3 3072
		)

	# custom rule to sign the binary
	add_custom_command(OUTPUT ${BIN}.signed.so
		COMMAND oesign $<TARGET_FILE:${BIN}> ${SIGNCONF} ${CMAKE_CURRENT_BINARY_DIR}/${BIN}-private.pem
		DEPENDS oesign ${BIN} ${SIGNCONF} ${BIN}-private.pem
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		)

	# signed binary is a default target
	add_custom_target(${BIN}-signed ALL
		DEPENDS ${BIN}.signed.so
		)



endfunction(add_enclave_executable)




