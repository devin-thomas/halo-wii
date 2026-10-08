// Offline DIA attribution only: no executable loading, symbol server or registry setup.
// Build with MSVC C++17, the DIA include directory, ole32.lib and oleaut32.lib.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dia2.h>
#include <cvconst.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
constexpr std::size_t query_limit = 64;
constexpr UINT name_limit = 4096;

struct Query {
    DWORD rva = 0;
    enum SymTagEnum tag = SymTagNull;
};

struct Result {
    Query query;
    HRESULT lookup_hr = S_FALSE;
    DWORD tag = 0;
    DWORD symbol_rva = 0;
    LONG dia_displacement = 0;
    std::int64_t displacement = 0;
    ULONGLONG length = 0;
    bool length_available = false;
    bool byte_extent = false;
    DWORD location = 0;
    DWORD data_kind = 0;
    std::string name;
    const char* length_source = "unavailable";
    const char* classification = "missing";
};

struct Options {
    const wchar_t* dll = nullptr;
    const wchar_t* pdb = nullptr;
    GUID guid{};
    std::string guid_text;
    DWORD age = 0;
    bool have_guid = false;
    bool have_age = false;
    std::array<Query, query_limit> queries{};
    std::size_t count = 0;
};

struct Module {
    HMODULE value = nullptr;
    ~Module() { if (value != nullptr) FreeLibrary(value); }
};

struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

struct StringValue {
    BSTR value = nullptr;
    ~StringValue() { SysFreeString(value); }
};

void json_string(std::ostream& out, const std::string& value)
{
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out << '\\' << static_cast<char>(c);
        } else if (c < 0x20) {
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(c) << std::dec;
        } else {
            out << static_cast<char>(c);
        }
    }
    out << '"';
}

void hex32(std::ostream& out, DWORD value)
{
    out << "\"0x" << std::hex << std::setw(8) << std::setfill('0')
        << value << std::dec << '"';
}

int error(const char* stage, HRESULT hr, int code)
{
    std::cerr << "{\"error\":{\"stage\":";
    json_string(std::cerr, stage);
    std::cerr << ",\"hresult\":";
    hex32(std::cerr, static_cast<DWORD>(hr));
    std::cerr << "}}\n";
    return code;
}

bool unsigned32(const wchar_t* text, DWORD& result)
{
    if (text == nullptr || *text == L'\0') return false;
    unsigned int base = 10;
    if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text += 2;
    }
    if (*text == L'\0') return false;
    std::uint64_t value = 0;
    for (; *text != L'\0'; ++text) {
        unsigned int digit;
        if (*text >= L'0' && *text <= L'9') digit = static_cast<unsigned int>(*text - L'0');
        else if (*text >= L'a' && *text <= L'f') digit = static_cast<unsigned int>(*text - L'a' + 10);
        else if (*text >= L'A' && *text <= L'F') digit = static_cast<unsigned int>(*text - L'A' + 10);
        else return false;
        if (digit >= base || value > (0xffffffffULL - digit) / base) return false;
        value = value * base + digit;
    }
    result = static_cast<DWORD>(value);
    return true;
}

bool absolute_path(const wchar_t* path)
{
    const std::size_t size = std::wcslen(path);
    if (size == 0 || size >= 32767) return false;
    const bool drive = size >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/');
    if (!drive) return false;
    const wchar_t root[] = {path[0], L':', L'\\', L'\0'};
    const UINT kind = GetDriveTypeW(root);
    return kind == DRIVE_FIXED || kind == DRIVE_REMOVABLE ||
        kind == DRIVE_CDROM || kind == DRIVE_RAMDISK;
}

bool parse_guid(const wchar_t* text, Options& options)
{
    if (std::wcslen(text) != 36) return false;
    wchar_t wrapped[39] = L"{";
    std::string canonical;
    for (std::size_t i = 0; i < 36; ++i) {
        const wchar_t c = text[i];
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? c != L'-' : !((c >= L'0' && c <= L'9') ||
            (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
        wrapped[i + 1] = c;
        canonical.push_back(static_cast<char>(c >= L'a' && c <= L'f' ? c - 32 : c));
    }
    wrapped[37] = L'}';
    if (CLSIDFromString(wrapped, &options.guid) != S_OK) return false;
    options.guid_text = canonical;
    return true;
}

bool parse(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return false;
        const wchar_t* name = argv[i];
        const wchar_t* value = argv[i + 1];
        if (std::wcscmp(name, L"--dia-dll") == 0) {
            if (options.dll != nullptr || !absolute_path(value)) return false;
            options.dll = value;
        } else if (std::wcscmp(name, L"--pdb") == 0) {
            if (options.pdb != nullptr || !absolute_path(value)) return false;
            options.pdb = value;
        } else if (std::wcscmp(name, L"--guid") == 0) {
            if (options.have_guid || !parse_guid(value, options)) return false;
            options.have_guid = true;
        } else if (std::wcscmp(name, L"--age") == 0) {
            if (options.have_age || !unsigned32(value, options.age)) return false;
            options.have_age = true;
        } else if (std::wcscmp(name, L"--function-rva") == 0 ||
                   std::wcscmp(name, L"--data-rva") == 0) {
            if (options.count == query_limit) return false;
            Query& query = options.queries[options.count];
            if (!unsigned32(value, query.rva)) return false;
            query.tag = std::wcscmp(name, L"--function-rva") == 0 ?
                SymTagFunction : SymTagData;
            ++options.count;
        } else return false;
    }
    return options.dll != nullptr && options.pdb != nullptr && options.have_guid &&
        options.have_age && options.count > 0;
}

HRESULT utf8_name(BSTR name, std::string& output)
{
    const UINT length = SysStringLen(name);
    if (length > name_limit) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    if (length == 0) return S_OK;
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name,
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (count == 0) return HRESULT_FROM_WIN32(GetLastError());
    output.resize(static_cast<std::size_t>(count));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name,
        static_cast<int>(length), output.data(), count, nullptr, nullptr) != count)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

