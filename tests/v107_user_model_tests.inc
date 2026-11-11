// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001 U6) - USER-MODEL SURFACE COMPLETENESS
// (research R1 P1) + prompt-label governance accessor + provider failure
// sanitization. CEO standing requirement: users must be able to download and
// use arbitrary external-provider models and local open-weight models
// directly; the app must catch bad files early and explain failures.
//
// Covered here, all against public seams:
//
//  (1) GGUF header parser (src/gguf_validator.*): magic/version/counts and
//      the general.architecture metadata key, over an in-memory byte buffer
//      (pure) and over a real temp file (created + deleted here, nothing
//      else touches the filesystem). G-01 (round 2): a >=3 MB metadata
//      section streams through the windowed validator (Ok), and the
//      truncation-after-arch rejection survives on the file path too.
//
//  (2) OpenAI failure-detail helpers (src/openai_compatible_client.hpp):
//      ExtractOpenAiErrorMessage pulls error.message out of a non-2xx body
//      (with a bounded raw-prefix fallback), and SanitizeOpenAiErrorDetail
//      guarantees the masking contract — the API key can NEVER appear in a
//      displayed/logged detail ("***" replacement), the detail is a single
//      line, and it is bounded to kMaxOpenAiErrorDetailChars. G-02 (round
//      2): adversarial bidi-interleaved key echoes; G-11 (round 2): the
//      fallback/extract chain stops on a UTF-8 boundary at the 160-byte cap.
//
//  (3) DetectLanguageForPrompt (src/smart_bypass.hpp): the ONE prompt-label
//      accessor — dominance-gated (U2 model inside DetectLanguage) and the
//      "Unknown" -> "Auto Detect" contract, so no garbage label can reach an
//      engine prompt or the back-translation ladder source.
//
//  (4) Structural pins: the manager-window wiring (HF completed-download +
//      orphan-file registration gates, the LBN_DBLCLK activate handler, the
//      active-binding list marker), the worker.cpp prompt-label seam, and
//      the main.cpp drag label seam all ride the documented surfaces.
//
// NOTE: the suite links ggufvalidator symbols, so src/gguf_validator.cpp
// must join Emebalachat_core first (the U6 report gives the CMakeLists line).
// ---------------------------------------------------------------------------

#include "../src/gguf_validator.hpp"
#include "../src/openai_compatible_client.hpp"
#include "../src/smart_bypass.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

// ---- little-endian GGUF fixture builder -----------------------------------

class GgufBuilder {
public:
    GgufBuilder& Magic() {
        bytes_ += "GGUF";
        return *this;
    }
    GgufBuilder& BadMagic() {
        bytes_ += "GGLF";
        return *this;
    }
    GgufBuilder& U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_ += static_cast<char>((v >> (8 * i)) & 0xFF);
        return *this;
    }
    GgufBuilder& U64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes_ += static_cast<char>((v >> (8 * i)) & 0xFF);
        return *this;
    }
    GgufBuilder& Str(const std::string& s) {
        U64(static_cast<std::uint64_t>(s.size()));
        bytes_ += s;
        return *this;
    }
    // One metadata KV: key + value-type + value.
    GgufBuilder& KvString(const std::string& key, const std::string& value) {
        Str(key);
        U32(8); // STRING
        Str(value);
        return *this;
    }
    // One tensor-info record (GGUF v2/v3): string name, u32 n_dims,
    // n_dims x u64 dims, u32 ggml_type, u64 offset.
    GgufBuilder& TensorInfo(const std::string& name, std::uint32_t n_dims = 1,
                            std::uint32_t ggml_type = 0, std::uint64_t offset = 4096) {
        Str(name);
        U32(n_dims);
        for (std::uint32_t d = 0; d < n_dims; ++d) {
            U64(1);
        }
        U32(ggml_type);
        U64(offset);
        return *this;
    }
    const std::string& Bytes() const { return bytes_; }

private:
    std::string bytes_;
};

// A structurally valid minimal header: v3, one tensor, general.architecture
// as the first (and only) metadata KV, followed by ONE COMPLETE tensor-info
// record (R2-D: the validator now reads past the arch key).
std::string V107ValidHeader(std::uint32_t version = 3,
                            const std::string& arch = "llama") {
    return GgufBuilder().Magic().U32(version).U64(1).U64(1)
        .KvString("general.architecture", arch)
        .TensorInfo("probe.weight").Bytes();
}

} // namespace

