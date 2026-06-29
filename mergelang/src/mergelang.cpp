//==========================================================================
//
//  mergelang.cpp
//
//  Notepad4 Language Merge Tool
//
//  Merges resources from a locale DLL into the main executable files,
//  creating self-contained, language-specific builds without requiring
//  the locale DLLs at runtime.
//
//  Usage:
//    mergelang.exe
//      - Scans the "locale" subdirectory for available languages,
//        displays a menu, and merges the user's selection.
//
//==========================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef STRICT_TYPED_ITEMIDS
#define STRICT_TYPED_ITEMIDS
#endif
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")


//==========================================================================
//
//  Language name mapping
//
//==========================================================================

static const std::map<std::wstring, std::wstring>& GetLangNameMap()
{
    static const std::map<std::wstring, std::wstring> s_map = {
        { L"en",      L"English" },
        { L"de",      L"German" },
        { L"fr",      L"French" },
        { L"it",      L"Italian" },
        { L"ja",      L"Japanese" },
        { L"ko",      L"Korean" },
        { L"pl",      L"Polish" },
        { L"pt-BR",   L"Portuguese (Brazil)" },
        { L"ru",      L"Russian" },
        { L"sl",      L"Slovenian" },
        { L"zh-Hans", L"Chinese (Simplified)" },
        { L"zh-Hant", L"Chinese (Traditional)" },
    };
    return s_map;
}


//==========================================================================
//
//  GetLangId
//
//  Maps a locale code string to a Windows LANGID value.
//
//==========================================================================

static WORD GetLangId(const std::wstring& code)
{
    static const struct { LPCWSTR code; WORD langId; } s_map[] = {
        { L"en",      MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US) },
        { L"de",      MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN) },
        { L"fr",      MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH) },
        { L"it",      MAKELANGID(LANG_ITALIAN, SUBLANG_ITALIAN) },
        { L"ja",      MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT) },
        { L"ko",      MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT) },
        { L"pl",      MAKELANGID(LANG_POLISH, SUBLANG_DEFAULT) },
        { L"pt-BR",   MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN) },
        { L"ru",      MAKELANGID(LANG_RUSSIAN, SUBLANG_DEFAULT) },
        { L"sl",      MAKELANGID(LANG_SLOVENIAN, SUBLANG_DEFAULT) },
        { L"zh-Hans", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED) },
        { L"zh-Hant", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL) },
    };
    for (const auto& entry : s_map) {
        if (code == entry.code)
            return entry.langId;
    }
    return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}


//==========================================================================
//
//  IniWriteLanguage
//
//  Writes the selected language ID to Notepad4.ini so that the
//  merged EXE starts with the correct UI language on first launch.
//
//==========================================================================

static void IniWriteLanguage(const std::wstring& exeDir, WORD langId)
{
    // Build the language ID string (e.g. "1041")
    WCHAR langStr[16];
    wsprintfW(langStr, L"%u", langId);

    // Write to Notepad4.ini
    {
        WCHAR iniPath[MAX_PATH];
        lstrcpyW(iniPath, exeDir.c_str());
        PathAppendW(iniPath, L"Notepad4.ini");
        if (WritePrivateProfileStringW(L"Settings2", L"UILanguage", langStr, iniPath)) {
            wprintf(L"  Updated Notepad4.ini: UILanguage=%u\n", langId);
        }
    }

    // Write to matepath.ini (same directory)
    {
        WCHAR iniPath[MAX_PATH];
        lstrcpyW(iniPath, exeDir.c_str());
        PathAppendW(iniPath, L"matepath.ini");
        if (WritePrivateProfileStringW(L"Settings2", L"UILanguage", langStr, iniPath)) {
            wprintf(L"  Updated matepath.ini: UILanguage=%u\n", langId);
        }
    }
}


//==========================================================================
//
//  Locale information
//==========================================================================

struct LocaleInfo {
    std::wstring code;
    std::wstring displayName;
    std::wstring notepad4Dll;
    std::wstring matepathDll;
};


//==========================================================================
//
//  ScanLocaleDirectory
//
//  Scans the locale subdirectory for available language DLLs.
//
//==========================================================================

