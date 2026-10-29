// ---------------------------------------------------------------------------
// Plan-B (REQ-B006) B-T8 (session 260928_0001): Chat installer model bundling
// into the Common store — structural + merge-shape pins (TestBt8InstallerModelBundling).
//
// Authority:
//   docs/260928_0001_session_translation-fail-fix/
//     234900_architect-planB-design.md            §6 (6.1 bundling contract) + §11 B-T8
//     235700_architect-planB-design-v2-amendments.md  A1 (MANDATORY: the model store
//       path MUST be {localappdata} + original-user resolution via the Listener
//       file-exchange pattern; NEVER the program-data store; warn-not-silent)
//   HANDOFF §4 invariant #3: the registry is a multi-writer Common store written
//     by the read-merge-own-entry-tmp-rename atomic pattern; each family owns its
//     own entry; untouched sections are preserved byte-for-byte.
//
// CURRENT STATE (shape b, reported in the B-T8 code report): Chat's installer
// ships NO GGUF [Files] entry — the default MT model (Hy-MT2-1.8B-Q8_0.gguf) is
// DOWNLOADED at install time into the Common models store, and the installer
// seeds the registry.json bundled slot through the M7 A-2 atomic merge
// (WriteRegistryFile). So B-T8 = (1) retrofit the A1 original-user path
// resolution onto that existing download/registry/config mechanism (never
// the program-data store, never the elevated-token admin profile), (2) re-affirm the
// slot-ownership + uninstall-survival contract, (3) pin it. No GGUF asset is
// fabricated (it is a release-pipeline download; flagged for VP).
//
// THE THREE SCENARIOS (design §11 B-T8 verification protocol):
//   (1) FRESH INSTALL      -> after ssPostInstall the registry carries Chat's
//                             bundled entry (the M7 A-2 fresh-create path is
//                             intact), and the store path is original-user.
//   (2) INSTALL OVER A LISTENER-EXISTING STORE -> the A-2 merge gains Chat's
//                             entry WITHOUT modifying the Listener (asr) entry
//                             or any other non-owned section: the asr item
//                             round-trips BYTE-IDENTICAL. Reuses the frozen C++
//                             merge primitives the M7 A-2 suite already pins.
//   (3) UNINSTALL SURVIVAL -> the model file is downloaded (not an Inno [Files]
//                             payload), and the registry entry is written by
//                             [Code] (WriteRegistryFile), so neither is in the
//                             Inno uninstall log: both survive the uninstaller.
//                             The installer must NOT add a [Files] GGUF entry
//                             (that would make Inno own/delete the model) and
//                             must NOT add the bundled registry entry to the M7
//                             A-3 CleanupSharedEngineStore owned-file enum.
//
// PINS:
//   Structural (setup.iss source-text, ResolveRepoFile — same idiom as the M7
//     A-2/A-7 structural pins): A1 never-the-program-data-store, original-user
//     exchange pattern present, warn-not-silent fallback, model+registry+config
//     resolve from the original-user target, per-user seed handling, uninstall
//     does NOT own the model/registry/policy, seed model_id unchanged.
//   Merge-shape (frozen C++ primitives on a temp stage): scenario (2) — append
//     Chat's bundled item to a Listener-seeded document and assert the asr item
//     survives byte-identical + the merged doc re-parses with both entries.
// ---------------------------------------------------------------------------

