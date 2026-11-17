#include <Windows.h>

#include <iostream>
#include <string_view>

int wmain(const int argc, wchar_t* argv[])
{
    if (argc != 2)
    {
        std::wcerr << L"Usage: desktopworld_manifest_validation <executable>\n";
        return 1;
    }

    const HMODULE module = LoadLibraryExW(
        argv[1],
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE);

    if (module == nullptr)
    {
        std::wcerr << L"LoadLibraryExW failed: " << GetLastError() << L"\n";
        return 2;
    }

    const HRSRC resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(1),
        RT_MANIFEST);

    if (resource == nullptr)
    {
        std::wcerr << L"RT_MANIFEST resource #1 was not found.\n";
        FreeLibrary(module);
        return 3;
    }

    const DWORD resource_size = SizeofResource(module, resource);
    const HGLOBAL loaded_resource = LoadResource(module, resource);
    const void* resource_data =
        loaded_resource == nullptr ? nullptr : LockResource(loaded_resource);

    if (resource_size == 0 || resource_data == nullptr)
    {
        std::wcerr << L"RT_MANIFEST resource could not be read.\n";
        FreeLibrary(module);
        return 4;
    }

    const std::string_view manifest_bytes(
        static_cast<const char*>(resource_data),
        resource_size);

    constexpr std::string_view windows_10_id_utf8 =
        "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}";
    constexpr std::wstring_view windows_10_id_wide =
        L"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}";

    const std::string_view windows_10_id_utf16(
        reinterpret_cast<const char*>(windows_10_id_wide.data()),
        windows_10_id_wide.size() * sizeof(wchar_t));

    if (manifest_bytes.find(windows_10_id_utf8) == std::string_view::npos &&
        manifest_bytes.find(windows_10_id_utf16) == std::string_view::npos)
    {
        std::wcerr << L"Windows 10 supportedOS declaration is missing.\n";
        FreeLibrary(module);
        return 5;
    }

    std::wcout << L"Embedded Windows 10 supportedOS manifest verified.\n";
    FreeLibrary(module);
    return 0;
}
