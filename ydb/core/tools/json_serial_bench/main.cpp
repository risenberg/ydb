#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>
#include <library/cpp/json/yson/json2yson.h>
#include <yql/essentials/types/binary_json/read.h>
#include <yql/essentials/types/binary_json/write.h>

#include <util/datetime/cputimer.h>
#include <util/generic/buffer.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/stream/file.h>
#include <util/stream/str.h>
#include <util/string/split.h>

#include <util/system/byteorder.h>

#include <cstdint>
#include <cstring>

namespace {

// Big-endian encoding helpers for MessagePack.
inline void WriteBE16(TBuffer& buf, uint16_t v) {
    char b[2];
    b[0] = static_cast<char>((v >> 8) & 0xff);
    b[1] = static_cast<char>(v & 0xff);
    buf.Append(b, 2);
}

inline void WriteBE32(TBuffer& buf, uint32_t v) {
    char b[4];
    b[0] = static_cast<char>((v >> 24) & 0xff);
    b[1] = static_cast<char>((v >> 16) & 0xff);
    b[2] = static_cast<char>((v >> 8) & 0xff);
    b[3] = static_cast<char>(v & 0xff);
    buf.Append(b, 4);
}

inline void WriteBE64(TBuffer& buf, uint64_t v) {
    char b[8];
    for (int i = 7; i >= 0; --i) {
        b[7 - i] = static_cast<char>((v >> (i * 8)) & 0xff);
    }
    buf.Append(b, 8);
}

}  // namespace