static std::vector<LocaleInfo> ScanLocaleDirectory(const std::wstring& localePath)
{
    std::vector<LocaleInfo> locales;
    const auto& langNames = GetLangNameMap();

    // Pattern: locale\*\Notepad4.dll
    std::wstring pattern = localePath + L"*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &findData,
        FindExSearchLimitToDirectories, nullptr, 0);

    if (hFind == INVALID_HANDLE_VALUE)
        return locales;

    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        // Skip "." and ".."
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
            continue;

        std::wstring code = findData.cFileName;

        // Check for Notepad4.dll in this locale folder
        std::wstring np4DllPath = localePath + code + L"\\Notepad4.dll";
        if (GetFileAttributesW(np4DllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        // Check for matepath.dll (optional, skip if missing)
        std::wstring mpDllPath = localePath + code + L"\\matepath.dll";

        // Get display name
        auto it = langNames.find(code);
        std::wstring displayName = (it != langNames.end()) ? it->second : code;

        LocaleInfo info;
        info.code = code;
        info.displayName = displayName;
        info.notepad4Dll = std::move(np4DllPath);
        if (GetFileAttributesW(mpDllPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            info.matepathDll = std::move(mpDllPath);
        locales.push_back(std::move(info));

    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    return locales;
}


//==========================================================================
//
//  ShowMenu
//
//  Displays the language selection menu and returns the user's choice.
//
//==========================================================================

static int ShowMenu(const std::vector<LocaleInfo>& locales)
{
    wprintf(L"\n");
    wprintf(L"============================================\n");
    wprintf(L"  Notepad4 Language Merge Tool\n");
    wprintf(L"============================================\n");
    wprintf(L"\n");
    wprintf(L"Available languages:\n");
    wprintf(L"\n");

    for (size_t i = 0; i < locales.size(); ++i) {
        wprintf(L"  %2zu. %s\n", i + 1, locales[i].displayName.c_str());
    }

    wprintf(L"\n");
    wprintf(L"Select a language (1-%zu, or 0 to exit): ", locales.size());

    int choice = -1;
    wchar_t input[16];
    if (fgetws(input, 16, stdin)) {
        choice = static_cast<int>(wcstol(input, nullptr, 10));
    }

    return choice;
}


//==========================================================================
//
//  MergeDllIntoExe
//
//  Copies all resources from the source DLL into the target EXE using
//  Windows resource update APIs.
//
//==========================================================================

static HANDLE g_hUpdate = nullptr;
static DWORD g_mergeCount = 0;
static DWORD g_errorCount = 0;

static BOOL CALLBACK EnumLangProc(
    HMODULE hDll,
    LPCWSTR lpType,
    LPCWSTR lpName,
    WORD    wLang,
    LONG_PTR /*lParam*/
)
{
    HRSRC hRes = FindResourceExW(hDll, lpType, lpName, wLang);
    if (!hRes)
        return TRUE;

    DWORD size = SizeofResource(hDll, hRes);
    if (size == 0)
        return TRUE;

    HGLOBAL hGlob = LoadResource(hDll, hRes);
    if (!hGlob)
        return TRUE;

    LPVOID data = LockResource(hGlob);
    if (!data)
        return TRUE;

    // Write resource at its original language ID. Notepad4 will set the
    // thread UI language to match the user's selection (via SetThreadUILanguage),
    // so LoadString/FindResource will find the exact language match.
    WORD targetLang = wLang;

    if (!UpdateResourceW(g_hUpdate, lpType, lpName, targetLang, data, size)) {
        DWORD err = GetLastError();
        ++g_errorCount;
        if (err != ERROR_RESOURCE_DATA_NOT_FOUND && err != ERROR_RESOURCE_TYPE_NOT_FOUND) {
            // Only print unexpected errors
            wprintf(L"    Warning: UpdateResourceW failed (type=%p, name=%p, lang=%u, error=%u)\n",
                lpType, lpName, targetLang, err);
        }
    } else {
        ++g_mergeCount;
    }

    return TRUE;
}

static BOOL CALLBACK EnumNameProc(
    HMODULE hDll,
    LPCWSTR lpType,
    LPWSTR  lpName,
    LONG_PTR lParam
)
{
    EnumResourceLanguagesExW(hDll, lpType, lpName, EnumLangProc, lParam,
        RESOURCE_ENUM_LN, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    return TRUE;
}

static BOOL CALLBACK EnumTypeProc(
    HMODULE hDll,
    LPWSTR  lpType,
    LONG_PTR lParam
)
{
    EnumResourceNamesExW(hDll, lpType, EnumNameProc, lParam,
        RESOURCE_ENUM_LN, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    return TRUE;
}

static bool MergeDllIntoExe(const wchar_t* exePath, const wchar_t* dllPath)
{
    // Load the locale DLL as a data file (don't execute code)
    HMODULE hDll = LoadLibraryExW(dllPath, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!hDll) {
        wprintf(L"  ERROR: Cannot load '%s' (error %u)\n", dllPath, GetLastError());
        return false;
    }

    // Open the EXE for resource updates
    g_hUpdate = BeginUpdateResourceW(exePath, FALSE);
    if (!g_hUpdate) {
        wprintf(L"  ERROR: Cannot open '%s' for update (error %u)\n", exePath, GetLastError());
        FreeLibrary(hDll);
        return false;
    }

    // Enumerate all resources in the DLL and copy to the EXE
    g_mergeCount = 0;
    g_errorCount = 0;
    BOOL success = EnumResourceTypesExW(hDll, EnumTypeProc, 0,
        RESOURCE_ENUM_LN, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));

    // Close the update handle
    if (!EndUpdateResourceW(g_hUpdate, FALSE)) {
        wprintf(L"  ERROR: Failed to save updates to '%s' (error %u)\n", exePath, GetLastError());
        FreeLibrary(hDll);
        return false;
    }

    wprintf(L"    Merged %u resources, %u errors.\n", g_mergeCount, g_errorCount);

    FreeLibrary(hDll);
    return success != FALSE;
}


//==========================================================================
//
//  wmain
//
//==========================================================================

static void DeleteDirectoryRecursive(const std::wstring& path);
static void ScheduleSelfDeletion(const std::wstring& exePath);

int wmain()
{
    // Suppress critical error dialogs (e.g. "No Disk" for removable drives)
    SetErrorMode(SEM_FAILCRITICALERRORS);

    // Set console to UTF-8 code page for correct multi-language display
    SetConsoleOutputCP(CP_UTF8);

    // Get the directory containing this tool
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        wprintf(L"FATAL: Cannot determine tool location.\n");
        return 1;
    }

    // Remove the executable name to get the directory
    wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
    if (lastSlash)
        *lastSlash = L'\0';

    std::wstring toolDir = modulePath;
    std::wstring localePath = toolDir + L"\\locale\\";

    // Check if the locale directory exists
    DWORD attr = GetFileAttributesW(localePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"ERROR: Cannot find the 'locale' subdirectory.\n");
        wprintf(L"Please run this tool from the Notepad4 installation directory.\n");
        wprintf(L"\nPress Enter to exit...");
        getchar();
        return 1;
    }

    // Scan for available languages
    wprintf(L"Scanning for language packs...\n");
    auto locales = ScanLocaleDirectory(localePath);

    // Add English reset option and sort all entries alphabetically
    {
        LocaleInfo englishInfo;
        englishInfo.code = L"en";
        englishInfo.displayName = L"English";
        locales.push_back(std::move(englishInfo));
    }
    std::sort(locales.begin(), locales.end(),
        [](const LocaleInfo& a, const LocaleInfo& b) {
            return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
        });

    while (true) {
        int choice = ShowMenu(locales);

        if (choice <= 0) {
            wprintf(L"\nExiting.\n");
            return 0;
        }

        if (choice > static_cast<int>(locales.size())) {
            wprintf(L"\nInvalid selection. Please try again.\n");
            continue;
        }

        const auto& selected = locales[choice - 1];
        wprintf(L"\n%s (%s)...\n",
            (selected.code == L"en") ? L"Resetting to English" : L"Merging",
            selected.displayName.c_str());

        // Construct EXE paths (same directory as the tool)
        std::wstring notepad4Exe = toolDir + L"\\Notepad4.exe";
        std::wstring matepathExe = toolDir + L"\\matepath.exe";

        // Verify Notepad4.exe exists
        if (GetFileAttributesW(notepad4Exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  ERROR: Cannot find Notepad4.exe in the current directory.\n");
            wprintf(L"\nPress Enter to exit...");
            getchar();
            return 1;
        }

        if (selected.code != L"en")
        {
            // Merge Notepad4.dll -> Notepad4.exe
            wprintf(L"  Merging '%s' -> Notepad4.exe ...\n", selected.notepad4Dll.c_str());
            if (!MergeDllIntoExe(notepad4Exe.c_str(), selected.notepad4Dll.c_str())) {
                wprintf(L"  FAILED to merge Notepad4 resources.\n");
                wprintf(L"\nPress Enter to exit...");
                getchar();
                return 1;
            }

            // Merge matepath.dll -> matepath.exe (if available)
            if (!selected.matepathDll.empty()) {
                if (GetFileAttributesW(matepathExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    wprintf(L"  Merging '%s' -> matepath.exe ...\n", selected.matepathDll.c_str());
                    if (!MergeDllIntoExe(matepathExe.c_str(), selected.matepathDll.c_str())) {
                        wprintf(L"  FAILED to merge matepath resources.\n");
                        wprintf(L"\nPress Enter to exit...");
                        getchar();
                        return 1;
                    }
                } else {
                    wprintf(L"  Skipping matepath.exe (not found).\n");
                }
            }
        }

        wprintf(L"\n  SUCCESS! %s.\n",
            (selected.code == L"en") ? L"Reset to English" : selected.displayName.c_str());
        wprintf(L"  Updating configuration file...\n");

        // Write the language ID to Notepad4.ini
        WORD langId = (selected.code == L"en")
            ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
            : GetLangId(selected.code);
        IniWriteLanguage(toolDir, langId);

        wprintf(L"\n  Notepad4.exe is now configured for %s.\n",
            (selected.code == L"en") ? L"English" : selected.displayName.c_str());

        // Clean up: delete locale directory and schedule self-deletion
        wprintf(L"\n  Cleaning up...\n");

        // Recursively delete the locale directory
        DeleteDirectoryRecursive(toolDir + L"\\locale");

        // Schedule deletion of mergelang.exe via a temp batch file
        WCHAR selfPath[MAX_PATH];
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        ScheduleSelfDeletion(selfPath);

        wprintf(L"  The locale folder and mergelang.exe will be removed.\n");
        break;
    }

    return 0;
}


//==========================================================================
//
//  Cleanup helpers
//
//==========================================================================

static void DeleteDirectoryRecursive(const std::wstring& path)
{
    std::wstring searchPath = path + L"\\*";
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;

        std::wstring fullPath = path + L"\\" + ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryRecursive(fullPath);
        } else {
            DeleteFileW(fullPath.c_str());
        }
    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);
    RemoveDirectoryW(path.c_str());
}

static void ScheduleSelfDeletion(const std::wstring& exePath)
{
    WCHAR tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempPath))
        return;

    // Create a unique .bat file in the temp directory
    WCHAR batPath[MAX_PATH];
    do {
        wsprintfW(batPath, L"%s\\mg_%08lx.bat", tempPath, GetTickCount());
    } while (GetFileAttributesW(batPath) != INVALID_FILE_ATTRIBUTES);

    HANDLE hBat = CreateFileW(batPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hBat == INVALID_HANDLE_VALUE)
        return;

    WCHAR content[2048];
    wsprintfW(content,
        L"@echo off\r\n"
        L"ping -n 2 127.0.0.1 > nul\r\n"
        L"del /f /q \"%s\"\r\n"
        L"del /f /q \"%%~f0\"\r\n",
        exePath.c_str());

    DWORD written;

    // Write UTF-8 BOM so cmd.exe recognizes the encoding
    const unsigned char utf8Bom[] = { 0xEF, 0xBB, 0xBF };
    WriteFile(hBat, utf8Bom, 3, &written, nullptr);

    // Convert to UTF-8 and write (cmd.exe requires ANSI/OEM or UTF-8 with BOM)
    char utf8Content[4096];
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, content, -1,
        utf8Content, sizeof(utf8Content), nullptr, nullptr);
    if (utf8Len > 1) {
        WriteFile(hBat, utf8Content, utf8Len - 1, &written, nullptr);
    }
    CloseHandle(hBat);

    // Launch the batch file via cmd.exe and exit
    // (the batch file will delete mergelang.exe after it exits)
    WCHAR cmdLine[2100];
    wsprintfW(cmdLine, L"cmd.exe /C \"%s\"", batPath);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}
