
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was zlibConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

# bloom (issue #93): zlib 1.3.2's own generated zlibConfig.cmake.in unconditionally include()s
# every entry of _ZLIB_supported_components ("shared" "static") whenever the caller's
# find_package(ZLIB) requests no explicit COMPONENTS -- regardless of which component export sets
# this exact build actually installed. Bloom's superbuild always builds zlib static-only
# (see dependencies/superbuild/projects/zlib.cmake: ZLIB_BUILD_SHARED OFF, ZLIB_BUILD_STATIC ON),
# so ZLIB-shared.cmake is never installed and the unconditional include() hard-fails Config-mode
# resolution with a missing-file error for any unqualified find_package(ZLIB). This checked-in
# replacement (installed over the generated file by the "bloom_fix_static_only_config" step in
# zlib.cmake, never a hand-edit of build output) is byte-for-byte identical to the upstream
# template except each include is guarded by an existence check, so it degrades gracefully for
# this static-only profile while still behaving exactly like upstream if a future profile ever
# ships both component export sets. See dependencies/licenses/zlib/review.md "ZLIB Resolution".
set(_ZLIB_supported_components "shared" "static")

if(ZLIB_FIND_COMPONENTS)
    foreach(_comp ${ZLIB_FIND_COMPONENTS})
        if(NOT _comp IN_LIST _ZLIB_supported_components)
            set(ZLIB_FOUND False)
            set(ZLIB_NOT_FOUND_MESSAGE "Unsupported component: ${_comp}")
        elseif(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/ZLIB-${_comp}.cmake")
            set(ZLIB_FOUND False)
            set(ZLIB_NOT_FOUND_MESSAGE "Component not installed by this zlib build: ${_comp}")
        else()
            include("${CMAKE_CURRENT_LIST_DIR}/ZLIB-${_comp}.cmake")
        endif()
    endforeach(_comp ${ZLIB_FIND_COMPONENTS})
else(ZLIB_FIND_COMPONENTS)
    foreach(_component_config IN LISTS _ZLIB_supported_components)
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ZLIB-${_component_config}.cmake")
            include("${CMAKE_CURRENT_LIST_DIR}/ZLIB-${_component_config}.cmake")
        endif()
    endforeach(_component_config IN LISTS _ZLIB_supported_components)
endif(ZLIB_FIND_COMPONENTS)
