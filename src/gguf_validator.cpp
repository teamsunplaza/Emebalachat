#include "gguf_validator.hpp"

#include <cstring>
#include <fstream>
#include <vector>

namespace emebalachat {
namespace ggufvalidator {

namespace {

// G-01 (261111_0001 round 2): shared structural constants. The scan runs
// over EITHER an in-memory buffer (ParseGgufHeader) or a windowed file
// cursor (ValidateGgufFile) through the same Reader contract:
//   size()                  — total input size in bytes
//   ReadU32 / ReadU64       — little-endian fixed fields
//   Skip(n)                 — consume n bytes without materializing
//   ReadStringEquals(exp,eq)— consume a GGUF string, compare it to `exp`
//   ReadStringCapture(out,t)— consume a GGUF string, capture it when it
//                             fits kMaxCapturedStringBytes (else skip and
//                             set `t` = true)
//   SkipString()            — consume a GGUF string, discarding the bytes
// Every method returns false on truncation/garbage and poisons the reader;
// the scan then reports the structural verdict, never an exception.
constexpr std::uint64_t kMaxGgufStringBytes = 16ull * 1024 * 1024;

// "GGUF" read as a little-endian u32 (G=0x47 G=0x47 U=0x55 F=0x46).
constexpr std::uint32_t kGgufMagic = 0x46554747u;

constexpr std::string_view kArchitectureKey = "general.architecture";

// ---- Reader 1: bounds-checked little-endian cursor over an in-memory buffer.
// Every read advances only when the full field fits; a short buffer poisons
// the cursor and every subsequent read fails. ----
class MemoryReader {
public:
    explicit MemoryReader(std::string_view bytes) : bytes_(bytes) {}

    std::uint64_t size() const { return bytes_.size(); }

    bool ReadU32(std::uint32_t& out) {
        std::uint64_t v = 0;
        if (!ReadLe(4, v)) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    }
    bool ReadU64(std::uint64_t& out) { return ReadLe(8, out); }

    bool Skip(std::uint64_t n) {
        if (pos_ == npos || n > bytes_.size() - pos_) return Poison();
        pos_ += static_cast<std::size_t>(n);
        return true;
    }

    bool ReadStringEquals(std::string_view expected, bool& eq) {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes || pos_ == npos || len > bytes_.size() - pos_) {
            return Poison();
        }
        eq = len == expected.size() &&
             bytes_.compare(pos_, static_cast<std::size_t>(len), expected) == 0;
        pos_ += static_cast<std::size_t>(len);
        return true;
    }

    bool ReadStringCapture(std::string& out, bool& too_long) {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes || pos_ == npos || len > bytes_.size() - pos_) {
            return Poison();
        }
        if (len > kMaxCapturedStringBytes) {
            too_long = true;
            out.clear();
            pos_ += static_cast<std::size_t>(len);
            return true;
        }
        too_long = false;
        out.assign(bytes_.substr(pos_, static_cast<std::size_t>(len)));
        pos_ += static_cast<std::size_t>(len);
        return true;
    }

    bool SkipString() {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes) return Poison();
        return Skip(len);
    }

private:
    static constexpr std::size_t npos = std::string_view::npos;

    bool Poison() {
        pos_ = npos;
        return false;
    }

    bool ReadLe(std::size_t n, std::uint64_t& out) {
        if (pos_ == npos || bytes_.size() - pos_ < n) return Poison();
        out = 0;
        for (std::size_t i = 0; i < n; ++i) {
            out |= static_cast<std::uint64_t>(
                       static_cast<unsigned char>(bytes_[pos_ + i]))
                   << (8 * i);
        }
        pos_ += n;
        return true;
    }

    std::string_view bytes_;
    std::size_t pos_ = 0;
};

// ---- Reader 2: sliding-window cursor over a binary file. One heap buffer
// of kStreamWindowBytes is the ONLY allocation; values larger than the
// window are skipped by offset arithmetic (bounded by the file size), so a
// multi-MB metadata section validates without ever being materialized. ----
class FileReader {
public:
    FileReader(std::ifstream& in, std::uint64_t file_size)
        : in_(in), file_size_(file_size), window_(kStreamWindowBytes) {}

    std::uint64_t size() const { return file_size_; }

    bool ReadU32(std::uint32_t& out) {
        std::uint64_t v = 0;
        if (!ReadLe(4, v)) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    }
    bool ReadU64(std::uint64_t& out) { return ReadLe(8, out); }

    bool Skip(std::uint64_t n) {
        if (n > file_size_ - pos_) return false;
        pos_ += n;
        // Drop the window once pos_ leaves it; the next Ensure refills at
        // the new offset (a value skip is O(1) — the skipped bytes are
        // never read).
        if (pos_ - win_start_ > static_cast<std::uint64_t>(win_len_)) {
            win_start_ = pos_;
            win_off_ = 0;
            win_len_ = 0;
        } else {
            win_off_ = static_cast<std::size_t>(pos_ - win_start_);
        }
        return true;
    }

