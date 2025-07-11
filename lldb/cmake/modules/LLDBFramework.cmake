message(STATUS "LLDB.framework: build path is '${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}'")
message(STATUS "LLDB.framework: install path is '${LLDB_FRAMEWORK_INSTALL_DIR}'")
message(STATUS "LLDB.framework: resources subdirectory is 'Versions/${LLDB_FRAMEWORK_VERSION}/Resources'")

# Configure liblldb as a framework bundle
if(NOT APPLE_EMBEDDED)
  set_target_properties(liblldb PROPERTIES
    FRAMEWORK ON
    FRAMEWORK_VERSION ${LLDB_FRAMEWORK_VERSION}

    OUTPUT_NAME LLDB
    VERSION ${LLDB_VERSION}
    LIBRARY_OUTPUT_DIRECTORY ${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}

    # Compatibility version
    SOVERSION "1.0.0"

    MACOSX_FRAMEWORK_IDENTIFIER com.apple.LLDB.framework
    MACOSX_FRAMEWORK_BUNDLE_VERSION ${LLDB_VERSION}
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${LLDB_VERSION}
    MACOSX_FRAMEWORK_INFO_PLIST ${LLDB_SOURCE_DIR}/resources/LLDB-Info.plist.in
  )
else()
  set_target_properties(liblldb PROPERTIES
    FRAMEWORK ON
    FRAMEWORK_VERSION ${LLDB_FRAMEWORK_VERSION}

    # Note: iOS doesn't specify version, as the framework layout is flat.
    OUTPUT_NAME LLDB
    LIBRARY_OUTPUT_DIRECTORY ${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}

    MACOSX_FRAMEWORK_IDENTIFIER com.apple.LLDB.framework
    MACOSX_FRAMEWORK_BUNDLE_VERSION ${LLDB_VERSION}
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${LLDB_VERSION}
    MACOSX_FRAMEWORK_INFO_PLIST ${LLDB_SOURCE_DIR}/resources/LLDB-Info.plist.in
  )
endif()

# Used in llvm_add_library() to set default output directories for multi-config
# generators. Overwrite to account for special framework output directory.
set_output_directory(liblldb
  BINARY_DIR ${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}
  LIBRARY_DIR ${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}
)

lldb_add_post_install_steps_darwin(liblldb ${LLDB_FRAMEWORK_INSTALL_DIR})

# Affects the layout of the framework bundle (default is macOS layout).
if(APPLE_EMBEDDED)
  set_target_properties(liblldb PROPERTIES
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${IPHONEOS_DEPLOYMENT_TARGET}")
else()
  set_target_properties(liblldb PROPERTIES
    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "${MACOSX_DEPLOYMENT_TARGET}")
endif()

# Add -Wdocumentation parameter
set(CMAKE_XCODE_ATTRIBUTE_CLANG_WARN_DOCUMENTATION_COMMENTS "YES")

# On iOS, there is no versioned framework layout. Skip this symlink step.
if(NOT APPLE_EMBEDDED)
  # Apart from this one, CMake creates all required symlinks in the framework bundle.
  add_custom_command(TARGET liblldb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            Versions/Current/Headers
            ${LLDB_FRAMEWORK_ABSOLUTE_BUILD_DIR}/LLDB.framework/Headers
    COMMENT "LLDB.framework: create Headers symlink"
  )
endif()

find_program(unifdef_EXECUTABLE unifdef)

# All necessary header files will be staged in the include directory in the build directory,
# so just copy the files from there into the framework's staging directory.
set(lldb_build_dir_header_staging "${CMAKE_BINARY_DIR}/include/lldb")
set(lldb_framework_header_staging "${CMAKE_CURRENT_BINARY_DIR}/FrameworkHeaders")
file(GLOB lldb_build_dir_header_staging_list ${lldb_build_dir_header_staging}/*)
foreach(header ${lldb_build_dir_header_staging_list})

  get_filename_component(basename ${header} NAME)
  set(staged_header ${lldb_framework_header_staging}/${basename})

  if(unifdef_EXECUTABLE)
    # unifdef returns 0 when the file is unchanged and 1 if something was changed.
    # That means if we successfully remove SWIG code, the build system believes
    # that the command has failed and stops. This is undesirable.
    set(copy_command ${unifdef_EXECUTABLE} -USWIG -o ${staged_header} ${header} || (exit 0))
  else()
    set(copy_command ${CMAKE_COMMAND} -E copy ${header} ${staged_header})
  endif()

  add_custom_command(
    DEPENDS ${header} OUTPUT ${staged_header}
    COMMAND ${copy_command}
    COMMENT "LLDB.framework: collect framework header and remove SWIG macros")

  list(APPEND lldb_staged_headers ${staged_header})
endforeach()

# Wrap output in a target, so lldb-framework can depend on it.
add_custom_target(liblldb-resource-headers DEPENDS lldb-sbapi-dwarf-enums ${lldb_staged_headers})
set_target_properties(liblldb-resource-headers PROPERTIES FOLDER "LLDB/Resources")
# We're taking the header files from where they've been staged in the build directory's include folder,
# so create a dependency on the build step that creates that directory.
add_dependencies(liblldb-resource-headers liblldb-header-staging)
add_dependencies(liblldb liblldb-resource-headers)

# Take the headers from the staging directory and fix up their includes for the framework.
# Then write them to the output directory.
# Also, run unifdef to remove any specified guards from the header files.
file(GLOB lldb_framework_header_staging_list ${lldb_framework_header_staging}/*)
foreach(header ${lldb_framework_header_staging_list})

  set(input_header ${header})
  get_filename_component(header_basename ${input_header} NAME)
  set(output_header $<TARGET_FILE_DIR:liblldb>/Headers/${header_basename})

  add_custom_command(TARGET liblldb POST_BUILD
    COMMAND ${LLDB_SOURCE_DIR}/scripts/framework-header-fix.py -f lldb_main -i ${input_header} -o ${output_header} -p ${unifdef_EXECUTABLE} USWIG
    COMMENT "LLDB.framework: Fix up and copy framework headers"
  )
endforeach()

# Copy vendor-specific headers from clang (without staging).
if(NOT APPLE_EMBEDDED)
  if (TARGET clang-resource-headers)
    add_dependencies(liblldb clang-resource-headers)
    set(clang_resource_headers_dir $<TARGET_PROPERTY:clang-resource-headers,INTERFACE_INCLUDE_DIRECTORIES>)
  else()
    set(clang_resource_headers_dir ${LLDB_EXTERNAL_CLANG_RESOURCE_DIR}/include)
    if(NOT EXISTS ${clang_resource_headers_dir})
      message(WARNING "Expected directory for clang-resource headers not found: ${clang_resource_headers_dir}")
    endif()
  endif()

  add_custom_command(TARGET liblldb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${clang_resource_headers_dir}
            $<TARGET_FILE_DIR:liblldb>/Resources/Clang/include
    COMMENT "LLDB.framework: copy clang vendor-specific headers"
  )
endif()

# Add an rpath pointing to the directory where LLDB.framework is installed.
# This allows frameworks (relying on @rpath) to be installed in the same folder and found at runtime.
set_property(TARGET liblldb APPEND PROPERTY INSTALL_RPATH
  "@loader_path/../../../")