bool fixed_scalar(DWORD basic)
{
    switch (basic) {
    case btChar: case btWChar: case btInt: case btUInt: case btFloat:
    case btBCD: case btBool: case btLong: case btULong: case btCurrency:
    case btDate: case btVariant: case btComplex: case btBSTR: case btHresult:
    case btChar16: case btChar32: case btChar8:
        return true;
    default:
        return false;
    }
}

HRESULT resolve(IDiaSession* session, const Query& query, Result& result,
                const char*& stage)
{
    result.query = query;
    ComPtr<IDiaSymbol> symbol;
    stage = "findSymbolByRVAEx";
    result.lookup_hr = session->findSymbolByRVAEx(query.rva, query.tag,
        symbol.GetAddressOf(), &result.dia_displacement);
    if (result.lookup_hr == S_FALSE) return S_OK;
    if (result.lookup_hr != S_OK) return result.lookup_hr;
    if (!symbol) return E_UNEXPECTED;
    stage = "symbol.get_symTag";
    HRESULT hr = symbol->get_symTag(&result.tag);
    if (hr != S_OK) return hr;
    if (result.tag != static_cast<DWORD>(query.tag)) return E_UNEXPECTED;
    stage = "symbol.get_relativeVirtualAddress";
    hr = symbol->get_relativeVirtualAddress(&result.symbol_rva);
    if (hr != S_OK) return hr;
    result.displacement = static_cast<std::int64_t>(query.rva) - result.symbol_rva;
    stage = "symbol.get_name";
    StringValue name;
    hr = symbol->get_name(&name.value);
    if (hr != S_OK) return hr;
    stage = "symbol.name_utf8";
    hr = utf8_name(name.value, result.name);
    if (hr != S_OK) return hr;

    if (query.tag == SymTagData) {
        stage = "data.get_locationType";
        hr = symbol->get_locationType(&result.location);
        if (hr != S_OK) return hr;
        stage = "data.get_dataKind";
        hr = symbol->get_dataKind(&result.data_kind);
        if (hr != S_OK) return hr;
        // Static data's object extent comes from its type; get_length on data
        // can instead describe a bitfield. Never treat such bits as byte extents.
        if (result.location == LocIsStatic) {
            ComPtr<IDiaSymbol> type;
            stage = "data.get_type";
            hr = symbol->get_type(type.GetAddressOf());
            if (hr != S_OK && hr != S_FALSE) return hr;
            if (hr == S_OK) {
                if (!type) return E_UNEXPECTED;
                stage = "data.type.get_length";
                hr = type->get_length(&result.length);
                if (hr != S_OK && hr != S_FALSE) return hr;
                result.length_available = hr == S_OK;
                if (result.length_available) {
                    result.length_source = "data_type";
                    DWORD type_tag = 0;
                    stage = "data.type.get_symTag";
                    hr = type->get_symTag(&type_tag);
                    if (hr != S_OK) return hr;
                    result.byte_extent = type_tag == SymTagUDT || type_tag == SymTagArrayType ||
                        type_tag == SymTagPointerType || type_tag == SymTagEnum || type_tag == SymTagTypedef;
                    if (type_tag == SymTagBaseType) {
                        DWORD basic = 0;
                        stage = "data.type.get_baseType";
                        hr = type->get_baseType(&basic);
                        if (hr != S_OK && hr != S_FALSE) return hr;
                        result.byte_extent = hr == S_OK && fixed_scalar(basic);
                    }
                }
            }
        }
    } else {
        stage = "function.get_length";
        hr = symbol->get_length(&result.length);
        if (hr != S_OK && hr != S_FALSE) return hr;
        result.length_available = hr == S_OK;
        result.byte_extent = result.length_available;
        if (result.length_available) result.length_source = "function_bytes";
    }
    if (result.byte_extent && result.length > 0x100000000ULL - result.symbol_rva) {
        stage = "symbol.extent_outside_RVA_space";
        return E_UNEXPECTED;
    }
    const bool static_data = query.tag != SymTagData || result.location == LocIsStatic;
    result.classification = "nearby_unqualified";
    if (static_data && !result.name.empty()) {
        if (result.displacement == 0) result.classification = "exact_start";
        else if (result.displacement > 0 && result.byte_extent &&
                 static_cast<ULONGLONG>(result.displacement) < result.length)
            result.classification = "containing";
    }
    return S_OK;
}

