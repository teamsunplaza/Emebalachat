// ---------------------------------------------------------------------------
// REQ-UC (session 260930_0004): update checker v1 — headless pins for the
// pure logic in src/update_checker.hpp (updatelogic:: + the asset picker),
// plus one functional BCrypt leg (ComputeFileSha256Hex on a temp file).
// The network seams (CheckForUpdate / DownloadUpdate) are deliberately NOT
// exercised here: they need a live socket and are thin wrappers verified by
// inspection + the quiet-failure contract in the About window.
// Staged as an .inc next to this runner; registered at the end of main(),
// per the end-of-file pattern.
// ---------------------------------------------------------------------------

void TestUpdateCheckerVersionParse() {
    std::cout << "[RUN] Testing REQ-UC version parse + compare..." << std::endl;
    const int failures_before = g_failed_count;

    // Parse matrix: equal/older/newer/v-prefix/malformed/prerelease-suffix.
    const auto v_full = updatelogic::ParseVersion("0.10.2");
    TEST_CHECK(v_full && v_full->major == 0 && v_full->minor == 10 && v_full->patch == 2,
               "UC: 0.10.2 parses to the (0,10,2) triplet");
    const auto v_prefix = updatelogic::ParseVersion("v0.11.0");
    TEST_CHECK(v_prefix && v_prefix->major == 0 && v_prefix->minor == 11 && v_prefix->patch == 0,
               "UC: v0.11.0 parses with the leading v");
    const auto v_upper = updatelogic::ParseVersion("V2");
    TEST_CHECK(v_upper && v_upper->major == 2 && v_upper->minor == 0 && v_upper->patch == 0,
               "UC: 'V2' parses (uppercase V, single component)");
    const auto v_two = updatelogic::ParseVersion("1.2");
    TEST_CHECK(v_two && v_two->major == 1 && v_two->minor == 2 && v_two->patch == 0,
               "UC: '1.2' parses (two components)");
    const auto v_pre = updatelogic::ParseVersion("v0.10.2-beta.3");
    TEST_CHECK(v_pre && v_pre->major == 0 && v_pre->minor == 10 && v_pre->patch == 2,
               "UC: the -prerelease suffix is stripped before the compare");
    const auto v_build = updatelogic::ParseVersion("1.2+build.7");
    TEST_CHECK(v_build && v_build->major == 1 && v_build->minor == 2 && v_build->patch == 0,
               "UC: the +build suffix is stripped before the compare");

    // Malformed inputs must NEVER parse (fail-closed: garbage cannot read
    // as "newer" than the running build).
    TEST_CHECK(!updatelogic::ParseVersion(""), "UC: empty string is malformed");
    TEST_CHECK(!updatelogic::ParseVersion("v"), "UC: bare 'v' is malformed");
    TEST_CHECK(!updatelogic::ParseVersion("1.2.3.4"), "UC: 4 components are malformed");
    TEST_CHECK(!updatelogic::ParseVersion("0.10.x"), "UC: non-numeric component is malformed");
    TEST_CHECK(!updatelogic::ParseVersion("-1.2"), "UC: a leading minus is malformed");
    TEST_CHECK(!updatelogic::ParseVersion("1..2"), "UC: an empty component is malformed");
    TEST_CHECK(!updatelogic::ParseVersion("latest"), "UC: a word tag is malformed");

    // Three-way compare matrix.
    const auto a = updatelogic::ParseVersion("0.10.2");
    const auto b = updatelogic::ParseVersion("0.11.0");
    const auto c = updatelogic::ParseVersion("0.10.3");
    const auto d = updatelogic::ParseVersion("0.9.9");
    TEST_CHECK(updatelogic::CompareVersions(*a, *a) == 0, "UC: equal versions compare 0");
    TEST_CHECK(updatelogic::CompareVersions(*b, *a) > 0, "UC: 0.11.0 > 0.10.2");
    TEST_CHECK(updatelogic::CompareVersions(*a, *b) < 0, "UC: 0.10.2 < 0.11.0");
    TEST_CHECK(updatelogic::CompareVersions(*c, *a) > 0, "UC: patch bump is newer");
    TEST_CHECK(updatelogic::CompareVersions(*a, *d) > 0, "UC: 0.10.2 > 0.9.9 (minor over patch)");

    // The offer decision against the running build (kAppVersionA == 0.10.2).
    const auto current = updatelogic::ParseVersion(kAppVersionA);
    TEST_CHECK(current && updatelogic::CompareVersions(*current, *a) == 0,
               "UC: kAppVersionA parses and equals 0.10.2");
    TEST_CHECK(updatelogic::CompareVersions(*b, *current) > 0,
               "UC: v0.11.0 offered over 0.10.2 -> UpdateAvailable");
    TEST_CHECK(updatelogic::CompareVersions(*a, *current) == 0,
               "UC: v0.10.2 offered over 0.10.2 -> UpToDate (never self-update)");
    TEST_CHECK(updatelogic::CompareVersions(*d, *current) < 0,
               "UC: v0.9.9 offered over 0.10.2 -> UpToDate");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC version parse + compare passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC version parse + compare: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerJsonExtraction() {
    std::cout << "[RUN] Testing REQ-UC release JSON extraction..." << std::endl;
    const int failures_before = g_failed_count;

    // Full GitHub-shaped payload: nested objects, escapes, two assets.
    const std::string payload = R"json({
        "url": "https://api.github.com/repos/teamsunplaza/Emebalachat/releases/1",
        "tag_name": "v0.11.0",
        "name": "Emebala Chat 0.11.0",
        "html_url": "https://github.com/teamsunplaza/Emebalachat/releases/tag/v0.11.0",
        "body": "## What's new\r\nSHA256: AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233\r\nEnjoy!",
        "assets": [
            { "name": "checksums.txt", "size": 256,
              "browser_download_url": "https://github.com/teamsunplaza/Emebalachat/releases/download/v0.11.0/checksums.txt" },
            { "name": "Emebalachat_Setup_0.11.0.exe", "size": 734003200,
              "browser_download_url": "https://github.com/teamsunplaza/Emebalachat/releases/download/v0.11.0/Emebalachat_Setup_0.11.0.exe" }
        ],
        "draft": false,
        "prerelease": false
    })json";

    const auto info = updatelogic::ParseLatestRelease(payload);
    TEST_CHECK(info.has_value(), "UC: full payload parses");
    if (info) {
        TEST_CHECK(info->tag == "v0.11.0", "UC: tag_name extracted");
        TEST_CHECK(info->html_url == "https://github.com/teamsunplaza/Emebalachat/releases/tag/v0.11.0",
                   "UC: html_url extracted");
        TEST_CHECK(info->body.find("SHA256:") != std::string::npos,
                   "UC: body extracted with the hash line inside");
        TEST_CHECK(info->assets.size() == 2, "UC: both assets extracted");
        if (info->assets.size() == 2) {
            TEST_CHECK(info->assets[1].name == "Emebalachat_Setup_0.11.0.exe",
                       "UC: asset name extracted");
            TEST_CHECK(info->assets[1].size == 734003200, "UC: asset size parsed (uint64)");
            TEST_CHECK(info->assets[0].size == 256, "UC: small asset size parsed");
            TEST_CHECK(info->assets[1].download_url.find("/v0.11.0/") != std::string::npos,
                       "UC: asset download URL extracted");
        }
        // The picker prefers the .exe even though it is not the first asset.
        const auto* pick = PickInstallerAsset(*info);
        TEST_CHECK(pick && pick->name == "Emebalachat_Setup_0.11.0.exe",
                   "UC: PickInstallerAsset prefers the .exe asset");
    }

    // Fallback: no .exe anywhere -> first asset with a URL wins.
    {
        const std::string no_exe = R"json({"tag_name":"v0.11.0","assets":[
            {"name":"a.bin","size":10,"browser_download_url":"https://example.com/a.bin"},
            {"name":"b.bin","size":20,"browser_download_url":"https://example.com/b.bin"}]})json";
        const auto info2 = updatelogic::ParseLatestRelease(no_exe);
        TEST_CHECK(info2 && info2->assets.size() == 2, "UC: non-exe payload parses");
        if (info2) {
            const auto* pick = PickInstallerAsset(*info2);
            TEST_CHECK(pick && pick->name == "a.bin",
                       "UC: PickInstallerAsset falls back to the first URL-bearing asset");
        }
    }

    // Fail-closed: missing tag_name / malformed JSON / not an object.
    TEST_CHECK(!updatelogic::ParseLatestRelease(R"({"name":"no tag here"})"),
               "UC: missing tag_name fails closed");
    TEST_CHECK(!updatelogic::ParseLatestRelease("not json at all"),
               "UC: non-JSON fails closed");
    TEST_CHECK(!updatelogic::ParseLatestRelease("[\"tag_name\"]"),
               "UC: a bare array fails closed");
    TEST_CHECK(!updatelogic::ParseLatestRelease("{\"tag_name\":42}"),
               "UC: a non-string tag fails closed");
    TEST_CHECK(!updatelogic::ParseLatestRelease("{\"tag_name\":\"\"}"),
               "UC: an empty tag fails closed");
    TEST_CHECK(!updatelogic::ParseLatestRelease("{\"tag_name\":\"v1\", \"body\": \"unterminated"),
               "UC: truncated JSON fails closed");

    // Nested look-alikes must not leak: an object holding a "tag_name" inside
    // "assets" is not the release tag.
    {
        const std::string nested = R"json({"assets":[{"tag_name":"v9.9.9","size":1}],
            "body":"x"})json";
        TEST_CHECK(!updatelogic::ParseLatestRelease(nested),
                   "UC: a nested tag_name does not satisfy the top-level tag");
    }

    // An asset entry without a URL is unpickable and skipped.
    {
        const std::string no_url = R"json({"tag_name":"v0.11.0","assets":[
            {"name":"notes.txt","size":5}]})json";
        const auto info3 = updatelogic::ParseLatestRelease(no_url);
        TEST_CHECK(info3 && info3->assets.empty(),
                   "UC: an entry without browser_download_url is skipped");
        if (info3) {
            TEST_CHECK(PickInstallerAsset(*info3) == nullptr,
                       "UC: no pickable asset -> nullptr (the UI falls to the quiet retry state)");
        }
    }

    // Empty assets array is fine.
    {
        const std::string empty_assets = R"json({"tag_name":"v0.11.0","assets":[]})json";
        const auto info4 = updatelogic::ParseLatestRelease(empty_assets);
        TEST_CHECK(info4 && info4->assets.empty(), "UC: an empty assets array parses");
    }

    // Escapes decode inside strings.
    {
        const std::string esc = R"json({"tag_name":"v1.0.0","body":"line1\nline2\t\"q\" SHA256: aabbccdd00112233aabbccdd00112233aabbccdd00112233aabbccdd00112233"})json";
        const auto info5 = updatelogic::ParseLatestRelease(esc);
        TEST_CHECK(info5 && info5->body.find('\n') != std::string::npos &&
                       info5->body.find('\t') != std::string::npos &&
                       info5->body.find("\"q\"") != std::string::npos,
                   "UC: JSON escapes decode inside the body");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC release JSON extraction passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC release JSON extraction: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerHashLineParser() {
    std::cout << "[RUN] Testing REQ-UC release-notes hash line parser..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string kLower =
        "aabbccdd00112233aabbccdd00112233aabbccdd00112233aabbccdd00112233";
    const std::string kUpper =
        "AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233";

    // Present: whole-line and after-prose forms; case-insensitive; the digest
    // normalizes to lowercase.
    const auto h1 = updatelogic::ExtractSha256FromNotes("SHA256: " + kUpper);
    TEST_CHECK(h1 && *h1 == kLower, "UC: uppercase hex normalizes to lowercase");
    const auto h2 = updatelogic::ExtractSha256FromNotes(
        "First line\n  SHA256:" + kLower + "\nLast line");
    TEST_CHECK(h2 && *h2 == kLower, "UC: a mid-body indented line is found");
    const auto h3 = updatelogic::ExtractSha256FromNotes("SHA256:" + kLower + "\r\n");
    TEST_CHECK(h3 && *h3 == kLower, "UC: CRLF line endings are tolerated");

    // Absent -> nullopt (fail-closed, never a silent pass).
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes(""),
               "UC: empty release notes carry no hash");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("just some notes\nnothing here"),
               "UC: notes without the line yield nullopt");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("sha256: " + kLower),
               "UC: the prefix is case-SENSITIVE (spec: 'SHA256:')");

    // Malformed -> nullopt (a bad hash line is a verification failure).
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("SHA256:"),
               "UC: a bare prefix is malformed");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("SHA256:  "),
               "UC: whitespace-only after the prefix is malformed");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("SHA256:" + kLower.substr(0, 63)),
               "UC: 63 hex chars is malformed");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("SHA256:" + kLower + "aa"),
               "UC: 66 hex chars is malformed");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes("SHA256:" + kLower.substr(0, 63) + "g"),
               "UC: a non-hex char is malformed");
    TEST_CHECK(!updatelogic::ExtractSha256FromNotes(
                   "prose SHA256: " + kLower),
               "UC: the prefix must start the line (after optional blanks)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC hash line parser passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC hash line parser: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerDownloadTargetPath() {
    std::cout << "[RUN] Testing REQ-UC download target path builder..." << std::endl;
    const int failures_before = g_failed_count;

    TEST_CHECK(updatelogic::BuildDownloadTargetPath(L"C:\\Temp", "v0.11.0") ==
                   L"C:\\Temp\\Emebalachat_Update_0.11.0.exe",
               "UC: the v-prefix is stripped from the file name");
    TEST_CHECK(updatelogic::BuildDownloadTargetPath(L"C:\\Temp\\", "0.10.2") ==
                   L"C:\\Temp\\Emebalachat_Update_0.10.2.exe",
               "UC: a trailing backslash is not doubled");
    TEST_CHECK(updatelogic::BuildDownloadTargetPath(L"C:\\Temp", "1.0-beta_2") ==
                   L"C:\\Temp\\Emebalachat_Update_1.0-beta_2.exe",
               "UC: dots/dashes/underscores survive sanitization");

    // Hostile tags can never escape the temp dir.
    const std::wstring evil = updatelogic::BuildDownloadTargetPath(L"C:\\Temp", "..\\..\\evil");
    TEST_CHECK(evil.find(L"/") == std::wstring::npos &&
                   evil.find(L"Emebalachat_Update_") != std::wstring::npos,
               "UC: path separators are stripped from the tag");
    TEST_CHECK(evil.find(L"..\\") == std::wstring::npos,
               "UC: no parent-directory segment survives");
    const std::wstring empty_tag = updatelogic::BuildDownloadTargetPath(L"C:\\Temp", "@@@");
    TEST_CHECK(empty_tag.find(L"Emebalachat_Update_update.exe") != std::wstring::npos,
               "UC: a fully-sanitized-away tag falls back to 'update'");

    // No env expansion, no env dependence: the caller passes %TEMP% verbatim.
    TEST_CHECK(updatelogic::BuildDownloadTargetPath(L"%TEMP%", "2.0").find(L"%TEMP%\\Emebalachat_Update_2.0.exe") == 0,
               "UC: the builder never expands the temp dir (caller's job)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC download target path builder passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC download target path builder: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerFormattingAndUrl() {
    std::cout << "[RUN] Testing REQ-UC size formatting + https URL split..." << std::endl;
    const int failures_before = g_failed_count;

    // Size formatting (nearest MiB).
    TEST_CHECK(updatelogic::FormatSizeMb(734003200) == L"700 MB",
               "UC: 700 MiB reads '700 MB'");
    TEST_CHECK(updatelogic::FormatSizeMb(0) == L"0 MB", "UC: zero bytes reads '0 MB'");
    TEST_CHECK(updatelogic::FormatSizeMb(1) == L"1 MB", "UC: a non-empty payload never reads 0 MB");
    TEST_CHECK(updatelogic::FormatSizeMb(1024 * 1024) == L"1 MB", "UC: exactly 1 MiB");
    TEST_CHECK(updatelogic::FormatProgressMb(352321536, 734003200) == L"336 / 700 MB",
               "UC: the progress meta line floors to MiB");

    // https URL split for WinHTTP.
    const auto u1 = updatelogic::SplitHttpsUrl(
        "https://api.github.com/repos/teamsunplaza/Emebalachat/releases/latest");
    TEST_CHECK(u1 && u1->first == "api.github.com" &&
                   u1->second == "/repos/teamsunplaza/Emebalachat/releases/latest",
               "UC: api.github.com URL splits into host + path");
    const auto u2 = updatelogic::SplitHttpsUrl("https://objects.githubusercontent.com/x?y=1");
    TEST_CHECK(u2 && u2->first == "objects.githubusercontent.com" && u2->second == "/x?y=1",
               "UC: query strings ride the path");
    const auto u3 = updatelogic::SplitHttpsUrl("https://example.com");
    TEST_CHECK(u3 && u3->first == "example.com" && u3->second == "/",
               "UC: a host-only URL gets the root path");

    // Fail-closed: only plain https hosts.
    TEST_CHECK(!updatelogic::SplitHttpsUrl("http://example.com/a"),
               "UC: plaintext http is rejected (fail-closed)");
    TEST_CHECK(!updatelogic::SplitHttpsUrl("ftp://example.com/a"),
               "UC: ftp is rejected");
    TEST_CHECK(!updatelogic::SplitHttpsUrl("https://user@example.com/a"),
               "UC: userinfo is rejected");
    TEST_CHECK(!updatelogic::SplitHttpsUrl("https://example.com:8080/a"),
               "UC: an explicit port is rejected for v1");
    TEST_CHECK(!updatelogic::SplitHttpsUrl("example.com/a"),
               "UC: a missing scheme is rejected");
    TEST_CHECK(!updatelogic::SplitHttpsUrl("HTTPS://example.com/a"),
               "UC: the scheme is case-sensitive lowercase (rejected, fail-closed)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC size formatting + URL split passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC size formatting + URL split: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerFileSha256() {
    std::cout << "[RUN] Testing REQ-UC streaming SHA-256 (CNG)..." << std::endl;
    const int failures_before = g_failed_count;

    // Functional leg: hash a temp file with known content and compare against
    // the independently computed digest (stdlib-verifiable constant).
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / L"emebala_uc_sha_test";
    if (ec) {
        TEST_CHECK(false, "UC: temp dir unavailable for the SHA fixture");
    } else {
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / L"payload.bin";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "abc"; // SHA-256("abc") is a well-known digest
        }
        std::string hex;
        TEST_CHECK(ComputeFileSha256Hex(file.wstring(), hex),
                   "UC: ComputeFileSha256Hex succeeds on a readable file");
        TEST_CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                   "UC: the digest matches the independently known SHA-256('abc')");
        TEST_CHECK(!ComputeFileSha256Hex((dir / L"missing.bin").wstring(), hex) && hex.empty(),
                   "UC: a missing file hashes to failure (no digest)");

        // Bigger-than-one-chunk file: 3 MiB of 'A' — verifies the streaming
        // loop crosses the 1 MiB buffer boundary. Digest computed externally:
        // python -c "import hashlib;print(hashlib.sha256(b'A'*(3*1024*1024)).hexdigest())"
        const std::filesystem::path big = dir / L"big.bin";
        {
            std::ofstream out(big, std::ios::binary | std::ios::trunc);
            std::vector<char> chunk(4096, 'A');
            for (int i = 0; i < 768; ++i) out.write(chunk.data(), chunk.size());
        }
        std::string big_hex;
        TEST_CHECK(ComputeFileSha256Hex(big.wstring(), big_hex) &&
                       big_hex == "a57493c037eb3a8a86ff87072edde226110455cd7fdab2f87bb4df072f67ffb0",
                   "UC: a 3 MiB payload hashes across chunk boundaries");
        std::error_code ec2;
        std::filesystem::remove_all(dir, ec2);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC streaming SHA-256 passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC streaming SHA-256: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUpdateCheckerAboutSeam() {
    std::cout << "[RUN] Testing REQ-UC About-window seams (state enum + token replace)..." << std::endl;
    const int failures_before = g_failed_count;

    // The zone state machine exposes exactly the spec's states (compile-time
    // pinning keeps Render/WndProc in lockstep with the tests).
    static_assert(static_cast<int>(AboutWindow::UpdateZoneState::Idle) == 0 &&
                      static_cast<int>(AboutWindow::UpdateZoneState::ErrHash) == 10,
                  "REQ-UC: the 11 zone states keep their declaration order");

    // {v}/{s} placeholder replacement (the consent dialog rows).
    TEST_CHECK(AboutWindow::ReplaceToken(L"you have {v}", L"{v}", L"v0.10.2") ==
                   L"you have v0.10.2",
               "UC: {v} replaced with the running version");
    TEST_CHECK(AboutWindow::ReplaceToken(L"about {s} / about {s}", L"{s}", L"700 MB") ==
                   L"about 700 MB / about 700 MB",
               "UC: every {s} occurrence is replaced");
    TEST_CHECK(AboutWindow::ReplaceToken(L"no token", L"{v}", L"x") == L"no token",
               "UC: a token-free string passes through unchanged");
    TEST_CHECK(AboutWindow::ReplaceToken(L"{v}", L"{v}", L"") == L"",
               "UC: an empty replacement empties the token");

    // The {v}/{s} placeholders survive in the i18n tables (all 37 locales),
    // and the ko/en master strings are byte-exact to the spec.
    const UiLocale initial = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::UpdateCheck) == L"업데이트 확인",
               "UC: ko UpdateCheck matches the spec master table");
    TEST_CHECK(I18n::Get(StringId::UpdateDlgPrivacy) == L"확인/다운로드 시 GitHub에 연결됩니다.",
               "UC: ko privacy line matches the spec (the GitHub consent notice)");
    TEST_CHECK(I18n::Get(StringId::UpdateDlgVersionCur) == L"현재 {v}",
               "UC: ko version parenthetical keeps the {v} token");
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::UpdateErrHash) ==
                   L"The downloaded file did not pass verification.",
               "UC: en hash-mismatch line matches the spec");
    TEST_CHECK(I18n::Get(StringId::UpdateDlgSizeApprox) == L"about {s}",
               "UC: en size parenthetical keeps the {s} token");

    // Every new StringId resolves non-empty in ALL 37 selectable locales
    // (the EnumCount completeness loop in TestR6P5P6I18n covers the whole
    // table; this is the per-key pin with a clear failure message).
    int ok = 0;
    for (const auto& entry : GetSupportedUiLocales()) {
        I18n::SetLocale(entry.locale);
        bool all = true;
        for (int id = static_cast<int>(StringId::UpdateCheck);
             id <= static_cast<int>(StringId::UpdateSnoozedLine); ++id) {
            if (I18n::Get(static_cast<StringId>(id)).empty()) all = false;
        }
        // The four placeholder-bearing strings keep their token intact.
        if (I18n::Get(StringId::UpdateDlgVersionCur).find(L"{v}") == std::wstring::npos) all = false;
        if (I18n::Get(StringId::UpdateDlgSizeApprox).find(L"{s}") == std::wstring::npos) all = false;
        if (all) ++ok;
    }
    TEST_CHECK(ok == 37, "UC: all 25 update strings non-empty (+tokens intact) in all 37 locales");
    I18n::SetLocale(initial);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC About-window seams passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC About-window seams: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-UC SEC (session 260930_0004 security remediation): pins for the three
