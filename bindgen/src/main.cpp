#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <cstdio>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

enum class EmitMode {
    Json,
    Node,
    Lua,
};

enum class TypeUse {
    General,
    FunctionParam,
    FunctionReturn,
    RecordField,
};

struct Options {
    EmitMode emit = EmitMode::Json;
    std::string outputPath;
    std::string moduleName = "urb_bindings";
    std::string libraryPath;
    std::vector<std::string> headers;
    std::vector<std::string> includeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> clangArgs;
    bool verbose = false;
    bool buildShim = true;
};

struct RecordInfo;
struct EnumInfo;

struct TypeInfo {
    enum class Kind {
        Primitive,
        Pointer,
        Function,
        CString,
        Array,
        Record,
        Enum,
        Unsupported,
    } kind = Kind::Unsupported;

    std::string cSpelling;
    std::string urbName;
    std::string reason;
    std::uint64_t arrayCount = 0;
    std::shared_ptr<TypeInfo> element;
    std::shared_ptr<TypeInfo> pointee;
    std::shared_ptr<TypeInfo> functionResult;
    std::vector<std::shared_ptr<TypeInfo>> functionArgs;
    bool functionVariadic = false;
    std::shared_ptr<RecordInfo> record;
    std::shared_ptr<EnumInfo> enumInfo;
};

struct FieldInfo {
    std::string name;
    TypeInfo type;
    long long offsetBits = -1;
    bool isBitField = false;
    int bitWidth = 0;
};

struct RecordInfo {
    std::string key;
    std::string name;
    std::string kind;
    long long size = -1;
    long long align = -1;
    bool complete = false;
    std::vector<FieldInfo> fields;
};

struct EnumConstantInfo {
    std::string name;
    std::string valueLiteral;
};

struct EnumInfo {
    std::string key;
    std::string name;
    std::string underlyingUrb;
    std::vector<EnumConstantInfo> constants;
};

struct ParamInfo {
    std::string name;
    TypeInfo type;
};

struct FunctionInfo {
    std::string key;
    std::string name;
    TypeInfo result;
    std::vector<ParamInfo> params;
    bool variadic = false;
    bool supported = false;
    std::string signature;
    std::string reason;
};

struct SupportInfo {
    bool ok = true;
    std::string reason;
};

std::string cxStringToStd(CXString s)
{
    const char *p = clang_getCString(s);
    std::string out = p ? p : "";
    clang_disposeString(s);
    return out;
}

std::string normalizePath(const std::string &path)
{
    if (path.empty()) return path;
    try {
        fs::path p(path);
        if (fs::exists(p)) {
            return fs::weakly_canonical(fs::absolute(p)).generic_string();
        }
        return fs::absolute(p).generic_string();
    } catch (...) {
        return path;
    }
}

std::string basenameNoExt(const std::string &path)
{
    fs::path p(path);
    return p.stem().string();
}

