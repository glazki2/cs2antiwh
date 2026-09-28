# vim: set sts=2 ts=2 sw=2 et:

function(CS2GLAZ_generate_protobuf out_sources_var out_include_dir_var)
    if (WIN32)
        set(_protoc_rel "${CS2GLAZ_SDK_WINDOWS_PROTOC_PATH}")
        set(_platform_subdir "windows-x86_64")
    else ()
        set(_protoc_rel "${CS2GLAZ_SDK_LINUX_PROTOC_PATH}")
        set(_platform_subdir "linuxsteamrt64")
    endif ()

    set(_protoc "${CS2GLAZ_SDK_ROOT}/${_protoc_rel}")
    if (NOT EXISTS "${_protoc}")
        message(FATAL_ERROR "protoc не найден по пути ${_protoc}.")
    endif ()

    set(_gen_dir "${CMAKE_BINARY_DIR}/cs2glaz-protobuf/${_platform_subdir}")
    file(MAKE_DIRECTORY "${_gen_dir}")

    set(_proto_entries
            "valveextensions|common/valveextensions.proto|common"
            "network_connection|common/network_connection.proto|common"
            "networkbasetypes|common/networkbasetypes.proto|common"
            "source2_steam_stats|common/source2_steam_stats.proto|common"
            "netmessages|common/netmessages.proto|common"
            "gameevents|game/shared/gameevents.proto|game/shared"
            "usermessages|game/shared/usermessages.proto|game/shared"
            "usercmd|game/shared/usercmd.proto|game/shared"
            "cs_gameevents|game/shared/cs/cs_gameevents.proto|game/shared/cs"
            "cs_usercmd|game/shared/cs/cs_usercmd.proto|game/shared/cs"
            "networksystem_protomessages|networksystem/networksystem_protomessages.proto|networksystem"
    )

    set(_cc_sources "")
    set(_all_generated "")

    foreach (_entry ${_proto_entries})
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _proto_rel)
        list(GET _parts 2 _dir_rel)

        set(_proto_file "${CS2GLAZ_SDK_ROOT}/${_proto_rel}")
        set(_proto_dir "${CS2GLAZ_SDK_ROOT}/${_dir_rel}")
        set(_out_h "${_gen_dir}/${_name}.pb.h")
        set(_out_cc "${_gen_dir}/${_name}.pb.cc")

        if (NOT EXISTS "${_proto_file}")
            message(FATAL_ERROR "Proto-файл не найден: ${_proto_file}")
        endif ()

        
        set(_include_args "--proto_path=${_proto_dir}")
        foreach (_inc ${CS2GLAZ_SDK_INCLUDE_DIRS})
            if (IS_DIRECTORY "${_inc}")
                list(APPEND _include_args "--proto_path=${_inc}")
            endif ()
        endforeach ()

        add_custom_command(
                OUTPUT "${_out_h}" "${_out_cc}"
                COMMAND "${_protoc}" ${_include_args} "--cpp_out=${_gen_dir}" "${_proto_file}"
                DEPENDS "${_proto_file}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                COMMENT "cs2glaz: protoc -> ${_name}.pb.cc"
                VERBATIM
        )

        list(APPEND _all_generated "${_out_h}" "${_out_cc}")
        list(APPEND _cc_sources "${_out_cc}")
    endforeach ()

    add_custom_target(cs2glaz-protobuf-gen DEPENDS ${_all_generated})

    set(${out_sources_var} "${_cc_sources}" PARENT_SCOPE)
    set(${out_include_dir_var} "${_gen_dir}" PARENT_SCOPE)
endfunction()