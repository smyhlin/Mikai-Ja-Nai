# MinGW compiler runtime DLL discovery, SHA-256 verification against Qt6::Core,
# and post-build deployment for executable targets.

set(MIKAI_MINGW_RUNTIME_DIR "" CACHE PATH
  "MinGW runtime DLL directory shared by Qt and the selected mpv module")
if(MINGW AND MIKAI_MINGW_RUNTIME_DIR)
  set(MIKAI_COMPILER_RUNTIME_FILES)
  foreach(runtime IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    set(runtime_file "${MIKAI_MINGW_RUNTIME_DIR}/${runtime}")
    if(NOT EXISTS "${runtime_file}")
      message(FATAL_ERROR "The selected MinGW runtime is missing: ${runtime_file}")
    endif()
    list(APPEND MIKAI_COMPILER_RUNTIME_FILES "${runtime_file}")
  endforeach()
endif()

# The three DLLs are only deployable when they *are* the compiler runtime the
# configured Qt links: Qt6Core.dll imports them by name and the executable's
# directory wins that lookup, so another MinGW generation would replace Qt's
# thread runtime rather than share it.
function(mikai_qt_compiler_runtime_dir out_var)
  set(${out_var} "" PARENT_SCOPE)
  if(NOT TARGET Qt6::Core)
    return()
  endif()
  set(qt_core_location "")
  foreach(property IN ITEMS IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION IMPORTED_LOCATION_DEBUG)
    get_target_property(qt_core_location Qt6::Core "${property}")
    if(qt_core_location)
      break()
    endif()
    set(qt_core_location "")
  endforeach()
  if(NOT qt_core_location)
    return()
  endif()
  get_filename_component(runtime_dir "${qt_core_location}" DIRECTORY)
  set(${out_var} "${runtime_dir}" PARENT_SCOPE)
endfunction()

function(mikai_deploy_mingw_runtime_targets)
  if(NOT MINGW OR NOT MIKAI_MINGW_RUNTIME_DIR)
    return()
  endif()

  # A selection that is not Qt's runtime cannot be deployed - it would replace
  # the compiler runtime Qt's own modules resolve - so refuse it by name.
  mikai_qt_compiler_runtime_dir(qt_runtime_dir)
  foreach(runtime_file IN LISTS MIKAI_COMPILER_RUNTIME_FILES)
    get_filename_component(runtime_name "${runtime_file}" NAME)
    set(qt_runtime_file "${qt_runtime_dir}/${runtime_name}")
    set(mismatch "")
    if(NOT qt_runtime_dir)
      set(mismatch "the compiler runtime Qt links could not be located")
    elseif(NOT EXISTS "${qt_runtime_file}")
      set(mismatch "Qt's runtime directory has no ${runtime_name} at ${qt_runtime_file}")
    else()
      file(SHA256 "${runtime_file}" selected_hash)
      file(SHA256 "${qt_runtime_file}" qt_hash)
      if(NOT selected_hash STREQUAL qt_hash)
        set(mismatch "${qt_runtime_file} is a different MinGW generation")
      endif()
    endif()
    if(mismatch)
      message(FATAL_ERROR
        "MIKAI_MINGW_RUNTIME_DIR is ${MIKAI_MINGW_RUNTIME_DIR}, but ${runtime_name} is not "
        "the compiler runtime Qt links: ${mismatch}. Deploying it beside a Qt executable "
        "would replace the thread runtime Qt was built against. Point the option at the Qt "
        "MinGW runtime, or leave it unset.")
    endif()
  endforeach()

  # POST_BUILD must be attached in the target's defining directory.
  get_property(directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS directory_targets)
    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MIKAI_COMPILER_RUNTIME_FILES}
          "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
      )
    endif()
  endforeach()
endfunction()