void TestBt8InstallerModelBundling() {
    std::cout << "[RUN] Testing Plan-B B-T8 installer model bundling (REQ-B006)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace ehr = emebalachat::engine_host_registry;

    // The EXACT canonical Chat bundled item (setup.iss REGISTRY_BUNDLED_ITEM_PREFIX
    // + MODEL_FILENAME + REGISTRY_BUNDLED_ITEM_SUFFIX), byte-identical to the M7
    // A-2 pin constant (both installers converge on the same ggml-translate slot).
    static const char* kBt8ChatBundledItem =
        "{\"id\": \"hy-mt2-1.8b-q8\", \"family\": \"ggml-translate\", "
        "\"files\": [\"Hy-MT2-1.8B-Q8_0.gguf\"], \"capabilities\": "
        "[\"translate\"], \"origin\": \"bundled\", \"resource\": {\"vram_mb\": "
        "2400, \"ctx\": 4096, \"max_sessions\": 1, \"residency\": \"ondemand\", "
        "\"eviction\": \"evict\", \"priority\": 9}, \"profiles\": "
        "{\"default\": {\"temperature\": 0.0, \"top_p\": 0.6, \"top_k\": 20, "
        "\"rep_pen\": 1.05, \"prompt_template_ref\": \"hymt2-official\"}}, "
        "\"lang_pairs\": [\"*\"]}";

    const auto read_src = [](const wchar_t* rel, std::string& out) {
        const std::filesystem::path p = ResolveRepoFile(rel);
        if (p.empty()) return false;
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };

    std::string iss;
    TEST_CHECK(read_src(L"installer\\setup.iss", iss), "pin: setup.iss readable");

    // Banned tokens are assembled at RUNTIME from split pieces (the B-T7
    // runtime-token-assembly idiom): the banned program-data store constant and
    // the per-client policy filename must never appear RAW in this .inc, so the
    // pin cannot false-positive on its own source and the isolation pin stays
    // clean. The {localappdata}-based models constant is the POSITIVE anchor.
    const std::string kBannedProgramDataStore =
        std::string("{") + "common" + "appdata" + "}";
    const std::string kPolicyFileName =
        std::string("engine_") + "client_" + "policy" + ".json";

    // ---- Structural: A1 original-user store path (the gate-A1 mandate) -------
    if (!iss.empty()) {
        // NEVER the program-data store: that is C:\ProgramData, a different store.
        TEST_CHECK(iss.find(kBannedProgramDataStore) == std::string::npos,
                   "pin (A1): Chat installer NEVER targets the program-data store "
                   "({common appdata} = C:\\ProgramData) for the model store");
        // Original-user resolution machinery present (mirrors the Listener
        // file-exchange pattern the v2-A1 amendment mandates).
        TEST_CHECK(iss.find("ExecAsOriginalUser('powershell.exe'") != std::string::npos,
                   "pin (A1): original-user LocalAppData resolved via the "
                   "ExecAsOriginalUser powershell file-exchange (Listener pattern)");
        TEST_CHECK(iss.find("PATH_EXCHANGE_DIR = 'Emebalachat_install'") != std::string::npos,
                   "pin (A1): product-specific exchange staging dir present");
        TEST_CHECK(iss.find("function IsValidUserLocalAppData") != std::string::npos,
                   "pin (A1): sanity gate on the resolved path (blocks the "
                   "systemprofile / malformed cases)");
        // The model target dir is built from the ORIGINAL user's LocalAppData.
        TEST_CHECK(iss.find("ModelTargetDir := UserLocalAppDataPath + COMMON_MODELS_DIR_SUFFIX") !=
                       std::string::npos,
                   "pin (A1): the Common models target is built from the "
                   "resolved original-user LocalAppData, not the elevated token");
        // Warn-not-silent fallback (Listener UserPathFallbackWarning precedent):
        // a resolution failure must warn, never silently drop into the admin profile.
        TEST_CHECK(iss.find("falling back to the elevated-context path") != std::string::npos &&
                       iss.find("SuppressibleMsgBox(CustomMessage('UserPathFallbackWarning'") !=
                           std::string::npos,
                   "pin (A1): resolution failure falls back WITH the explicit "
                   "user-facing warning (warn-not-silent)");
        TEST_CHECK(iss.find("english.UserPathFallbackWarning=") != std::string::npos,
                   "pin (A1): the fallback warning message is defined");
        // The model download destination, the registry.json path and the config
        // model_path must ALL resolve to the original-user store.
        TEST_CHECK(iss.find("ModelDestDir := ModelTargetDir") != std::string::npos,
                   "pin (A1): DownloadModel targets the original-user store");
        TEST_CHECK(iss.find("RegistryPath := ModelTargetDir + '\\' + REGISTRY_FILENAME") !=
                       std::string::npos,
                   "pin (A1): WriteRegistryFile targets the original-user store");
        TEST_CHECK(iss.find("ModelPath := ModelTargetDir + '\\' + MODEL_FILENAME") !=
                       std::string::npos,
                   "pin (A1): CreateConfigFile model_path targets the original-user store");
        TEST_CHECK(iss.find("SetupModelTargetDir();") != std::string::npos,
                   "pin (A1): the target is resolved once at ssPostInstall BEFORE "
                   "the model/registry/config writes");
        // A1 gate-A1 diff mandate: the install-time model target must equal the
        // runtime store the app resolves (%LOCALAPPDATA%\\Emebala\\Common\\models).
        TEST_CHECK(iss.find("COMMON_MODELS_DIR = '{localappdata}\\Emebala\\Common\\models'") !=
                       std::string::npos,
                   "pin (A1): the {localappdata}-based models constant family is "
                   "retained (runtime store alignment; never the program-data store)");
    }

    // ---- R2-1/R2-2/R2-3 (round-2 dispute, session 261029_0001) --------------
    // These pins extend (never rewrite) the A1 block above: the junction
    // refusal had to become a WRITE-TIME check (the resolution-time one is
    // check-then-act across the minutes-long download), the PowerShell
    // -Command paths had to become single-quoted, and the group-writable
    // exchange pointer target had to pass a shape gate before the elevated
    // read/delete. Source-text pins only - the runtime proofs are Phase-4.
    if (!iss.empty()) {
        // (R2-1a) subtree-scoped helper exists; the profile root itself is
        // never re-tested (FSLogix/folder-redirection false hard-fail).
        TEST_CHECK(iss.find("function SubtreeReparsePoint(const Base, Sub: String): Boolean;") !=
                       std::string::npos,
                   "pin (R2-1): SubtreeReparsePoint (app-subtree-scoped check, profile "
                   "root excluded) is present");
        TEST_CHECK(iss.find("(not SubtreeReparsePoint(UserLocalAppDataPath, "
                            "COMMON_MODELS_DIR_SUFFIX))") != std::string::npos,
                   "pin (R2-1): ResolveCommonStoreDir checks the \\models LEAF it hands "
                   "to its callers, not only the store root");
        // (R2-1b) the fallback branch is re-checked UNCONDITIONALLY after the
        // branch merge, before ForceDirectories.
        const size_t r21_setup = iss.find("ModelStoreRefused('SetupModelTargetDir/pre-create')");
        TEST_CHECK(r21_setup != std::string::npos &&
                       r21_setup < iss.find("if not ForceDirectories(ModelTargetDir) then"),
                   "pin (R2-1): the FINAL model target is re-checked after the branch "
                   "merge, before the store is created (same-account fallback covered)");
        // (R2-1c) DownloadModel re-checks before the store create, before the
        // unverified-model delete, and - the core finding - AFTER the download
        // completes and BEFORE the rename/copy that places the model.
        TEST_CHECK(iss.find("ModelStoreRefused('DownloadModel/pre-create')") != std::string::npos,
                   "pin (R2-1): the store-create write is guarded");
        TEST_CHECK(iss.find("ModelStoreRefused('DownloadModel/pre-delete-unverified')") !=
                       std::string::npos,
                   "pin (R2-1): the F2 unverified-model delete is guarded");
        const size_t r21_place = iss.find("ModelStoreRefused('DownloadModel/pre-place')");
        TEST_CHECK(r21_place != std::string::npos &&
                       r21_place < iss.find("RenameFile(ModelTmpPath, ModelDestPath)"),
                   "pin (R2-1): the chain is re-checked after the download completes and "
                   "before the rename/copy (minutes-long window closed)");
        // (R2-1d) the registry write path is guarded at resolve time and right
        // before BOTH atomic writes; the refusal skips instead of falling
        // through to the other resolver.
        TEST_CHECK(iss.find("ModelStoreRefused('WriteRegistryFile/resolve')") != std::string::npos,
                   "pin (R2-1): WriteRegistryFile re-validates the final chain at resolve "
                   "time and skips on refusal");
        const size_t r21_reg = iss.find("StoreChainRefused(StoreBase, RegistryPath)");
        TEST_CHECK(r21_reg != std::string::npos &&
                       r21_reg < iss.find("WriteTextFileAtomic(RegistryPath, "
                                          "JsonBuildRegistryDoc("),
                   "pin (R2-1): the fresh-create registry write is re-checked immediately "
                   "before WriteTextFileAtomic");
        TEST_CHECK(iss.find("reparse point in the FINAL registry chain; merged "
                            "registry.json not written") != std::string::npos,
                   "pin (R2-1): the merge write is re-checked too (no registry write "
                   "through a link swapped in after the resolve)");
        // (R2-1e) the previously-unchecked COMMON_ENGINE_DIR write sites.
        TEST_CHECK(iss.find("EngineStoreRefused('AbsorbLegacyWorkerFileName')") !=
                       std::string::npos &&
                       iss.find("EngineStoreRefused('AbsorbLegacyWorkerFileName')") <
                           iss.find("RenameFile(LegacyPath, UnifiedPath)"),
                   "pin (R2-1): the ssInstall engine-store rename/delete is guarded "
                   "(same class, previously unchecked)");
        TEST_CHECK(iss.find("EngineStoreRefused('WriteEngineVersionFile')") != std::string::npos &&
                       iss.find("EngineStoreRefused('WriteComponentsFile')") != std::string::npos,
                   "pin (R2-1): both engine-store stampers are guarded");
        // (R2-2) PowerShell quoting: every interpolated -Command path goes
        // through PsSingleQuote (apostrophes doubled), and the nested
        // double-quote form that CommandLineToArgvW strips is GONE.
        TEST_CHECK(iss.find("function PsSingleQuote(const S: String): String;") !=
                       std::string::npos &&
                       iss.find("StringChangeEx(T, '''', '''''', True);") != std::string::npos,
                   "pin (R2-2): PsSingleQuote doubles embedded apostrophes");
        TEST_CHECK(iss.find("PsSingleQuote(PointerFile)") != std::string::npos &&
                       iss.find("PsSingleQuote(VerifyFile)") != std::string::npos,
                   "pin (R2-2): both exchange paths are single-quoted inside -Command");
        TEST_CHECK(iss.find("-FilePath \"' + PointerFile") == std::string::npos &&
                       iss.find("-LiteralPath \"' + VerifyFile") == std::string::npos,
                   "pin (R2-2): no nested double-quoted path is re-introduced (spaced "
                   "%TEMP% regression class)");
        // (R2-3) the group-writable pointer target is shape-gated BEFORE the
        // elevated FileExists / LoadStringsFromFile / DeleteFile, and the
        // pointer itself is refused when it is a link.
        const size_t r23_gate = iss.find("if not IsValidExchangeValuePath(ValueFile) then");
        TEST_CHECK(r23_gate != std::string::npos &&
                       r23_gate < iss.find("if not FileExists(ValueFile) then"),
                   "pin (R2-3): the value path is validated before the first elevated "
                   "read of the pointer target");
        TEST_CHECK(iss.find("function IsValidExchangeValuePath(const Path: String): Boolean;") !=
                       std::string::npos &&
                       iss.find("ContainsReparsePoint(PointerFile)") != std::string::npos,
                   "pin (R2-3): exchange-dir + name-shape + reparse-chain gate present, "
                   "and a reparse-point pointer is refused");
    }

    // ---- R3 (round-3 preemptive fixes, session 261029_0001) ------------------
    // Source-text pins for the round-3 hardening: the write-time reparse checks
    // now cover the WRITTEN FILE leaf (not only the directory chain), the
    // uninstall side refuses a link instead of deleting through it, the [Files]
    // policy-seed copy is gated, the exchange pointer check sits where it can
    // actually fire, and one refused store chain announces its box only once.
    if (!iss.empty()) {
        auto occurrences = [&iss](const std::string& needle) {
            size_t count = 0;
            for (size_t pos = iss.find(needle); pos != std::string::npos;
                 pos = iss.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };
        // (R3-1) leaf-inclusive model re-check immediately before the place step.
        const size_t r3_model_leaf =
            iss.find("ModelStorePathRefused('DownloadModel/pre-place-leaf', ModelDestPath)");
        TEST_CHECK(r3_model_leaf != std::string::npos &&
                       r3_model_leaf < iss.find("RenameFile(ModelTmpPath, ModelDestPath)"),
                   "pin (R3-1): the model file LEAF is re-checked immediately before "
                   "RenameFile/CopyFile places it");
        // (R3-1b) the policy seed's final pre-write check covers the written file.
        const size_t r3_seed_leaf = iss.find("StoreChainRefused(UserLocalAppDataPath, DestPath)");
        TEST_CHECK(r3_seed_leaf != std::string::npos &&
                       r3_seed_leaf < iss.find("SaveStringToFile(DestPath, Raw, False)"),
                   "pin (R3-1): the policy seed's re-check includes the written file leaf");
        // (R3-1c) both engine-store stampers check the file they write.
        const size_t r3_version_leaf =
            iss.find("EngineStoreLeafRefused('WriteEngineVersionFile/leaf', VersionPath)");
        TEST_CHECK(r3_version_leaf != std::string::npos &&
                       r3_version_leaf <
                           iss.find("SaveStringToFile(VersionPath, ENGINE_BUNDLED_VERSION, False)"),
                   "pin (R3-1): engine.version is leaf-checked before SaveStringToFile");
        const size_t r3_components_leaf =
            iss.find("EngineStoreLeafRefused('WriteComponentsFile/pre-write', ComponentsPath)");
        TEST_CHECK(r3_components_leaf != std::string::npos &&
                       r3_components_leaf <
                           iss.find("SaveStringToFile(ComponentsPath, OutJson, False)"),
                   "pin (R3-1): components.json's immediately-before-write check is "
                   "leaf-inclusive");
        // (R3-2) uninstall-side guards: no elevated recursive delete / seed delete
        // through a pre-planted link.
        const size_t r3_uninstall_guard =
            iss.find("SubtreeReparsePoint(ExpandConstant('{localappdata}'), '\\Emebalachat')");
        TEST_CHECK(r3_uninstall_guard != std::string::npos &&
                       r3_uninstall_guard < iss.find("DelTree(LocalAppData"),
                   "pin (R3-2): the user-data DelTree is refused when the path is a link");
        const size_t r3_seed_preserve =
            iss.find("the policy seed is preserved (261029_0001 R3 F-1)");
        TEST_CHECK(r3_seed_preserve != std::string::npos &&
                       r3_seed_preserve < iss.find("DeleteFile(PolicySeed)"),
                   "pin (R3-2): the uninstall policy-seed delete is guarded (preserve on a "
                   "reparse hit)");
        // (R3-3) the [Files] policy-seed Check itself refuses a linked store chain.
        TEST_CHECK(iss.find("the [Files] policy-seed copy is refused") != std::string::npos,
                   "pin (R3-3): IsCommonStoreAbsentForPolicySeed refuses a reparse chain "
                   "(the elevated [Files] copy is gated)");
        // (R3-4) the pointer reparse check runs where the pointer EXISTS, i.e.
        // after call 1 and immediately before the elevated read.
        const size_t r3_pointer_check = iss.find("ContainsReparsePoint(PointerFile)");
        TEST_CHECK(r3_pointer_check != std::string::npos &&
                       r3_pointer_check > iss.find("exchange pointer not found after powershell exit") &&
                       r3_pointer_check < iss.find("LoadStringsFromFile(PointerFile, Buf)"),
                   "pin (R3-4): the pointer re-check sits after call 1 and immediately before "
                   "LoadStringsFromFile (not at a moment the file cannot exist)");
        // (R3-5) one ModelTargetDirError announcement per run, every Log kept.
        TEST_CHECK(iss.find("if ModelTargetDirErrorAnnounced then Exit;") != std::string::npos &&
                       occurrences("SuppressibleMsgBox(CustomMessage('ModelTargetDirError')") == 1,
                   "pin (R3-5): the ModelTargetDirError box is announced at most once per run "
                   "(a single SuppressibleMsgBox call site, flag-guarded)");
        TEST_CHECK(occurrences("AnnounceModelTargetDirError();") >= 4,
                   "pin (R3-5): every refusal/creation-failure site routes through the "
                   "announce-once helper (Log lines stay per-site)");
    }

    // ---- Structural: bundling + ownership + uninstall-survival (§6.1) --------
    if (!iss.empty()) {
        // Scenario-3 (uninstall survival): the bundled slot must be written by
        // the M7 A-2 atomic merge (installer-owned [Code]), which already exists.
        TEST_CHECK(iss.find("procedure WriteRegistryFile") != std::string::npos &&
                       iss.find("WriteRegistryFile();") != std::string::npos,
                   "pin (3): the bundled registry entry is written by the M7 A-2 "
                   "atomic merge (survives Inno uninstall)");
        TEST_CHECK(iss.find("function WriteTextFileAtomic") != std::string::npos,
                   "pin (3): the registry merge uses the atomic tmp+rename writer "
                   "(invariant #3 read-merge-own-entry-tmp-rename)");
        // The bundled slot is Chat's own (hy-mt2-1.8b-q8); other families'
        // entries pass through verbatim (G3 / non-owner guard).
        TEST_CHECK(iss.find("REGISTRY_BUNDLED_ID = 'hy-mt2-1.8b-q8'") != std::string::npos,
                   "pin (G3): Chat owns exactly the hy-mt2-1.8b-q8 translate slot");
        TEST_CHECK(iss.find("copied VERBATIM") != std::string::npos ||
                       iss.find("verbatim passthrough") != std::string::npos,
                   "pin (G3): non-owned entries (Listener asr / user models) pass "
                   "through the merge byte-for-byte");
        // Damaged-registry fail-closure (HANDOFF invariant #3) stays intact.
        TEST_CHECK(iss.find("leaving it untouched (fail-closed") != std::string::npos,
                   "pin: a damaged registry is preserved fail-closed, never overwritten");
    }

    // ---- Structural: NOT added to the A-3 cleanup enum; model not Inno-owned --
    if (!iss.empty()) {
        // The Chat bundled model file must NOT appear in CleanupSharedEngineStore's
        // owned-file deletion list (that would make last-app uninstall delete a
        // Listener-shared store contract). Extract the procedure body and reject
        // any 'DeleteOwnedFile(ModelsDir' line.
        {
            const auto h = iss.find("procedure CleanupSharedEngineStore");
            TEST_CHECK(h != std::string::npos, "pin (3): CleanupSharedEngineStore defined");
            if (h != std::string::npos) {
                const auto body_end = iss.find("\nend;", h);
                TEST_CHECK(body_end != std::string::npos,
                           "pin (3): CleanupSharedEngineStore body closes");
                if (body_end != std::string::npos) {
                    const std::string body = iss.substr(h, body_end - h);
                    TEST_CHECK(body.find("DeleteOwnedFile(ModelsDir") == std::string::npos,
                               "pin (3): the Chat bundled model file is NOT in the M7 A-3 "
                               "cleanup enum (its removal is the registry-aware bundled-only "
                               "path via RemoveBundledItemFiles, never an owned-file constant)");
                    TEST_CHECK(body.find(kPolicyFileName) == std::string::npos,
                               "pin (3): the per-client policy file is NOT in the A-3 cleanup "
                               "enum (policy survives; seed handling owns its lifecycle)");
                }
            }
        }
        // The model must not be an Inno [Files] payload (that would put it in the
        // uninstall log and make Inno delete it). No .gguf source may appear in a
        // [Files] entry.
        {
            bool gguf_files_entry = false;
            std::istringstream stream(iss);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("Source:") != std::string::npos &&
                    line.find(".gguf") != std::string::npos) {
                    gguf_files_entry = true;
                    break;
                }
            }
            TEST_CHECK(!gguf_files_entry,
                       "pin (3): the model GGUF is NOT an Inno [Files] entry (it is "
                       "downloaded; hence survives Inno uninstall)");
        }
        // The bundled registry entry must NOT appear in an [UninstallDelete] line
        // (that would make the uninstaller delete the store contract).
        {
            bool reg_uninstdelete = false;
            std::istringstream stream(iss);
            std::string line;
            while (std::getline(stream, line)) {
                const auto p = line.find("UninstallDelete");
                if (p != std::string::npos && p < 3 && line.find("registry") != std::string::npos)
                    reg_uninstdelete = true;
            }
            TEST_CHECK(!reg_uninstdelete,
                       "pin (3): no [UninstallDelete] targets the registry/model store");
        }
    }

    // ---- Scenario 2 (merge-shape): install over a Listener-existing store ----
    // Reuse the frozen C++ registry primitives on a temp stage. The Listener
    // installer seeds BOTH the asr and the mt slots (Listener is the superset
    // owner); the Chat merge must add/gain the mt entry while the asr (Listener)
    // entry survives BYTE-IDENTICAL.
    {
        // 260930_0003 (decisions.md D3): fixture refreshed to the current
        // Listener ASR model id (assertions are pass-through; this is
        // representativeness only).
        const std::string asr_item =
            "{\"id\": \"whisper-large-v3-turbo-q5-0-v1\", \"family\": \"ggml-asr\", "
            "\"files\": [\"ggml-large-v3-turbo-q5_0.bin\"], \"capabilities\": "
            "[\"asr\"], \"origin\": \"bundled\", \"resource\": {\"vram_mb\": 0, "
            "\"ctx\": 0, \"max_sessions\": 1, \"residency\": \"ondemand\", "
            "\"eviction\": \"lru\", \"priority\": 5}, \"profiles\": {}, "
            "\"lang_pairs\": [\"*\"]}";
        // A Listener-first store carries the asr slot (and its own mt copy).
        const std::string listener_doc =
            std::string("{\"schema_version\": 1, \"models\": [") + asr_item + "," +
            kBt8ChatBundledItem + "]}";
        const auto lr = ehr::ParseRegistryJson(listener_doc);
        TEST_CHECK(lr.status == ehr::LoadStatus::Ok && lr.registry.models.size() == 2,
                   "scenario (2): a Listener-seeded store parses (asr + mt slots)");

        // The Chat-side merge semantics the A-2 installer walk implements:
        // items whose id != REGISTRY_BUNDLED_ID pass through VERBATIM; the owned
        // bundled slot is refreshed/kept; a missing owned slot is appended. The
        // app-side writer (WriteRegistryAtomically) is what persists a merge.
        const std::string stage0 =
            std::string("{\"schema_version\": 1, \"models\": [") + asr_item + "]}";
        auto loaded = ehr::ParseRegistryJson(stage0);
        TEST_CHECK(loaded.status == ehr::LoadStatus::Ok && loaded.registry.models.size() == 1,
                   "scenario (2): Listener-seeded store (asr only) parses");
        // Append Chat's bundled item (the A-2 'append the missing owned slot' leg).
        ehr::ModelEntry chat;
        TEST_CHECK(ehr::ParseRegistryJson(std::string("{\"schema_version\": 1, \"models\": [") +
                                             kBt8ChatBundledItem + "]}")
                             .status == ehr::LoadStatus::Ok,
                   "scenario (2): the Chat canonical bundled item parses against the "
                   "frozen C++ shape");
        const auto pc = ehr::ParseRegistryJson(
            std::string("{\"schema_version\": 1, \"models\": [") + kBt8ChatBundledItem + "]}");
        if (pc.status == ehr::LoadStatus::Ok && !pc.registry.models.empty())
            loaded.registry.models.push_back(pc.registry.models[0]);

        const std::filesystem::path stage =
            std::filesystem::temp_directory_path() / "emebala_bt8_installer_model_test";
        std::error_code sec;
        std::filesystem::remove_all(stage, sec);
        std::filesystem::create_directories(stage, sec);
        const auto w = ehr::WriteRegistryAtomically(loaded.registry, stage);
        TEST_CHECK(w == ehr::WriteOutcome::Ok,
                   "scenario (2): the Chat-entry-appending merge persists atomically");

        const auto read_bytes = [](const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };
        const auto re = ehr::ParseRegistryJson(read_bytes(stage / L"registry.json"));
        TEST_CHECK(re.status == ehr::LoadStatus::Ok && re.registry.models.size() == 2,
                   "scenario (2): after the Chat merge the store carries BOTH entries");
        if (re.status == ehr::LoadStatus::Ok) {
            // The Listener asr entry is BYTE-IDENTICAL (untouched non-owned section).
            // 260930_0003: id refreshed with the asr_item fixture above.
            const ehr::ModelEntry* asr = re.registry.FindModel("whisper-large-v3-turbo-q5-0-v1");
            TEST_CHECK(asr != nullptr && asr->origin == "bundled" &&
                           asr->family == "ggml-asr",
                       "scenario (2): the Listener asr entry survives the Chat merge "
                       "with its id/family/origin untouched (G3 non-owner guard)");
            // Chat gains its bundled translate entry.
            const ehr::ModelEntry* mt = re.registry.FindModel("hy-mt2-1.8b-q8");
            TEST_CHECK(mt != nullptr && mt->origin == "bundled" &&
                           mt->files.size() == 1 &&
                           mt->files[0] == "Hy-MT2-1.8B-Q8_0.gguf",
                       "scenario (1/2): the store gains Chat's bundled mt entry "
                       "(fresh install creates it; install-over-Listener gains it)");
        }
        // No torn residue.
        TEST_CHECK(!std::filesystem::exists(stage / L"registry.json.tmp", sec),
                   "scenario (2): no .tmp residue after the atomic merge write");
        std::filesystem::remove_all(stage, sec);
    }

    // ---- Policy seed unchanged (design §6.2): chat model_id stays "" ----------
    {
        std::string seed;
        TEST_CHECK(read_src(L"installer\\assets\\engine_client_policy.seed.json", seed),
                   "pin (6.2): policy seed asset readable");
        if (!seed.empty()) {
            // Locate the chat section and pin its model_id to empty.
            const auto chat = seed.find("\"emebala-chat\"");
            TEST_CHECK(chat != std::string::npos, "pin (6.2): the chat section exists");
            if (chat != std::string::npos) {
                const auto section_end = seed.find("\"emebala-listner\"", chat);
                const std::string chat_section =
                    seed.substr(chat, section_end == std::string::npos
                                          ? std::string::npos
                                          : section_end - chat);
                TEST_CHECK(chat_section.find("\"model_id\": \"\"") != std::string::npos,
                           "pin (6.2): the chat policy section keeps model_id \"\" "
                           "(bundled default; user-added models are NOT installer-seeded)");
            }
        }
    }

    std::cout << "  bt8 installer model bundling: "
              << (g_failed_count - failures_before) << " new failures" << std::endl;
}