std::string quoteJson(const std::string &value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

std::string quoteJs(const std::string &value)
{
    return quoteJson(value);
}

std::string quoteLua(const std::string &value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\" << std::setw(3) << std::setfill('0') << static_cast<int>(ch)
                    << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

std::string trim(const std::string &value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string indent(int n)
{
    return std::string(static_cast<std::size_t>(n) * 2, ' ');
}

bool isCStringPointee(CXType type)
{
    type = clang_getCanonicalType(type);
    if (type.kind == CXType_Elaborated) {
        type = clang_Type_getNamedType(type);
        type = clang_getCanonicalType(type);
    }
    return type.kind == CXType_Char_S || type.kind == CXType_SChar || type.kind == CXType_Char_U;
}

bool isAnonymousRecordName(const std::string &name)
{
    return name.empty() || name.rfind("(anonymous", 0) == 0;
}

std::string cursorFilePath(CXCursor cursor)
{
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getExpansionLocation(loc, &file, &line, &column, &offset);
    (void)line;
    (void)column;
    (void)offset;
    if (!file) return {};
    return normalizePath(cxStringToStd(clang_getFileName(file)));
}

std::string cursorStableKey(CXCursor cursor)
{
    cursor = clang_getCanonicalCursor(cursor);
    std::string usr = cxStringToStd(clang_getCursorUSR(cursor));
    if (!usr.empty()) return usr;

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getExpansionLocation(loc, &file, &line, &column, &offset);
    std::ostringstream out;
    out << static_cast<int>(clang_getCursorKind(cursor)) << ':'
        << normalizePath(file ? cxStringToStd(clang_getFileName(file)) : std::string("<unknown>"))
        << ':' << line << ':' << column << ':' << offset;
    return out.str();
}

std::optional<std::string> urbPrimitiveForType(CXType type)
{
    type = clang_getCanonicalType(type);
    if (type.kind == CXType_Elaborated) {
        type = clang_Type_getNamedType(type);
        type = clang_getCanonicalType(type);
    }

    const long long size = clang_Type_getSizeOf(type);
    switch (type.kind) {
    case CXType_Void: return std::string("void");
    case CXType_Bool: return std::string("bool");
    case CXType_Char_S:
    case CXType_SChar: return std::string("i8");
    case CXType_Char_U:
    case CXType_UChar: return std::string("u8");
    case CXType_Short: return std::string("i16");
    case CXType_UShort: return std::string("u16");
    case CXType_Int: return std::string("i32");
    case CXType_UInt: return std::string("u32");
    case CXType_Long:
        if (size == 4) return std::string("i32");
        if (size == 8) return std::string("i64");
        break;
    case CXType_ULong:
        if (size == 4) return std::string("u32");
        if (size == 8) return std::string("u64");
        break;
    case CXType_LongLong: return std::string("i64");
    case CXType_ULongLong: return std::string("u64");
    case CXType_Float: return std::string("f32");
    case CXType_Double: return std::string("f64");
    default:
        break;
    }
    return std::nullopt;
}

std::string join(const std::vector<std::string> &values, const std::string &sep)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << sep;
        out << values[i];
    }
    return out.str();
}

std::string escapeHeaderInclude(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

std::string shellQuote(const std::string &value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string sharedLibraryExtension()
{
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::string sanitizeIdentifier(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_') out.push_back(static_cast<char>(ch));
        else out.push_back('_');
    }
    if (out.empty()) out = "x";
    if (std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

struct CallableFunctionInfo {
    FunctionInfo fn;
    bool direct = false;
    bool richDirect = false;
    bool shimmed = false;
    bool returnsByValueRecord = false;
    bool returnsViaOutPointer = false;
    std::vector<bool> paramByValueRecord;
    std::string publicSignature;
    std::string bindSymbol;
    std::string bindSignature;
};

std::vector<std::string> detectCompilerIncludeDirs()
{
    std::vector<std::string> out;
    const char *compilerEnv = std::getenv("CC");
    const std::string compiler = (compilerEnv && *compilerEnv) ? compilerEnv : "cc";
    const std::string command = shellQuote(compiler) + " -x c -E -v /dev/null -o /dev/null 2>&1";

#ifdef _WIN32
    FILE *pipe = _popen(command.c_str(), "r");
#else
    FILE *pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        return out;
    }

    char buffer[4096];
    bool inBlock = false;
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        if (line.find("#include <...> search starts here:") != std::string::npos) {
            inBlock = true;
            continue;
        }
        if (!inBlock) continue;
        if (line.find("End of search list.") != std::string::npos) {
            break;
        }

        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        std::string path = line.substr(first);
        const std::string suffix = " (framework directory)";
        if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
            path.erase(path.size() - suffix.size());
        }
        path = normalizePath(path);
        if (!path.empty() && std::find(out.begin(), out.end(), path) == out.end()) {
            out.push_back(path);
        }
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

class BindgenApp {
public:
    explicit BindgenApp(Options options)
        : options_(std::move(options))
    {
        if (options_.moduleName.empty() && !options_.outputPath.empty()) {
            options_.moduleName = basenameNoExt(options_.outputPath);
        }
        if (options_.moduleName.empty()) {
            options_.moduleName = "urb_bindings";
        }
        for (const std::string &header : options_.headers) {
            targetHeaders_.insert(normalizePath(header));
        }
    }

    ~BindgenApp()
    {
        if (tu_) clang_disposeTranslationUnit(tu_);
        if (index_) clang_disposeIndex(index_);
    }

    bool run()
    {
        if (!parseTranslationUnit()) {
            return false;
        }
        collectTopLevelDecls();
        const std::string text = emitOutput();
        if (options_.outputPath.empty()) {
            std::cout << text;
        } else {
            std::ofstream out(options_.outputPath, std::ios::binary);
            if (!out) {
                std::cerr << "failed to open output file: " << options_.outputPath << "\n";
                return false;
            }
            out << text;

            const std::string shimText = emitShimSource();
            if (!shimText.empty()) {
                const std::string shimPath = shimSourcePath();
                std::ofstream shimOut(shimPath, std::ios::binary);
                if (!shimOut) {
                    std::cerr << "failed to open shim output file: " << shimPath << "\n";
                    return false;
                }
                shimOut << shimText;
                if (options_.buildShim && !buildShimLibrary(shimPath)) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    Options options_;
    CXIndex index_ = nullptr;
    CXTranslationUnit tu_ = nullptr;
    std::vector<std::string> clangArgStorage_;
    std::vector<const char *> clangArgs_;
    std::unordered_set<std::string> targetHeaders_;

    std::unordered_map<std::string, std::shared_ptr<RecordInfo>> recordsByKey_;
    std::unordered_map<std::string, std::shared_ptr<EnumInfo>> enumsByKey_;
    std::unordered_set<std::string> exportedRecordKeys_;
    std::unordered_set<std::string> exportedEnumKeys_;
    std::unordered_map<std::string, std::vector<std::string>> recordAliases_;
    std::unordered_map<std::string, std::vector<std::string>> enumAliases_;
    std::unordered_map<std::string, FunctionInfo> functionsByKey_;
    std::unordered_map<std::string, bool> buildingRecords_;
    std::unordered_map<std::string, bool> buildingEnums_;
    std::vector<std::string> autoIncludeDirs_;

    bool parseTranslationUnit()
    {
        std::ostringstream wrapper;
        wrapper << "#define __URB_BINDGEN__ 1\n";
        for (const std::string &header : options_.headers) {
            wrapper << "#include \"" << escapeHeaderInclude(normalizePath(header)) << "\"\n";
        }
        const std::string wrapperSource = wrapper.str();
        const std::string wrapperName = "urb_bindgen_input.c";

        clangArgStorage_.push_back("-x");
        clangArgStorage_.push_back("c");
        clangArgStorage_.push_back("-std=c11");
        clangArgStorage_.push_back("-D__URB_BINDGEN__=1");

        autoIncludeDirs_ = detectCompilerIncludeDirs();
        for (const std::string &includeDir : autoIncludeDirs_) {
            clangArgStorage_.push_back("-isystem");
            clangArgStorage_.push_back(includeDir);
        }

        for (const std::string &includeDir : options_.includeDirs) {
            clangArgStorage_.push_back("-I" + includeDir);
        }
        for (const std::string &define : options_.defines) {
            clangArgStorage_.push_back("-D" + define);
        }
        for (const std::string &header : options_.headers) {
            try {
                const fs::path parent = fs::absolute(fs::path(header)).parent_path();
                if (!parent.empty()) {
                    clangArgStorage_.push_back("-I" + parent.generic_string());
                }
            } catch (...) {
            }
        }
        for (const std::string &arg : options_.clangArgs) {
            clangArgStorage_.push_back(arg);
        }
        for (const std::string &arg : clangArgStorage_) {
            clangArgs_.push_back(arg.c_str());
        }

        CXUnsavedFile unsaved[] = {
            { wrapperName.c_str(), wrapperSource.c_str(), wrapperSource.size() }
        };

        index_ = clang_createIndex(0, 0);
        unsigned parseOptions = CXTranslationUnit_KeepGoing;
        CXErrorCode ec = clang_parseTranslationUnit2(
            index_,
            wrapperName.c_str(),
            clangArgs_.data(),
            static_cast<int>(clangArgs_.size()),
            unsaved,
            1,
            parseOptions,
            &tu_);

        if (ec != CXError_Success || !tu_) {
            std::cerr << "failed to parse translation unit with libclang\n";
            return false;
        }

        if (options_.verbose && !autoIncludeDirs_.empty()) {
            std::cerr << "auto include dirs:\n";
            for (const std::string &dir : autoIncludeDirs_) {
                std::cerr << "  " << dir << "\n";
            }
        }

        const unsigned diagnosticCount = clang_getNumDiagnostics(tu_);
        bool fatal = false;
        for (unsigned i = 0; i < diagnosticCount; ++i) {
            CXDiagnostic diagnostic = clang_getDiagnostic(tu_, i);
            const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
            const std::string message = cxStringToStd(clang_formatDiagnostic(
                diagnostic,
                clang_defaultDiagnosticDisplayOptions()));
            if (!message.empty()) {
                std::cerr << message << "\n";
            }
            if (severity == CXDiagnostic_Error || severity == CXDiagnostic_Fatal) {
                fatal = true;
            }
            clang_disposeDiagnostic(diagnostic);
        }
        return !fatal;
    }

    bool isTargetCursor(CXCursor cursor) const
    {
        const std::string file = cursorFilePath(cursor);
        return !file.empty() && targetHeaders_.find(file) != targetHeaders_.end();
    }

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor, CXClientData clientData)
    {
        auto *self = static_cast<BindgenApp *>(clientData);
        self->visitTopLevel(cursor);
        return CXChildVisit_Continue;
    }

    void collectTopLevelDecls()
    {
        CXCursor root = clang_getTranslationUnitCursor(tu_);
        clang_visitChildren(root, &BindgenApp::visitor, this);
    }

    void visitTopLevel(CXCursor cursor)
    {
        if (!isTargetCursor(cursor)) return;

        switch (clang_getCursorKind(cursor)) {
        case CXCursor_FunctionDecl:
            collectFunction(cursor);
            break;
        case CXCursor_StructDecl:
        case CXCursor_UnionDecl:
            collectRecord(cursor);
            break;
        case CXCursor_EnumDecl:
            collectEnum(cursor);
            break;
        case CXCursor_TypedefDecl:
            collectTypedef(cursor);
            break;
        default:
            break;
        }
    }

    std::shared_ptr<RecordInfo> buildRecord(CXCursor cursor)
    {
        cursor = clang_getCanonicalCursor(cursor);
        if (!clang_isDeclaration(clang_getCursorKind(cursor))) return nullptr;

        CXCursor definition = clang_getCursorDefinition(cursor);
        if (!clang_Cursor_isNull(definition)) {
            cursor = clang_getCanonicalCursor(definition);
        }

        const std::string key = cursorStableKey(cursor);
        auto it = recordsByKey_.find(key);
        if (it != recordsByKey_.end()) {
            return it->second;
        }

        auto record = std::make_shared<RecordInfo>();
        record->key = key;
        record->name = cxStringToStd(clang_getCursorSpelling(cursor));
        record->kind = clang_getCursorKind(cursor) == CXCursor_UnionDecl ? "union" : "struct";
        recordsByKey_[key] = record;

        if (buildingRecords_[key]) {
            return record;
        }
        buildingRecords_[key] = true;

        const CXType type = clang_getCursorType(cursor);
        record->size = clang_Type_getSizeOf(type);
        record->align = clang_Type_getAlignOf(type);
        record->complete = clang_isCursorDefinition(cursor) != 0 && record->size >= 0 && record->align >= 0;

        if (record->complete) {
            struct FieldVisitorState {
                BindgenApp *self;
                RecordInfo *record;
            } state { this, record.get() };

            auto fieldVisitor = [](CXCursor child, CXCursor, CXClientData data) {
                auto *state = static_cast<FieldVisitorState *>(data);
                if (clang_getCursorKind(child) == CXCursor_FieldDecl) {
                    FieldInfo field;
                    field.name = cxStringToStd(clang_getCursorSpelling(child));
                    field.offsetBits = clang_Cursor_getOffsetOfField(child);
                    field.isBitField = clang_Cursor_isBitField(child) != 0;
                    if (field.isBitField) {
                        field.bitWidth = clang_getFieldDeclBitWidth(child);
                    }
                    field.type = state->self->buildTypeInfo(clang_getCursorType(child), TypeUse::RecordField);
                    state->record->fields.push_back(std::move(field));
                }
                return CXChildVisit_Continue;
            };

            clang_visitChildren(cursor, fieldVisitor, &state);
        }

        buildingRecords_[key] = false;
        return record;
    }

    std::shared_ptr<EnumInfo> buildEnum(CXCursor cursor)
    {
        cursor = clang_getCanonicalCursor(cursor);
        CXCursor definition = clang_getCursorDefinition(cursor);
        if (!clang_Cursor_isNull(definition)) {
            cursor = clang_getCanonicalCursor(definition);
        }

        const std::string key = cursorStableKey(cursor);
        auto it = enumsByKey_.find(key);
        if (it != enumsByKey_.end()) {
            return it->second;
        }

        auto info = std::make_shared<EnumInfo>();
        info->key = key;
        info->name = cxStringToStd(clang_getCursorSpelling(cursor));
        enumsByKey_[key] = info;

        if (buildingEnums_[key]) {
            return info;
        }
        buildingEnums_[key] = true;

        CXType underlying = clang_getEnumDeclIntegerType(cursor);
        auto urb = urbPrimitiveForType(underlying);
        info->underlyingUrb = urb.value_or("i32");

        auto enumVisitor = [](CXCursor child, CXCursor, CXClientData data) {
            auto *info = static_cast<EnumInfo *>(data);
            if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl) {
                EnumConstantInfo constant;
                constant.name = cxStringToStd(clang_getCursorSpelling(child));
                constant.valueLiteral = std::to_string(clang_getEnumConstantDeclValue(child));
                info->constants.push_back(std::move(constant));
            }
            return CXChildVisit_Continue;
        };
        clang_visitChildren(cursor, enumVisitor, info.get());

        buildingEnums_[key] = false;
        return info;
    }

    TypeInfo buildTypeInfo(CXType type, TypeUse use)
    {
        TypeInfo info;
        info.cSpelling = cxStringToStd(clang_getTypeSpelling(type));

        type = clang_getCanonicalType(type);
        if (type.kind == CXType_Elaborated) {
            type = clang_Type_getNamedType(type);
            type = clang_getCanonicalType(type);
        }

        if (auto primitive = urbPrimitiveForType(type)) {
            info.kind = TypeInfo::Kind::Primitive;
            info.urbName = *primitive;
            return info;
        }

        switch (type.kind) {
        case CXType_Void:
            info.kind = TypeInfo::Kind::Primitive;
            info.urbName = "void";
            return info;
        case CXType_Enum: {
            auto enumInfo = buildEnum(clang_getTypeDeclaration(type));
            info.kind = TypeInfo::Kind::Enum;
            info.enumInfo = enumInfo;
            info.urbName = enumInfo ? enumInfo->underlyingUrb : "i32";
            return info;
        }
        case CXType_Pointer: {
            CXType pointee = clang_getPointeeType(type);
            CXType canonicalPointee = clang_getCanonicalType(pointee);
            if (canonicalPointee.kind == CXType_Elaborated) {
                canonicalPointee = clang_Type_getNamedType(canonicalPointee);
                canonicalPointee = clang_getCanonicalType(canonicalPointee);
            }
            if (isCStringPointee(canonicalPointee)) {
                info.kind = TypeInfo::Kind::CString;
                info.urbName = "cstring";
                return info;
            }
            info.kind = TypeInfo::Kind::Pointer;
            info.urbName = "pointer";
            TypeInfo pointeeInfo = buildTypeInfo(canonicalPointee, TypeUse::General);
            if (pointeeInfo.kind != TypeInfo::Kind::Unsupported
                && !(pointeeInfo.kind == TypeInfo::Kind::Primitive && pointeeInfo.urbName == "void")) {
                info.pointee = std::make_shared<TypeInfo>(std::move(pointeeInfo));
            }
            return info;
        }
        case CXType_ConstantArray: {
            const long long count = clang_getArraySize(type);
            TypeInfo element = buildTypeInfo(clang_getArrayElementType(type), use);
            if (use == TypeUse::FunctionParam) {
                if (element.kind == TypeInfo::Kind::CString) {
                    return element;
                }
                TypeInfo ptr;
                ptr.cSpelling = info.cSpelling;
                ptr.kind = TypeInfo::Kind::Pointer;
                ptr.urbName = "pointer";
                return ptr;
            }
            info.kind = TypeInfo::Kind::Array;
            info.arrayCount = count > 0 ? static_cast<std::uint64_t>(count) : 0;
            info.element = std::make_shared<TypeInfo>(std::move(element));
            if (info.element->kind == TypeInfo::Kind::Primitive || info.element->kind == TypeInfo::Kind::Enum) {
                return info;
            }
            info.kind = TypeInfo::Kind::Unsupported;
            info.reason = "only primitive or enum arrays are supported";
            info.element.reset();
            info.arrayCount = 0;
            return info;
        }
        case CXType_IncompleteArray:
        case CXType_VariableArray:
            if (use == TypeUse::FunctionParam) {
                TypeInfo ptr;
                ptr.cSpelling = info.cSpelling;
                ptr.kind = TypeInfo::Kind::Pointer;
                ptr.urbName = "pointer";
                return ptr;
            }
            info.kind = TypeInfo::Kind::Unsupported;
            info.reason = "incomplete or variable arrays are not supported";
            return info;
        case CXType_Record: {
            auto record = buildRecord(clang_getTypeDeclaration(type));
            info.kind = TypeInfo::Kind::Record;
            info.record = record;
            if (!record) {
                info.kind = TypeInfo::Kind::Unsupported;
                info.reason = "record declaration could not be resolved";
            }
            return info;
        }
        case CXType_FunctionProto:
        case CXType_FunctionNoProto: {
            info.kind = TypeInfo::Kind::Function;
            info.functionResult = std::make_shared<TypeInfo>(buildTypeInfo(clang_getResultType(type), TypeUse::FunctionReturn));
            info.functionVariadic = type.kind == CXType_FunctionProto && clang_isFunctionTypeVariadic(type) != 0;
            const int argc = clang_getNumArgTypes(type);
            if (argc > 0) {
                info.functionArgs.reserve(static_cast<std::size_t>(argc));
                for (int i = 0; i < argc; ++i) {
                    info.functionArgs.push_back(std::make_shared<TypeInfo>(buildTypeInfo(clang_getArgType(type, i), TypeUse::FunctionParam)));
                }
            }
            return info;
        }
        case CXType_LongDouble:
        case CXType_Float128:
            info.kind = TypeInfo::Kind::Unsupported;
            info.reason = "long double and 128-bit floats are not supported by urb-ffi";
            return info;
        default:
            info.kind = TypeInfo::Kind::Unsupported;
            info.reason = "unsupported C type kind: " + std::to_string(static_cast<int>(type.kind));
            return info;
        }
    }

    void collectFunction(CXCursor cursor)
    {
        const auto linkage = clang_getCursorLinkage(cursor);
        if (!(linkage == CXLinkage_External || linkage == CXLinkage_UniqueExternal)) {
            return;
        }

        FunctionInfo fn;
        fn.key = cursorStableKey(cursor);
        fn.name = cxStringToStd(clang_getCursorSpelling(cursor));
        if (fn.name.empty()) return;

        CXType fnType = clang_getCursorType(cursor);
        fn.result = buildTypeInfo(clang_getResultType(fnType), TypeUse::FunctionReturn);
        fn.variadic = clang_Cursor_isVariadic(cursor) != 0;

        const int argc = clang_Cursor_getNumArguments(cursor);
        for (int i = 0; i < argc; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, i);
            ParamInfo param;
            param.name = cxStringToStd(clang_getCursorSpelling(arg));
            param.type = buildTypeInfo(clang_getCursorType(arg), TypeUse::FunctionParam);
            fn.params.push_back(std::move(param));
        }

        std::tie(fn.supported, fn.signature, fn.reason) = makeSignature(fn);
        functionsByKey_[fn.key] = std::move(fn);
    }

    void collectRecord(CXCursor cursor)
    {
        auto record = buildRecord(cursor);
        if (!record) return;
        if (!isAnonymousRecordName(record->name)) {
            exportedRecordKeys_.insert(record->key);
        }
    }

    void collectEnum(CXCursor cursor)
    {
        auto info = buildEnum(cursor);
        if (!info) return;
        if (!isAnonymousRecordName(info->name)) {
            exportedEnumKeys_.insert(info->key);
        }
    }

    void collectTypedef(CXCursor cursor)
    {
        const std::string alias = cxStringToStd(clang_getCursorSpelling(cursor));
        if (alias.empty()) return;

        CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
        underlying = clang_getCanonicalType(underlying);
        if (underlying.kind == CXType_Elaborated) {
            underlying = clang_Type_getNamedType(underlying);
            underlying = clang_getCanonicalType(underlying);
        }

        if (underlying.kind == CXType_Record) {
            auto record = buildRecord(clang_getTypeDeclaration(underlying));
            if (record) {
                exportedRecordKeys_.insert(record->key);
                auto &aliases = recordAliases_[record->key];
                if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
                    aliases.push_back(alias);
                }
            }
        } else if (underlying.kind == CXType_Enum) {
            auto info = buildEnum(clang_getTypeDeclaration(underlying));
            if (info) {
                exportedEnumKeys_.insert(info->key);
                auto &aliases = enumAliases_[info->key];
                if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
                    aliases.push_back(alias);
                }
            }
        }
    }

    std::tuple<bool, std::string, std::string> makeSignature(const FunctionInfo &fn) const
    {
        auto typeToSig = [](const TypeInfo &type) -> std::optional<std::string> {
            switch (type.kind) {
            case TypeInfo::Kind::Primitive:
                return type.urbName;
            case TypeInfo::Kind::Pointer:
            case TypeInfo::Kind::CString:
                return type.urbName;
            case TypeInfo::Kind::Enum:
                return type.urbName;
            default:
                return std::nullopt;
            }
        };

        auto ret = typeToSig(fn.result);
        if (!ret) {
            return { false, {}, "unsupported return type: " + fn.result.cSpelling };
        }

        std::vector<std::string> argSigs;
        argSigs.reserve(fn.params.size() + (fn.variadic ? 1 : 0));
        for (const ParamInfo &param : fn.params) {
            auto sig = typeToSig(param.type);
            if (!sig) {
                return { false, {}, "unsupported parameter type in " + fn.name + ": " + param.type.cSpelling };
            }
            argSigs.push_back(*sig);
        }
        if (fn.variadic) {
            argSigs.push_back("...");
        }
        std::ostringstream out;
        out << *ret << ' ' << fn.name << '(' << join(argSigs, ", ") << ')';
        return { true, out.str(), {} };
    }

    SupportInfo checkSchemaType(const TypeInfo &type, std::vector<std::string> &path) const
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::Pointer:
        case TypeInfo::Kind::CString:
        case TypeInfo::Kind::Enum:
            return { true, {} };
        case TypeInfo::Kind::Array:
            if (!type.element) return { false, "array element type is missing" };
            if (type.element->kind == TypeInfo::Kind::Primitive || type.element->kind == TypeInfo::Kind::Enum) {
                return { true, {} };
            }
            return { false, "only arrays of primitives or enums are supported" };
        case TypeInfo::Kind::Record:
            if (!type.record) return { false, "nested record is missing" };
            return checkSchemaRecord(type.record, path);
        case TypeInfo::Kind::Unsupported:
            return { false, type.reason.empty() ? "unsupported field type" : type.reason };
        default:
            return { false, "unsupported field type" };
        }
    }

    SupportInfo checkSchemaRecord(const std::shared_ptr<RecordInfo> &record, std::vector<std::string> &path) const
    {
        if (!record) return { false, "record is missing" };
        if (!record->complete) return { false, "record is incomplete" };
        if (std::find(path.begin(), path.end(), record->key) != path.end()) {
            return { false, "by-value recursive schemas are not supported" };
        }

        path.push_back(record->key);
        for (const FieldInfo &field : record->fields) {
            if (field.name.empty()) {
                path.pop_back();
                return { false, "anonymous fields are not supported by the current schema model" };
            }
            if (field.name.rfind("__", 0) == 0) {
                path.pop_back();
                return { false, "field names beginning with __ are reserved by the current schema model" };
            }
            if (field.isBitField) {
                path.pop_back();
                return { false, "bit-fields are not supported by the current schema model" };
            }
            SupportInfo support = checkSchemaType(field.type, path);
            if (!support.ok) {
                path.pop_back();
                return { false, "field " + field.name + ": " + support.reason };
            }
        }
        path.pop_back();
        return { true, {} };
    }

    std::vector<std::shared_ptr<RecordInfo>> exportedRecords() const
    {
        std::vector<std::shared_ptr<RecordInfo>> out;
        for (const std::string &key : exportedRecordKeys_) {
            auto it = recordsByKey_.find(key);
            if (it != recordsByKey_.end()) out.push_back(it->second);
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
            const std::string an = !a->name.empty() ? a->name : a->key;
            const std::string bn = !b->name.empty() ? b->name : b->key;
            return an < bn;
        });
        return out;
    }

    std::vector<std::shared_ptr<EnumInfo>> exportedEnums() const
    {
        std::vector<std::shared_ptr<EnumInfo>> out;
        for (const std::string &key : exportedEnumKeys_) {
            auto it = enumsByKey_.find(key);
            if (it != enumsByKey_.end()) out.push_back(it->second);
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
            const std::string an = !a->name.empty() ? a->name : a->key;
            const std::string bn = !b->name.empty() ? b->name : b->key;
            return an < bn;
        });
        return out;
    }

    std::vector<FunctionInfo> exportedFunctions() const
    {
        std::vector<FunctionInfo> out;
        out.reserve(functionsByKey_.size());
        for (const auto &entry : functionsByKey_) out.push_back(entry.second);
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        return out;
    }

    std::vector<std::string> namesForRecord(const std::shared_ptr<RecordInfo> &record) const
    {
        std::vector<std::string> names;
        if (record && !isAnonymousRecordName(record->name)) names.push_back(record->name);
        auto it = recordAliases_.find(record ? record->key : std::string());
        if (it != recordAliases_.end()) {
            for (const std::string &alias : it->second) {
                if (std::find(names.begin(), names.end(), alias) == names.end()) {
                    names.push_back(alias);
                }
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<std::string> namesForEnum(const std::shared_ptr<EnumInfo> &info) const
    {
        std::vector<std::string> names;
        if (info && !isAnonymousRecordName(info->name)) names.push_back(info->name);
        auto it = enumAliases_.find(info ? info->key : std::string());
        if (it != enumAliases_.end()) {
            for (const std::string &alias : it->second) {
                if (std::find(names.begin(), names.end(), alias) == names.end()) {
                    names.push_back(alias);
                }
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    static std::optional<std::string> directSigAtom(const TypeInfo &type)
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::Pointer:
        case TypeInfo::Kind::CString:
        case TypeInfo::Kind::Enum:
            return type.urbName;
        default:
            return std::nullopt;
        }
    }

    std::string displayTypeName(const TypeInfo &type) const
    {
        const std::string spelling = trim(type.cSpelling);
        if (!spelling.empty()) return spelling;
        if (type.kind == TypeInfo::Kind::Pointer) {
            if (type.pointee) {
                return "pointer(" + displayTypeName(*type.pointee) + ")";
            }
            return type.urbName.empty() ? std::string("pointer") : type.urbName;
        }
        if (type.kind == TypeInfo::Kind::Function) {
            std::vector<std::string> args;
            for (const auto &arg : type.functionArgs) {
                args.push_back(arg ? displayTypeName(*arg) : std::string("pointer"));
            }
            if (type.functionVariadic) args.push_back("...");
            return "function(" + (type.functionResult ? displayTypeName(*type.functionResult) : std::string("void"))
                + " (" + join(args, ", ") + "))";
        }
        if (type.kind == TypeInfo::Kind::Record && type.record) {
            const auto names = namesForRecord(type.record);
            if (!names.empty()) return names.front();
            if (!type.record->name.empty()) return type.record->name;
        }
        if (type.kind == TypeInfo::Kind::Enum && type.enumInfo) {
            const auto names = namesForEnum(type.enumInfo);
            if (!names.empty()) return names.front();
            if (!type.enumInfo->name.empty()) return type.enumInfo->name;
        }
        return type.urbName.empty() ? std::string("void") : type.urbName;
    }

    std::string publicSignatureFor(const FunctionInfo &fn) const
    {
        if (fn.supported && !fn.signature.empty()) return fn.signature;

        std::vector<std::string> argSigs;
        argSigs.reserve(fn.params.size() + (fn.variadic ? 1 : 0));
        for (const ParamInfo &param : fn.params) {
            argSigs.push_back(displayTypeName(param.type));
        }
        if (fn.variadic) argSigs.push_back("...");

        std::ostringstream out;
        out << displayTypeName(fn.result) << ' ' << fn.name << '(' << join(argSigs, ", ") << ')';
        return out.str();
    }

    SupportInfo checkByValueRecord(const TypeInfo &type) const
    {
        if (type.kind != TypeInfo::Kind::Record) return { true, {} };
        std::vector<std::string> path;
        return checkSchemaRecord(type.record, path);
    }

    bool shimCompatibleType(const TypeInfo &type) const
    {
        if (directSigAtom(type)) return true;
        if (type.kind != TypeInfo::Kind::Record) return false;
        return checkByValueRecord(type).ok;
    }

    bool directDescriptorCompatibleType(const TypeInfo &type) const
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::CString:
        case TypeInfo::Kind::Enum:
            return true;
        case TypeInfo::Kind::Pointer:
            if (type.pointee && type.pointee->kind == TypeInfo::Kind::Function) {
                return directDescriptorCompatibleType(*type.pointee);
            }
            return true;
        case TypeInfo::Kind::Array:
            return type.element && (type.element->kind == TypeInfo::Kind::Primitive || type.element->kind == TypeInfo::Kind::Enum);
        case TypeInfo::Kind::Record:
            return checkByValueRecord(type).ok;
        case TypeInfo::Kind::Function:
            if (!type.functionResult || !directDescriptorCompatibleType(*type.functionResult)) return false;
            for (const auto &arg : type.functionArgs) {
                if (!arg || !directDescriptorCompatibleType(*arg)) return false;
            }
            return true;
        case TypeInfo::Kind::Unsupported:
        default:
            return false;
        }
    }

    std::string recordSchemaName(const TypeInfo &type) const
    {
        const std::string spelling = trim(type.cSpelling);
        if (type.record) {
            const auto names = namesForRecord(type.record);
            if (!spelling.empty() && std::find(names.begin(), names.end(), spelling) != names.end()) {
                return spelling;
            }
            if (!names.empty()) return names.front();
            if (!type.record->name.empty()) return type.record->name;
        }
        return spelling;
    }

    std::string wrapperRecordKey(const TypeInfo &type) const
    {
        if (type.kind != TypeInfo::Kind::Record || !type.record) return {};
        return preferredRecordName(type.record);
    }

    std::string shimSymbolFor(const FunctionInfo &fn) const
    {
        return "urbshim_" + sanitizeIdentifier(options_.moduleName) + "_" + sanitizeIdentifier(fn.name);
    }

    std::string fallbackParamName(const ParamInfo &param, std::size_t index) const
    {
        const std::string name = trim(param.name);
        if (!name.empty()) return sanitizeIdentifier(name);
        return "arg" + std::to_string(index);
    }

    std::string cTypeForShim(const TypeInfo &type) const
    {
        return displayTypeName(type);
    }

    std::string constPointerTypeForShim(const TypeInfo &type) const
    {
        const std::string base = trim(cTypeForShim(type));
        if (base.rfind("const ", 0) == 0) return base + " *";
        return "const " + base + " *";
    }

    std::optional<CallableFunctionInfo> classifyCallable(const FunctionInfo &fn) const
    {
        CallableFunctionInfo info;
        info.fn = fn;
        info.publicSignature = publicSignatureFor(fn);

        if (fn.supported) {
            info.direct = true;
            info.bindSymbol = fn.name;
            info.bindSignature = fn.signature;
            return info;
        }

        if (fn.variadic) return std::nullopt;
        if (!directDescriptorCompatibleType(fn.result)) return std::nullopt;

        info.direct = true;
        info.richDirect = true;
        info.bindSymbol = fn.name;
        info.returnsByValueRecord = (fn.result.kind == TypeInfo::Kind::Record);
        if (info.returnsByValueRecord && wrapperRecordKey(fn.result).empty()) {
            return std::nullopt;
        }

        for (const ParamInfo &param : fn.params) {
            if (!directDescriptorCompatibleType(param.type)) return std::nullopt;
            const bool byValueRecord = (param.type.kind == TypeInfo::Kind::Record);
            if (byValueRecord && wrapperRecordKey(param.type).empty()) return std::nullopt;
            info.paramByValueRecord.push_back(byValueRecord);
        }
        return info;
    }

    std::vector<CallableFunctionInfo> callableFunctions() const
    {
        std::vector<CallableFunctionInfo> out;
        for (const FunctionInfo &fn : exportedFunctions()) {
            auto callable = classifyCallable(fn);
            if (callable) out.push_back(std::move(*callable));
        }
        return out;
    }

    std::vector<FunctionInfo> unsupportedWrapperFunctions() const
    {
        std::vector<FunctionInfo> out;
        for (const FunctionInfo &fn : exportedFunctions()) {
            if (!classifyCallable(fn)) out.push_back(fn);
        }
        return out;
    }

    bool hasShimmedFunctions(const std::vector<CallableFunctionInfo> &functions) const
    {
        return std::any_of(functions.begin(), functions.end(), [](const CallableFunctionInfo &fn) {
            return fn.shimmed;
        });
    }

    bool needsRecordHelpers(const std::vector<CallableFunctionInfo> &functions) const
    {
        return std::any_of(functions.begin(), functions.end(), [](const CallableFunctionInfo &fn) {
            return fn.returnsByValueRecord
                || std::any_of(fn.paramByValueRecord.begin(), fn.paramByValueRecord.end(), [](bool byValue) {
                    return byValue;
                });
        });
    }

    std::string shimLibraryFileName() const
    {
        const std::string base = !options_.outputPath.empty()
            ? fs::path(options_.outputPath).stem().string()
            : sanitizeIdentifier(options_.moduleName);
        return base + ".shim" + sharedLibraryExtension();
    }

    std::string shimSourcePath() const
    {
        if (options_.outputPath.empty()) return {};
        const fs::path output(options_.outputPath);
        return (output.parent_path() / (output.stem().string() + ".shim.c")).generic_string();
    }

    std::string shimBinaryPath() const
    {
        if (options_.outputPath.empty()) return {};
        const fs::path output(options_.outputPath);
        return (output.parent_path() / shimLibraryFileName()).generic_string();
    }

    std::vector<std::string> shimIncludeDirs() const
    {
        std::vector<std::string> dirs;
        auto addDir = [&](const std::string &dir) {
            const std::string normalized = normalizePath(dir);
            if (!normalized.empty() && std::find(dirs.begin(), dirs.end(), normalized) == dirs.end()) {
                dirs.push_back(normalized);
            }
        };

        for (const std::string &dir : options_.includeDirs) addDir(dir);
        for (const std::string &header : options_.headers) {
            try {
                const fs::path parent = fs::absolute(fs::path(header)).parent_path();
                if (!parent.empty()) addDir(parent.generic_string());
            } catch (...) {
            }
        }
        return dirs;
    }

    std::string resolveLibraryPathForBuild() const
    {
        if (options_.libraryPath.empty()) return {};

        auto tryPath = [](const fs::path &p) -> std::string {
            try {
                if (!p.empty() && fs::exists(p)) {
                    return normalizePath(p.generic_string());
                }
            } catch (...) {
            }
            return {};
        };

        const fs::path raw(options_.libraryPath);
        if (raw.is_absolute()) {
            const std::string found = tryPath(raw);
            if (!found.empty()) return found;
        }

        if (!options_.outputPath.empty()) {
            const fs::path outputDir = fs::absolute(fs::path(options_.outputPath)).parent_path();
            const std::string found = tryPath(outputDir / raw);
            if (!found.empty()) return found;
        }

        const std::string found = tryPath(raw);
        if (!found.empty()) return found;

        return {};
    }

    std::string libraryLinkArgForBuild() const
    {
        const std::string resolved = resolveLibraryPathForBuild();
        if (!resolved.empty()) return shellQuote(resolved);
        if (options_.libraryPath.empty()) return {};

        const std::string raw = trim(options_.libraryPath);
        if (raw.empty()) return {};
        if (raw.find('/') != std::string::npos || raw.find('\\') != std::string::npos) {
            return shellQuote(raw);
        }
        if (!raw.empty() && raw.front() == '-') {
            return raw;
        }
        if (raw.find('.') != std::string::npos) {
            return "-l:" + raw;
        }
        return "-l" + raw;
    }

    std::string shimRpathArg() const
    {
        const std::string resolved = resolveLibraryPathForBuild();
        if (resolved.empty() || options_.outputPath.empty()) return {};

        try {
            const fs::path shimDir = fs::absolute(fs::path(options_.outputPath)).parent_path();
            const fs::path libDir = fs::path(resolved).parent_path();
            if (shimDir.empty() || libDir.empty()) return {};

            fs::path rel = fs::relative(libDir, shimDir);
            std::string rpath = "$ORIGIN";
            const std::string relText = rel.generic_string();
            if (!relText.empty() && relText != ".") rpath += "/" + relText;
            return "-Wl,-rpath," + shellQuote(rpath);
        } catch (...) {
            const fs::path libDir = fs::path(resolved).parent_path();
            if (libDir.empty()) return {};
            return "-Wl,-rpath," + shellQuote(libDir.generic_string());
        }
    }

    bool buildShimLibrary(const std::string &shimPath) const
    {
        const std::string outputPath = shimBinaryPath();
        if (outputPath.empty()) return true;

        const char *compilerEnv = std::getenv("CC");
        const std::string compiler = (compilerEnv && *compilerEnv) ? compilerEnv : "cc";
        const char *cppFlags = std::getenv("CPPFLAGS");
        const char *cFlags = std::getenv("CFLAGS");
        const char *ldFlags = std::getenv("LDFLAGS");

        std::vector<std::string> parts;
        parts.push_back(shellQuote(compiler));
        if (cppFlags && *cppFlags) parts.push_back(cppFlags);
        if (cFlags && *cFlags) parts.push_back(cFlags);
        parts.push_back("-shared");
#ifndef _WIN32
        parts.push_back("-fPIC");
#endif
        for (const std::string &dir : shimIncludeDirs()) {
            parts.push_back("-I" + shellQuote(dir));
        }
        parts.push_back("-o");
        parts.push_back(shellQuote(outputPath));
        parts.push_back(shellQuote(shimPath));

        const std::string rpathArg = shimRpathArg();
        if (!rpathArg.empty()) parts.push_back(rpathArg);

        const std::string linkArg = libraryLinkArgForBuild();
        if (!linkArg.empty()) parts.push_back(linkArg);
        if (ldFlags && *ldFlags) parts.push_back(ldFlags);

        const std::string command = join(parts, " ");
        if (options_.verbose) {
            std::cerr << "building shim library: " << command << "\n";
        }

        const int status = std::system(command.c_str());
        if (status != 0) {
            std::cerr << "failed to build shim library: " << outputPath << "\n";
            std::cerr << "command: " << command << "\n";
            return false;
        }
        return true;
    }

    std::string emitShimSource() const
    {
        const auto functions = callableFunctions();
        if (!hasShimmedFunctions(functions)) return {};

        std::ostringstream out;
        out << "/* Generated by urb-bindgen. */\n";
        out << "/* Module: " << options_.moduleName << " */\n\n";
        out << "#if defined(_WIN32)\n";
        out << "#  define URB_BINDGEN_EXPORT __declspec(dllexport)\n";
        out << "#else\n";
        out << "#  define URB_BINDGEN_EXPORT __attribute__((visibility(\"default\")))\n";
        out << "#endif\n\n";
        for (const std::string &header : options_.headers) {
            out << "#include \"" << escapeHeaderInclude(normalizePath(header)) << "\"\n";
        }
        out << "\n";

        for (const CallableFunctionInfo &callable : functions) {
            if (!callable.shimmed) continue;

            std::vector<std::string> params;
            std::vector<std::string> args;
            if (callable.returnsViaOutPointer) {
                params.push_back(cTypeForShim(callable.fn.result) + " *out_result");
            }

            for (std::size_t i = 0; i < callable.fn.params.size(); ++i) {
                const ParamInfo &param = callable.fn.params[i];
                const std::string name = fallbackParamName(param, i);
                if (callable.paramByValueRecord[i]) {
                    params.push_back(constPointerTypeForShim(param.type) + " " + name);
                    args.push_back("*" + name);
                } else {
                    params.push_back(cTypeForShim(param.type) + " " + name);
                    args.push_back(name);
                }
            }

            out << "URB_BINDGEN_EXPORT "
                << (callable.returnsViaOutPointer ? "void" : cTypeForShim(callable.fn.result))
                << ' ' << callable.bindSymbol << '(' << join(params, ", ") << ")\n{\n";

            const std::string callExpr = callable.fn.name + "(" + join(args, ", ") + ")";
            if (callable.returnsViaOutPointer) {
                out << "    *out_result = " << callExpr << ";\n";
            } else if (callable.fn.result.kind == TypeInfo::Kind::Primitive && callable.fn.result.urbName == "void") {
                out << "    " << callExpr << ";\n";
            } else {
                out << "    return " << callExpr << ";\n";
            }
            out << "}\n\n";
        }

        return out.str();
    }

    std::string emitOutput() const
    {
        switch (options_.emit) {
        case EmitMode::Json: return emitJson();
        case EmitMode::Node: return emitNode();
        case EmitMode::Lua: return emitLua();
        }
        throw std::runtime_error("unknown emit mode");
    }

    std::string emitJson() const
    {
        std::ostringstream out;
        out << "{\n";
        out << indent(1) << "\"module\": " << quoteJson(options_.moduleName) << ",\n";
        out << indent(1) << "\"library\": " << (options_.libraryPath.empty() ? std::string("null") : quoteJson(options_.libraryPath)) << ",\n";

        out << indent(1) << "\"headers\": [";
        for (std::size_t i = 0; i < options_.headers.size(); ++i) {
            if (i) out << ", ";
            out << quoteJson(normalizePath(options_.headers[i]));
        }
        out << "],\n";

        const auto records = exportedRecords();
        out << indent(1) << "\"records\": [\n";
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto &record = records[i];
            std::vector<std::string> path;
            const SupportInfo support = checkSchemaRecord(record, path);
            out << indent(2) << "{\n";
            out << indent(3) << "\"kind\": " << quoteJson(record->kind) << ",\n";
            out << indent(3) << "\"name\": " << quoteJson(record->name) << ",\n";
            out << indent(3) << "\"aliases\": [";
            const auto aliases = namesForRecord(record);
            for (std::size_t j = 0; j < aliases.size(); ++j) {
                if (j) out << ", ";
                out << quoteJson(aliases[j]);
            }
            out << "],\n";
            out << indent(3) << "\"size\": " << record->size << ",\n";
            out << indent(3) << "\"align\": " << record->align << ",\n";
            out << indent(3) << "\"supported_schema\": " << (support.ok ? "true" : "false") << ",\n";
            out << indent(3) << "\"support_reason\": " << (support.reason.empty() ? std::string("null") : quoteJson(support.reason)) << ",\n";
            out << indent(3) << "\"fields\": [\n";
            for (std::size_t j = 0; j < record->fields.size(); ++j) {
                const FieldInfo &field = record->fields[j];
                out << indent(4) << "{\n";
                out << indent(5) << "\"name\": " << quoteJson(field.name) << ",\n";
                out << indent(5) << "\"offset_bits\": " << field.offsetBits << ",\n";
                out << indent(5) << "\"bit_field\": " << (field.isBitField ? "true" : "false") << ",\n";
                out << indent(5) << "\"c_type\": " << quoteJson(field.type.cSpelling) << ",\n";
                out << indent(5) << "\"urb_type\": " << emitJsonTypeSummary(field.type) << "\n";
                out << indent(4) << "}" << (j + 1 < record->fields.size() ? "," : "") << "\n";
            }
            out << indent(3) << "]\n";
            out << indent(2) << "}" << (i + 1 < records.size() ? "," : "") << "\n";
        }
        out << indent(1) << "],\n";

        const auto enums = exportedEnums();
        out << indent(1) << "\"enums\": [\n";
        for (std::size_t i = 0; i < enums.size(); ++i) {
            const auto &info = enums[i];
            out << indent(2) << "{\n";
            out << indent(3) << "\"name\": " << quoteJson(info->name) << ",\n";
            out << indent(3) << "\"aliases\": [";
            const auto aliases = namesForEnum(info);
            for (std::size_t j = 0; j < aliases.size(); ++j) {
                if (j) out << ", ";
                out << quoteJson(aliases[j]);
            }
            out << "],\n";
            out << indent(3) << "\"underlying\": " << quoteJson(info->underlyingUrb) << ",\n";
            out << indent(3) << "\"constants\": [\n";
            for (std::size_t j = 0; j < info->constants.size(); ++j) {
                const auto &constant = info->constants[j];
                out << indent(4) << "{ \"name\": " << quoteJson(constant.name)
                    << ", \"value\": " << quoteJson(constant.valueLiteral) << " }"
                    << (j + 1 < info->constants.size() ? "," : "") << "\n";
            }
            out << indent(3) << "]\n";
            out << indent(2) << "}" << (i + 1 < enums.size() ? "," : "") << "\n";
        }
        out << indent(1) << "],\n";

        const auto functions = exportedFunctions();
        out << indent(1) << "\"functions\": [\n";
        for (std::size_t i = 0; i < functions.size(); ++i) {
            const auto &fn = functions[i];
            const auto callable = classifyCallable(fn);
            out << indent(2) << "{\n";
            out << indent(3) << "\"name\": " << quoteJson(fn.name) << ",\n";
            out << indent(3) << "\"signature\": " << (fn.signature.empty() ? std::string("null") : quoteJson(fn.signature)) << ",\n";
            out << indent(3) << "\"supported\": " << (fn.supported ? "true" : "false") << ",\n";
            out << indent(3) << "\"wrapper_supported\": " << (callable ? "true" : "false") << ",\n";
            out << indent(3) << "\"wrapper_mode\": "
                << (callable ? quoteJson(callable->shimmed ? std::string("shim") : std::string("direct")) : std::string("null"))
                << ",\n";
            out << indent(3) << "\"wrapper_signature\": "
                << (callable ? quoteJson(callable->publicSignature) : std::string("null")) << ",\n";
            out << indent(3) << "\"reason\": " << (fn.reason.empty() ? std::string("null") : quoteJson(fn.reason)) << ",\n";
            out << indent(3) << "\"return_type\": " << emitJsonTypeSummary(fn.result) << ",\n";
            out << indent(3) << "\"parameters\": [\n";
            for (std::size_t j = 0; j < fn.params.size(); ++j) {
                const auto &param = fn.params[j];
                out << indent(4) << "{ \"name\": " << quoteJson(param.name)
                    << ", \"type\": " << emitJsonTypeSummary(param.type) << " }"
                    << (j + 1 < fn.params.size() ? "," : "") << "\n";
            }
            out << indent(3) << "]\n";
            out << indent(2) << "}" << (i + 1 < functions.size() ? "," : "") << "\n";
        }
        out << indent(1) << "]\n";
        out << "}\n";
        return out.str();
    }

    std::string emitJsonTypeSummary(const TypeInfo &type) const
    {
        std::ostringstream out;
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::Pointer:
        case TypeInfo::Kind::CString:
            out << "{ \"kind\": " << quoteJson(kindName(type.kind))
                << ", \"urb\": " << quoteJson(type.urbName)
                << ", \"c\": " << quoteJson(type.cSpelling);
            if (type.pointee) {
                out << ", \"pointee\": " << emitJsonTypeSummary(*type.pointee);
            }
            out << " }";
            break;
        case TypeInfo::Kind::Function:
            out << "{ \"kind\": \"function\", \"c\": " << quoteJson(type.cSpelling)
                << ", \"variadic\": " << (type.functionVariadic ? "true" : "false")
                << ", \"return\": " << (type.functionResult ? emitJsonTypeSummary(*type.functionResult) : std::string("null"))
                << ", \"args\": [";
            for (std::size_t i = 0; i < type.functionArgs.size(); ++i) {
                if (i) out << ", ";
                out << (type.functionArgs[i] ? emitJsonTypeSummary(*type.functionArgs[i]) : std::string("null"));
            }
            out << "] }";
            break;
        case TypeInfo::Kind::Enum:
            out << "{ \"kind\": \"enum\", \"urb\": " << quoteJson(type.urbName)
                << ", \"name\": " << quoteJson(type.enumInfo ? type.enumInfo->name : std::string())
                << ", \"c\": " << quoteJson(type.cSpelling) << " }";
            break;
        case TypeInfo::Kind::Array:
            out << "{ \"kind\": \"array\", \"count\": " << type.arrayCount
                << ", \"element\": " << (type.element ? emitJsonTypeSummary(*type.element) : std::string("null"))
                << ", \"c\": " << quoteJson(type.cSpelling) << " }";
            break;
        case TypeInfo::Kind::Record:
            out << "{ \"kind\": \"record\", \"name\": "
                << quoteJson(type.record ? type.record->name : std::string())
                << ", \"record_kind\": "
                << quoteJson(type.record ? type.record->kind : std::string())
                << ", \"c\": " << quoteJson(type.cSpelling) << " }";
            break;
        case TypeInfo::Kind::Unsupported:
            out << "{ \"kind\": \"unsupported\", \"reason\": " << quoteJson(type.reason)
                << ", \"c\": " << quoteJson(type.cSpelling) << " }";
            break;
        }
        return out.str();
    }

    static std::string kindName(TypeInfo::Kind kind)
    {
        switch (kind) {
        case TypeInfo::Kind::Primitive: return "primitive";
        case TypeInfo::Kind::Pointer: return "pointer";
        case TypeInfo::Kind::Function: return "function";
        case TypeInfo::Kind::CString: return "cstring";
        case TypeInfo::Kind::Array: return "array";
        case TypeInfo::Kind::Record: return "record";
        case TypeInfo::Kind::Enum: return "enum";
        case TypeInfo::Kind::Unsupported: return "unsupported";
        }
        return "unknown";
    }

    std::string preferredRecordName(const std::shared_ptr<RecordInfo> &record) const
    {
        if (!record) return {};
        const auto names = namesForRecord(record);
        if (!names.empty()) return names.front();
        if (!record->name.empty()) return record->name;
        return {};
    }

    std::string preferredEnumName(const std::shared_ptr<EnumInfo> &info) const
    {
        if (!info) return {};
        const auto names = namesForEnum(info);
        if (!names.empty()) return names.front();
        if (!info->name.empty()) return info->name;
        return {};
    }

    std::string preferredRecordName(const RecordInfo &record) const
    {
        auto it = recordsByKey_.find(record.key);
        if (it != recordsByKey_.end()) {
            return preferredRecordName(it->second);
        }
        if (!record.name.empty()) return record.name;
        return {};
    }

    std::string emitNodeAbiField(const FieldInfo &field, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        out << "{ name: " << quoteJs(field.name)
            << ", type: " << emitNodeAbiType(field.type, level, std::move(path)) << " }";
        return out.str();
    }

    std::string emitNodeAbiRecordType(const RecordInfo &record, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        const std::string ctor = record.kind == "union" ? "ffi.type.union" : "ffi.type.struct";
        out << ctor << "([";
        path.push_back(record.key);
        if (!record.fields.empty()) out << "\n";
        for (std::size_t i = 0; i < record.fields.size(); ++i) {
            out << indent(level + 1) << emitNodeAbiField(record.fields[i], level + 1, path);
            out << (i + 1 < record.fields.size() ? ",\n" : "\n");
        }
        out << indent(level) << "]";
        const std::string name = preferredRecordName(record);
        if (!name.empty()) {
            out << ", { name: " << quoteJs(name) << " }";
        }
        out << ")";
        return out.str();
    }

    std::string emitNodeAbiType(const TypeInfo &type, int level, std::vector<std::string> path) const
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
            if (type.urbName == "void") return "ffi.type.void()";
            return "ffi.type." + sanitizeIdentifier(type.urbName) + "()";
        case TypeInfo::Kind::CString:
            return "ffi.type.cstring()";
        case TypeInfo::Kind::Pointer:
            if (!type.pointee) return "ffi.type.pointer()";
            return "ffi.type.pointer(" + emitNodeAbiType(*type.pointee, level, std::move(path)) + ")";
        case TypeInfo::Kind::Function: {
            if (!type.functionResult) return "null";
            std::ostringstream out;
            out << "ffi.type.func(" << emitNodeAbiType(*type.functionResult, level, {}) << ", [";
            if (!type.functionArgs.empty()) out << "\n";
            for (std::size_t i = 0; i < type.functionArgs.size(); ++i) {
                out << indent(level + 1) << (type.functionArgs[i] ? emitNodeAbiType(*type.functionArgs[i], level + 1, {}) : std::string("ffi.type.pointer()"));
                out << (i + 1 < type.functionArgs.size() ? ",\n" : "\n");
            }
            out << indent(level) << "]";
            if (type.functionVariadic) out << ", { varargs: true }";
            out << ")";
            return out.str();
        }
        case TypeInfo::Kind::Enum: {
            const std::string enumName = preferredEnumName(type.enumInfo);
            if (enumName.empty()) return "ffi.type." + sanitizeIdentifier(type.urbName) + "()";
            return "ffi.type.enum(ENUMS[" + quoteJs(enumName) + "], { underlying: ffi.type." + sanitizeIdentifier(type.urbName) + "() })";
        }
        case TypeInfo::Kind::Array:
            if (!type.element) return "null";
            return "ffi.type.array(" + emitNodeAbiType(*type.element, level, std::move(path)) + ", " + std::to_string(type.arrayCount) + ")";
        case TypeInfo::Kind::Record:
            if (!type.record) return "null";
            return emitNodeAbiRecordType(*type.record, level, std::move(path));
        case TypeInfo::Kind::Unsupported:
            return "null";
        }
        return "null";
    }

    std::string emitNodeFunctionDescriptor(const FunctionInfo &fn, int level) const
    {
        std::ostringstream out;
        out << "ffi.type.func(" << emitNodeAbiType(fn.result, level, {}) << ", [";
        if (!fn.params.empty()) out << "\n";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            out << indent(level + 1) << emitNodeAbiType(fn.params[i].type, level + 1, {});
            out << (i + 1 < fn.params.size() ? ",\n" : "\n");
        }
        out << indent(level) << "], { name: " << quoteJs(fn.name);
        if (fn.variadic) out << ", varargs: true";
        out << " })";
        return out.str();
    }

    std::string emitLuaAbiField(const FieldInfo &field, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        out << "{ name = " << quoteLua(field.name)
            << ", type = " << emitLuaAbiType(field.type, level, std::move(path)) << " }";
        return out.str();
    }

    std::string emitLuaAbiRecordType(const RecordInfo &record, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        const std::string ctor = record.kind == "union" ? "ffi.type.union" : "ffi.type.struct";
        out << ctor << "({";
        path.push_back(record.key);
        if (!record.fields.empty()) out << "\n";
        for (std::size_t i = 0; i < record.fields.size(); ++i) {
            out << indent(level + 1) << emitLuaAbiField(record.fields[i], level + 1, path) << ',';
            out << "\n";
        }
        out << indent(level) << "}";
        const std::string name = preferredRecordName(record);
        if (!name.empty()) {
            out << ", { name = " << quoteLua(name) << " }";
        }
        out << ")";
        return out.str();
    }

    std::string emitLuaAbiType(const TypeInfo &type, int level, std::vector<std::string> path) const
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
            if (type.urbName == "void") return "ffi.type.void()";
            return "ffi.type." + sanitizeIdentifier(type.urbName) + "()";
        case TypeInfo::Kind::CString:
            return "ffi.type.cstring()";
        case TypeInfo::Kind::Pointer:
            if (!type.pointee) return "ffi.type.pointer()";
            return "ffi.type.pointer(" + emitLuaAbiType(*type.pointee, level, std::move(path)) + ")";
        case TypeInfo::Kind::Function: {
            if (!type.functionResult) return "nil";
            std::ostringstream out;
            out << "ffi.type.func(" << emitLuaAbiType(*type.functionResult, level, {}) << ", {";
            if (!type.functionArgs.empty()) out << "\n";
            for (std::size_t i = 0; i < type.functionArgs.size(); ++i) {
                out << indent(level + 1) << (type.functionArgs[i] ? emitLuaAbiType(*type.functionArgs[i], level + 1, {}) : std::string("ffi.type.pointer()")) << ',';
                out << "\n";
            }
            out << indent(level) << "}";
            if (type.functionVariadic) out << ", { varargs = true }";
            out << ")";
            return out.str();
        }
        case TypeInfo::Kind::Enum: {
            const std::string enumName = preferredEnumName(type.enumInfo);
            if (enumName.empty()) return "ffi.type." + sanitizeIdentifier(type.urbName) + "()";
            return "ffi.type.enum(ENUMS[" + quoteLua(enumName) + "], { underlying = ffi.type." + sanitizeIdentifier(type.urbName) + "() })";
        }
        case TypeInfo::Kind::Array:
            if (!type.element) return "nil";
            return "ffi.type.array(" + emitLuaAbiType(*type.element, level, std::move(path)) + ", " + std::to_string(type.arrayCount) + ")";
        case TypeInfo::Kind::Record:
            if (!type.record) return "nil";
            return emitLuaAbiRecordType(*type.record, level, std::move(path));
        case TypeInfo::Kind::Unsupported:
            return "nil";
        }
        return "nil";
    }

    std::string emitLuaFunctionDescriptor(const FunctionInfo &fn, int level) const
    {
        std::ostringstream out;
        out << "ffi.type.func(" << emitLuaAbiType(fn.result, level, {}) << ", {";
        if (!fn.params.empty()) out << "\n";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            out << indent(level + 1) << emitLuaAbiType(fn.params[i].type, level + 1, {}) << ',';
            out << "\n";
        }
        out << indent(level) << "}, { name = " << quoteLua(fn.name);
        if (fn.variadic) out << ", varargs = true";
        out << " })";
        return out.str();
    }

    std::string emitNode() const
    {
        const auto records = exportedRecords();
        const auto enums = exportedEnums();
        const auto functions = callableFunctions();
        const auto unsupportedFunctions = unsupportedWrapperFunctions();
        const bool needsShims = hasShimmedFunctions(functions);
        const bool needsRecordWrappers = needsRecordHelpers(functions);
        std::ostringstream out;
        out << "'use strict';\n\n";
        out << "// Generated by urb-bindgen.\n";
        out << "// Module: " << options_.moduleName << "\n\n";
        if (needsShims) {
            out << "const path = require('path');\n\n";
        }
        out << "const DEFAULT_LIBRARY = " << (options_.libraryPath.empty() ? "null" : quoteJs(options_.libraryPath)) << ";\n\n";
        if (needsShims) {
            out << "const DEFAULT_SHIM_LIBRARY = path.join(__dirname, " << quoteJs(shimLibraryFileName()) << ");\n\n";
        }

        out << "const ENUMS = Object.freeze({\n";
        for (std::size_t i = 0; i < enums.size(); ++i) {
            const auto &info = enums[i];
            const auto names = namesForEnum(info);
            for (std::size_t j = 0; j < names.size(); ++j) {
                out << indent(1) << quoteJs(names[j]) << ": Object.freeze({\n";
                for (std::size_t k = 0; k < info->constants.size(); ++k) {
                    const auto &constant = info->constants[k];
                    out << indent(2) << quoteJs(constant.name) << ": " << constant.valueLiteral
                        << (k + 1 < info->constants.size() ? "," : "") << "\n";
                }
                out << indent(1) << "})";
                const bool last = (i + 1 == enums.size() && j + 1 == names.size());
                out << (last ? "" : ",") << "\n";
            }
        }
        out << "});\n\n";

        out << "const RECORDS = Object.freeze({\n";
        bool emittedAnyRecord = false;
        for (const auto &record : records) {
            std::vector<std::string> path;
            const SupportInfo support = checkSchemaRecord(record, path);
            if (!support.ok) continue;
            const auto names = namesForRecord(record);
            for (const std::string &name : names) {
                emittedAnyRecord = true;
                out << indent(1) << quoteJs(name) << ": " << emitNodeRecord(*record, 1, {}) << ",\n";
            }
        }
        if (!emittedAnyRecord) {
            out << indent(1) << "// no schema-compatible records exported\n";
        }
        out << "});\n\n";

        out << "const SIGNATURES = Object.freeze({\n";
        bool emittedAnySignature = false;
        for (const auto &fn : functions) {
            emittedAnySignature = true;
            out << indent(1) << quoteJs(fn.fn.name) << ": " << quoteJs(fn.publicSignature) << ",\n";
        }
        if (!emittedAnySignature) {
            out << indent(1) << "// no supported functions exported\n";
        }
        out << "});\n\n";

        out << "const UNSUPPORTED_FUNCTIONS = Object.freeze({\n";
        bool emittedAnyUnsupported = false;
        for (const auto &fn : unsupportedFunctions) {
            emittedAnyUnsupported = true;
            out << indent(1) << quoteJs(fn.name) << ": " << quoteJs(fn.reason.empty() ? std::string("not wrapper-compatible") : fn.reason) << ",\n";
        }
        if (!emittedAnyUnsupported) {
            out << indent(1) << "// all discovered functions are wrapper-callable\n";
        }
        out << "});\n\n";

        if (needsRecordWrappers) {
            out << "function schemaHasField(schema, fieldName) {\n";
            out << indent(1) << "return !!schema && typeof schema === 'object' && !Array.isArray(schema) && Object.prototype.hasOwnProperty.call(schema, fieldName);\n";
            out << "}\n\n";
            out << "function isPointerLike(value, schema = null) {\n";
            out << indent(1) << "if (value == null) return false;\n";
            out << indent(1) << "if (typeof value === 'number' || typeof value === 'bigint' || Buffer.isBuffer(value)) return true;\n";
            out << indent(1) << "return typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, 'ptr') && !schemaHasField(schema, 'ptr');\n";
            out << "}\n\n";
            out << "function pointerValue(value) {\n";
            out << indent(1) << "return value && typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, 'ptr') ? value.ptr : value;\n";
            out << "}\n\n";
            out << "function isRecordSchema(desc) {\n";
            out << indent(1) << "return !!desc && typeof desc === 'object' && !Array.isArray(desc);\n";
            out << "}\n\n";
            out << "function fieldAddress(memory, basePtr, schema, fieldName) {\n";
            out << indent(1) << "return BigInt(basePtr) + BigInt(memory.struct_offsetof(schema, fieldName));\n";
            out << "}\n\n";
            out << "function copyRecord(memory, schema, ptr, value) {\n";
            out << indent(1) << "const size = BigInt(memory.struct_sizeof(schema));\n";
            out << indent(1) << "if (isPointerLike(value, schema)) {\n";
            out << indent(2) << "memory.copy(ptr, pointerValue(value), size);\n";
            out << indent(2) << "return;\n";
            out << indent(1) << "}\n";
            out << indent(1) << "memory.zero(ptr, size);\n";
            out << indent(1) << "const view = memory.view(ptr, schema);\n";
            out << indent(1) << "const source = value && typeof value === 'object' ? value : {};\n";
            out << indent(1) << "for (const [fieldName, desc] of Object.entries(schema)) {\n";
            out << indent(2) << "if (fieldName.startsWith('__')) continue;\n";
            out << indent(2) << "const fieldValue = source[fieldName];\n";
            out << indent(2) << "if (fieldValue === undefined) continue;\n";
            out << indent(2) << "const addr = fieldAddress(memory, ptr, schema, fieldName);\n";
            out << indent(2) << "if (Array.isArray(desc)) {\n";
            out << indent(3) << "memory.writeArray(addr, desc[0], fieldValue);\n";
            out << indent(2) << "} else if (isRecordSchema(desc)) {\n";
            out << indent(3) << "copyRecord(memory, desc, addr, fieldValue);\n";
            out << indent(2) << "} else {\n";
            out << indent(3) << "view[fieldName] = fieldValue;\n";
            out << indent(2) << "}\n";
            out << indent(1) << "}\n";
            out << "}\n\n";
            out << "function snapshotRecord(memory, schema, ptr) {\n";
            out << indent(1) << "const view = memory.view(ptr, schema);\n";
            out << indent(1) << "const out = {};\n";
            out << indent(1) << "if (schema.__union) out.__union = true;\n";
            out << indent(1) << "for (const [fieldName, desc] of Object.entries(schema)) {\n";
            out << indent(2) << "if (fieldName.startsWith('__')) continue;\n";
            out << indent(2) << "const addr = fieldAddress(memory, ptr, schema, fieldName);\n";
            out << indent(2) << "if (Array.isArray(desc)) {\n";
            out << indent(3) << "out[fieldName] = memory.readArray(addr, desc[0], desc[1]);\n";
            out << indent(2) << "} else if (isRecordSchema(desc)) {\n";
            out << indent(3) << "out[fieldName] = snapshotRecord(memory, desc, addr);\n";
            out << indent(2) << "} else {\n";
            out << indent(3) << "out[fieldName] = view[fieldName];\n";
            out << indent(2) << "}\n";
            out << indent(1) << "}\n";
            out << indent(1) << "return out;\n";
            out << "}\n\n";
            out << "function prepareRecordArg(memory, schema, value) {\n";
            out << indent(1) << "if (isPointerLike(value, schema)) return { ptr: pointerValue(value), owned: false };\n";
            out << indent(1) << "const ptr = memory.alloc(BigInt(memory.struct_sizeof(schema)));\n";
            out << indent(1) << "copyRecord(memory, schema, ptr, value);\n";
            out << indent(1) << "return { ptr, owned: true };\n";
            out << "}\n\n";
            out << "function freePreparedRecord(memory, prepared) {\n";
            out << indent(1) << "if (prepared && prepared.owned) memory.free(prepared.ptr);\n";
            out << "}\n\n";
        }

        out << "function openRequired(ffi, libraryPath, label) {\n";
        out << indent(1) << "const handle = ffi.open(libraryPath, ffi.flags.NOW | ffi.flags.LOCAL);\n";
        out << indent(1) << "if (handle == null) {\n";
        out << indent(2) << "const reason = ffi.dlerror?.() || 'unknown dynamic loader error';\n";
        out << indent(2) << "throw new Error(`failed to open ${label} ${libraryPath}: ${reason}`);\n";
        out << indent(1) << "}\n";
        out << indent(1) << "return handle;\n";
        out << "}\n\n";

        out << "function bindRequired(ffi, handle, symbol, signature, label) {\n";
        out << indent(1) << "const ptr = ffi.sym(handle, symbol);\n";
        out << indent(1) << "if (ptr == null || ptr === 0 || ptr === 0n) {\n";
        out << indent(2) << "const reason = ffi.dlerror?.() || 'symbol not found';\n";
        out << indent(2) << "throw new Error(`failed to resolve ${label} symbol ${symbol}: ${reason}`);\n";
        out << indent(1) << "}\n";
        out << indent(1) << "return ffi.bind(ptr, signature);\n";
        out << "}\n\n";

        out << "function load(runtime = require('urb-ffi'), libraryPath = DEFAULT_LIBRARY";
        if (needsShims) out << ", shimLibraryPath = DEFAULT_SHIM_LIBRARY";
        out << ") {\n";
        out << indent(1) << "const { ffi, memory } = runtime;\n";
        out << indent(1) << "const chosenLibrary = libraryPath ?? DEFAULT_LIBRARY;\n";
        out << indent(1) << "if (!chosenLibrary) throw new Error('libraryPath is required');\n";
        out << indent(1) << "const handle = openRequired(ffi, chosenLibrary, 'target library');\n";
        out << indent(1) << "let closed = false;\n";
        if (needsShims) {
            out << indent(1) << "let shimHandle = null;\n";
            out << indent(1) << "const shimBindings = Object.create(null);\n";
            out << indent(1) << "const getShimBinding = (symbol, signature) => {\n";
            out << indent(2) << "if (!shimLibraryPath) throw new Error(`shim library is required for ${symbol}`);\n";
            out << indent(2) << "if (!shimHandle) shimHandle = openRequired(ffi, shimLibraryPath, 'shim library');\n";
            out << indent(2) << "if (!shimBindings[symbol]) shimBindings[symbol] = bindRequired(ffi, shimHandle, symbol, signature, 'shim');\n";
            out << indent(2) << "return shimBindings[symbol];\n";
            out << indent(1) << "};\n";
        }
        out << indent(1) << "const functions = {\n";
        bool emittedFunction = false;
        for (const auto &fn : functions) {
            emittedFunction = true;
            if (fn.direct && !fn.richDirect) {
                out << indent(2) << quoteJs(fn.fn.name) << ": bindRequired(ffi, handle, " << quoteJs(fn.bindSymbol)
                    << ", " << quoteJs(fn.bindSignature) << ", 'target'),\n";
                continue;
            }

            if (fn.direct && fn.richDirect) {
                out << indent(2) << quoteJs(fn.fn.name) << ": (() => {\n";
                out << indent(3) << "const __fn = bindRequired(ffi, handle, " << quoteJs(fn.bindSymbol)
                    << ", " << emitNodeFunctionDescriptor(fn.fn, 3) << ", 'target');\n";
                out << indent(3) << "return (";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (i) out << ", ";
                    out << "arg" << i;
                }
                out << ") => {\n";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (!fn.paramByValueRecord[i]) continue;
                    out << indent(4) << "const __arg" << i << " = prepareRecordArg(memory, RECORDS[" << quoteJs(wrapperRecordKey(fn.fn.params[i].type)) << "], arg" << i << ");\n";
                }
                out << indent(4) << "try {\n";
                out << indent(5) << "const __result = __fn(";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (i) out << ", ";
                    out << (fn.paramByValueRecord[i] ? ("__arg" + std::to_string(i) + ".ptr") : ("arg" + std::to_string(i)));
                }
                out << ");\n";
                if (fn.returnsByValueRecord) {
                    out << indent(5) << "return snapshotRecord(memory, RECORDS[" << quoteJs(wrapperRecordKey(fn.fn.result)) << "], __result);\n";
                } else {
                    out << indent(5) << "return __result;\n";
                }
                out << indent(4) << "} finally {\n";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (!fn.paramByValueRecord[i]) continue;
                    out << indent(5) << "freePreparedRecord(memory, __arg" << i << ");\n";
                }
                out << indent(4) << "}\n";
                out << indent(3) << "};\n";
                out << indent(2) << "})(),\n";
                continue;
            }

            out << indent(2) << quoteJs(fn.fn.name) << ": (";
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (i) out << ", ";
                out << "arg" << i;
            }
            out << ") => {\n";
            out << indent(3) << "const __fn = getShimBinding(" << quoteJs(fn.bindSymbol) << ", " << quoteJs(fn.bindSignature) << ");\n";
            if (fn.returnsViaOutPointer) {
                out << indent(3) << "const __retPtr = memory.alloc(BigInt(memory.struct_sizeof(RECORDS[" << quoteJs(wrapperRecordKey(fn.fn.result)) << "])));\n";
            }
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (!fn.paramByValueRecord[i]) continue;
                out << indent(3) << "const __arg" << i << " = prepareRecordArg(memory, RECORDS[" << quoteJs(wrapperRecordKey(fn.fn.params[i].type)) << "], arg" << i << ");\n";
            }
            out << indent(3) << "try {\n";
            out << indent(4);
            if (fn.returnsViaOutPointer) {
                out << "__fn(__retPtr";
            } else {
                out << "return __fn(";
            }
            bool firstArg = fn.returnsViaOutPointer;
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (firstArg) out << ", ";
                out << (fn.paramByValueRecord[i] ? ("__arg" + std::to_string(i) + ".ptr") : ("arg" + std::to_string(i)));
                firstArg = true;
            }
            out << ");\n";
            if (fn.returnsViaOutPointer) {
                out << indent(4) << "return snapshotRecord(memory, RECORDS[" << quoteJs(wrapperRecordKey(fn.fn.result)) << "], __retPtr);\n";
            }
            out << indent(3) << "} finally {\n";
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (!fn.paramByValueRecord[i]) continue;
                out << indent(4) << "freePreparedRecord(memory, __arg" << i << ");\n";
            }
            if (fn.returnsViaOutPointer) {
                out << indent(4) << "memory.free(__retPtr);\n";
            }
            out << indent(3) << "}\n";
            out << indent(2) << "},\n";
        }
        if (!emittedFunction) {
            out << indent(2) << "// no supported functions exported\n";
        }
        out << indent(1) << "};\n";
        out << indent(1) << "return {\n";
        out << indent(2) << "ffi,\n";
        out << indent(2) << "memory,\n";
        out << indent(2) << "handle,\n";
        out << indent(2) << "enums: ENUMS,\n";
        out << indent(2) << "records: RECORDS,\n";
        out << indent(2) << "signatures: SIGNATURES,\n";
        out << indent(2) << "unsupportedFunctions: UNSUPPORTED_FUNCTIONS,\n";
        out << indent(2) << "functions,\n";
        out << indent(2) << "close() {\n";
        if (needsShims) {
            out << indent(3) << "if (shimHandle) { ffi.close(shimHandle); shimHandle = null; }\n";
        }
        out << indent(3) << "if (!closed) { ffi.close(handle); closed = true; }\n";
        out << indent(2) << "},\n";
        out << indent(1) << "};\n";
        out << "}\n\n";

        out << "module.exports = {\n";
        out << indent(1) << "moduleName: " << quoteJs(options_.moduleName) << ",\n";
        out << indent(1) << "defaultLibrary: DEFAULT_LIBRARY,\n";
        out << indent(1) << "enums: ENUMS,\n";
        out << indent(1) << "records: RECORDS,\n";
        out << indent(1) << "signatures: SIGNATURES,\n";
        out << indent(1) << "unsupportedFunctions: UNSUPPORTED_FUNCTIONS,\n";
        out << indent(1) << "load,\n";
        out << "};\n";
        return out.str();
    }

    std::string emitLua() const
    {
        const auto records = exportedRecords();
        const auto enums = exportedEnums();
        const auto functions = callableFunctions();
        const auto unsupportedFunctions = unsupportedWrapperFunctions();
        const bool needsShims = hasShimmedFunctions(functions);
        const bool needsRecordWrappers = needsRecordHelpers(functions);
        std::ostringstream out;
        out << "-- Generated by urb-bindgen.\n";
        out << "-- Module: " << options_.moduleName << "\n\n";
        out << "local DEFAULT_LIBRARY = " << (options_.libraryPath.empty() ? "nil" : quoteLua(options_.libraryPath)) << "\n\n";
        if (needsShims) {
            out << "local function urb_bindgen_dir()\n";
            out << indent(1) << "local source = debug.getinfo(1, 'S').source\n";
            out << indent(1) << "if source:sub(1, 1) == '@' then\n";
            out << indent(2) << "return source:match('^@(.+)/[^/]+$') or '.'\n";
            out << indent(1) << "end\n";
            out << indent(1) << "return '.'\n";
            out << "end\n\n";
            out << "local DEFAULT_SHIM_LIBRARY = urb_bindgen_dir() .. '/' .. " << quoteLua(shimLibraryFileName()) << "\n\n";
        }

        out << "local ENUMS = {\n";
        for (const auto &info : enums) {
            const auto names = namesForEnum(info);
            for (const std::string &name : names) {
                out << indent(1) << "[" << quoteLua(name) << "] = {\n";
                for (const auto &constant : info->constants) {
                    out << indent(2) << "[" << quoteLua(constant.name) << "] = " << constant.valueLiteral << ",\n";
                }
                out << indent(1) << "},\n";
            }
        }
        out << "}\n\n";

        out << "local RECORDS = {\n";
        bool emittedAnyRecord = false;
        for (const auto &record : records) {
            std::vector<std::string> path;
            const SupportInfo support = checkSchemaRecord(record, path);
            if (!support.ok) continue;
            const auto names = namesForRecord(record);
            for (const std::string &name : names) {
                emittedAnyRecord = true;
                out << indent(1) << "[" << quoteLua(name) << "] = " << emitLuaRecord(*record, 1, {}) << ",\n";
            }
        }
        if (!emittedAnyRecord) {
            out << indent(1) << "-- no schema-compatible records exported\n";
        }
        out << "}\n\n";

        out << "local SIGNATURES = {\n";
        bool emittedAnySignature = false;
        for (const auto &fn : functions) {
            emittedAnySignature = true;
            out << indent(1) << "[" << quoteLua(fn.fn.name) << "] = " << quoteLua(fn.publicSignature) << ",\n";
        }
        if (!emittedAnySignature) {
            out << indent(1) << "-- no supported functions exported\n";
        }
        out << "}\n\n";

        out << "local UNSUPPORTED_FUNCTIONS = {\n";
        bool emittedAnyUnsupported = false;
        for (const auto &fn : unsupportedFunctions) {
            emittedAnyUnsupported = true;
            out << indent(1) << "[" << quoteLua(fn.name) << "] = " << quoteLua(fn.reason.empty() ? std::string("not wrapper-compatible") : fn.reason) << ",\n";
        }
        if (!emittedAnyUnsupported) {
            out << indent(1) << "-- all discovered functions are wrapper-callable\n";
        }
        out << "}\n\n";

        if (needsRecordWrappers) {
            out << "local function schema_has_field(schema, field_name)\n";
            out << indent(1) << "for _, field in ipairs(schema or {}) do\n";
            out << indent(2) << "if field.name == field_name then return true end\n";
            out << indent(1) << "end\n";
            out << indent(1) << "return false\n";
            out << "end\n\n";
            out << "local function is_pointer_like(value, schema)\n";
            out << indent(1) << "return type(value) == 'number' or (type(value) == 'table' and value.ptr ~= nil and not schema_has_field(schema, 'ptr'))\n";
            out << "end\n\n";
            out << "local function pointer_value(value)\n";
            out << indent(1) << "return type(value) == 'table' and value.ptr ~= nil and value.ptr or value\n";
            out << "end\n\n";
            out << "local function field_address(memory, base_ptr, schema, field_name)\n";
            out << indent(1) << "return base_ptr + memory.struct_offsetof(schema, field_name)\n";
            out << "end\n\n";
            out << "local function copy_record(memory, schema, ptr, value)\n";
            out << indent(1) << "local size = memory.struct_sizeof(schema)\n";
            out << indent(1) << "if is_pointer_like(value, schema) then\n";
            out << indent(2) << "memory.copy(ptr, pointer_value(value), size)\n";
            out << indent(2) << "return\n";
            out << indent(1) << "end\n";
            out << indent(1) << "memory.zero(ptr, size)\n";
            out << indent(1) << "local view = memory.view(ptr, schema)\n";
            out << indent(1) << "local source = type(value) == 'table' and value or {}\n";
            out << indent(1) << "for _, field in ipairs(schema) do\n";
            out << indent(2) << "local field_value = source[field.name]\n";
            out << indent(2) << "if field_value ~= nil then\n";
            out << indent(3) << "local addr = field_address(memory, ptr, schema, field.name)\n";
            out << indent(3) << "if field.schema then\n";
            out << indent(4) << "copy_record(memory, field.schema, addr, field_value)\n";
            out << indent(3) << "elseif field.count then\n";
            out << indent(4) << "memory.write_array(addr, field.type, field_value)\n";
            out << indent(3) << "else\n";
            out << indent(4) << "view[field.name] = field_value\n";
            out << indent(3) << "end\n";
            out << indent(2) << "end\n";
            out << indent(1) << "end\n";
            out << "end\n\n";
            out << "local function snapshot_record(memory, schema, ptr)\n";
            out << indent(1) << "local view = memory.view(ptr, schema)\n";
            out << indent(1) << "local out = {}\n";
            out << indent(1) << "if schema.__union then out.__union = true end\n";
            out << indent(1) << "for _, field in ipairs(schema) do\n";
            out << indent(2) << "local addr = field_address(memory, ptr, schema, field.name)\n";
            out << indent(2) << "if field.schema then\n";
            out << indent(3) << "out[field.name] = snapshot_record(memory, field.schema, addr)\n";
            out << indent(2) << "elseif field.count then\n";
            out << indent(3) << "out[field.name] = memory.read_array(addr, field.type, field.count)\n";
            out << indent(2) << "else\n";
            out << indent(3) << "out[field.name] = view[field.name]\n";
            out << indent(2) << "end\n";
            out << indent(1) << "end\n";
            out << indent(1) << "return out\n";
            out << "end\n\n";
            out << "local function prepare_record_arg(memory, schema, value)\n";
            out << indent(1) << "if is_pointer_like(value, schema) then return { ptr = pointer_value(value), owned = false } end\n";
            out << indent(1) << "local ptr = memory.alloc(memory.struct_sizeof(schema))\n";
            out << indent(1) << "copy_record(memory, schema, ptr, value)\n";
            out << indent(1) << "return { ptr = ptr, owned = true }\n";
            out << "end\n\n";
            out << "local function free_prepared_record(memory, prepared)\n";
            out << indent(1) << "if prepared and prepared.owned then memory.free(prepared.ptr) end\n";
            out << "end\n\n";
        }

        out << "local function open_required(ffi, library_path, label)\n";
        out << indent(1) << "local handle = ffi.open(library_path, ffi.flags.NOW + ffi.flags.LOCAL)\n";
        out << indent(1) << "if handle == nil then\n";
        out << indent(2) << "local reason = ffi.dlerror and ffi.dlerror() or 'unknown dynamic loader error'\n";
        out << indent(2) << "error(string.format('failed to open %s %s: %s', label, library_path, reason), 0)\n";
        out << indent(1) << "end\n";
        out << indent(1) << "return handle\n";
        out << "end\n\n";

        out << "local function bind_required(ffi, handle, symbol, signature, label)\n";
        out << indent(1) << "local ptr = ffi.sym(handle, symbol)\n";
        out << indent(1) << "if ptr == nil or ptr == 0 then\n";
        out << indent(2) << "local reason = ffi.dlerror and ffi.dlerror() or 'symbol not found'\n";
        out << indent(2) << "error(string.format('failed to resolve %s symbol %s: %s', label, symbol, reason), 0)\n";
        out << indent(1) << "end\n";
        out << indent(1) << "return ffi.bind(ptr, signature)\n";
        out << "end\n\n";

        out << "local function load(urb, library_path";
        if (needsShims) out << ", shim_library_path";
        out << ")\n";
        out << indent(1) << "urb = urb or require('urb_ffi')\n";
        out << indent(1) << "local ffi, memory = urb.ffi, urb.memory\n";
        out << indent(1) << "local chosen_library = library_path or DEFAULT_LIBRARY\n";
        out << indent(1) << "assert(chosen_library, 'library_path is required')\n";
        out << indent(1) << "local handle = open_required(ffi, chosen_library, 'target library')\n";
        out << indent(1) << "local closed = false\n";
        if (needsShims) {
            out << indent(1) << "local chosen_shim_library = shim_library_path or DEFAULT_SHIM_LIBRARY\n";
            out << indent(1) << "local shim_handle = nil\n";
            out << indent(1) << "local shim_bindings = {}\n";
            out << indent(1) << "local function get_shim_binding(symbol, signature)\n";
            out << indent(2) << "assert(chosen_shim_library, 'shim_library_path is required')\n";
            out << indent(2) << "if not shim_handle then shim_handle = open_required(ffi, chosen_shim_library, 'shim library') end\n";
            out << indent(2) << "if not shim_bindings[symbol] then shim_bindings[symbol] = bind_required(ffi, shim_handle, symbol, signature, 'shim') end\n";
            out << indent(2) << "return shim_bindings[symbol]\n";
            out << indent(1) << "end\n";
        }
        out << indent(1) << "local functions = {\n";
        bool emittedFunction = false;
        for (const auto &fn : functions) {
            emittedFunction = true;
            if (fn.direct && !fn.richDirect) {
                out << indent(2) << "[" << quoteLua(fn.fn.name) << "] = bind_required(ffi, handle, " << quoteLua(fn.bindSymbol)
                    << ", " << quoteLua(fn.bindSignature) << ", 'target'),\n";
                continue;
            }

            if (fn.direct && fn.richDirect) {
                out << indent(2) << "[" << quoteLua(fn.fn.name) << "] = (function()\n";
                out << indent(3) << "local __fn = bind_required(ffi, handle, " << quoteLua(fn.bindSymbol)
                    << ", " << emitLuaFunctionDescriptor(fn.fn, 3) << ", 'target')\n";
                out << indent(3) << "return function(";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (i) out << ", ";
                    out << "arg" << i;
                }
                out << ")\n";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (!fn.paramByValueRecord[i]) continue;
                    out << indent(4) << "local __arg" << i << " = prepare_record_arg(memory, RECORDS[" << quoteLua(wrapperRecordKey(fn.fn.params[i].type)) << "], arg" << i << ")\n";
                }
                out << indent(4) << "local ok, result = pcall(function()\n";
                out << indent(5) << "local __result = __fn(";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (i) out << ", ";
                    out << (fn.paramByValueRecord[i] ? ("__arg" + std::to_string(i) + ".ptr") : ("arg" + std::to_string(i)));
                }
                out << ")\n";
                if (fn.returnsByValueRecord) {
                    out << indent(5) << "return snapshot_record(memory, RECORDS[" << quoteLua(wrapperRecordKey(fn.fn.result)) << "], __result)\n";
                } else {
                    out << indent(5) << "return __result\n";
                }
                out << indent(4) << "end)\n";
                for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                    if (!fn.paramByValueRecord[i]) continue;
                    out << indent(4) << "free_prepared_record(memory, __arg" << i << ")\n";
                }
                out << indent(4) << "if not ok then error(result, 0) end\n";
                out << indent(4) << "return result\n";
                out << indent(3) << "end\n";
                out << indent(2) << "end)(),\n";
                continue;
            }

            out << indent(2) << "[" << quoteLua(fn.fn.name) << "] = function(";
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (i) out << ", ";
                out << "arg" << i;
            }
            out << ")\n";
            out << indent(3) << "local __fn = get_shim_binding(" << quoteLua(fn.bindSymbol) << ", " << quoteLua(fn.bindSignature) << ")\n";
            if (fn.returnsViaOutPointer) {
                out << indent(3) << "local __ret_ptr = memory.alloc(memory.struct_sizeof(RECORDS[" << quoteLua(wrapperRecordKey(fn.fn.result)) << "]))\n";
            }
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (!fn.paramByValueRecord[i]) continue;
                out << indent(3) << "local __arg" << i << " = prepare_record_arg(memory, RECORDS[" << quoteLua(wrapperRecordKey(fn.fn.params[i].type)) << "], arg" << i << ")\n";
            }
            out << indent(3) << "local ok, result = pcall(function()\n";
            out << indent(4);
            if (fn.returnsViaOutPointer) {
                out << "__fn(__ret_ptr";
            } else {
                out << "return __fn(";
            }
            bool firstArg = fn.returnsViaOutPointer;
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (firstArg) out << ", ";
                out << (fn.paramByValueRecord[i] ? ("__arg" + std::to_string(i) + ".ptr") : ("arg" + std::to_string(i)));
                firstArg = true;
            }
            out << ")\n";
            if (fn.returnsViaOutPointer) {
                out << indent(4) << "return snapshot_record(memory, RECORDS[" << quoteLua(wrapperRecordKey(fn.fn.result)) << "], __ret_ptr)\n";
            }
            out << indent(3) << "end)\n";
            for (std::size_t i = 0; i < fn.fn.params.size(); ++i) {
                if (!fn.paramByValueRecord[i]) continue;
                out << indent(3) << "free_prepared_record(memory, __arg" << i << ")\n";
            }
            if (fn.returnsViaOutPointer) {
                out << indent(3) << "memory.free(__ret_ptr)\n";
            }
            out << indent(3) << "if not ok then error(result, 0) end\n";
            out << indent(3) << "return result\n";
            out << indent(2) << "end,\n";
        }
        if (!emittedFunction) {
            out << indent(2) << "-- no supported functions exported\n";
        }
        out << indent(1) << "}\n";
        out << indent(1) << "return {\n";
        out << indent(2) << "ffi = ffi,\n";
        out << indent(2) << "memory = memory,\n";
        out << indent(2) << "handle = handle,\n";
        out << indent(2) << "enums = ENUMS,\n";
        out << indent(2) << "records = RECORDS,\n";
        out << indent(2) << "signatures = SIGNATURES,\n";
        out << indent(2) << "unsupported_functions = UNSUPPORTED_FUNCTIONS,\n";
        out << indent(2) << "functions = functions,\n";
        out << indent(2) << "close = function()\n";
        if (needsShims) {
            out << indent(3) << "if shim_handle then ffi.close(shim_handle); shim_handle = nil end\n";
        }
        out << indent(3) << "if not closed then ffi.close(handle); closed = true end\n";
        out << indent(2) << "end,\n";
        out << indent(1) << "}\n";
        out << "end\n\n";

        out << "return {\n";
        out << indent(1) << "module_name = " << quoteLua(options_.moduleName) << ",\n";
        out << indent(1) << "default_library = DEFAULT_LIBRARY,\n";
        out << indent(1) << "enums = ENUMS,\n";
        out << indent(1) << "records = RECORDS,\n";
        out << indent(1) << "signatures = SIGNATURES,\n";
        out << indent(1) << "unsupported_functions = UNSUPPORTED_FUNCTIONS,\n";
        out << indent(1) << "load = load,\n";
        out << "}\n";
        return out.str();
    }

    std::string emitNodeRecord(const RecordInfo &record, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        out << "{";
        bool needComma = false;
        if (record.kind == "union") {
            out << "\n" << indent(level + 1) << quoteJs("__union") << ": true";
            needComma = true;
        }
        path.push_back(record.key);
        for (const FieldInfo &field : record.fields) {
            if (needComma) out << ',';
            out << "\n" << indent(level + 1) << quoteJs(field.name) << ": "
                << emitNodeFieldType(field.type, level + 1, path);
            needComma = true;
        }
        if (needComma) out << "\n" << indent(level);
        out << "}";
        return out.str();
    }

    std::string emitNodeFieldType(const TypeInfo &type, int level, std::vector<std::string> path) const
    {
        switch (type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::CString:
        case TypeInfo::Kind::Enum:
            return quoteJs(type.urbName);
        case TypeInfo::Kind::Pointer:
            if (type.pointee && type.pointee->kind == TypeInfo::Kind::Record && type.pointee->record) {
                return "{ type: " + quoteJs("pointer") + ", to: " + emitNodeRecord(*type.pointee->record, level, std::move(path)) + " }";
            }
            return quoteJs(type.urbName);
        case TypeInfo::Kind::Function:
            return quoteJs("pointer");
        case TypeInfo::Kind::Array:
            return "[" + quoteJs(type.element ? type.element->urbName : std::string("u8")) + ", " + std::to_string(type.arrayCount) + "]";
        case TypeInfo::Kind::Record:
            return emitNodeRecord(*type.record, level, std::move(path));
        case TypeInfo::Kind::Unsupported:
            return "null";
        }
        return "null";
    }

    std::string emitLuaRecord(const RecordInfo &record, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        out << "{";
        if (record.kind == "union") {
            out << "\n" << indent(level + 1) << "__union = true,";
        }
        path.push_back(record.key);
        for (const FieldInfo &field : record.fields) {
            out << "\n" << indent(level + 1) << emitLuaField(field, level + 1, path) << ',';
        }
        if (record.kind == "union" || !record.fields.empty()) {
            out << "\n" << indent(level);
        }
        out << "}";
        return out.str();
    }

    std::string emitLuaField(const FieldInfo &field, int level, std::vector<std::string> path) const
    {
        std::ostringstream out;
        out << "{ name = " << quoteLua(field.name);
        switch (field.type.kind) {
        case TypeInfo::Kind::Primitive:
        case TypeInfo::Kind::CString:
        case TypeInfo::Kind::Enum:
            out << ", type = " << quoteLua(field.type.urbName);
            break;
        case TypeInfo::Kind::Pointer:
            if (field.type.pointee && field.type.pointee->kind == TypeInfo::Kind::Record && field.type.pointee->record) {
                out << ", type = " << quoteLua("pointer")
                    << ", to = " << emitLuaRecord(*field.type.pointee->record, level, std::move(path));
            } else {
                out << ", pointer = true";
            }
            break;
        case TypeInfo::Kind::Function:
            out << ", pointer = true";
            break;
        case TypeInfo::Kind::Array:
            out << ", type = " << quoteLua(field.type.element ? field.type.element->urbName : std::string("u8"))
                << ", count = " << field.type.arrayCount;
            break;
        case TypeInfo::Kind::Record:
            out << ", schema = " << emitLuaRecord(*field.type.record, level, std::move(path));
            break;
        case TypeInfo::Kind::Unsupported:
            out << ", unsupported = " << quoteLua(field.type.reason);
            break;
        }
        out << " }";
        return out.str();
    }
};