    bool ReadStringEquals(std::string_view expected, bool& eq) {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes || len > file_size_ - pos_) return false;
        if (len == expected.size()) {
            if (!Ensure(static_cast<std::size_t>(len))) return false;
            eq = std::memcmp(window_.data() + win_off_, expected.data(),
                             static_cast<std::size_t>(len)) == 0;
        } else {
            eq = false;
        }
        return Skip(len);
    }

    bool ReadStringCapture(std::string& out, bool& too_long) {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes || len > file_size_ - pos_) return false;
        if (len > kMaxCapturedStringBytes) {
            too_long = true;
            out.clear();
            return Skip(len);
        }
        if (!Ensure(static_cast<std::size_t>(len))) return false;
        out.assign(window_.data() + win_off_, static_cast<std::size_t>(len));
        too_long = false;
        return Skip(len);
    }

    bool SkipString() {
        std::uint64_t len = 0;
        if (!ReadU64(len)) return false;
        if (len > kMaxGgufStringBytes || len > file_size_ - pos_) return false;
        return Skip(len);
    }

private:
    // Makes window_[win_off_ .. win_off_ + n) hold the next n bytes of the
    // input (n must fit the window — the scan never asks for more). Refills
    // by seeking to pos_, so skipped ranges are never read.
    bool Ensure(std::size_t n) {
        if (n > kStreamWindowBytes) return false;
        if (win_off_ + n <= win_len_) return true;
        win_start_ = pos_;
        win_off_ = 0;
        win_len_ = 0;
        const std::uint64_t avail = file_size_ - pos_;
        const std::size_t want = avail > static_cast<std::uint64_t>(kStreamWindowBytes)
                                     ? kStreamWindowBytes
                                     : static_cast<std::size_t>(avail);
        if (want == 0) return false; // the input ended before the field did
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos_));
        if (!in_) return false;
        in_.read(window_.data(), static_cast<std::streamsize>(want));
        const std::size_t got = static_cast<std::size_t>(in_.gcount());
        win_len_ = got;
        return got == want;
    }

    bool ReadLe(std::size_t n, std::uint64_t& out) {
        if (n > file_size_ - pos_) return false;
        if (!Ensure(n)) return false;
        out = 0;
        for (std::size_t i = 0; i < n; ++i) {
            out |= static_cast<std::uint64_t>(
                       static_cast<unsigned char>(window_[win_off_ + i]))
                   << (8 * i);
        }
        pos_ += n;
        win_off_ += n;
        return true;
    }

    std::ifstream& in_;
    std::uint64_t file_size_;
    std::uint64_t pos_ = 0;
    std::uint64_t win_start_ = 0; // absolute offset of window_[0]
    std::size_t win_off_ = 0;     // pos_ - win_start_ (invariant while valid)
    std::size_t win_len_ = 0;     // valid bytes in window_
    std::vector<char> window_;
};

// GGUF metadata value types (the stable spec numbering llama.cpp parses):
//   0 UINT8, 1 INT8, 2 UINT16, 3 INT16, 4 UINT32, 5 INT32, 6 FLOAT32,
//   7 BOOL, 8 STRING, 9 ARRAY, 10 UINT64, 11 INT64, 12 FLOAT64.
enum GgufValueType : std::uint32_t {
    kTypeString = 8,
    kTypeArray = 9,
};

