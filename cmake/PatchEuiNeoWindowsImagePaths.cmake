function(relaydesk_patch_eui_neo_windows_image_paths eui_source_dir)
    cmake_path(ABSOLUTE_PATH eui_source_dir
               NORMALIZE
               OUTPUT_VARIABLE eui_patch_source_dir)
    if(DEFINED RELAYDESK_BUILD_DIR)
        set(eui_allowed_fetchcontent_dir "${RELAYDESK_BUILD_DIR}/_deps")
        cmake_path(ABSOLUTE_PATH eui_allowed_fetchcontent_dir
                   NORMALIZE
                   OUTPUT_VARIABLE eui_allowed_fetchcontent_dir)
        cmake_path(IS_PREFIX eui_allowed_fetchcontent_dir
                   "${eui_patch_source_dir}"
                   NORMALIZE
                   eui_source_is_fetchcontent)
        if(NOT eui_source_is_fetchcontent)
            message(FATAL_ERROR "Refusing to patch EUI-NEO outside the FetchContent build tree")
        endif()
    endif()

    set(eui_stb_source "${eui_source_dir}/core/render/stb_image_impl.cpp")
    if(EXISTS "${eui_stb_source}")
        file(READ "${eui_stb_source}" eui_stb_content)
        string(FIND "${eui_stb_content}" "STBI_WINDOWS_UTF8" eui_stb_utf8_pos)
        if(eui_stb_utf8_pos EQUAL -1)
            set(eui_stb_anchor [=[#define STB_IMAGE_IMPLEMENTATION]=])
            set(eui_stb_replacement [=[#if defined(_WIN32)
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION]=])
            string(FIND "${eui_stb_content}" "${eui_stb_anchor}" eui_stb_anchor_pos)
            if(eui_stb_anchor_pos EQUAL -1)
                message(WARNING "EUI-NEO stb_image implementation anchor changed upstream; UTF-8 filename patch was not applied")
            else()
                string(REPLACE "${eui_stb_anchor}"
                               "${eui_stb_replacement}"
                               eui_stb_content
                               "${eui_stb_content}")
                file(WRITE "${eui_stb_source}" "${eui_stb_content}")
            endif()
        endif()
    else()
        message(WARNING "EUI-NEO stb_image implementation source not found: ${eui_stb_source}")
    endif()

    set(eui_image_source "${eui_source_dir}/core/render/image_source.cpp")
    if(NOT EXISTS "${eui_image_source}")
        message(WARNING "EUI-NEO image source file not found: ${eui_image_source}")
        return()
    endif()

    file(READ "${eui_image_source}" eui_image_content)

    set(eui_path_helper_anchor [=[bool looksLikeGifFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }
    char buffer[6] = {};
    input.read(buffer, sizeof(buffer));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(buffer))) {
        return false;
    }
    return std::string(buffer, sizeof(buffer)) == "GIF87a" || std::string(buffer, sizeof(buffer)) == "GIF89a";
}

]=])
    set(eui_path_helper_block [=[bool looksLikeGifFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }
    char buffer[6] = {};
    input.read(buffer, sizeof(buffer));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(buffer))) {
        return false;
    }
    return std::string(buffer, sizeof(buffer)) == "GIF87a" || std::string(buffer, sizeof(buffer)) == "GIF89a";
}

std::filesystem::path pathFromUtf8(const std::string& path) {
#if defined(_WIN32)
    if (path.empty()) {
        return {};
    }
    const int requiredSize = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    if (requiredSize <= 0) {
        return std::filesystem::path(path);
    }

    std::wstring wide(static_cast<std::size_t>(requiredSize - 1), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wide.data(), requiredSize);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(path);
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring wide = path.native();
    if (wide.empty()) {
        return {};
    }
    const int requiredSize = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredSize <= 0) {
        return path.string();
    }

    std::string utf8(static_cast<std::size_t>(requiredSize - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1, utf8.data(), requiredSize, nullptr, nullptr);
    return utf8;
#else
    return path.string();
#endif
}

]=])
    string(FIND "${eui_image_content}" "std::filesystem::path pathFromUtf8" eui_path_helper_pos)
    if(eui_path_helper_pos EQUAL -1)
        string(FIND "${eui_image_content}" "${eui_path_helper_anchor}" eui_path_helper_anchor_pos)
        if(eui_path_helper_anchor_pos EQUAL -1)
            message(WARNING "EUI-NEO image path helper anchor changed upstream; UTF-8 path helper patch was not applied")
        else()
            string(REPLACE "${eui_path_helper_anchor}"
                           "${eui_path_helper_block}"
                           eui_image_content
                           "${eui_image_content}")
        endif()
    endif()

    set(eui_exe_dir_windows_old [=[#ifdef _WIN32
    std::vector<char> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer.data()).parent_path();
#elif defined(__APPLE__)]=])
    set(eui_exe_dir_windows_new [=[#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer.data()).parent_path();
#elif defined(__APPLE__)]=])
    string(FIND "${eui_image_content}" "GetModuleFileNameW(nullptr, buffer.data()" eui_exe_dir_wide_pos)
    if(eui_exe_dir_wide_pos EQUAL -1)
        string(FIND "${eui_image_content}" "${eui_exe_dir_windows_old}" eui_exe_dir_windows_pos)
        if(eui_exe_dir_windows_pos EQUAL -1)
            message(WARNING "EUI-NEO executable directory block changed upstream; wide path patch was not applied")
        else()
            string(REPLACE "${eui_exe_dir_windows_old}"
                           "${eui_exe_dir_windows_new}"
                           eui_image_content
                           "${eui_image_content}")
        endif()
    endif()

    set(eui_raw_path_old [=[    const std::filesystem::path raw(source);]=])
    set(eui_raw_path_new [=[    const std::filesystem::path raw = pathFromUtf8(source);]=])
    string(FIND "${eui_image_content}" "${eui_raw_path_new}" eui_raw_path_new_pos)
    if(eui_raw_path_new_pos EQUAL -1)
        string(FIND "${eui_image_content}" "${eui_raw_path_old}" eui_raw_path_old_pos)
        if(eui_raw_path_old_pos EQUAL -1)
            message(WARNING "EUI-NEO local image raw path block changed upstream; UTF-8 path patch was not applied")
        else()
            string(REPLACE "${eui_raw_path_old}"
                           "${eui_raw_path_new}"
                           eui_image_content
                           "${eui_image_content}")
        endif()
    endif()

    set(eui_absolute_return_old [=[            return std::filesystem::absolute(candidate, error).string();]=])
    set(eui_absolute_return_new [=[            const std::filesystem::path absolutePath =
                std::filesystem::absolute(candidate, error);
            return error ? pathToUtf8(candidate) : pathToUtf8(absolutePath);]=])
    string(FIND "${eui_image_content}" "return error ? pathToUtf8(candidate) : pathToUtf8(absolutePath);" eui_absolute_return_new_pos)
    if(eui_absolute_return_new_pos EQUAL -1)
        string(FIND "${eui_image_content}" "${eui_absolute_return_old}" eui_absolute_return_old_pos)
        if(eui_absolute_return_old_pos EQUAL -1)
            message(WARNING "EUI-NEO local image return block changed upstream; UTF-8 return patch was not applied")
        else()
            string(REPLACE "${eui_absolute_return_old}"
                           "${eui_absolute_return_new}"
                           eui_image_content
                           "${eui_image_content}")
        endif()
    endif()

    file(WRITE "${eui_image_source}" "${eui_image_content}")
    message(STATUS "Patched EUI-NEO image loading to use UTF-8 Windows paths")
endfunction()