void print_result(const Result& result)
{
    std::cout << "{\"requested_tag\":";
    json_string(std::cout, result.query.tag == SymTagFunction ? "function" : "data");
    std::cout << ",\"requested_rva\":";
    hex32(std::cout, result.query.rva);
    std::cout << ",\"classification\":";
    json_string(std::cout, result.classification);
    std::cout << ",\"lookup_hresult\":";
    hex32(std::cout, static_cast<DWORD>(result.lookup_hr));
    if (result.lookup_hr == S_OK) {
        std::cout << ",\"name\":";
        json_string(std::cout, result.name);
        std::cout << ",\"symbol_tag\":" << result.tag << ",\"symbol_rva\":";
        hex32(std::cout, result.symbol_rva);
        std::cout << ",\"displacement\":" << result.displacement
            << ",\"dia_displacement\":" << result.dia_displacement << ",\"length\":";
        if (result.length_available) std::cout << result.length;
        else std::cout << "null";
        std::cout << ",\"length_source\":";
        json_string(std::cout, result.length_source);
        std::cout << ",\"length_unit\":";
        json_string(std::cout, result.byte_extent ? "bytes" : "unqualified");
        if (result.query.tag == SymTagData)
            std::cout << ",\"location_type\":" << result.location
                << ",\"data_kind\":" << result.data_kind;
    }
    std::cout << '}';
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!parse(argc, argv, options)) return error("arguments", E_INVALIDARG, 2);
    Module module;
    module.value = LoadLibraryExW(options.dll, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module.value == nullptr) return error("LoadLibraryExW", HRESULT_FROM_WIN32(GetLastError()), 3);
    Apartment apartment;
    if (FAILED(apartment.hr)) return error("CoInitializeEx", apartment.hr, 5);
    const FARPROC address = GetProcAddress(module.value, "DllGetClassObject");
    if (address == nullptr) return error("DllGetClassObject.export", HRESULT_FROM_WIN32(GetLastError()), 3);
    using FactoryFunction = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    FactoryFunction create_factory = nullptr;
    static_assert(sizeof(create_factory) == sizeof(address), "Win32 export pointer width");
    std::memcpy(&create_factory, &address, sizeof(create_factory));
    ComPtr<IClassFactory> factory;
    HRESULT hr = create_factory(__uuidof(DiaSource), __uuidof(IClassFactory),
        reinterpret_cast<void**>(factory.GetAddressOf()));
    if (hr != S_OK || !factory) return error("DllGetClassObject", hr == S_OK ? E_UNEXPECTED : hr, 3);
    ComPtr<IDiaDataSource> source;
    hr = factory->CreateInstance(nullptr, __uuidof(IDiaDataSource),
        reinterpret_cast<void**>(source.GetAddressOf()));
    if (hr != S_OK || !source) return error("CreateInstance", hr == S_OK ? E_UNEXPECTED : hr, 3);
    hr = source->loadAndValidateDataFromPdb(options.pdb, &options.guid, 0, options.age);
    if (hr != S_OK) return error("loadAndValidateDataFromPdb", hr, 3);
    ComPtr<IDiaSession> session;
    hr = source->openSession(session.GetAddressOf());
    if (hr != S_OK || !session) return error("openSession", hr == S_OK ? E_UNEXPECTED : hr, 5);
    ComPtr<IDiaSymbol> global;
    hr = session->get_globalScope(global.GetAddressOf());
    if (hr != S_OK || !global) return error("get_globalScope", hr == S_OK ? E_UNEXPECTED : hr, 5);
    GUID actual_guid{};
    DWORD actual_age = 0;
    hr = global->get_guid(&actual_guid);
    if (hr != S_OK) return error("global.get_guid", hr, 5);
    hr = global->get_age(&actual_age);
    if (hr != S_OK) return error("global.get_age", hr, 5);
    if (!IsEqualGUID(actual_guid, options.guid) || actual_age != options.age)
        return error("validated_identity_consistency", E_UNEXPECTED, 5);
    std::array<Result, query_limit> results{};
    int code = 0;
    for (std::size_t i = 0; i < options.count; ++i) {
        const char* stage = nullptr;
        hr = resolve(session.Get(), options.queries[i], results[i], stage);
        if (hr != S_OK) return error(stage, hr, 5);
        if (std::strcmp(results[i].classification, "exact_start") != 0 &&
            std::strcmp(results[i].classification, "containing") != 0) code = 4;
    }
    std::cout << "{\"scope\":\"offline_PDB_RVA_resolution_not_shutdown_cause_proof\","
        "\"identity_validated\":true,\"guid\":";
    json_string(std::cout, options.guid_text);
    std::cout << ",\"age\":" << actual_age << ",\"results\":[";
    for (std::size_t i = 0; i < options.count; ++i) {
        if (i != 0) std::cout << ',';
        print_result(results[i]);
    }
    std::cout << "],\"exit_code\":" << code << "}\n" << std::flush;
    if (!std::cout) return error("stdout", HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), 5);
    return code;
}
