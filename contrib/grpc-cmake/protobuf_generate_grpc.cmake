#[[
Add custom commands to process ``.proto`` files to C++::
protobuf_generate_grpc_cpp(<SRCS> <HDRS>
    [DESCRIPTORS <DESC>] [EXPORT_MACRO <MACRO>] [<ARGN>...])

``SRCS``
    Variable to define with autogenerated source files
``HDRS``
    Variable to define with autogenerated header files
``DESCRIPTORS``
    Variable to define with autogenerated descriptor files, if requested.
``EXPORT_MACRO``
    is a macro which should expand to ``__declspec(dllexport)`` or
    ``__declspec(dllimport)`` depending on what is being compiled.
``ARGN``
    ``.proto`` files
#]]

# Function to generate C++ files from .proto files.
# This function is a modified version of the function PROTOBUF_GENERATE_CPP() copied from https://github.com/Kitware/CMake/blob/master/Modules/FindProtobuf.cmake.
function(PROTOBUF_GENERATE_GRPC_CPP SRCS HDRS)
  cmake_parse_arguments(protobuf_generate_grpc_cpp "" "EXPORT_MACRO;DESCRIPTORS" "" ${ARGN})

  set(_proto_files "${protobuf_generate_grpc_cpp_UNPARSED_ARGUMENTS}")
  if(NOT _proto_files)
    message(SEND_ERROR "Error: PROTOBUF_GENERATE_GRPC_CPP() called without any proto files")
    return()
  endif()

  if(PROTOBUF_GENERATE_GRPC_CPP_APPEND_PATH)
    set(_append_arg APPEND_PATH)
  endif()

  if(protobuf_generate_grpc_cpp_DESCRIPTORS)
    set(_descriptors DESCRIPTORS)
  endif()

  if(DEFINED PROTOBUF_IMPORT_DIRS AND NOT DEFINED Protobuf_IMPORT_DIRS)
    set(Protobuf_IMPORT_DIRS "${PROTOBUF_IMPORT_DIRS}")
  endif()

  if(DEFINED Protobuf_IMPORT_DIRS)
    set(_import_arg IMPORT_DIRS ${Protobuf_IMPORT_DIRS})
  endif()

  set(_outvar)
  protobuf_generate_grpc(${_append_arg} ${_descriptors} LANGUAGE cpp EXPORT_MACRO ${protobuf_generate_cpp_EXPORT_MACRO} OUT_VAR _outvar ${_import_arg} PROTOS ${_proto_files})

  set(${SRCS})
  set(${HDRS})
  if(protobuf_generate_grpc_cpp_DESCRIPTORS)
    set(${protobuf_generate_grpc_cpp_DESCRIPTORS})
  endif()

  foreach(_file ${_outvar})
    if(_file MATCHES "cc$")
      list(APPEND ${SRCS} ${_file})
    elseif(_file MATCHES "desc$")
      list(APPEND ${protobuf_generate_grpc_cpp_DESCRIPTORS} ${_file})
    else()
      list(APPEND ${HDRS} ${_file})
    endif()
  endforeach()
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
  if(protobuf_generate_grpc_cpp_DESCRIPTORS)
    set(${protobuf_generate_grpc_cpp_DESCRIPTORS} "${${protobuf_generate_grpc_cpp_DESCRIPTORS}}" PARENT_SCOPE)
  endif()
endfunction()