int RunV107UserModelTests(int& failures) {
    std::cout << "[RUN] Testing v0.10.7 user-model surface (261111_0001 U6)..."
              << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::ggufvalidator::GgufHeaderInfo;
    using emebalachat::ggufvalidator::GgufValidationResult;
    using emebalachat::ggufvalidator::ParseGgufHeader;
    using emebalachat::ggufvalidator::ValidateGgufFile;

    // =====================================================================
    // 1) GGUF header parser — the early-registration garbage gate.
    // =====================================================================
    {
        GgufHeaderInfo info;
        TEST_CHECK(ParseGgufHeader("", &info) == GgufValidationResult::TooSmall,
                   "V107 GGUF: empty buffer is TooSmall");
        TEST_CHECK(ParseGgufHeader("GGU", &info) == GgufValidationResult::TooSmall,
                   "V107 GGUF: 3-byte buffer is TooSmall");
        TEST_CHECK(ParseGgufHeader("GGUF", &info) == GgufValidationResult::TooSmall,
                   "V107 GGUF: bare magic without the fixed header is TooSmall");

        const std::string bad_magic = GgufBuilder().BadMagic().U32(3).U64(1).U64(1).Bytes();
        TEST_CHECK(ParseGgufHeader(bad_magic, &info) == GgufValidationResult::BadMagic,
                   "V107 GGUF: wrong magic is BadMagic");

        const std::string v1 = GgufBuilder().Magic().U32(1).U64(1).U64(1).Bytes();
        TEST_CHECK(ParseGgufHeader(v1, &info) == GgufValidationResult::UnsupportedVersion,
                   "V107 GGUF: container version 1 is UnsupportedVersion");
        const std::string v4 = GgufBuilder().Magic().U32(4).U64(1).U64(1).Bytes();
        TEST_CHECK(ParseGgufHeader(v4, &info) == GgufValidationResult::UnsupportedVersion,
                   "V107 GGUF: container version 4 is UnsupportedVersion");

        const std::string zero_tensors = GgufBuilder().Magic().U32(3).U64(0).U64(1).Bytes();
        TEST_CHECK(ParseGgufHeader(zero_tensors, &info) == GgufValidationResult::MalformedHeader,
                   "V107 GGUF: zero tensors is MalformedHeader");
        const std::string absurd_tensors =
            GgufBuilder().Magic().U32(3).U64(1000001).U64(1).Bytes();
        TEST_CHECK(ParseGgufHeader(absurd_tensors, &info) == GgufValidationResult::MalformedHeader,
                   "V107 GGUF: >1M claimed tensors is MalformedHeader");
        const std::string absurd_meta =
            GgufBuilder().Magic().U32(3).U64(1).U64(1000001).Bytes();
        TEST_CHECK(ParseGgufHeader(absurd_meta, &info) == GgufValidationResult::MalformedHeader,
                   "V107 GGUF: >1M claimed metadata entries is MalformedHeader");

        // The happy path, both supported container versions.
        const std::string v3 = V107ValidHeader(3, "llama");
        TEST_CHECK(ParseGgufHeader(v3, &info) == GgufValidationResult::Ok,
                   "V107 GGUF: v3 header with general.architecture is Ok");
        TEST_CHECK(info.version == 3 && info.tensor_count == 1 && info.metadata_count == 1 &&
                       info.architecture == "llama",
                   "V107 GGUF: header info extracts version/counts/architecture");
        const std::string v2 = V107ValidHeader(2, "qwen2");
        TEST_CHECK(ParseGgufHeader(v2, &info) == GgufValidationResult::Ok &&
                       info.version == 2 && info.architecture == "qwen2",
                   "V107 GGUF: v2 header parses and reports its arch");

        // The architecture key does not have to be first — the KV scan walks.
        std::string reordered = GgufBuilder().Magic().U32(3).U64(1).U64(2)
            .KvString("general.name", "probe").KvString("general.architecture", "gemma")
            .TensorInfo("probe.weight").Bytes();
        TEST_CHECK(ParseGgufHeader(reordered, &info) == GgufValidationResult::Ok &&
                       info.architecture == "gemma",
                   "V107 GGUF: a reordered header still finds general.architecture");

        // Array values must be skippable (general.tags is conventionally an
        // array of strings and precedes nothing here — it IS the only KV).
        std::string with_array = GgufBuilder().Magic().U32(3).U64(1).U64(2)
            .Str("general.tags").U32(9).U32(8).U64(2).Str("a").Str("b")
            .KvString("general.architecture", "llama").TensorInfo("probe.weight").Bytes();
        TEST_CHECK(ParseGgufHeader(with_array, &info) == GgufValidationResult::Ok,
                   "V107 GGUF: an array-typed KV before the arch key is skipped");

        // Missing / empty / mistyped architecture.
        const std::string no_arch = GgufBuilder().Magic().U32(3).U64(1).U64(1)
            .KvString("general.name", "x").Bytes();
        TEST_CHECK(ParseGgufHeader(no_arch, &info) == GgufValidationResult::NoArchitecture,
                   "V107 GGUF: metadata without the arch key is NoArchitecture");
        const std::string empty_arch = V107ValidHeader(3, "");
        TEST_CHECK(ParseGgufHeader(empty_arch, &info) == GgufValidationResult::NoArchitecture,
                   "V107 GGUF: an EMPTY general.architecture is NoArchitecture");
        const std::string zero_meta = GgufBuilder().Magic().U32(3).U64(1).U64(0).Bytes();
        TEST_CHECK(ParseGgufHeader(zero_meta, &info) == GgufValidationResult::NoArchitecture,
                   "V107 GGUF: zero metadata entries is NoArchitecture");

        // Truncated metadata: the count promises two KVs; the first is a
        // non-arch key and the buffer ends inside the second — the scan
        // must hit the wall and report the structural verdict.
        std::string truncated = GgufBuilder().Magic().U32(3).U64(1).U64(2)
            .KvString("general.name", "probe").U64(10).Str("abc").Bytes();
        TEST_CHECK(ParseGgufHeader(truncated, &info) == GgufValidationResult::MalformedHeader,
                   "V107 GGUF: metadata count beyond the buffer is MalformedHeader (truncated download)");

        // Unknown value type poisons the scan.
        std::string bad_type = GgufBuilder().Magic().U32(3).U64(1).U64(1)
            .Str("general.name").U32(99).Bytes();
        TEST_CHECK(ParseGgufHeader(bad_type, &info) == GgufValidationResult::MalformedHeader,
                   "V107 GGUF: an unknown metadata value type is MalformedHeader");

        // ---- R2-D (261111_0001): truncation past the arch key is rejected ----
        // The old parser returned Ok inside the arch branch, so none of the
        // shapes below were caught. All carry tensor_count=1 (the 0-tensor
        // policy itself stays rejected at the count check, pinned above).
        {
            // Truncated IMMEDIATELY after the arch value (arch-first layout):
            // no tensor-info record at all.
            const std::string cut_after_arch = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "llama").Bytes();
            TEST_CHECK(ParseGgufHeader(cut_after_arch, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: truncation right after the arch field is MalformedHeader");

            // Arch present, then the REMAINING declared metadata is truncated:
            // metadata_count=2, the second KV's string value is cut short.
            std::string cut_in_meta = GgufBuilder().Magic().U32(3).U64(1).U64(2)
                .KvString("general.architecture", "llama")
                .Str("general.name").U32(8).U64(10).Bytes(); // promises 10, buffer ends
            cut_in_meta += "ab";
            TEST_CHECK(ParseGgufHeader(cut_in_meta, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: truncation inside the remaining metadata (arch present) is MalformedHeader");

            // Truncated FIRST tensor-info record: name string cut mid-way.
            std::string cut_tensor_name = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "llama").U64(16).Bytes(); // name len 16, no bytes
            cut_tensor_name += "we";
            TEST_CHECK(ParseGgufHeader(cut_tensor_name, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: a truncated first tensor-info name is MalformedHeader");

            // First tensor-info record present but n_dims/dims cut off.
            const std::string cut_tensor_dims = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "llama").Str("w").U32(2).U64(1).Bytes();
            TEST_CHECK(ParseGgufHeader(cut_tensor_dims, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: a truncated first tensor-info dims array is MalformedHeader");

            // Absurd n_dims in the first tensor-info record.
            const std::string absurd_dims = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "llama").Str("w").U32(1000000).Bytes();
            TEST_CHECK(ParseGgufHeader(absurd_dims, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: an absurd n_dims in the first tensor-info is MalformedHeader");

            // Explicit 0-tensor policy pin: tensor_count == 0 is rejected at
            // the count check (gguf_validator.cpp), BEFORE any metadata scan.
            const std::string zero_t = GgufBuilder().Magic().U32(3).U64(0).U64(1)
                .KvString("general.architecture", "llama").Bytes();
            TEST_CHECK(ParseGgufHeader(zero_t, &info) == GgufValidationResult::MalformedHeader,
                       "V107 R2-D GGUF: the 0-tensor policy stays MalformedHeader (count check)");

            // A valid full header with ONE complete tensor-info record (2
            // dims) is Ok and still extracts the arch.
            const std::string full = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "qwen2")
                .TensorInfo("w", /*n_dims=*/2, /*ggml_type=*/0, /*offset=*/8192).Bytes();
            TEST_CHECK(ParseGgufHeader(full, &info) == GgufValidationResult::Ok &&
                           info.architecture == "qwen2" && info.tensor_count == 1,
                       "V107 R2-D GGUF: a valid header with one complete tensor-info record is Ok");
        }
    }

    // =====================================================================
    // 2) File-level validation over a real (temporary) file — created and
    //    deleted here; nothing else touches the disk.
    // =====================================================================
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        const std::filesystem::path good = dir / L"emebala_v107_probe_good.gguf";
        const std::filesystem::path bad = dir / L"emebala_v107_probe_bad.gguf";
        const std::filesystem::path tiny = dir / L"emebala_v107_probe_tiny.gguf";
        const std::filesystem::path missing = dir / L"emebala_v107_probe_missing.gguf";

        {
            std::ofstream out(good, std::ios::binary | std::ios::trunc);
            const std::string bytes = V107ValidHeader(3, "llama");
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            std::ofstream out(bad, std::ios::binary | std::ios::trunc);
            out << "this is not a gguf file at all";
        }
        {
            std::ofstream out(tiny, std::ios::binary | std::ios::trunc);
            out << "GGUF";
        }

        GgufHeaderInfo info;
        TEST_CHECK(ValidateGgufFile(good, &info) == GgufValidationResult::Ok &&
                       info.architecture == "llama",
                   "V107 GGUF file: the valid temp header validates end to end");
        TEST_CHECK(ValidateGgufFile(bad, &info) == GgufValidationResult::BadMagic,
                   "V107 GGUF file: a text file is rejected with BadMagic");
        TEST_CHECK(ValidateGgufFile(tiny, &info) == GgufValidationResult::TooSmall,
                   "V107 GGUF file: a 4-byte file is TooSmall");
        TEST_CHECK(ValidateGgufFile(missing, &info) == GgufValidationResult::FileUnreadable,
                   "V107 GGUF file: a missing file is FileUnreadable");

        std::error_code ec;
        std::filesystem::remove(good, ec);
        std::filesystem::remove(bad, ec);
        std::filesystem::remove(tiny, ec);
        TEST_CHECK(!std::filesystem::exists(good) && !std::filesystem::exists(bad) &&
                       !std::filesystem::exists(tiny),
                   "V107 GGUF file: probe temp files are deleted");

        // ---- G-01 (261111_0001 round 2): large-metadata GGUFs validate with
        // a bounded sliding window. Round-1's fixed 2 MiB probe false-rejected
        // every real tokenizer-bearing model (bundled Hy-MT2 metadata alone is
        // 5.07 MB), so these fixtures pin the windowed path: a >=3 MB metadata
        // section walks to the end and the first tensor-info record is still
        // validated. Temp files, deleted below.
        {
            const std::filesystem::path bigmeta = dir / L"emebala_v107_probe_bigmeta.gguf";
            const std::filesystem::path bigstr = dir / L"emebala_v107_probe_bigstr.gguf";
            const std::filesystem::path bigcut = dir / L"emebala_v107_probe_bigcut.gguf";
            const std::filesystem::path cutfile = dir / L"emebala_v107_probe_cutafterarch.gguf";

            // Fixture A: tokenizer-like metadata — one ARRAY KV of 60000
            // strings x (8-byte length + 56-byte payload) ~= 3.84 MB of value
            // bytes, plus the arch KV and a trailing small KV, then one
            // complete tensor-info record. The windowed SkipValue walks every
            // array element structurally without materializing the value.
            GgufBuilder big;
            big.Magic().U32(3).U64(1).U64(3)
               .KvString("general.architecture", "llama");
            big.Str("tokenizer.ggml.tokens").U32(9).U32(8).U64(60000);
            const std::string tok(56, 't');
            for (int i = 0; i < 60000; ++i) {
                big.Str(tok);
            }
            big.KvString("general.name", "bigmeta-probe").TensorInfo("probe.weight");
            const std::string big_bytes = big.Bytes();
            TEST_CHECK(big_bytes.size() >= 3 * 1024 * 1024,
                       "V107 G-01 GGUF: the fixture's metadata section is >= 3 MB");
            {
                std::ofstream out(bigmeta, std::ios::binary | std::ios::trunc);
                out.write(big_bytes.data(), static_cast<std::streamsize>(big_bytes.size()));
            }
            TEST_CHECK(ValidateGgufFile(bigmeta, &info) == GgufValidationResult::Ok &&
                           info.architecture == "llama" && info.metadata_count == 3 &&
                           info.tensor_count == 1,
                       "V107 G-01 GGUF: a >=3 MB metadata section validates Ok (windowed scan)");

            // Fixture B: one single STRING value of 4 MB — the large value is
            // skipped by offset, never buffered whole.
            GgufBuilder big2;
            big2.Magic().U32(3).U64(1).U64(2)
                .KvString("general.architecture", "qwen2")
                .KvString("general.description", std::string(4 * 1024 * 1024, 'd'))
                .TensorInfo("probe.weight");
            const std::string big2_bytes = big2.Bytes();
            {
                std::ofstream out(bigstr, std::ios::binary | std::ios::trunc);
                out.write(big2_bytes.data(), static_cast<std::streamsize>(big2_bytes.size()));
            }
            TEST_CHECK(ValidateGgufFile(bigstr, &info) == GgufValidationResult::Ok &&
                           info.architecture == "qwen2",
                       "V107 G-01 GGUF: a single 4 MB metadata value skips cleanly (Ok)");

            // The same large bytes with the tail cut inside the tensor-info
            // record's offset field must still fail — the windowed scan walks
            // the whole metadata AND validates the first tensor record.
            {
                std::ofstream out(bigcut, std::ios::binary | std::ios::trunc);
                out.write(big_bytes.data(),
                          static_cast<std::streamsize>(big_bytes.size() - 8));
            }
            TEST_CHECK(ValidateGgufFile(bigcut, &info) == GgufValidationResult::MalformedHeader,
                       "V107 G-01 GGUF: large metadata truncated in the tensor-info record is MalformedHeader");

            // G-01(d) on the FILE path (the R2-D in-memory twin pins
            // ParseGgufHeader above): truncation immediately after
            // general.architecture is still MalformedHeader.
            const std::string cut_after_arch = GgufBuilder().Magic().U32(3).U64(1).U64(1)
                .KvString("general.architecture", "llama").Bytes();
            {
                std::ofstream out(cutfile, std::ios::binary | std::ios::trunc);
                out.write(cut_after_arch.data(),
                          static_cast<std::streamsize>(cut_after_arch.size()));
            }
            TEST_CHECK(ValidateGgufFile(cutfile, &info) == GgufValidationResult::MalformedHeader,
                       "V107 G-01 GGUF file: truncation right after the arch field is MalformedHeader");

            std::filesystem::remove(bigmeta, ec);
            std::filesystem::remove(bigstr, ec);
            std::filesystem::remove(bigcut, ec);
            std::filesystem::remove(cutfile, ec);
            TEST_CHECK(!std::filesystem::exists(bigmeta) && !std::filesystem::exists(bigstr) &&
                           !std::filesystem::exists(bigcut) && !std::filesystem::exists(cutfile),
                       "V107 G-01 GGUF: large-metadata probe temp files are deleted");
        }
    }

    // =====================================================================
    // 3) Provider failure detail — extraction + the masking contract.
    // =====================================================================
    {
        using emebalachat::ExtractOpenAiErrorMessage;
        using emebalachat::SanitizeOpenAiErrorDetail;

        TEST_CHECK(ExtractOpenAiErrorMessage("").empty(),
                   "V107 OPENAI: empty error body extracts nothing");
        TEST_CHECK(ExtractOpenAiErrorMessage(
                       "{\"error\":{\"message\":\"Credit accounting is temporarily "
                       "unavailable\",\"type\":\"server_error\"}}") ==
                       "Credit accounting is temporarily unavailable",
                   "V107 OPENAI: error.message is extracted from the standard shape");
        TEST_CHECK(ExtractOpenAiErrorMessage("{\"error\":\"plain string\"}") == "plain string",
                   "V107 OPENAI: the plain-string error shape extracts too");
        const std::string raw = ExtractOpenAiErrorMessage("500 Internal\r\nServer Error");
        TEST_CHECK(raw.find('\r') == std::string::npos && raw.find('\n') == std::string::npos &&
                       !raw.empty(),
                   "V107 OPENAI: a non-JSON error body falls back to a control-free prefix");

        // The masking guarantee: the API key can NEVER appear in a detail,
        // even when the provider echoes it back inside the error body.
        const std::string key = "sk-test-abc123456789";
        const std::string echoed =
            "Invalid key " + key + " for this endpoint, key=" + key;
        const std::string masked = SanitizeOpenAiErrorDetail(echoed, key);
        TEST_CHECK(masked.find(key) == std::string::npos,
                   "V107 OPENAI: the API key never survives sanitization");
        TEST_CHECK(masked.find("***") != std::string::npos,
                   "V107 OPENAI: the masked detail carries the *** marker");
        TEST_CHECK(SanitizeOpenAiErrorDetail("HTTP 503: over\n quota\t now", "").find('\n') ==
                       std::string::npos,
                   "V107 OPENAI: the detail is a single line");
        std::string long_msg(500, 'x');
        const std::string bounded = SanitizeOpenAiErrorDetail(long_msg, "");
        TEST_CHECK(bounded.size() <= emebalachat::kMaxOpenAiErrorDetailChars,
                   "V107 OPENAI: the detail is bounded to kMaxOpenAiErrorDetailChars");
        TEST_CHECK(SanitizeOpenAiErrorDetail("", key).empty(),
                   "V107 OPENAI: an empty message sanitizes to empty");
        // The exact 260912 lesson payload, end to end.
        const std::string detail_503 = SanitizeOpenAiErrorDetail(
            ExtractOpenAiErrorMessage("{\"error\":{\"message\":\"Credit accounting is "
                                      "temporarily unavailable\"}}"),
            key);
        TEST_CHECK(detail_503 == "Credit accounting is temporarily unavailable",
                   "V107 OPENAI: the 503 credit-accounting message extracts verbatim");

        // ---- R2-L (261111_0001): the bound never splits a UTF-8 sequence ----
        // 160-char bound: build a message of 158 ASCII chars followed by a
        // 3-byte UTF-8 char (U+20AC EURO SIGN, E2 82 AC). The old per-byte
        // bound cut inside the sequence (E2 82 kept, AC dropped), which
        // ToUtf16 renders as a replacement glyph. The sanitized detail must
        // end on a whole sequence: either the euro sign survives whole or it
        // is dropped entirely — never a partial E2 82 tail.
        {
            const std::string euro = "\xE2\x82\xAC"; // U+20AC, 3 bytes
            std::string msg(158, 'a');
            msg += euro; // bytes 159..161 — straddles the 160-char bound
            const std::string out = SanitizeOpenAiErrorDetail(msg, "");
            TEST_CHECK(out.size() <= emebalachat::kMaxOpenAiErrorDetailChars,
                       "V107 R2-L OPENAI: the multi-byte-straddling detail is still bounded");
            const bool whole_euro = out.size() >= 3 &&
                out.compare(out.size() - 3, 3, euro) == 0;
            const bool no_partial = out.find("\xE2\x82") == std::string::npos ||
                                    whole_euro;
            TEST_CHECK(no_partial,
                       "V107 R2-L OPENAI: the detail never ends mid-UTF-8-sequence (no E2 82 tail)");
            TEST_CHECK(out.find('\xAC') == std::string::npos || whole_euro,
                       "V107 R2-L OPENAI: no lone continuation byte survives the bound");
            // A message whose last in-bound char IS the complete sequence is kept whole.
            std::string msg2(157, 'b');
            msg2 += euro; // bytes 158..160 — exactly reaches the bound
            const std::string out2 = SanitizeOpenAiErrorDetail(msg2, "");
            TEST_CHECK(out2.size() == 160 && out2.compare(157, 3, euro) == 0,
                       "V107 R2-L OPENAI: a multi-byte char that exactly fills the bound survives whole");
            // A multi-byte run straddling the bound stays VALID UTF-8: with
            // flags=0 MultiByteToWideChar (what ToUtf16 uses) maps any invalid
            // byte to U+FFFD, so a clean conversion is only possible when no
            // sequence was split.
            std::string msg3(150, 'c');
            for (int k = 0; k < 10; ++k) {
                msg3 += "\xC3\xA9"; // U+00E9 é, 2 bytes each; bytes 151..170
            }
            const std::string out3 = SanitizeOpenAiErrorDetail(msg3, "");
            TEST_CHECK(out3.size() <= emebalachat::kMaxOpenAiErrorDetailChars,
                       "V107 R2-L OPENAI: a multi-byte run at the bound is still bounded");
            TEST_CHECK(ToUtf16(out3) == ToUtf16(ToUtf8(ToUtf16(out3))),
                       "V107 R2-L OPENAI: the bounded detail round-trips UTF-8 -> UTF-16 -> UTF-8 losslessly (no split sequence)");
        }

        // ---- R2-M (261111_0001, security): bidi controls are stripped ----
        {
            // U+202E (RLO, E2 80 AE) wrapping "evil" — the classic display-spoof.
            const std::string spoof = std::string("\xE2\x80\xAE") + "evil" + "\xE2\x80\xAE";
            const std::string out = SanitizeOpenAiErrorDetail(spoof, "");
            TEST_CHECK(out == "evil",
                       "V107 R2-M OPENAI: U+202E RLO overrides are stripped whole");
            // The full bidi-control set: LRE/RLE/LRO/RLO/PDF (202A-202E),
            // isolates LRI/RLI/FSI/PDI (2066-2069), LRM/RLM (200E/200F),
            // ALM (061C), WJ + invisible ops (2060-2064), BOM (FEFF).
            std::string all;
            const char* const controls[] = {
                "\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE",
                "\xE2\x81\xA6", "\xE2\x81\xA7", "\xE2\x81\xA8", "\xE2\x81\xA9",
                "\xE2\x80\x8E", "\xE2\x80\x8F",
                "\xD8\x9C",
                "\xE2\x81\xA0", "\xE2\x81\xA1", "\xE2\x81\xA2", "\xE2\x81\xA3", "\xE2\x81\xA4",
                "\xEF\xBB\xBF",
            };
            for (const char* c : controls) {
                all += std::string("x") + c + std::string("y");
            }
            const std::string stripped = SanitizeOpenAiErrorDetail(all, "");
            bool any_left = false;
            for (const char* c : controls) {
                if (stripped.find(c) != std::string::npos) {
                    any_left = true;
                    break;
                }
            }
            TEST_CHECK(!any_left,
                       "V107 R2-M OPENAI: every bidi control class (202A-202E/2066-2069/200E-200F/061C/2060-2064/FEFF) is stripped");
            TEST_CHECK(stripped.find('x') != std::string::npos &&
                           stripped.find('y') != std::string::npos,
                       "V107 R2-M OPENAI: the surrounding legitimate text survives");
            // ZWNJ/ZWJ (U+200C/200D) are legitimate text chars (Persian/Indic)
            // and must NOT be stripped.
            const std::string zw = std::string("a\xE2\x80\x8C") + "b" + std::string("\xE2\x80\x8D") + "c";
            TEST_CHECK(SanitizeOpenAiErrorDetail(zw, "").find("\xE2\x80\x8C") != std::string::npos &&
                           SanitizeOpenAiErrorDetail(zw, "").find("\xE2\x80\x8D") != std::string::npos,
                       "V107 R2-M OPENAI: ZWNJ/ZWJ (200C/200D) are kept (legitimate text)");
        }

        // ---- G-02 (261111_0001 round 2, security): the configured API key can
        // NEVER appear in the detail under control-interleaving. A provider
        // body can split the key with a dropped bidi control ("sk-se<U+202E>cret"
        // for key "sk-secret"): the raw key bytes are never contiguous, so a
        // mask-BEFORE-strip pass cannot see them and stripping reassembles the
        // cleartext key. The fix normalizes first and masks after; these cases
        // pin it. The "no fragment" scan covers every key substring of length
        // >= 2 (single bytes are unavoidable in ordinary text like "bad"; a
        // meaningful leak is >= 2 contiguous key bytes).
        {
            const std::string secret_key = "sk-secret";
            auto leaks_key_fragment = [](const std::string& detail,
                                         const std::string& k) {
                for (size_t len = 2; len <= k.size(); ++len) {
                    for (size_t i = 0; i + len <= k.size(); ++i) {
                        if (detail.find(k.substr(i, len)) != std::string::npos) return true;
                    }
                }
                return false;
            };

            // U+202E (RLO, E2 80 AE) split mid-key.
            const std::string rlo_split =
                std::string("bad ") + "sk-se" + "\xE2\x80\xAE" + "cret";
            const std::string rlo_detail = SanitizeOpenAiErrorDetail(rlo_split, secret_key);
            TEST_CHECK(rlo_detail.find(secret_key) == std::string::npos &&
                           !leaks_key_fragment(rlo_detail, secret_key),
                       "V107 G-02 OPENAI: a U+202E-split key never reassembles in the detail");
            TEST_CHECK(rlo_detail.find("***") != std::string::npos,
                       "V107 G-02 OPENAI: the U+202E-split key is masked to ***");

            // U+2066 (LRI) / U+2069 (PDI) isolate split mid-key.
            const std::string iso_split =
                std::string("bad ") + "sk-se" + "\xE2\x81\xA6" + "cret" + "\xE2\x81\xA9";
            const std::string iso_detail = SanitizeOpenAiErrorDetail(iso_split, secret_key);
            TEST_CHECK(iso_detail.find(secret_key) == std::string::npos &&
                           !leaks_key_fragment(iso_detail, secret_key),
                       "V107 G-02 OPENAI: a U+2066/U+2069-split key never reassembles in the detail");

            // End to end through the extractor: the JSON body carries the split
            // as \u-escapes, which the JSON decoder turns into the raw control
            // bytes before sanitization sees them.
            const std::string e2e_rlo = SanitizeOpenAiErrorDetail(
                ExtractOpenAiErrorMessage(
                    "{\"error\":{\"message\":\"bad sk-se\\u202Ecret\"}}"),
                secret_key);
            TEST_CHECK(e2e_rlo.find(secret_key) == std::string::npos &&
                           !leaks_key_fragment(e2e_rlo, secret_key) &&
                           e2e_rlo.find("***") != std::string::npos,
                       "V107 G-02 OPENAI: extract->sanitize masks a \\u202E-split key end to end");
            const std::string e2e_iso = SanitizeOpenAiErrorDetail(
                ExtractOpenAiErrorMessage(
                    "{\"error\":{\"message\":\"bad sk-se\\u2066cret\\u2069\"}}"),
                secret_key);
            TEST_CHECK(e2e_iso.find(secret_key) == std::string::npos &&
                           !leaks_key_fragment(e2e_iso, secret_key),
                       "V107 G-02 OPENAI: extract->sanitize masks a \\u2066/\\u2069-split key end to end");
        }

        // ---- F-4 (261111_0001 round 3, security): the ZERO-WIDTH joins the
        // R2-M bidi set missed. ZWSP (U+200B), SOFT HYPHEN (U+00AD), CGJ
        // (U+034F) and MVS (U+180E) render as NOTHING, so a provider echoing
        // the key split by one of them showed the cleartext key visually
        // intact after the mask pass ran on the un-stripped text. Each row
        // asserts (a) the invisible char is gone and (b) no >= 2-byte
        // fragment of the key survives (the same leak definition as G-02).
        {
            const std::string secret_key = "sk-secret";
            auto leaks_key_fragment = [](const std::string& detail,
                                         const std::string& k) {
                for (size_t len = 2; len <= k.size(); ++len) {
                    for (size_t i = 0; i + len <= k.size(); ++i) {
                        if (detail.find(k.substr(i, len)) != std::string::npos) return true;
                    }
                }
                return false;
            };
            struct InvisibleRow {
                const char* seq;
                const char* name;
            };
            const InvisibleRow rows[] = {
                {"\xE2\x80\x8B", "U+200B ZWSP"},
                {"\xC2\xAD", "U+00AD SOFT HYPHEN"},
                {"\xCD\x8F", "U+034F CGJ"},
                {"\xE1\xA0\x8E", "U+180E MVS"},
            };
            for (const InvisibleRow& row : rows) {
                const std::string split =
                    std::string("bad ") + "sk-se" + row.seq + "cret";
                const std::string detail = SanitizeOpenAiErrorDetail(split, secret_key);
                TEST_CHECK(detail.find(row.seq) == std::string::npos,
                           std::string("V107 F-4 OPENAI: ") + row.name +
                               " is stripped from the detail");
                TEST_CHECK(!leaks_key_fragment(detail, secret_key),
                           std::string("V107 F-4 OPENAI: a ") + row.name +
                               "-split key leaves no >=2-byte key fragment");
                TEST_CHECK(detail.find("***") != std::string::npos,
                           std::string("V107 F-4 OPENAI: a ") + row.name +
                               "-split key is masked to ***");
            }
            // End to end through the JSON extractor: the \u escapes decode to
            // the raw invisible bytes before sanitization sees them.
            const char* const e2e_escapes[] = {"\\u200B", "\\u00AD", "\\u034F", "\\u180E"};
            for (const char* esc : e2e_escapes) {
                const std::string body =
                    std::string("{\"error\":{\"message\":\"bad sk-se") + esc + "cret\"}}";
                const std::string detail =
                    SanitizeOpenAiErrorDetail(ExtractOpenAiErrorMessage(body), secret_key);
                TEST_CHECK(!leaks_key_fragment(detail, secret_key),
                           std::string("V107 F-4 OPENAI: extract->sanitize leaves no key "
                                       "fragment for ") + esc);
            }
            // The strip is targeted, not a blanket zero-width sweep: ZWNJ/ZWJ
            // stay (asserted above in the R2-M block) and an ordinary soft
            // hyphen-free message is untouched.
            TEST_CHECK(SanitizeOpenAiErrorDetail("plain message", secret_key) == "plain message",
                       "V107 F-4 OPENAI: ordinary text is unchanged by the wider strip set");
        }

        // ---- G-11 (261111_0001 round 2): the non-JSON fallback stops on a
        // UTF-8 boundary at the 160-byte cap (it pre-truncates at 160 BYTES,
        // so a multi-byte char straddling the cap used to surface as a
        // replacement glyph after extract->sanitize).
        {
            const std::string euro = "\xE2\x82\xAC"; // U+20AC, 3 bytes
            // 158 ASCII + the euro's first two bytes hit the cap mid-sequence:
            // the incomplete tail is DROPPED.
            const std::string cut = ExtractOpenAiErrorMessage(
                std::string(158, 'a') + euro + "zz");
            TEST_CHECK(cut == std::string(158, 'a'),
                       "V107 G-11 OPENAI: the 160-byte fallback cut drops an incomplete UTF-8 tail");
            // A multi-byte char that exactly fills the cap survives whole.
            const std::string fit = ExtractOpenAiErrorMessage(
                std::string(157, 'b') + euro + "zz");
            TEST_CHECK(fit.size() == 160 && fit.compare(157, 3, euro) == 0,
                       "V107 G-11 OPENAI: a multi-byte char that exactly fills the cap survives whole");
            // A body that simply ends mid-sequence drops the tail too.
            TEST_CHECK(ExtractOpenAiErrorMessage("abc\xE2\x82") == "abc",
                       "V107 G-11 OPENAI: a body ending mid-sequence drops the incomplete tail");
            // Extract -> sanitize end to end at the boundary: bounded, and the
            // UTF-8 -> UTF-16 -> UTF-8 round trip is lossless (a split
            // sequence would map to U+FFFD and break it).
            const std::string e2e = SanitizeOpenAiErrorDetail(
                ExtractOpenAiErrorMessage(std::string(158, 'a') + euro + "tail"), "");
            TEST_CHECK(e2e == std::string(158, 'a') &&
                           e2e.size() <= emebalachat::kMaxOpenAiErrorDetailChars,
                       "V107 G-11 OPENAI: extract->sanitize at the 160-byte boundary stays whole and bounded");
            TEST_CHECK(ToUtf16(e2e) == ToUtf16(ToUtf8(ToUtf16(e2e))),
                       "V107 G-11 OPENAI: the boundary detail round-trips UTF-8 -> UTF-16 -> UTF-8 losslessly");
        }
    }

    // =====================================================================
    // 3b) F-1 / F-7 (261111_0001 round 3, security): the /v1/models walk is
    //     TERMINATING and BOUNDED. ParseOpenAiModelsJson is the pure seam over
    //     the production parser the detached ListModels worker runs, so a
    //     malformed provider body cannot hang a worker (F-1) or flood the
    //     settings combo (F-7) without a check failing first.
    // =====================================================================
    {
        using emebalachat::ParseOpenAiModelsJson;

        // Happy path unchanged: every id, in body order.
        {
            const std::vector<std::string> ids = ParseOpenAiModelsJson(
                "{\"object\":\"list\",\"data\":[{\"id\":\"gpt-a\",\"object\":\"model\"},"
                "{\"id\":\"gpt-b\",\"object\":\"model\"}]}");
            TEST_CHECK(ids.size() == 2 && ids[0] == "gpt-a" && ids[1] == "gpt-b",
                       "V107 F-1 OPENAI: a well-formed model list still returns every id in order");
        }
        // F-1 regression fixtures. Before the fix these calls NEVER returned
        // (SkipValue's scalar arm stops AT the close delimiter without
        // consuming it, and the array loop had no progress guard), so the
        // ListModels worker spun at 100% CPU forever; the checks below could
        // not even be reached. After the fix each body terminates with the
        // ids the walk actually contains.
        {
            TEST_CHECK(ParseOpenAiModelsJson("{\"data\":[}").empty(),
                       "V107 F-1 OPENAI: {\"data\":[} terminates with no ids (no infinite walk)");
            TEST_CHECK(ParseOpenAiModelsJson("{\"data\":[123}]}").empty(),
                       "V107 F-1 OPENAI: {\"data\":[123}]} terminates with no ids (stray '}' consumed)");
            // A stray '}' BEFORE a valid element: the array-level recovery
            // consumes it and the walk still finds the real model.
            const std::vector<std::string> recovered = ParseOpenAiModelsJson(
                "{\"data\":[123},{\"id\":\"after\"}]}");
            TEST_CHECK(recovered.size() == 1 && recovered[0] == "after",
                       "V107 F-1 OPENAI: the array-level stray '}' is consumed and the following id is recovered");
            // Truncated bodies terminate too (no closing ']' at all).
            const std::vector<std::string> truncated =
                ParseOpenAiModelsJson("{\"data\":[{\"id\":\"t\"");
            TEST_CHECK(truncated.size() == 1 && truncated[0] == "t",
                       "V107 F-1 OPENAI: a truncated data array terminates after the last complete id");
            // A non-object body, and a body whose "data" is not an array: the
            // walker returns empty instead of scanning forever.
            TEST_CHECK(ParseOpenAiModelsJson("").empty() &&
                           ParseOpenAiModelsJson("[]").empty() &&
                           ParseOpenAiModelsJson("{\"data\":\"nope\"}").empty() &&
                           ParseOpenAiModelsJson("{\"data\":[{").empty(),
                       "V107 F-1 OPENAI: empty / non-object / non-array / chopped bodies all terminate empty");
        }
        // F-7: the id cap. 600 well-formed model objects -> exactly
        // kMaxOpenAiModelIds ids, in body order, and the walk stops there.
        {
            std::string body = "{\"data\":[";
            for (int i = 0; i < 600; ++i) {
                if (i) body += ",";
                body += "{\"id\":\"m" + std::to_string(i) + "\"}";
            }
            body += "]}";
            const std::vector<std::string> ids = ParseOpenAiModelsJson(body);
            TEST_CHECK(ids.size() == emebalachat::kMaxOpenAiModelIds,
                       "V107 F-7 OPENAI: a 600-model body is capped at kMaxOpenAiModelIds");
            TEST_CHECK(!ids.empty() && ids.front() == "m0" && ids.back() == "m511",
                       "V107 F-7 OPENAI: the cap keeps the FIRST ids of the body, in order");
        }
        // F-7: the per-id length cap. An id one byte over the cap is dropped
        // (not truncated, so a display could never show a mutated id); an id
        // exactly at the cap and a normal neighbour both survive.
        {
            const std::string too_long(emebalachat::kMaxOpenAiModelIdChars + 1, 'L');
            const std::string at_cap(emebalachat::kMaxOpenAiModelIdChars, 'E');
            const std::string body = "{\"data\":[{\"id\":\"" + too_long +
                                     "\"},{\"id\":\"ok-id\"},{\"id\":\"" + at_cap + "\"}]}";
            const std::vector<std::string> ids = ParseOpenAiModelsJson(body);
            TEST_CHECK(ids.size() == 2 && ids[0] == "ok-id" && ids[1] == at_cap,
                       "V107 F-7 OPENAI: an over-long model id is dropped; the at-cap id is kept");
        }
    }

    // =====================================================================
    // 4) The prompt-label accessor — dominance gate + Unknown -> AUTO.
    // =====================================================================
    {
        using emebalachat::DetectLanguageForPrompt;

        TEST_CHECK(DetectLanguageForPrompt(L"") == "Auto Detect",
                   "V107 PROMPT: empty input resolves to the AUTO marker");
        TEST_CHECK(DetectLanguageForPrompt(L"   12345 !!!   ") == "Auto Detect",
                   "V107 PROMPT: non-linguistic input resolves to the AUTO marker");
        // R2-04 repro: one Hangul syllable in an English sentence must not
        // label the prompt source Korean (U2's dominance gate rides through).
        TEST_CHECK(DetectLanguageForPrompt(L"The configuration file is corrupted. \ud55c") !=
                       "Korean",
                   "V107 PROMPT: the mixed-script English sentence is not labeled Korean");
        // Pure-script text keeps its exact script label.
        std::wstring ko = L"한국어로 된 문장입니다.";
        TEST_CHECK(DetectLanguageForPrompt(ko) == "Korean",
                   "V107 PROMPT: Korean-dominant text still reads Korean");
    }

    // =====================================================================
    // 5) Structural pins — the wiring seams the review swarm audits.
    // =====================================================================
    {
        const std::filesystem::path mgr_cpp = ResolveRepoFile(L"src\\ui\\gguf_model_manager_window.cpp");
        TEST_CHECK(!mgr_cpp.empty(), "V107 pin: manager window source resolves");
        if (!mgr_cpp.empty()) {
            std::ifstream in(mgr_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            // Registration gate wired into BOTH registration paths that land
            // in this TU (the file-picker pipeline is main.cpp-owned).
            TEST_CHECK(src.find("RejectInvalidGguf(dlg, st->worker_tmp)") != std::string::npos,
                       "V107 pin: the completed HF download is validated before registration");
            TEST_CHECK(src.find("RejectInvalidGguf(dlg, models_dir / ToUtf16(filename))") !=
                           std::string::npos,
                       "V107 pin: the orphan-file reuse path is validated before registration");
            // The activate action: double-click + the same config binding
            // statement set as the add-pipeline reuse paths.
            TEST_CHECK(src.find("LBN_DBLCLK") != std::string::npos &&
                           src.find("StringId::GgufManagerActivated") != std::string::npos &&
                           src.find("StringId::GgufManagerActiveSuffix") != std::string::npos,
                       "V107 pin: the manager carries the activate handler + active marker");
            const size_t dblclk = src.find("LBN_DBLCLK");
            const size_t bind = src.find("SetUserModelId(id)");
            // SaveToFile() occurs in earlier manager flows too (rename/
            // HF reuse) — pin the FIRST persist after the activate handler.
            const size_t save = src.find("SaveToFile()", dblclk);
            TEST_CHECK(dblclk != std::string::npos && bind != std::string::npos &&
                           save != std::string::npos && dblclk < bind && bind < save,
                       "V107 pin: activate binds user_model_id and persists it");
        }

        const std::filesystem::path worker_cpp = ResolveRepoFile(L"src\\worker.cpp");
        if (!worker_cpp.empty()) {
            std::ifstream in(worker_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("DetectLanguageForPrompt(engine_input)") != std::string::npos,
                       "V107 pin: the typing-path prompt label uses the gated accessor");
            TEST_CHECK(src.find("LastChatErrorDetail") != std::string::npos,
                       "V107 pin: the typing-path failure log reads the sanitized slot");
        }

        const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
        if (!main_cpp.empty()) {
            std::ifstream in(main_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("DetectLanguageForPrompt(selected)") != std::string::npos,
                       "V107 pin: the drag-path label uses the gated accessor");
            TEST_CHECK(src.find("LastChatErrorDetail") != std::string::npos,
                       "V107 pin: the drag failure notice carries the provider detail");
        }

        const std::filesystem::path client_cpp = ResolveRepoFile(L"src\\openai_compatible_client.cpp");
        if (!client_cpp.empty()) {
            std::ifstream in(client_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            // R2-P: the sanitize + key-mask + bound passes live INSIDE the
            // single failure recorder (fail()), so every recorded detail is
            // guaranteed masked — not at the individual call sites.
            TEST_CHECK(src.find("SanitizeOpenAiErrorDetail(raw_detail, key)") != std::string::npos,
                       "V107 R2-P pin: the failure recorder sanitizes + masks at the single write site");
            // R2-E: the unlocalized English failure literals are gone from
            // the provider path (the localized notice already carries the
            // message).
            TEST_CHECK(src.find("fail(\"network error\")") == std::string::npos &&
                           src.find("fail(\"invalid response\")") == std::string::npos,
                       "V107 R2-E pin: no unlocalized 'network error' / 'invalid response' literals in the client");
        }
    }

    const int failed = g_failed_count - failures_before;
    // Report the suite delta to the caller WITHOUT double counting: when the
    // caller passes the harness global itself, it is already authoritative.
    if (&failures != &g_failed_count) {
        failures += failed;
    }
    if (failed == 0) {
        std::cout << "[PASS] v0.10.7 user-model surface passed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.7 user-model surface: " << failed
                  << " check(s) failed." << std::endl;
    }
    return failed;
}