void printUsage(const char *argv0)
{
    std::cerr
        << "usage: " << argv0 << " --header <file.h> [--header ...] --emit <json|node|lua> [options]\n"
        << "\n"
        << "options:\n"
        << "  --output <path>         output file (default: stdout)\n"
        << "  --module <name>         logical module name (default: urb_bindings)\n"
        << "  --library <path>        default library path/name for node/lua emitters\n"
        << "  --include <dir>         extra include directory\n"
        << "  --define <NAME[=VAL]>   preprocessor define\n"
        << "  --clang-arg <arg>       raw extra clang argument\n"
        << "  --no-build-shim         only write .shim.c, do not compile the shared library\n"
        << "  --verbose               print extra progress info\n"
        << "\n"
        << "examples:\n"
        << "  urb-bindgen --header /usr/include/stdio.h --library libc.so.6 --emit json\n"
        << "  urb-bindgen --header ./foo.h --emit node --output foo.js --library ./libfoo.so\n";
}

bool parseArgs(int argc, char **argv, Options &options)
{
    auto needValue = [&](int &i, const char *flag) -> std::string {
        if (i + 1 >= argc) {
            throw std::runtime_error(std::string(flag) + " expects a value");
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--header") {
            options.headers.push_back(normalizePath(needValue(i, "--header")));
        } else if (arg == "--output") {
            options.outputPath = needValue(i, "--output");
        } else if (arg == "--module") {
            options.moduleName = needValue(i, "--module");
        } else if (arg == "--library") {
            options.libraryPath = needValue(i, "--library");
        } else if (arg == "--include") {
            options.includeDirs.push_back(normalizePath(needValue(i, "--include")));
        } else if (arg == "--define") {
            options.defines.push_back(needValue(i, "--define"));
        } else if (arg == "--clang-arg") {
            options.clangArgs.push_back(needValue(i, "--clang-arg"));
        } else if (arg == "--no-build-shim") {
            options.buildShim = false;
        } else if (arg == "--emit") {
            const std::string mode = needValue(i, "--emit");
            if (mode == "json") options.emit = EmitMode::Json;
            else if (mode == "node") options.emit = EmitMode::Node;
            else if (mode == "lua") options.emit = EmitMode::Lua;
            else throw std::runtime_error("unknown emit mode: " + mode);
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (options.headers.empty()) {
        throw std::runtime_error("at least one --header is required");
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    try {
        if (!parseArgs(argc, argv, options)) {
            printUsage(argv[0]);
            return 0;
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n\n";
        printUsage(argv[0]);
        return 1;
    }

    try {
        BindgenApp app(std::move(options));
        return app.run() ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