# Helper function.
# This function is a modified version of the function protobuf_generate() copied from https://github.com/Kitware/CMake/blob/master/Modules/FindProtobuf.cmake.
function(protobuf_generate_grpc)
  set(_options APPEND_PATH DESCRIPTORS)
  set(_singleargs LANGUAGE OUT_VAR EXPORT_MACRO PROTOC_OUT_DIR)
  if(COMMAND target_sources)
    list(APPEND _singleargs TARGET)
  endif()
  set(_multiargs PROTOS IMPORT_DIRS GENERATE_EXTENSIONS)

  cmake_parse_arguments(protobuf_generate_grpc "${_options}" "${_singleargs}" "${_multiargs}" "${ARGN}")

  if(NOT protobuf_generate_grpc_PROTOS AND NOT protobuf_generate_grpc_TARGET)
    message(SEND_ERROR "Error: protobuf_generate_grpc called without any targets or source files")
    return()
  endif()

  if(NOT protobuf_generate_grpc_OUT_VAR AND NOT protobuf_generate_grpc_TARGET)
    message(SEND_ERROR "Error: protobuf_generate_grpc called without a target or output variable")
    return()
  endif()

  if(NOT protobuf_generate_grpc_LANGUAGE)
    set(protobuf_generate_grpc_LANGUAGE cpp)
  endif()
  string(TOLOWER ${protobuf_generate_grpc_LANGUAGE} protobuf_generate_grpc_LANGUAGE)

  if(NOT protobuf_generate_grpc_PROTOC_OUT_DIR)
    set(protobuf_generate_grpc_PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
  endif()

  if(protobuf_generate_grpc_EXPORT_MACRO AND protobuf_generate_grpc_LANGUAGE STREQUAL cpp)
    set(_dll_export_decl "dllexport_decl=${protobuf_generate_grpc_EXPORT_MACRO}:")
  endif()

  if(NOT protobuf_generate_grpc_GENERATE_EXTENSIONS)
    if(protobuf_generate_grpc_LANGUAGE STREQUAL cpp)
      set(protobuf_generate_grpc_GENERATE_EXTENSIONS .pb.h .pb.cc .grpc.pb.h .grpc.pb.cc)
    elseif(protobuf_generate_grpc_LANGUAGE STREQUAL python)
      set(protobuf_generate_grpc_GENERATE_EXTENSIONS _pb2.py)
    else()
      message(SEND_ERROR "Error: protobuf_generate_grpc given unknown Language ${LANGUAGE}, please provide a value for GENERATE_EXTENSIONS")
      return()
    endif()
  endif()

  if(NOT protobuf_generate_grpc_PLUGIN)
    if(protobuf_generate_grpc_LANGUAGE STREQUAL cpp)
      set(protobuf_generate_grpc_PLUGIN "grpc_cpp_plugin")
    elseif(protobuf_generate_grpc_LANGUAGE STREQUAL python)
      set(protobuf_generate_grpc_PLUGIN "grpc_python_plugin")
    else()
      message(SEND_ERROR "Error: protobuf_generate_grpc given unknown Language ${LANGUAGE}, please provide a value for PLUGIN")
      return()
    endif()
  endif()

  if(protobuf_generate_grpc_TARGET)
    get_target_property(_source_list ${protobuf_generate_grpc_TARGET} SOURCES)
    foreach(_file ${_source_list})
      if(_file MATCHES "proto$")
        list(APPEND protobuf_generate_grpc_PROTOS ${_file})
      endif()
    endforeach()
  endif()

  if(NOT protobuf_generate_grpc_PROTOS)
    message(SEND_ERROR "Error: protobuf_generate_grpc could not find any .proto files")
    return()
  endif()

  if(protobuf_generate_grpc_APPEND_PATH)
    # Create an include path for each file specified
    foreach(_file ${protobuf_generate_grpc_PROTOS})
      get_filename_component(_abs_file ${_file} ABSOLUTE)
      get_filename_component(_abs_path ${_abs_file} PATH)
      list(FIND _protobuf_include_path ${_abs_path} _contains_already)
      if(${_contains_already} EQUAL -1)
          list(APPEND _protobuf_include_path -I ${_abs_path})
      endif()
    endforeach()
  else()
    set(_protobuf_include_path -I ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  foreach(DIR ${protobuf_generate_grpc_IMPORT_DIRS})
    get_filename_component(ABS_PATH ${DIR} ABSOLUTE)
    list(FIND _protobuf_include_path ${ABS_PATH} _contains_already)
    if(${_contains_already} EQUAL -1)
        list(APPEND _protobuf_include_path -I ${ABS_PATH})
    endif()
  endforeach()

  set(_generated_srcs_all)
  foreach(_proto ${protobuf_generate_grpc_PROTOS})
    get_filename_component(_abs_file ${_proto} ABSOLUTE)
    get_filename_component(_abs_dir ${_abs_file} DIRECTORY)
    get_filename_component(_basename ${_proto} NAME_WE)
    file(RELATIVE_PATH _rel_dir ${CMAKE_CURRENT_SOURCE_DIR} ${_abs_dir})

    set(_possible_rel_dir)
    if(NOT protobuf_generate_grpc_APPEND_PATH)
        set(_possible_rel_dir ${_rel_dir}/)
    endif()

    set(_generated_srcs)
    foreach(_ext ${protobuf_generate_grpc_GENERATE_EXTENSIONS})
      list(APPEND _generated_srcs "${protobuf_generate_grpc_PROTOC_OUT_DIR}/${_possible_rel_dir}${_basename}${_ext}")
    endforeach()

    if(protobuf_generate_grpc_DESCRIPTORS AND protobuf_generate_grpc_LANGUAGE STREQUAL cpp)
      set(_descriptor_file "${CMAKE_CURRENT_BINARY_DIR}/${_basename}.desc")
      set(_dll_desc_out "--descriptor_set_out=${_descriptor_file}")
      list(APPEND _generated_srcs ${_descriptor_file})
    endif()
    list(APPEND _generated_srcs_all ${_generated_srcs})

    add_custom_command(
      OUTPUT ${_generated_srcs}
      COMMAND $<TARGET_FILE:protobuf::protoc>
      ARGS --${protobuf_generate_grpc_LANGUAGE}_out ${_dll_export_decl}${protobuf_generate_grpc_PROTOC_OUT_DIR}
           --grpc_out ${_dll_export_decl}${protobuf_generate_grpc_PROTOC_OUT_DIR}
           --plugin=protoc-gen-grpc=$<TARGET_FILE:${protobuf_generate_grpc_PLUGIN}>
           ${_dll_desc_out} ${_protobuf_include_path} ${_abs_file}
      DEPENDS ${_abs_file} protobuf::protoc ${protobuf_generate_grpc_PLUGIN}
      COMMENT "Running ${protobuf_generate_grpc_LANGUAGE} protocol buffer compiler on ${_proto}"
      VERBATIM)
  endforeach()

  set_source_files_properties(${_generated_srcs_all} PROPERTIES GENERATED TRUE)
  if(protobuf_generate_grpc_OUT_VAR)
    set(${protobuf_generate_grpc_OUT_VAR} ${_generated_srcs_all} PARENT_SCOPE)
  endif()
  if(protobuf_generate_grpc_TARGET)
    target_sources(${protobuf_generate_grpc_TARGET} PRIVATE ${_generated_srcs_all})
  endif()
endfunction()