// Skips one metadata value. Strings and array elements carry their own
// lengths; arrays recurse (bounded depth — nested arrays are legal in the
// container). Unknown types fail the scan (a garbage file). Every call
// consumes at least one input byte or fails, and the cursor only moves
// forward, so the walk terminates on any input.
template <typename Reader>
bool SkipValue(Reader& c, std::uint32_t type, int depth) {
    if (depth > 8) return false;
    switch (type) {
        case 0: case 1: case 7:            // uint8 / int8 / bool
            return c.Skip(1);
        case 2: case 3:                    // uint16 / int16
            return c.Skip(2);
        case 4: case 5: case 6:            // uint32 / int32 / float32
            return c.Skip(4);
        case 10: case 11: case 12:         // uint64 / int64 / float64
            return c.Skip(8);
        case kTypeString:
            return c.SkipString();
        case kTypeArray: {
            std::uint32_t elem_type = 0;
            std::uint64_t count = 0;
            if (!c.ReadU32(elem_type) || !c.ReadU64(count)) return false;
            if (count > kMaxMetadataCount) return false;
            for (std::uint64_t i = 0; i < count; ++i) {
                if (!SkipValue(c, elem_type, depth + 1)) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

// The structural scan shared by the in-memory parser and the file validator.
// Verdicts match the round-1 parser exactly (see tests/v107_user_model_tests.inc):
//   * too-short input before/at the fixed header -> TooSmall,
//   * wrong magic -> BadMagic, version not 2/3 -> UnsupportedVersion,
//   * insane counts / unknown value type / any truncation inside the
//     declared metadata or the FIRST tensor-info record -> MalformedHeader
//     (R2-D: truncation right after general.architecture no longer passes),
//   * no non-empty general.architecture string -> NoArchitecture.
template <typename Reader>
GgufValidationResult ScanGguf(Reader& r, GgufHeaderInfo* out_info) {
    if (out_info) {
        *out_info = GgufHeaderInfo{};
    }

    // Magic first so a renamed text file gets the precise verdict even when
    // it is shorter than the fixed header.
    if (r.size() < 4) {
        return GgufValidationResult::TooSmall;
    }
    std::uint32_t magic = 0;
    if (!r.ReadU32(magic)) {
        return GgufValidationResult::TooSmall;
    }
    if (magic != kGgufMagic) {
        return GgufValidationResult::BadMagic;
    }
    if (r.size() < kGgufFixedHeaderBytes) {
        return GgufValidationResult::TooSmall;
    }

    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t metadata_count = 0;
    if (!r.ReadU32(version) || !r.ReadU64(tensor_count) || !r.ReadU64(metadata_count)) {
        return GgufValidationResult::TooSmall; // unreachable (size checked), defensive
    }
    if (version != 2 && version != 3) {
        return GgufValidationResult::UnsupportedVersion;
    }
    if (tensor_count == 0 || tensor_count > kMaxTensorCount ||
        metadata_count > kMaxMetadataCount) {
        return GgufValidationResult::MalformedHeader;
    }

    // Metadata KV scan: the architecture key is conventionally the FIRST
    // entry (llama.cpp writes it first), but every key is walked so a
    // reordered header still validates. A short/garbage input exhausts the
    // cursor mid-scan — the reader failure reports MalformedHeader.
    //
    // R2-D (261111_0001): the scan no longer returns at the architecture key.
    // The old early Ok left the REMAINING declared metadata KVs and ALL
    // tensor-info records unread, so a file truncated right after the arch
    // field (arch-first layout, tensor_count >= 1) passed registration.
    // The scan finishes all metadata_count KVs with the bounded SkipValue and
    // then validates that the FIRST tensor-info record parses: name length +
    // name, n_dims within bound, dims, type, offset. Any short read ->
    // MalformedHeader; only then Ok. Residual accepted: truncation inside
    // tensor-info record 2..N is NOT detected here — llama.cpp rejects such
    // a file at load time anyway (the gate's job is the early reject of files
    // that can never load, not a full container parse).
    //
    // G-01 (261111_0001 round 2): the same scan now runs over a sliding
    // window on the file path — the metadata section need NOT fit any probe,
    // so multi-MB tokenizer arrays validate instead of false-rejecting.
    std::string arch;
    bool have_arch = false;
    for (std::uint64_t i = 0; i < metadata_count; ++i) {
        bool is_arch = false;
        if (!r.ReadStringEquals(kArchitectureKey, is_arch)) {
            return GgufValidationResult::MalformedHeader;
        }
        std::uint32_t type = 0;
        if (!r.ReadU32(type)) {
            return GgufValidationResult::MalformedHeader;
        }
        if (is_arch) {
            bool too_long = false;
            if (type != kTypeString || !r.ReadStringCapture(arch, too_long) ||
                too_long || arch.empty()) {
                return GgufValidationResult::NoArchitecture;
            }
            have_arch = true;
            continue;
        }
        if (!SkipValue(r, type, 0)) {
            return GgufValidationResult::MalformedHeader;
        }
    }
    if (!have_arch) {
        return GgufValidationResult::NoArchitecture;
    }

    // First tensor-info record (GGUF v2/v3 layout): string name, u32 n_dims,
    // n_dims x u64 dims, u32 ggml_type, u64 offset. Every field must parse —
    // a truncated record is a truncated download.
    {
        std::uint32_t n_dims = 0;
        if (!r.SkipString()) {
            return GgufValidationResult::MalformedHeader;
        }
        if (!r.ReadU32(n_dims)) {
            return GgufValidationResult::MalformedHeader;
        }
        if (n_dims > kMaxTensorDims) {
            return GgufValidationResult::MalformedHeader;
        }
        for (std::uint32_t d = 0; d < n_dims; ++d) {
            std::uint64_t dim = 0;
            if (!r.ReadU64(dim)) {
                return GgufValidationResult::MalformedHeader;
            }
        }
        std::uint32_t ggml_type = 0;
        std::uint64_t offset = 0;
        if (!r.ReadU32(ggml_type) || !r.ReadU64(offset)) {
            return GgufValidationResult::MalformedHeader;
        }
    }

    if (out_info) {
        out_info->version = version;
        out_info->tensor_count = tensor_count;
        out_info->metadata_count = metadata_count;
        out_info->architecture = std::move(arch);
    }
    return GgufValidationResult::Ok;
}

} // namespace

GgufValidationResult ParseGgufHeader(std::string_view bytes, GgufHeaderInfo* out_info) {
    MemoryReader reader(bytes);
    return ScanGguf(reader, out_info);
}

GgufValidationResult ValidateGgufFile(const std::filesystem::path& path,
                                      GgufHeaderInfo* out_info) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return GgufValidationResult::FileUnreadable;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return GgufValidationResult::FileUnreadable;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return GgufValidationResult::FileUnreadable;
    }
    FileReader reader(in, static_cast<std::uint64_t>(file_size));
    return ScanGguf(reader, out_info);
}

} // namespace ggufvalidator
} // namespace emebalachat