// ============================================================================
// Hand-rolled BSON serializer (subset: string, int64, double, bool, null,
// embedded document, array).
// Spec: https://bsonspec.org/spec.html
// ============================================================================
namespace NBson {

static void AppendCString(TBuffer& buf, TStringBuf s) {
    buf.Append(s.data(), s.size());
    buf.Append('\0');
}

static void AppendInt32(TBuffer& buf, int32_t v) {
    buf.Append(reinterpret_cast<const char*>(&v), 4);
}

static void AppendInt64(TBuffer& buf, int64_t v) {
    buf.Append(reinterpret_cast<const char*>(&v), 8);
}

static void AppendDouble(TBuffer& buf, double v) {
    buf.Append(reinterpret_cast<const char*>(&v), 8);
}

static void SerializeValue(TBuffer& buf, TStringBuf key, const NJson::TJsonValue& val);

static void SerializeDocument(TBuffer& buf, const NJson::TJsonValue& val) {
    // Reserve 4 bytes for document size.
    size_t sizePos = buf.Size();
    int32_t placeholder = 0;
    buf.Append(reinterpret_cast<const char*>(&placeholder), 4);

    if (val.IsMap()) {
        for (const auto& [k, v] : val.GetMap()) {
            SerializeValue(buf, k, v);
        }
    }

    buf.Append('\0');  // Terminator.

    // Patch size.
    int32_t docSize = static_cast<int32_t>(buf.Size() - sizePos);
    std::memcpy(buf.Data() + sizePos, &docSize, 4);
}

static void SerializeArray(TBuffer& buf, const NJson::TJsonValue& val) {
    size_t sizePos = buf.Size();
    int32_t placeholder = 0;
    buf.Append(reinterpret_cast<const char*>(&placeholder), 4);

    if (val.IsArray()) {
        for (size_t i = 0; i < val.GetArray().size(); ++i) {
            char idxBuf[16];
            snprintf(idxBuf, sizeof(idxBuf), "%zu", i);
            SerializeValue(buf, TStringBuf(idxBuf), val.GetArray()[i]);
        }
    }

    buf.Append('\0');

    int32_t docSize = static_cast<int32_t>(buf.Size() - sizePos);
    std::memcpy(buf.Data() + sizePos, &docSize, 4);
}

static void SerializeValue(TBuffer& buf, TStringBuf key, const NJson::TJsonValue& val) {
    switch (val.GetType()) {
        case NJson::JSON_DOUBLE: {
            buf.Append('\x01');  // double
            AppendCString(buf, key);
            AppendDouble(buf, val.GetDouble());
            break;
        }
        case NJson::JSON_STRING: {
            buf.Append('\x02');  // string
            AppendCString(buf, key);
            TStringBuf s = val.GetString();
            AppendInt32(buf, static_cast<int32_t>(s.size() + 1));
            buf.Append(s.data(), s.size());
            buf.Append('\0');
            break;
        }
        case NJson::JSON_MAP: {
            buf.Append('\x03');  // embedded document
            AppendCString(buf, key);
            SerializeDocument(buf, val);
            break;
        }
        case NJson::JSON_ARRAY: {
            buf.Append('\x04');  // array
            AppendCString(buf, key);
            SerializeArray(buf, val);
            break;
        }
        case NJson::JSON_BOOLEAN: {
            buf.Append('\x08');  // boolean
            AppendCString(buf, key);
            buf.Append(val.GetBoolean() ? '\x01' : '\x00');
            break;
        }
        case NJson::JSON_NULL:
        case NJson::JSON_UNDEFINED: {
            buf.Append('\x0A');  // null
            AppendCString(buf, key);
            break;
        }
        case NJson::JSON_INTEGER: {
            buf.Append('\x12');  // int64
            AppendCString(buf, key);
            AppendInt64(buf, val.GetInteger());
            break;
        }
        case NJson::JSON_UINTEGER: {
            buf.Append('\x12');  // int64
            AppendCString(buf, key);
            AppendInt64(buf, static_cast<int64_t>(val.GetUInteger()));
            break;
        }
    }
}

TBuffer Serialize(const NJson::TJsonValue& val) {
    TBuffer buf;
    buf.Reserve(256);
    SerializeDocument(buf, val);
    return buf;
}

// BSON deserialization helpers.
static TStringBuf ReadCString(const char*& p, const char* end) {
    const char* start = p;
    while (p < end && *p != '\0') ++p;
    TStringBuf result(start, p - start);
    if (p < end) ++p;  // skip null terminator
    return result;
}

static int32_t ReadInt32(const char*& p) {
    int32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

static int64_t ReadInt64(const char*& p) {
    int64_t v;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
}

static double ReadDouble(const char*& p) {
    double v;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
}

static NJson::TJsonValue DeserializeElement(uint8_t type, const char*& p, const char* end);
static NJson::TJsonValue DeserializeDocument(const char*& p, const char* end);

static NJson::TJsonValue DeserializeElement(uint8_t type, const char*& p, const char* end) {
    switch (type) {
        case 0x01:  // double
            return NJson::TJsonValue(ReadDouble(p));
        case 0x02: {  // string
            int32_t len = ReadInt32(p);
            TString s(p, len - 1);
            p += len;
            return NJson::TJsonValue(s);
        }
        case 0x03:  // embedded document
            return DeserializeDocument(p, end);
        case 0x04: {  // array (BSON stores arrays as documents with "0","1",... keys)
            const char* arrStart = p;
            int32_t arrSize = ReadInt32(p);
            const char* arrEnd = arrStart + arrSize;
            if (arrEnd > end) arrEnd = end;

            NJson::TJsonValue arr(NJson::JSON_ARRAY);
            while (p < arrEnd - 1) {
                uint8_t elemType = static_cast<uint8_t>(*p++);
                if (elemType == 0) break;
                ReadCString(p, arrEnd);  // skip index key
                arr.AppendValue(DeserializeElement(elemType, p, arrEnd));
            }
            if (p < arrEnd) p = arrEnd;
            return arr;
        }
        case 0x08:  // boolean
            return NJson::TJsonValue(*p++ != 0);
        case 0x0A:  // null
            return NJson::TJsonValue(NJson::JSON_NULL);
        case 0x12:  // int64
            return NJson::TJsonValue(static_cast<long long>(ReadInt64(p)));
        default:
            return NJson::TJsonValue();
    }
}

static NJson::TJsonValue DeserializeDocument(const char*& p, const char* end) {
    if (p + 4 > end) return NJson::TJsonValue(NJson::JSON_MAP);
    int32_t docSize = ReadInt32(p);
    const char* docEnd = p - 4 + docSize;
    if (docEnd > end) docEnd = end;

    NJson::TJsonValue obj(NJson::JSON_MAP);
    while (p < docEnd - 1) {
        uint8_t type = static_cast<uint8_t>(*p++);
        if (type == 0) break;
        TStringBuf key = ReadCString(p, docEnd);
        obj[TString(key)] = DeserializeElement(type, p, docEnd);
    }
    if (p < docEnd) p = docEnd;
    return obj;
}

NJson::TJsonValue Deserialize(TStringBuf data) {
    const char* p = data.data();
    const char* end = p + data.size();
    return DeserializeDocument(p, end);
}

}  // namespace NBson

// ============================================================================
// Hand-rolled MessagePack serializer (subset matching JSON types).
// Spec: https://github.com/msgpack/msgpack/blob/master/spec.md
// ============================================================================
namespace NMsgPack {

static void PackNil(TBuffer& buf) {
    buf.Append('\xc0');
}

static void PackBool(TBuffer& buf, bool v) {
    buf.Append(v ? '\xc3' : '\xc2');
}

static void PackUint(TBuffer& buf, uint64_t v) {
    if (v <= 0x7f) {
        buf.Append(static_cast<char>(v));
    } else if (v <= 0xff) {
        buf.Append('\xcc');
        buf.Append(static_cast<char>(v));
    } else if (v <= 0xffff) {
        buf.Append('\xcd');
        WriteBE16(buf, static_cast<uint16_t>(v));
    } else if (v <= 0xffffffff) {
        buf.Append('\xce');
        WriteBE32(buf, static_cast<uint32_t>(v));
    } else {
        buf.Append('\xcf');
        WriteBE64(buf, v);
    }
}

static void PackInt(TBuffer& buf, int64_t v) {
    if (v >= 0) {
        PackUint(buf, static_cast<uint64_t>(v));
    } else if (v >= -32) {
        buf.Append(static_cast<char>(static_cast<int8_t>(v)));
    } else if (v >= -128) {
        buf.Append('\xd0');
        buf.Append(static_cast<char>(static_cast<int8_t>(v)));
    } else if (v >= -32768) {
        buf.Append('\xd1');
        WriteBE16(buf, static_cast<uint16_t>(static_cast<int16_t>(v)));
    } else if (v >= -2147483648LL) {
        buf.Append('\xd2');
        WriteBE32(buf, static_cast<uint32_t>(static_cast<int32_t>(v)));
    } else {
        buf.Append('\xd3');
        WriteBE64(buf, static_cast<uint64_t>(v));
    }
}

static void PackDouble(TBuffer& buf, double v) {
    buf.Append('\xcb');
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    WriteBE64(buf, bits);
}

static void PackStr(TBuffer& buf, TStringBuf s) {
    size_t len = s.size();
    if (len <= 31) {
        buf.Append(static_cast<char>(0xa0 | len));
    } else if (len <= 0xff) {
        buf.Append('\xd9');
        buf.Append(static_cast<char>(len));
    } else if (len <= 0xffff) {
        buf.Append('\xda');
        WriteBE16(buf, static_cast<uint16_t>(len));
    } else {
        buf.Append('\xdb');
        WriteBE32(buf, static_cast<uint32_t>(len));
    }
    buf.Append(s.data(), s.size());
}

static void PackValue(TBuffer& buf, const NJson::TJsonValue& val);

static void PackMap(TBuffer& buf, const NJson::TJsonValue& val) {
    size_t n = val.GetMap().size();
    if (n <= 15) {
        buf.Append(static_cast<char>(0x80 | n));
    } else if (n <= 0xffff) {
        buf.Append('\xde');
        WriteBE16(buf, static_cast<uint16_t>(n));
    } else {
        buf.Append('\xdf');
        WriteBE32(buf, static_cast<uint32_t>(n));
    }
    for (const auto& [k, v] : val.GetMap()) {
        PackStr(buf, k);
        PackValue(buf, v);
    }
}

static void PackArray(TBuffer& buf, const NJson::TJsonValue& val) {
    size_t n = val.GetArray().size();
    if (n <= 15) {
        buf.Append(static_cast<char>(0x90 | n));
    } else if (n <= 0xffff) {
        buf.Append('\xdc');
        WriteBE16(buf, static_cast<uint16_t>(n));
    } else {
        buf.Append('\xdd');
        WriteBE32(buf, static_cast<uint32_t>(n));
    }
    for (const auto& elem : val.GetArray()) {
        PackValue(buf, elem);
    }
}

static void PackValue(TBuffer& buf, const NJson::TJsonValue& val) {
    switch (val.GetType()) {
        case NJson::JSON_NULL:
        case NJson::JSON_UNDEFINED:
            PackNil(buf);
            break;
        case NJson::JSON_BOOLEAN:
            PackBool(buf, val.GetBoolean());
            break;
        case NJson::JSON_INTEGER:
            PackInt(buf, val.GetInteger());
            break;
        case NJson::JSON_UINTEGER:
            PackUint(buf, val.GetUInteger());
            break;
        case NJson::JSON_DOUBLE:
            PackDouble(buf, val.GetDouble());
            break;
        case NJson::JSON_STRING:
            PackStr(buf, val.GetString());
            break;
        case NJson::JSON_MAP:
            PackMap(buf, val);
            break;
        case NJson::JSON_ARRAY:
            PackArray(buf, val);
            break;
    }
}

TBuffer Serialize(const NJson::TJsonValue& val) {
    TBuffer buf;
    buf.Reserve(256);
    PackValue(buf, val);
    return buf;
}

static uint8_t ReadU8(const char*& p) {
    return static_cast<uint8_t>(*p++);
}

static uint16_t ReadBE16(const char*& p) {
    uint16_t v = (static_cast<uint16_t>(static_cast<uint8_t>(p[0])) << 8) |
                  static_cast<uint16_t>(static_cast<uint8_t>(p[1]));
    p += 2;
    return v;
}

static uint32_t ReadBE32(const char*& p) {
    uint32_t v = (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
                  static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
    p += 4;
    return v;
}

static uint64_t ReadBE64(const char*& p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(static_cast<uint8_t>(*p++));
    }
    return v;
}

static NJson::TJsonValue DeserializeValue(const char*& p, const char* end);

static NJson::TJsonValue DeserializeValue(const char*& p, const char* end) {
    if (p >= end) return NJson::TJsonValue();

    uint8_t b = ReadU8(p);

    // Positive fixint: 0x00..0x7f
    if (b <= 0x7f) {
        return NJson::TJsonValue(static_cast<long long>(b));
    }
    // Fixmap: 0x80..0x8f
    if ((b & 0xf0) == 0x80) {
        size_t n = b & 0x0f;
        NJson::TJsonValue obj(NJson::JSON_MAP);
        for (size_t i = 0; i < n && p < end; ++i) {
            auto key = DeserializeValue(p, end);
            auto val = DeserializeValue(p, end);
            if (key.IsString()) {
                obj[key.GetString()] = std::move(val);
            }
        }
        return obj;
    }
    // Fixarray: 0x90..0x9f
    if ((b & 0xf0) == 0x90) {
        size_t n = b & 0x0f;
        NJson::TJsonValue arr(NJson::JSON_ARRAY);
        for (size_t i = 0; i < n && p < end; ++i) {
            arr.AppendValue(DeserializeValue(p, end));
        }
        return arr;
    }
    // Fixstr: 0xa0..0xbf
    if ((b & 0xe0) == 0xa0) {
        size_t len = b & 0x1f;
        TString s(p, len);
        p += len;
        return NJson::TJsonValue(s);
    }
    // Negative fixint: 0xe0..0xff
    if (b >= 0xe0) {
        return NJson::TJsonValue(static_cast<long long>(static_cast<int8_t>(b)));
    }

    switch (b) {
        case 0xc0: return NJson::TJsonValue(NJson::JSON_NULL);
        case 0xc2: return NJson::TJsonValue(false);
        case 0xc3: return NJson::TJsonValue(true);
        case 0xcc: return NJson::TJsonValue(static_cast<unsigned long long>(ReadU8(p)));
        case 0xcd: return NJson::TJsonValue(static_cast<unsigned long long>(ReadBE16(p)));
        case 0xce: return NJson::TJsonValue(static_cast<unsigned long long>(ReadBE32(p)));
        case 0xcf: return NJson::TJsonValue(static_cast<unsigned long long>(ReadBE64(p)));
        case 0xd0: return NJson::TJsonValue(static_cast<long long>(static_cast<int8_t>(ReadU8(p))));
        case 0xd1: return NJson::TJsonValue(static_cast<long long>(static_cast<int16_t>(ReadBE16(p))));
        case 0xd2: return NJson::TJsonValue(static_cast<long long>(static_cast<int32_t>(ReadBE32(p))));
        case 0xd3: return NJson::TJsonValue(static_cast<long long>(static_cast<int64_t>(ReadBE64(p))));
        case 0xcb: {
            uint64_t bits = ReadBE64(p);
            double v;
            std::memcpy(&v, &bits, 8);
            return NJson::TJsonValue(v);
        }
        case 0xd9: {
            size_t len = ReadU8(p);
            TString s(p, len);
            p += len;
            return NJson::TJsonValue(s);
        }
        case 0xda: {
            size_t len = ReadBE16(p);
            TString s(p, len);
            p += len;
            return NJson::TJsonValue(s);
        }
        case 0xdb: {
            size_t len = ReadBE32(p);
            TString s(p, len);
            p += len;
            return NJson::TJsonValue(s);
        }
        case 0xdc: {
            size_t n = ReadBE16(p);
            NJson::TJsonValue arr(NJson::JSON_ARRAY);
            for (size_t i = 0; i < n && p < end; ++i) {
                arr.AppendValue(DeserializeValue(p, end));
            }
            return arr;
        }
        case 0xdd: {
            size_t n = ReadBE32(p);
            NJson::TJsonValue arr(NJson::JSON_ARRAY);
            for (size_t i = 0; i < n && p < end; ++i) {
                arr.AppendValue(DeserializeValue(p, end));
            }
            return arr;
        }
        case 0xde: {
            size_t n = ReadBE16(p);
            NJson::TJsonValue obj(NJson::JSON_MAP);
            for (size_t i = 0; i < n && p < end; ++i) {
                auto key = DeserializeValue(p, end);
                auto val = DeserializeValue(p, end);
                if (key.IsString()) {
                    obj[key.GetString()] = std::move(val);
                }
            }
            return obj;
        }
        case 0xdf: {
            size_t n = ReadBE32(p);
            NJson::TJsonValue obj(NJson::JSON_MAP);
            for (size_t i = 0; i < n && p < end; ++i) {
                auto key = DeserializeValue(p, end);
                auto val = DeserializeValue(p, end);
                if (key.IsString()) {
                    obj[key.GetString()] = std::move(val);
                }
            }
            return obj;
        }
        default:
            break;
    }
    return NJson::TJsonValue();
}

NJson::TJsonValue Deserialize(TStringBuf data) {
    const char* p = data.data();
    const char* end = p + data.size();
    return DeserializeValue(p, end);
}

}  // namespace NMsgPack

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        Cerr << "Usage: " << argv[0] << " <input.ndjson> [iterations=1]" << Endl;
        return 1;
    }

    const TString inputPath = argv[1];
    const int iterations = (argc >= 3) ? atoi(argv[2]) : 1;

    // Read and parse all lines.
    TVector<TString> lines;
    TVector<NJson::TJsonValue> docs;
    {
        TFileInput fin(inputPath);
        TString line;
        while (fin.ReadLine(line)) {
            if (line.empty()) continue;
            NJson::TJsonValue val;
            if (!NJson::ReadJsonTree(line, &val, false)) {
                Cerr << "Failed to parse JSON line: " << line.substr(0, 80) << "..." << Endl;
                continue;
            }
            lines.push_back(line);
            docs.push_back(std::move(val));
        }
    }

    Cerr << "Parsed " << docs.size() << " documents from " << inputPath << Endl;
    if (docs.empty()) {
        return 0;
    }

    // Benchmark JSON serialization (NJson::WriteJson).
    {
        size_t totalBytes = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& doc : docs) {
                TString s = NJson::WriteJson(doc, /*formatOutput=*/false, /*sortkeys=*/false, /*validateUtf8=*/false);
                totalBytes += s.size();
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        size_t avgSize = totalBytes / (docs.size() * iterations);
        Cerr << "JSON:       " << elapsed << "s, "
             << (docs.size() * iterations) / elapsed << " docs/s, "
             << "avg size " << avgSize << " bytes" << Endl;
    }

    // Benchmark BinaryJson serialization.
    {
        size_t totalBytes = 0;
        size_t errors = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& line : lines) {
                auto result = NKikimr::NBinaryJson::SerializeToBinaryJson(line);
                if (auto* bjson = std::get_if<NKikimr::NBinaryJson::TBinaryJson>(&result)) {
                    totalBytes += bjson->Size();
                } else {
                    ++errors;
                }
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        size_t avgSize = (docs.size() * iterations - errors) > 0
            ? totalBytes / (docs.size() * iterations - errors) : 0;
        Cerr << "BinaryJson: " << elapsed << "s, "
             << (docs.size() * iterations) / elapsed << " docs/s, "
             << "avg size " << avgSize << " bytes";
        if (errors > 0) {
            Cerr << " (" << errors << " errors)";
        }
        Cerr << Endl;
    }

    // Benchmark BSON serialization.
    {
        size_t totalBytes = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& doc : docs) {
                TBuffer buf = NBson::Serialize(doc);
                totalBytes += buf.Size();
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        size_t avgSize = totalBytes / (docs.size() * iterations);
        Cerr << "BSON:       " << elapsed << "s, "
             << (docs.size() * iterations) / elapsed << " docs/s, "
             << "avg size " << avgSize << " bytes" << Endl;
    }

    // Benchmark MessagePack serialization.
    {
        size_t totalBytes = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& doc : docs) {
                TBuffer buf = NMsgPack::Serialize(doc);
                totalBytes += buf.Size();
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        size_t avgSize = totalBytes / (docs.size() * iterations);
        Cerr << "MsgPack:    " << elapsed << "s, "
             << (docs.size() * iterations) / elapsed << " docs/s, "
             << "avg size " << avgSize << " bytes" << Endl;
    }

    // Benchmark YSON serialization.
    {
        size_t totalBytes = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& doc : docs) {
                TString yson;
                NJson2Yson::SerializeJsonValueAsYson(doc, yson);
                totalBytes += yson.size();
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        size_t avgSize = totalBytes / (docs.size() * iterations);
        Cerr << "YSON:       " << elapsed << "s, "
             << (docs.size() * iterations) / elapsed << " docs/s, "
             << "avg size " << avgSize << " bytes" << Endl;
    }

    Cerr << "\n=== Deserialization ===" << Endl;

    // Pre-serialize all documents for deserialization benchmarks.
    TVector<TString> jsonSerialized;
    TVector<TBuffer> binaryJsonSerialized;
    TVector<TBuffer> bsonSerialized;
    TVector<TBuffer> msgpackSerialized;
    TVector<TString> ysonSerialized;

    jsonSerialized.reserve(docs.size());
    binaryJsonSerialized.reserve(docs.size());
    bsonSerialized.reserve(docs.size());
    msgpackSerialized.reserve(docs.size());
    ysonSerialized.reserve(docs.size());

    for (size_t i = 0; i < docs.size(); ++i) {
        jsonSerialized.push_back(NJson::WriteJson(docs[i], false, false, false));
        auto bjResult = NKikimr::NBinaryJson::SerializeToBinaryJson(lines[i]);
        if (auto* bj = std::get_if<NKikimr::NBinaryJson::TBinaryJson>(&bjResult)) {
            binaryJsonSerialized.push_back(std::move(*bj));
        } else {
            binaryJsonSerialized.emplace_back();
        }
        bsonSerialized.push_back(NBson::Serialize(docs[i]));
        msgpackSerialized.push_back(NMsgPack::Serialize(docs[i]));
        TString yson;
        NJson2Yson::SerializeJsonValueAsYson(docs[i], yson);
        ysonSerialized.push_back(std::move(yson));
    }

    // Benchmark JSON deserialization.
    {
        size_t count = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& s : jsonSerialized) {
                NJson::TJsonValue val;
                NJson::ReadJsonTree(s, &val, false);
                ++count;
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        Cerr << "JSON:       " << elapsed << "s, "
             << count / elapsed << " docs/s" << Endl;
    }

    // Benchmark BinaryJson deserialization (to JSON text).
    {
        size_t count = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& buf : binaryJsonSerialized) {
                if (buf.Size() > 0) {
                    TString json = NKikimr::NBinaryJson::SerializeToJson(
                        TStringBuf(buf.Data(), buf.Size()));
                    ++count;
                }
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        Cerr << "BinaryJson: " << elapsed << "s, "
             << count / elapsed << " docs/s" << Endl;
    }

    // Benchmark BSON deserialization.
    {
        size_t count = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& buf : bsonSerialized) {
                auto val = NBson::Deserialize(TStringBuf(buf.Data(), buf.Size()));
                ++count;
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        Cerr << "BSON:       " << elapsed << "s, "
             << count / elapsed << " docs/s" << Endl;
    }

    // Benchmark MessagePack deserialization.
    {
        size_t count = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& buf : msgpackSerialized) {
                auto val = NMsgPack::Deserialize(TStringBuf(buf.Data(), buf.Size()));
                ++count;
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        Cerr << "MsgPack:    " << elapsed << "s, "
             << count / elapsed << " docs/s" << Endl;
    }

    // Benchmark YSON deserialization.
    {
        size_t count = 0;
        TSimpleTimer timer;
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& yson : ysonSerialized) {
                NJson::TJsonValue val;
                NJson2Yson::DeserializeYsonAsJsonValue(yson, &val);
                ++count;
            }
        }
        double elapsed = timer.Get().SecondsFloat();
        Cerr << "YSON:       " << elapsed << "s, "
             << count / elapsed << " docs/s" << Endl;
    }

    return 0;
}