// audit findings + the LOW one-liners. All pure seams — no network needed.
// ---------------------------------------------------------------------------

void TestUpdateCheckerSecurityPins() {
    std::cout << "[RUN] Testing REQ-UC security remediation pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- Finding 3 (MEDIUM, CWE-674): bounded JSON recursion ----------------
    // 60-deep nesting parses; 70-deep fails closed (cap = 64). The fixture is
    // built programmatically so the exact boundary is pinned forever.
    {
        const std::string ok60 = std::string("{\"tag_name\":\"v1\",\"x\":") +
                                 std::string(60, '[') + std::string(60, ']') + "}";
        TEST_CHECK(updatelogic::ParseLatestRelease(ok60).has_value(),
                   "UC-SEC: 60-deep nesting parses (under the depth cap)");
        const std::string bad70 = std::string("{\"tag_name\":\"v1\",\"x\":") +
                                  std::string(70, '[') + std::string(70, ']') + "}";
        TEST_CHECK(!updatelogic::ParseLatestRelease(bad70).has_value(),
                   "UC-SEC: 70-deep nesting fails closed (CWE-674 depth cap)");
        const std::string bad_obj = std::string("{\"tag_name\":\"v1\",\"x\":") +
                                    std::string(70, '{') + std::string(70, '}') + "}";
        TEST_CHECK(!updatelogic::ParseLatestRelease(bad_obj).has_value(),
                   "UC-SEC: 70-deep nested OBJECTS fail closed too");
        // 64 exactly at the cap passes, 65 fails (boundary pins).
        const std::string edge64 = std::string("{\"tag_name\":\"v1\",\"x\":") +
                                   std::string(64, '[') + std::string(64, ']') + "}";
        TEST_CHECK(updatelogic::ParseLatestRelease(edge64).has_value(),
                   "UC-SEC: nesting exactly at the 64 cap parses");
        const std::string edge65 = std::string("{\"tag_name\":\"v1\",\"x\":") +
                                   std::string(65, '[') + std::string(65, ']') + "}";
        TEST_CHECK(!updatelogic::ParseLatestRelease(edge65).has_value(),
                   "UC-SEC: one past the cap fails closed");
    }

    // ---- Finding 2 (MEDIUM): redirect-target host allowlist -----------------
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("api.github.com"),
               "UC-SEC: api.github.com is allowlisted");
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("github.com"),
               "UC-SEC: github.com is allowlisted");
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("objects.githubusercontent.com"),
               "UC-SEC: objects.githubusercontent.com is allowlisted");
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("release-assets.githubusercontent.com"),
               "UC-SEC: release-assets.githubusercontent.com is allowlisted");
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("a.b.githubusercontent.com"),
               "UC-SEC: deeper *.githubusercontent.com labels are allowlisted");
    TEST_CHECK(!updatelogic::IsAllowedUpdateHost("githubusercontent.com"),
               "UC-SEC: the bare githubusercontent.com domain is NOT allowlisted");
    TEST_CHECK(!updatelogic::IsAllowedUpdateHost("github.com.evil.com"),
               "UC-SEC: look-alike suffix hosts are refused");
    TEST_CHECK(!updatelogic::IsAllowedUpdateHost("notgithub.com"),
               "UC-SEC: unrelated hosts are refused");
    TEST_CHECK(!updatelogic::IsAllowedUpdateHost("api.github.com."),
               "UC-SEC: a trailing-dot host is refused");
    TEST_CHECK(!updatelogic::IsAllowedUpdateHost(""),
               "UC-SEC: an empty host is refused");

    // ---- Finding 1 (HIGH, CWE-400): download size cap ------------------------
    const std::uint64_t kGiB = 1024ull * 1024 * 1024;
    const std::uint64_t kMiB = 1024ull * 1024;
    const std::uint64_t kAdvertised = 734003200; // ~700 MB installer
    using updatelogic::CheckDownloadCap;
    using updatelogic::DownloadCapStatus;

    TEST_CHECK(CheckDownloadCap(100 * kMiB, kAdvertised, kAdvertised) ==
                   DownloadCapStatus::WithinCap,
               "UC-SEC: a normal in-flight download is within cap");
    TEST_CHECK(CheckDownloadCap(0, 3 * kGiB, 0) == DownloadCapStatus::OverAbsoluteCap,
               "UC-SEC: a fabricated 3 GiB Content-Length dies before streaming");
    TEST_CHECK(CheckDownloadCap(2 * kGiB + 1, 0, 0) == DownloadCapStatus::OverAbsoluteCap,
               "UC-SEC: the stream itself is capped at 2 GiB");
    TEST_CHECK(CheckDownloadCap(0, 2 * kGiB, 0) == DownloadCapStatus::WithinCap,
               "UC-SEC: exactly 2 GiB Content-Length is still within the cap");
    // Advertised cross-check: bound = advertised*2 + 64 MiB = 1535115264.
    const std::uint64_t bound = kAdvertised * 2 + 64 * kMiB;
    TEST_CHECK(CheckDownloadCap(bound, 0, kAdvertised) == DownloadCapStatus::WithinCap,
               "UC-SEC: received == bound is within cap (boundary)");
    TEST_CHECK(CheckDownloadCap(bound + 1, 0, kAdvertised) ==
                   DownloadCapStatus::OverAdvertisedCap,
               "UC-SEC: received one past the advertised bound fails fast");
    TEST_CHECK(CheckDownloadCap(0, bound + 1, kAdvertised) ==
                   DownloadCapStatus::OverAdvertisedCap,
               "UC-SEC: Content-Length past the advertised bound fails pre-stream");
    TEST_CHECK(CheckDownloadCap(bound + 1, 0, 0) == DownloadCapStatus::WithinCap,
               "UC-SEC: with no advertised size only the absolute cap applies (slack leg off)");
    TEST_CHECK(CheckDownloadCap(100, 0, UINT64_MAX) == DownloadCapStatus::WithinCap,
               "UC-SEC: a garbage clamped advertised size cannot overflow the bound math");

    // ---- LOW (Finding 5): the .exe preference is truly case-insensitive ------
    {
        updatelogic::ReleaseInfo ri;
        ri.tag = "v1.0.0";
        ri.assets.push_back({"notes.txt", "https://github.com/x/y/download/v1/notes.txt", 5});
        ri.assets.push_back({"SETUP.EXE", "https://github.com/x/y/download/v1/SETUP.EXE", 700});
        const auto* pick = PickInstallerAsset(ri);
        TEST_CHECK(pick && pick->name == "SETUP.EXE",
                   "UC-SEC: PickInstallerAsset prefers an uppercase .EXE");
    }

    // ---- LOW (Finding 6): control chars cannot forge log lines ---------------
    TEST_CHECK(update_detail::SanitizeForDiag("v1\r\n0.2\t\x7f") ==
                   std::string("v1??0.2??"),
               "UC-SEC: SanitizeForDiag neutralizes CR/LF/TAB/DEL in network-derived strings");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-UC security remediation pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-UC security remediation pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
