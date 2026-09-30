# vim: set sts=2 ts=2 sw=2 et:

function(CS2GLAZ_add_package target)
    set(_package_dir "${CMAKE_BINARY_DIR}/package")
    set(_addons_root "${_package_dir}/game/csgo/addons")
    set(_plugin_root "${_addons_root}/cs2glaz")
    set(_metamod_dir "${_addons_root}/metamod")
    set(_cfg_dir "${_package_dir}/game/csgo/cfg")

    if (WIN32)
        set(_platform_folder "win64")
    else ()
        set(_platform_folder "linuxsteamrt64")
    endif ()

    set(_bin_dir "${_plugin_root}/bin/${_platform_folder}")
    set(_gamedata_dir "${_plugin_root}/gamedata")
    set(_licenses_dir "${_plugin_root}/licenses")
    set(_translations_dir "${_plugin_root}/translations")

    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:${target}>"
            "${_bin_dir}/cs2glaz$<TARGET_FILE_SUFFIX:${target}>"
            COMMENT "cs2glaz: coping .dll/.so in package/"
    )


    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_gamedata_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/gamedata/cs2glaz.games.txt"
            "${_gamedata_dir}/cs2glaz.games.txt"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/gamedata/cs2glaz.signatures.txt"
            "${_gamedata_dir}/cs2glaz.signatures.txt"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_cfg_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/cfg/cs2glaz.cfg"
            "${_cfg_dir}/cs2glaz.cfg"
            COMMENT "cs2glaz: coping gamedata/cfg in package/"
    )


    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_translations_dir}"
            COMMENT "cs2glaz: coping translations in package/"
    )
    file(GLOB _translation_files
            "${CMAKE_SOURCE_DIR}/translations/*.txt"
    )
    foreach (_src ${_translation_files})
        get_filename_component(_name "${_src}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_translations_dir}/${_name}"
        )
    endforeach ()


    # --- optional documentation / notices / licenses ---------------------------
# Missing files must NOT break the build; they are packaged only when present.
set(_optional_docs "")

if(EXISTS "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
  list(APPEND _optional_docs "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
endif()

# Collect license files that actually exist (adjust the source list to cs2glaz).
set(_license_sources
  "${CMAKE_SOURCE_DIR}/LICENSE"
  "${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling/LICENSE"
  "${CS2GLAZ_FUNCHOOK_ROOT}/LICENSE"
  # add/remove entries to match what cs2glaz really vendors
)
foreach(_lic ${_license_sources})
  if(EXISTS "${_lic}")
    list(APPEND _optional_docs "${_lic}")
  endif()
endforeach()

# Copy each existing doc into the package (flat into plugin root / licenses).
foreach(_doc ${_optional_docs})
  get_filename_component(_doc_name "${_doc}" NAME)
  if(_doc_name STREQUAL "THIRD_PARTY_NOTICES.md")
    set(_doc_dest "${_plugin_root}/${_doc_name}")
  else()
    set(_doc_dest "${_licenses_dir}/${_doc_name}")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_licenses_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "${_doc}" "${_doc_dest}"
    COMMENT "cs2glaz: packaging ${_doc_name}"
  )
endforeach()

    set(_licenses_dir "${_plugin_root}/licenses")
    set(_license_pairs
            "${CMAKE_SOURCE_DIR}/LICENSE|cs2glaz-AGPL-3.0.txt"
            "${CMAKE_SOURCE_DIR}/licenses/CLIENTCVARVALUE-GPL-3.0.txt|CLIENTCVARVALUE-GPL-3.0.txt"
            "${CMAKE_SOURCE_DIR}/licenses/DYNLIBUTILS-MIT.txt|DYNLIBUTILS-MIT.txt"
            "${CMAKE_SOURCE_DIR}/licenses/CS2KZ-AGPL-3.0.txt|CS2KZ-AGPL-3.0.txt"
            "${CMAKE_SOURCE_DIR}/licenses/FUNCHOOK.txt|FUNCHOOK.txt"
            "${CMAKE_SOURCE_DIR}/licenses/DISTORM.txt|DISTORM.txt"
            "${CMAKE_SOURCE_DIR}/licenses/PICOSHA2-MIT.txt|PICOSHA2-MIT.txt"
            "${CMAKE_SOURCE_DIR}/licenses/MINIZ-MIT.txt|MINIZ-MIT.txt"
            "${CMAKE_SOURCE_DIR}/metamod-source/LICENSE.txt|METAMOD-SOURCE.txt"
            "${CS2GLAZ_SDK_ROOT}/thirdparty/protobuf-3.21.8/LICENSE|PROTOBUF.txt"
    )

    set(_real_license_pairs "")
    foreach (_pair ${_license_pairs})
        string(REPLACE "|" ";" _pair_list "${_pair}")
        list(GET _pair_list 0 _src)
        list(GET _pair_list 1 _dst)
        if (EXISTS "${_src}")
            list(APPEND _real_license_pairs "${_src}|${_dst}")
        endif ()
    endforeach ()

    if (_real_license_pairs)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_licenses_dir}"
                COMMENT "cs2glaz: packaging licenses to package/"
        )
        foreach (_pair ${_real_license_pairs})
            string(REPLACE "|" ";" _pair_list "${_pair}")
            list(GET _pair_list 0 _src)
            list(GET _pair_list 1 _dst)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_licenses_dir}/${_dst}"
            )
        endforeach ()
    endif ()


    file(MAKE_DIRECTORY "${_metamod_dir}")
    file(WRITE "${_metamod_dir}/cs2glaz.vdf"
            "\"Metamod Plugin\"
{
\t\"alias\"\t\"cs2glaz\"
\t\"file\"\t\"addons/cs2glaz/bin/${_platform_folder}/cs2glaz\"
}
")

    message(STATUS "cs2glaz: ${_package_dir}")
endfunction()