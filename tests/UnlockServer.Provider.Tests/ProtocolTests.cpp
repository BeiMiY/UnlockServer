#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdio>
#include <stdexcept>

// 将实际 Provider 的文件与事件操作重定向到临时目录，绝不接触已安装组件、
// 用户凭据或 LogonUI；认证结果由测试显式调用 ReportResult 注入。
static std::wstring testRoot;
static DWORD testConsole = 0xFFFFFFFF;
static std::wstring Redirect(const wchar_t* path)
{
    const wchar_t* name = wcsrchr(path, L'\\');
    return testRoot + L"\\" + (name ? name + 1 : path);
}
static HANDLE TestCreateFile(const wchar_t* path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    return CreateFileW(Redirect(path).c_str(), access, share, sa, disposition, flags, templateFile);
}
static BOOL TestDeleteFile(const wchar_t* path) { return DeleteFileW(Redirect(path).c_str()); }
static HANDLE TestCreateEvent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, const wchar_t* name)
{
    auto testName = L"Local\\UnlockServer.ProtocolTest." + std::to_wstring(GetCurrentProcessId());
    return CreateEventW(sa, manual, initial, name ? testName.c_str() : nullptr);
}
static DWORD TestConsoleId() { return testConsole; }
#define CreateFileW TestCreateFile
#define DeleteFileW TestDeleteFile
#define CreateEventW TestCreateEvent
#define WTSGetActiveConsoleSessionId TestConsoleId
#include "../../UnlockServer.Provider/Provider.cpp"

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
    printf("PASS %s\n", message);
}
static void Request(unsigned long long ticks)
{
    char raw[64]; sprintf_s(raw, "%llu", ticks);
    Check(WriteTextFile(kReqPath.c_str(), raw), "write isolated request");
}
static std::string Read(const std::wstring& path)
{
    char raw[128] = {};
    ReadUtf8File(path.c_str(), raw, sizeof(raw));
    return raw;
}
static void SaveDummyCredential()
{
    const char value[] = ".\nProtocolTestUser\nNotARealPassword\nS-1-5-21-1-2-3-1001";
    DATA_BLOB input = { sizeof(value) - 1, (BYTE*)value }, encrypted = {};
    Check(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &encrypted) != FALSE,
        "protect synthetic test data");
    HANDLE file = TestCreateFile(kCredPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, encrypted.pbData, encrypted.cbData, &written, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    LocalFree(encrypted.pbData);
    Check(ok, "save synthetic credentials only in temp directory");
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    testRoot = argv[1];
    CreateDirectoryW(testRoot.c_str(), nullptr);
    try
    {
        testConsole = CurrentSessionId();
        Check(kReqPath.find(L"unlock." + std::to_wstring(CurrentSessionId()) + L".req") != std::wstring::npos,
            "Provider path includes its own session");
        Check(WriteNowFile() && !Read(kNowPath).empty(), "console click writes session-scoped request");
        Check(HintCode() == 3, "console click shows waiting state");
        DeleteFileW(kNowPath.c_str());
        testConsole = CurrentSessionId() + 10;
        Check(!WriteNowFile() && Read(kNowPath).empty(), "remote lock screen cannot request console login");

        auto* cred = new UnlockCredential();
        cred->RuntimeClassInitialize(CPUS_LOGON);
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION data = {};
        PWSTR text = nullptr;
        CREDENTIAL_PROVIDER_STATUS_ICON icon;
        cred->GetSerialization(&response, &data, &text, &icon);
        Check(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED && !data.rgbSerialization,
            "selecting tile without Bluetooth-authorized request submits nothing");
        Request(NowDotNetTicks() - 21ULL * 10000000ULL);
        Check(!IsUnlockRequested(), "expired requests rejected");
        Request(NowDotNetTicks() + 10ULL * 10000000ULL);
        Check(!IsUnlockRequested(), "future requests rejected");

        SaveDummyCredential();
        auto token = NowDotNetTicks(); Request(token);
        cred->GetSerialization(&response, &data, &text, &icon);
        Check(response == CPGSR_RETURN_CREDENTIAL_FINISHED && data.rgbSerialization,
            "fresh authorized request serializes synthetic credentials");
        SecureZeroMemory(data.rgbSerialization, data.cbSerialization);
        CoTaskMemFree(data.rgbSerialization);
        Check(Read(kReqPath).empty() && Read(kResultPath) == std::to_string(token) + "|submitted",
            "serialization consumes request but does not report authentication success");
        cred->ReportResult((NTSTATUS)0xC000006D, 0, &text, &icon);
        Check(Read(kResultPath) == std::to_string(token) + "|fail", "failed authentication acknowledges original token");
        if (text) CoTaskMemFree(text);
        token = NowDotNetTicks(); Request(token);
        cred->GetSerialization(&response, &data, &text, &icon);
        SecureZeroMemory(data.rgbSerialization, data.cbSerialization);
        CoTaskMemFree(data.rgbSerialization);
        cred->ReportResult(0, 0, &text, &icon);
        Check(Read(kResultPath) == std::to_string(token) + "|ok", "successful authentication acknowledges original token");
        cred->Release();
        printf("Provider protocol tests passed without calling Windows logon.\n");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}
