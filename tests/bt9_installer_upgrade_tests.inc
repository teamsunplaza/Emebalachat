// ---------------------------------------------------------------------------
// B-T9 (session 261108_0002, v0.10.6 release, Workstream 4): Chat installer
// UPGRADE-SAFETY pins (TestBt9InstallerUpgradeSafety).
//
// Authority: the user report that drives v0.10.6 -
//   "I downloaded and installed a new version, yet the engine / the old
//    version was not properly replaced by the new one."
// plus the orchestrator audit of installer/setup.iss, which identified the
// concrete mechanisms behind that report:
//   G1  replacement was decided ONLY from the components.json record, never
//       from the bytes on disk -> a stale/corrupt store exe with a record >=
//       the bundle was kept forever;
//   G2  EngineHostUpdated/EngineWorkerUpdated latched on the [Files] Check
//       verdict, so ssPostInstall stamped engine.version + components.json
//       even when the copy silently failed (locked file, dismissed retry);
//   G3  the installer is ALWAYS elevated, so the engine landed in the ADMIN
//       profile's {localappdata} while the app reads the RUNNING user's store;
//   G4  worker.ggml-translate.manifest was not an independent repair target;
//   G6  taskkill was fire-and-forget and the legacy worker name was not killed;
//   G9  WriteComponentsFile dropped the owned entry it did not re-install.
// G5 (the non-owned ggml-asr slot is never replaced) is INTENTIONAL and is
// pinned as unchanged here, together with the frozen contracts the other
// installer gates own.
//
// METHOD: static source-text assertions against installer/setup.iss (the same
// ResolveRepoFile precedent as bt8/m7). A structural pin cannot execute Inno's
// runtime, so every pin states the CONTRACT SHAPE; the runtime proof is the
// user's own install (DoD of the release) and the installer gates.
// ---------------------------------------------------------------------------

void TestBt9InstallerUpgradeSafety() {
    std::cout << "[RUN] Testing B-T9 installer upgrade safety (v0.10.6 WS4)..."
              << std::endl;
    const int failures_before = g_failed_count;

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
    TEST_CHECK(read_src(L"installer\\setup.iss", iss), "bt9 pin: setup.iss readable");

    // Slice helper: the region of `s` between the first occurrence of `from`
    // and the next occurrence of `to` after it ("" when either is missing).
    const auto region_between = [](const std::string& s, const std::string& from,
                                   const std::string& to) {
        const size_t a = s.find(from);
        if (a == std::string::npos) return std::string();
        const size_t b = s.find(to, a + from.size());
        if (b == std::string::npos) return std::string();
        return s.substr(a, b - a);
    };
    // Procedure/function body: header -> the next top-level `end;` line. The
    // script is LF-only (no CR in the file), so "\nend;" is byte-exact.
    const auto body_of = [&iss](const std::string& header) {
        const size_t h = iss.find(header);
        if (h == std::string::npos) return std::string();
        const size_t e = iss.find("\nend;", h);
        if (e == std::string::npos) return std::string();
        return iss.substr(h, e - h);
    };

    // ---- G1: OWNED slots are replace-if-DIFFERENT (not replace-if-older) ----
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function EngineComponentRecordedVersion(") != std::string::npos,
                   "G1: the recorded per-component version is read as a VALUE, so "
                   "'names exactly the bundle' can be told apart from 'names "
                   "something newer'");
        const std::string rule_a = body_of("function EngineComponentRuleA(");
        TEST_CHECK(!rule_a.empty() &&
                       rule_a.find("ReplaceOnVersionMismatch: Boolean): Boolean;") !=
                           std::string::npos,
                   "G1: EngineComponentRuleA takes the owned-slot switch as an "
                   "explicit parameter (no silent behaviour change for other callers)");
        TEST_CHECK(!rule_a.empty() &&
                       rule_a.find("if RecordedVersion <> ENGINE_BUNDLED_VERSION then") !=
                           std::string::npos,
                   "G1: an owned component whose record differs from "
                   "ENGINE_BUNDLED_VERSION is REPLACED");
        TEST_CHECK(!rule_a.empty() &&
                       rule_a.find("RecordedVersion := "
                                   "EngineComponentRecordedVersion(ComponentsJson, "
                                   "ComponentName);") != std::string::npos,
                   "G1: the compare uses the exact-version reader (not the ordered "
                   "ComponentNeedsReplace verdict)");
        // Regression pin on the removed arm: the owned call site must pass the
        // switch, and the pre-WS4 two-argument call must be gone. A future edit
        // that drops the third argument silently restores "recorded newer =>
        // keep" - the exact defect the user reported.
        TEST_CHECK(iss.find("Result := EngineComponentRuleA(ComponentName, BundledAbi, True);") !=
                       std::string::npos,
                   "G1: the owned branch of SharedSlotReplaceDecision asks for "
                   "replace-if-different");
        TEST_CHECK(iss.find("EngineComponentRuleA(ComponentName, BundledAbi);") ==
                       std::string::npos,
                   "G1: the pre-WS4 two-argument rule A call is gone (no recorded-"
                   "newer keep arm can survive the refactor)");
        // G5 INTENTIONAL, unchanged: the non-owner branch still refuses to
        // touch an existing cross-app slot.
        TEST_CHECK(iss.find("'-owned slot \"' + ComponentName + '\" exists - NEVER "
                            "replace (G3).'") != std::string::npos,
                   "G5: the non-owned (ggml-asr) NEVER-replace guard is byte-identical");
        TEST_CHECK(iss.find("WORKER_ASR_FILENAME, ENGINE_WORKER_ASR_ABI_VERSION, False);") !=
                       std::string::npos,
                   "G5: the asr slot is still decided with IsOwner=False (Chat never "
                   "downgrades the Listener-owned family)");
        // The missing-file repair hole is preserved.
        TEST_CHECK(iss.find("owned slot \"' + ComponentName + '\" store file missing - "
                            "reinstalling (repair hole).") != std::string::npos,
                   "G1: the missing-store-file repair hole is preserved");
    }

    // ---- G2: post-copy SHA-256 verification gates the version records ------
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function EngineBundledFileSha256(const FileName: String): String;") !=
                       std::string::npos,
                   "G2: the bundled bytes' SHA-256 is obtained from the setup.exe "
                   "archive (the authoritative staged source)");
        TEST_CHECK(iss.find("ExtractTemporaryFile(FileName);") != std::string::npos,
                   "G2: the staged source is materialised via ExtractTemporaryFile "
                   "(a shipped setup.exe has no build tree next to it)");
        TEST_CHECK(iss.find("Result := GetSHA256OfFile(TmpPath);") != std::string::npos,
                   "G2: the bundled copy is hashed with GetSHA256OfFile");
        TEST_CHECK(iss.find("function EngineStoreFileStatus(const Site, StoreDir, FileName,") !=
                       std::string::npos,
                   "G2: one shared status helper (verified / defect / unavailable) is "
                   "used by every store-file check");
        TEST_CHECK(iss.find("if SameText(Actual, BundledHash) then") != std::string::npos,
                   "G2: verification is a byte-level SHA-256 equality test");
        TEST_CHECK(iss.find("procedure VerifyEngineOwnedStoreCopy();") != std::string::npos,
                   "G2: the post-copy verification step exists");
        TEST_CHECK(iss.find("EngineHostUpdated := (Status = 1);") != std::string::npos &&
                       iss.find("EngineWorkerUpdated := (Status = 1);") != std::string::npos,
                   "G2: the version-record latches are RE-LATCHED from the verified "
                   "store state, not from the [Files] Check verdict");
        TEST_CHECK(iss.find("if not EngineHostUpdated then") != std::string::npos,
                   "G2: engine.version is still gated on the (now verified) latch");
        // Ordering inside the ssPostInstall step: verify -> deploy -> stamp.
        const std::string post = region_between(iss, "if CurStep = ssPostInstall then",
                                                "if CurStep = ssDone then");
        TEST_CHECK(!post.empty(), "G2: the ssPostInstall step is locatable");
        if (!post.empty()) {
            const size_t verify_at = post.find("VerifyEngineOwnedStoreCopy();");
            const size_t deploy_at = post.find("DeployOwnedEngineStoreForOriginalUser();");
            const size_t version_at = post.find("WriteEngineVersionFile();");
            const size_t components_at = post.find("WriteComponentsFile();");
            TEST_CHECK(verify_at != std::string::npos &&
                           version_at != std::string::npos &&
                           components_at != std::string::npos && verify_at < version_at &&
                           verify_at < components_at,
                       "G2: the verification runs BEFORE both version records are "
                       "written (a failed copy can no longer be stamped as current)");
            TEST_CHECK(deploy_at != std::string::npos && verify_at < deploy_at &&
                           deploy_at < version_at,
                       "G3/G2: the original-user deployment runs between the "
                       "verification and the record writes (verified bytes only)");
        }
        // G9: an owned entry this run did not verified-replace is preserved.
        TEST_CHECK(iss.find("G9: preserving the existing \"orchestrator\" record "
                            "verbatim") != std::string::npos,
                   "G9: a non-replaced orchestrator entry is carried over verbatim");
        TEST_CHECK(iss.find("G9: preserving the existing \"ggml-translate\" record "
                            "verbatim") != std::string::npos,
                   "G9: a non-replaced ggml-translate entry is carried over verbatim");
        TEST_CHECK(iss.find("function ComposeComponentsJson(") != std::string::npos &&
                       iss.find("if not ComposeComponentsJson(ComponentsPath, EngineHostUpdated, "
                                "EngineWorkerUpdated,") != std::string::npos,
                   "G9: the merge is shared by both stores (elevated + original-user) "
                   "instead of being duplicated");
        // 261108_0002 (FIX-R1, R1-06): the per-store verdicts are PARAMETERS, so
        // the original-user store can be gated on its own copies.
        TEST_CHECK(iss.find("HostVerified, WorkerVerified: Boolean;") != std::string::npos,
                   "R1-06: the components merge takes the per-store verified verdicts as "
                   "parameters (no shared global latch decides both stores)");
        TEST_CHECK(iss.find("if not OrigUserHostVerified then") != std::string::npos &&
                       iss.find("if (not OrigUserHostVerified) and (not OrigUserWorkerVerified) then") !=
                           std::string::npos,
                   "R1-06: the original-user record writes are gated on their OWN verified "
                   "copies, not on the elevated-store latches");
        TEST_CHECK(iss.find("OrigUserWorkerVerified := OrigUserWorkerExeVerified and "
                            "OrigUserManifestVerified;") != std::string::npos,
                   "R1-06: the worker record needs both the exe and the manifest verified "
                   "in the original-user store");
        TEST_CHECK(iss.find("WARNING could not copy ' + SrcPath + ' -> ' +") !=
                       std::string::npos &&
                       iss.find("its original-user record will not be advanced.") !=
                           std::string::npos,
                   "R1-06: a failed original-user copy refuses the record (and reports)");
    }

    // ---- 261108_0002 (FIX-R1, R1-11): the install decision sees store bytes --
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function EngineOwnedStoreBytesStatus(const DeployedFileName: "
                            "String): Integer;") != std::string::npos,
                   "R1-11: the pre-copy SHA-256 status is a named decision input");
        TEST_CHECK(iss.find("if EngineOwnedStoreBytesStatus(DeployedFileName) = 2 then") !=
                       std::string::npos,
                   "R1-11: a byte mismatch under a CURRENT record turns into replace-needed");
        TEST_CHECK(iss.find("(corrupt/stale bytes under a current record) => replace-needed.')") !=
                       std::string::npos,
                   "R1-11: the replace-needed verdict is logged with its cause");
        TEST_CHECK(iss.find("function EngineBundledHashProbe(const FileName: String): String;") !=
                       std::string::npos &&
                       iss.find("EngineBundledHashHostProbed := True;") != std::string::npos &&
                       iss.find("EngineBundledHashWorkerProbed := True;") != std::string::npos,
                   "R1-11: the bundled bytes are probed ONCE per file per run (the "
                   "[Files] Checks ask the same question several times)");
        TEST_CHECK(iss.find("Result := EngineBundledHashProbe(FileName);") != std::string::npos,
                   "R1-11: the ssPostInstall verification shares the probe-once cache");
        TEST_CHECK(iss.find("Result := EngineComponentRuleA(ComponentName, BundledAbi, True);") !=
                           std::string::npos &&
                       iss.find("if not Result then", iss.find(
                           "Result := EngineComponentRuleA(ComponentName, BundledAbi, True);")) !=
                           std::string::npos,
                   "R1-11: the hash arm runs only on rule A's keep path (cost paid exactly "
                   "where a copy would be skipped)");
    }

    // ---- G4: the worker manifest is an INDEPENDENT repair target -----------
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function EngineWorkerManifestNamesBundle(const Path: String): "
                            "Boolean;") != std::string::npos,
                   "G4: the store manifest's content is tested against the bundled "
                   "backend identity");
        TEST_CHECK(iss.find("Pos('\"engine_version\":\"' + ENGINE_WORKER_ENGINE_VERSION + "
                            "'\"', Doc)") != std::string::npos,
                   "G4: the manifest test names the bundled engine_version (the "
                   "manifest carries no app-version field)");
        // 261108_0002 (FIX-R1, R1-07): the abi needle must terminate the number,
        // or a pinned abi 1 also matches a record carrying abi 10/11/100.
        TEST_CHECK(iss.find("Pos('\"abi_version\":' + IntToStr(ENGINE_WORKER_ABI_VERSION) + ',', "
                            "Doc)") != std::string::npos,
                   "R1-07: the abi_version needle carries the value's closing delimiter "
                   "(no cross-digit prefix match)");
        TEST_CHECK(iss.find("Pos('\"abi_version\":' + IntToStr(ENGINE_WORKER_ABI_VERSION), "
                            "Doc)") == std::string::npos,
                   "R1-07: the delimiter-less abi_version needle is gone");
        TEST_CHECK(iss.find("function EngineWorkerStoreDecision(): Boolean;") != std::string::npos,
                   "G4: BOTH worker [Files] entries consult ONE decision function");
        TEST_CHECK(iss.find("function ShouldInstallEngineWorkerManifest(): Boolean;") !=
                       std::string::npos,
                   "G4: the manifest entry has its own Check function");
        TEST_CHECK(iss.find("; Check: ShouldInstallEngineWorkerManifest") != std::string::npos,
                   "G4: the manifest [Files] entry is wired to that Check (a lost "
                   "manifest repairs itself)");
        TEST_CHECK(iss.find("EngineWorkerUpdated := EngineWorkerStoreDecision();") !=
                       std::string::npos,
                   "G4: the exe entry uses the same decision (worker + manifest move "
                   "together)");
        const std::string manifest_decision = body_of("function EngineWorkerStoreDecision(): "
                                                      "Boolean;");
        TEST_CHECK(!manifest_decision.empty() &&
                       manifest_decision.find("worker manifest missing or stale") !=
                           std::string::npos,
                   "G4: a missing/stale manifest forces the pair to install even when "
                   "the exe record is current");
    }

    // ---- G6: taskkill really waits; the legacy worker name is killed too ---
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function IsProcessImageRunning(const ImageName: String): Boolean;") !=
                       std::string::npos,
                   "G6: a process-existence probe exists for the post-kill wait");
        TEST_CHECK(iss.find("procedure WaitForProcessImageExit(const ImageName: String);") !=
                       std::string::npos,
                   "G6: the bounded wait procedure exists");
        TEST_CHECK(iss.find("ENGINE_EXIT_WAIT_STEPS = 40;") != std::string::npos &&
                       iss.find("ENGINE_EXIT_POLL_MS = 250;") != std::string::npos &&
                       iss.find("Sleep(ENGINE_EXIT_POLL_MS);") != std::string::npos,
                   "G6: the wait is bounded (~10 s) and polled - it can never hang the "
                   "installer");
        TEST_CHECK(iss.find("LEGACY_WORKER_FILENAME = "
                            "'Emebalachat.Engine.ggml-translate.exe';") != std::string::npos,
                   "G6: the legacy worker image name is a named constant");
        TEST_CHECK(iss.find("KillProcessImage(LEGACY_WORKER_FILENAME,") != std::string::npos,
                   "G6: the legacy worker is taskkilled (it holds a handle on the "
                   "pre-unification file the absorb must rename)");
        const std::string stop = body_of("procedure StopRunningEngineHost();");
        TEST_CHECK(!stop.empty() &&
                       stop.find("WaitForProcessImageExit(ENGINE_HOST_FILENAME);") !=
                           std::string::npos &&
                       stop.find("WaitForProcessImageExit(WORKER_FILENAME);") !=
                           std::string::npos &&
                       stop.find("WaitForProcessImageExit(LEGACY_WORKER_FILENAME);") !=
                           std::string::npos,
                   "G6: every kill is followed by a wait inside StopRunningEngineHost "
                   "(the [Files] copy no longer races a dying process)");
        TEST_CHECK(iss.find("if ShouldInstallEngineHost() or ShouldInstallEngineWorker() then") !=
                       std::string::npos,
                   "G6: the stop runs when EITHER owned slot needs replacement (a live "
                   "worker alone can fail the copy)");
    }

    // ---- G3: the engine also lands in the ORIGINAL user's store -------------
    if (!iss.empty()) {
        TEST_CHECK(iss.find("procedure DeployOwnedEngineStoreForOriginalUser();") !=
                       std::string::npos,
                   "G3: the original-user engine deployment step exists");
        TEST_CHECK(iss.find("OrigEngineDir := UserLocalAppDataPath + COMMON_STORE_SUFFIX + "
                            "'\\engine';") != std::string::npos,
                   "G3: it reuses the resolved original-user LocalAppData (the store the "
                   "running app reads), not the elevated token");
        TEST_CHECK(iss.find("if SameText(OrigEngineDir, ElevatedEngineDir) then") !=
                       std::string::npos,
                   "G3: same-account elevation deploys once (no duplicate store write)");
        TEST_CHECK(iss.find("if SubtreeReparsePoint(UserLocalAppDataPath, "
                            "COMMON_STORE_SUFFIX + '\\engine') then") != std::string::npos,
                   "G3: the directory chain is reparse-guarded exactly like the models "
                   "path (a planted junction is refused, log-only)");
        TEST_CHECK(iss.find("StoreWriteRefused('DeployOwnedEngineStoreForOriginalUser/"
                            "pre-copy',") != std::string::npos,
                   "G3: the destination FILE leaf is reparse-guarded at the last instant "
                   "before every copy (FIX-R4 write-time helper)");
        TEST_CHECK(iss.find("else if FileCopy(SrcPath, DestPath, False) then") !=
                       std::string::npos,
                   "G3: the verified elevated-store bytes are copied into the user's "
                   "store");
        TEST_CHECK(iss.find("if not CopyWanted then") != std::string::npos,
                   "G3: only files whose post-copy verification passed are deployed "
                   "(a failed copy is never laundered into the app's store)");
        TEST_CHECK(iss.find("WriteOriginalUserEngineVersionFile(OrigEngineDir);") !=
                           std::string::npos &&
                       iss.find("WriteOriginalUserComponentsFile(OrigEngineDir);") !=
                           std::string::npos,
                   "G3: the version records are written into the original-user store "
                   "too (the next installer reads them there)");
        TEST_CHECK(iss.find("StoreWriteRefused('WriteOriginalUserEngineVersionFile/"
                            "pre-write',") != std::string::npos,
                   "G3: the original-user engine.version write is leaf-inclusive "
                   "guarded at the last instant (FIX-R4 write-time helper)");
        // The elevated-context write that preserves backward compatibility.
        TEST_CHECK(iss.find("Source: \"..\\build\\Emebala.Engine.exe\"; DestDir: "
                            "\"{localappdata}\\Emebala\\Common\\engine\"") != std::string::npos,
                   "G3: the elevated-context [Files] deployment is retained "
                   "(backward compatibility)");
    }

    // ---- item 7: completion verification + a loud user-facing warning ------
    if (!iss.empty()) {
        TEST_CHECK(iss.find("procedure VerifyEngineStoreDeployment();") != std::string::npos,
                   "item 7: the ssDone store verification step exists");
        const std::string done =
            region_between(iss, "if CurStep = ssDone then", "\n// ---");
        TEST_CHECK(!done.empty(), "item 7: the ssDone step is locatable");
        if (!done.empty()) {
            TEST_CHECK(done.find("VerifyEngineStoreDeployment();") != std::string::npos,
                       "item 7: the completion check runs the engine-store verification");
            TEST_CHECK(done.find("if EngineStoreVerifyFailed then") != std::string::npos &&
                           done.find("mbError, MB_OK") != std::string::npos,
                       "item 7: a failed verification is surfaced as an error box, not "
                       "only logged");
            TEST_CHECK(done.find("unins000.exe") != std::string::npos,
                       "item 7: the pre-existing uninstaller guard is untouched");
        }
        TEST_CHECK(iss.find("EngineBundledHashHost := "
                            "EngineBundledFileSha256(ENGINE_HOST_FILENAME);") !=
                       std::string::npos,
                   "item 7: the ssDone re-check reuses the bundled hashes captured at "
                   "ssPostInstall (no second extraction assumption)");
    }

    // ---- req055 revision pin consistency (pre-existing broken-test class) ---
    // The REQ-055 suite pins the engine revision as TEXT. It drifted once
    // already (setup.iss moved to .r13 while the pin still said .r12 and the
    // suite failed), so the pin must be derived from - and checked against -
    // the installer source instead of being trusted in isolation.
    {
        const std::string marker = "#define ENGINE_REVISION \"";
        std::string rev;
        const size_t m = iss.find(marker);
        if (m != std::string::npos) {
            const size_t start = m + marker.size();
            const size_t end = iss.find('"', start);
            if (end != std::string::npos) rev = iss.substr(start, end - start);
        }
        TEST_CHECK(!rev.empty(),
                   "revision: setup.iss carries a parseable ENGINE_REVISION define");
        TEST_CHECK(rev != ".r12" && rev != ".r13",
                   "revision: the v0.10.6 engine binaries carry a NEW revision (not the "
                   "pre-WS4 .r12/.r13 stamps)");
        std::string pin_src;
        const bool pin_readable =
            read_src(L"tests\\req055_live_pin_tests.inc", pin_src);
        TEST_CHECK(pin_readable, "revision: the req055 pin suite is readable");
        if (pin_readable && !rev.empty()) {
            TEST_CHECK(pin_src.find("#define ENGINE_REVISION \\\"" + rev + "\\\"") !=
                           std::string::npos,
                       "revision: the REQ-055 pin tracks the CURRENT setup.iss revision "
                       "(the .r12-while-source-says-.r13 drift must not recur)");
        }
    }

    // ---- 261108_0002 (FIX-R2, R1-10): the elevated [Files] leaf guards ------
    // The dispute-verification instance CONFIRMED that the three elevated
    // engine-store [Files] entries were guarded only by EngineStoreRefused,
    // whose walk stops at the '\engine' DIRECTORY: a reparse point planted at
    // the fixed FILE name was untested before Inno's copy. These pins hold the
    // fix in place: a leaf-inclusive pre-copy gate on each entry, and a
    // write-time BeforeInstall re-check.
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function EngineStoreDestinationPath(const DeployedFileName: "
                            "String): String;") != std::string::npos,
                   "R1-10: the elevated destination path is built in ONE place");
        TEST_CHECK(iss.find("function EngineStoreLeafRefusedFor(const Site, DeployedFileName: "
                            "String): Boolean;") != std::string::npos,
                   "R1-10: the pre-copy leaf gate exists (reuses EngineStoreLeafRefused, the "
                   "existing leaf-inclusive predicate)");
        const std::string leaf_gate = body_of("function EngineStoreLeafRefusedFor(");
        TEST_CHECK(!leaf_gate.empty() &&
                       leaf_gate.find("EngineStoreLeafRefused(Site + '/before-copy', "
                                      "EngineStoreDestinationPath(DeployedFileName))") !=
                           std::string::npos,
                   "R1-10: the gate tests the EXACT destination file (leaf-inclusive), not "
                   "just the directory chain");
        TEST_CHECK(!leaf_gate.empty() &&
                       leaf_gate.find("LatchEngineStoreVerifyFailed(") != std::string::npos,
                   "R1-10: a refused leaf copy latches EngineStoreVerifyFailed (the ssDone "
                   "MsgBox surfaces it instead of a silent 'installed fine'); FIX-R3 routes it "
                   "through the one latch helper");
        // Each of the three Check functions consults its own destination leaf.
        const std::string host_check = body_of("function ShouldInstallEngineHost(): Boolean;");
        const std::string worker_check = body_of("function ShouldInstallEngineWorker(): Boolean;");
        const std::string manifest_check =
            body_of("function ShouldInstallEngineWorkerManifest(): Boolean;");
        TEST_CHECK(!host_check.empty() &&
                       host_check.find("EngineStoreLeafRefusedFor('ShouldInstallEngineHost', "
                                       "ENGINE_HOST_FILENAME)") != std::string::npos,
                   "R1-10: ShouldInstallEngineHost refuses a leaf reparse point");
        TEST_CHECK(!worker_check.empty() &&
                       worker_check.find("EngineStoreLeafRefusedFor('ShouldInstallEngineWorker', "
                                       "WORKER_FILENAME)") != std::string::npos,
                   "R1-10: ShouldInstallEngineWorker refuses a leaf reparse point");
        TEST_CHECK(!manifest_check.empty() &&
                       manifest_check.find(
                           "EngineStoreLeafRefusedFor('ShouldInstallEngineWorkerManifest', "
                           "WORKER_MANIFEST_FILENAME)") != std::string::npos,
                   "R1-10: ShouldInstallEngineWorkerManifest refuses a leaf reparse point");
        // The write-time re-check (BeforeInstall) on all three entries.
        TEST_CHECK(iss.find("procedure RefuseIfEngineLeafReparse(const DeployedFileName: "
                            "String);") != std::string::npos,
                   "R1-10: the write-time BeforeInstall hook exists");
        const std::string hook_body = body_of("procedure RefuseIfEngineLeafReparse(");
        TEST_CHECK(!hook_body.empty() &&
                       hook_body.find("DeleteFile(DestPath)") != std::string::npos,
                   "R1-10: on a hit the hook removes the LINK itself (DeleteFileW never "
                   "follows a reparse point) so the copy writes a real file");
        TEST_CHECK(!hook_body.empty() &&
                       hook_body.find("LatchEngineStoreVerifyFailed(") != std::string::npos,
                   "R1-10: the write-time refusal also latches the verify-failed flag (through "
                   "the FIX-R3 latch helper)");
        TEST_CHECK(iss.find("; BeforeInstall: RefuseIfEngineLeafReparse('Emebala.Engine.exe')") !=
                       std::string::npos,
                   "R1-10: the host entry carries its BeforeInstall re-check");
        TEST_CHECK(iss.find("; BeforeInstall: RefuseIfEngineLeafReparse("
                            "'Emebala.Engine.ggml-translate.exe')") != std::string::npos,
                   "R1-10: the worker entry carries its BeforeInstall re-check");
        TEST_CHECK(iss.find("; BeforeInstall: RefuseIfEngineLeafReparse("
                            "'worker.ggml-translate.manifest')") != std::string::npos,
                   "R1-10: the manifest entry carries its BeforeInstall re-check");
        // The literal names passed to the hook must equal the deployed-name
        // constants, or the hook would guard the wrong leaf.
        TEST_CHECK(iss.find("ENGINE_HOST_FILENAME = 'Emebala.Engine.exe';") != std::string::npos &&
                       iss.find("WORKER_FILENAME = 'Emebala.Engine.ggml-translate.exe';") !=
                           std::string::npos &&
                       iss.find("WORKER_MANIFEST_FILENAME = 'worker.ggml-translate.manifest';") !=
                           std::string::npos,
                   "R1-10: the BeforeInstall literals match the deployed-name constants");
        // The non-owned slots stay untouched (no BeforeInstall on the asr pair).
        TEST_CHECK(iss.find("WORKER_ASR_FILENAME") != std::string::npos,
                   "R1-10: the non-owned asr slot still exists and is outside this guard");
    }

    // ---- 261108_0002 (FIX-R2, item C): the verification's BYTE SOURCE ------
    // The WS4 post-copy verification and the R1-11 pre-copy byte-status arm
    // both hash the BUNDLED bytes obtained through ExtractTemporaryFile. A
    // throwaway probe (tools_tmp_261108_fixr2/probe/probe_nodc.iss, the real
    // entry shape: NO dontcopy, '..\'-prefixed Source) proved on Inno Setup 6
    // that the BASE-NAME argument works ("FORM2(probe_payload.txt)=OK") while
    // the '..\' form fails ("FORM1(...)=FAIL: ... was not found"). These pins
    // keep that proven shape: base name first, the install entries WITHOUT
    // dontcopy (dontcopy would stop the actual install), and the one-shot
    // probe-outcome log line.
    if (!iss.empty()) {
        TEST_CHECK(iss.find("ExtractTemporaryFile(FileName);") != std::string::npos &&
                       iss.find("ExtractTemporaryFile(FileName);") <
                           iss.find("ExtractTemporaryFile('..\\build\\' + FileName);"),
                   "item C: the bundled bytes are extracted by BASE NAME first (the "
                   "empirically proven argument form)");
        TEST_CHECK(iss.find("EngineBundledProbeLogged := True;") != std::string::npos &&
                       iss.find("bundled-bytes source = the install [Files] entry addressed by "
                                "BASE NAME") != std::string::npos,
                   "item C: the proven byte source is logged once per run (shape-only)");
        // The install entries must NOT carry dontcopy: that flag would prevent
        // the engine from ever being installed.
        TEST_CHECK(iss.find("Source: \"..\\build\\Emebala.Engine.exe\"; DestDir: "
                            "\"{localappdata}\\Emebala\\Common\\engine\"; Flags: ignoreversion "
                            "uninsneveruninstall;") != std::string::npos &&
                       iss.find("Source: \"..\\build\\Emebala.Engine.ggml-translate.exe\"; "
                                "DestDir: \"{localappdata}\\Emebala\\Common\\engine\"; Flags: "
                                "ignoreversion uninsneveruninstall;") != std::string::npos &&
                       iss.find("Source: \"..\\build\\worker.ggml-translate.manifest\"; DestDir: "
                                "\"{localappdata}\\Emebala\\Common\\engine\"; Flags: ignoreversion "
                                "uninsneveruninstall;") != std::string::npos,
                   "item C: the three install entries keep their copy flags (no dontcopy - the "
                   "engine must still install)");
    }

    // ---- 261108_0002 (FIX-R3, round-2 P0): the hook must be LEAF-ONLY ------
    // The previous hook used the CHAIN-scoped predicate as the pre-condition of
    // DeleteFile, so a parent junction swapped in after the Check could redirect
    // the elevated delete to a same-named file outside the store. The pins below
    // hold the split: a LEAF-ONLY attribute predicate, a PARENT-CHAIN-only walk,
    // the chain branch first and delete-free, and exactly one DeleteFile site.
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function LeafIsReparsePoint(const FilePath: String): Boolean;") !=
                       std::string::npos,
                   "R3/P0: a LEAF-ONLY reparse predicate exists");
        const std::string leaf_body = body_of("function LeafIsReparsePoint(");
        TEST_CHECK(!leaf_body.empty() &&
                       leaf_body.find("GetFileAttributesW(FilePath)") != std::string::npos,
                   "R3/P0: the leaf predicate is an ATTRIBUTE test on the final component");
        TEST_CHECK(!leaf_body.empty() &&
                       leaf_body.find("for I := 1 to Length(FilePath)") == std::string::npos &&
                       leaf_body.find("Copy(FilePath") == std::string::npos,
                   "R3/P0: the leaf predicate walks NO parent component (leaf-only by "
                   "construction, not by comment)");
        TEST_CHECK(iss.find("function ParentChainReparsePoint(const Base, FilePath: String; "
                            "out Offender: String): Boolean;") != std::string::npos,
                   "R3/P0: a PARENT-CHAIN-only walk exists and reports the offending component");
        const std::string chain_body = body_of("function ParentChainReparsePoint(");
        TEST_CHECK(!chain_body.empty() &&
                       chain_body.find("Offender := Prefix;") != std::string::npos &&
                       chain_body.find("GetFileAttributesW(FilePath)") == std::string::npos,
                   "R3/P0: the chain walk tests ancestors only (never the final component)");

        const std::string hook = body_of("procedure RefuseIfEngineLeafReparse(");
        TEST_CHECK(!hook.empty(),
                   "R3/P0: the BeforeInstall hook body is locatable");
        const size_t chain_at = hook.find("if ParentChainReparsePoint(");
        const size_t leaf_at = hook.find("if not LeafIsReparsePoint(");
        const size_t del_at = hook.find("DeleteFile(DestPath)");
        TEST_CHECK(chain_at != std::string::npos && leaf_at != std::string::npos &&
                       del_at != std::string::npos && chain_at < leaf_at && leaf_at < del_at,
                   "R3/P0: the hook runs parent-chain -> leaf -> delete in that order");
        // Exactly one DeleteFile in the hook, and none inside the chain branch.
        size_t del_count = 0;
        for (size_t at = hook.find("DeleteFile("); at != std::string::npos;
             at = hook.find("DeleteFile(", at + 1)) {
            ++del_count;
        }
        TEST_CHECK(del_count == 1,
                   "R3/P0: the hook has exactly ONE DeleteFile site (the leaf branch)");
        if (chain_at != std::string::npos) {
            const size_t chain_exit = hook.find("Exit;", chain_at);
            TEST_CHECK(chain_exit != std::string::npos,
                       "R3/P0: the parent-chain branch refuses and exits");
            const std::string chain_branch =
                hook.substr(chain_at, chain_exit == std::string::npos ? std::string::npos
                                                                      : chain_exit - chain_at);
            TEST_CHECK(chain_branch.find("DeleteFile") == std::string::npos,
                       "R3/P0: the parent-chain branch performs NO delete on the composed path");
            TEST_CHECK(chain_branch.find("LatchEngineStoreVerifyFailed(") != std::string::npos,
                       "R3/P0: a parent-chain refusal latches the loud ssDone failure");
        }
        TEST_CHECK(!hook.empty() &&
                       hook.find("' - refusing (no write, NO delete on the composed path; the link "
                                 "is never followed).'") != std::string::npos,
                   "R3/P0: the refusal states the fail-closed contract in the log");
    }

    // ---- 261108_0002 (FIX-R3, round-2 P2/R2-02): one latch, every refusal ---
    if (!iss.empty()) {
        TEST_CHECK(iss.find("procedure LatchEngineStoreVerifyFailed(const Reason: String);") !=
                       std::string::npos,
                   "R2-02: one common latch helper exists");
        // The flag must have exactly ONE assignment in the script: the helper.
        size_t latch_assigns = 0;
        for (size_t at = iss.find("EngineStoreVerifyFailed := True;"); at != std::string::npos;
             at = iss.find("EngineStoreVerifyFailed := True;", at + 1)) {
            ++latch_assigns;
        }
        TEST_CHECK(latch_assigns == 1,
                   "R2-02: EngineStoreVerifyFailed is assigned in exactly ONE place (the helper), "
                   "so no refusal branch can latch silently by hand");
        const std::string latch_body = body_of("procedure LatchEngineStoreVerifyFailed(");
        TEST_CHECK(!latch_body.empty() &&
                       latch_body.find("EngineStoreVerifyFailed := True;") != std::string::npos,
                   "R2-02: the single assignment lives inside the helper");
        // The previously-silent branches now call it.
        TEST_CHECK(iss.find("original-user deployment chain reparse refusal") != std::string::npos,
                   "R2-02: the original-user deployment chain refusal latches");
        // FIX-R4 moved the original-user record-chain refusals onto the shared
        // write-time helper, which latches through the same one helper; the
        // assertion below therefore pins the helper call sites (the latch path)
        // rather than the pre-FIX-R4 reason literals.
        TEST_CHECK(iss.find("original-user components.json chain reparse refusal") !=
                       std::string::npos &&
                       iss.find("StoreWriteRefused('WriteOriginalUserEngineVersionFile/"
                                "pre-write',") != std::string::npos &&
                       iss.find("StoreWriteRefused('WriteOriginalUserComponentsFile/"
                                "pre-write',") != std::string::npos,
                   "R2-02: every original-user record-chain refusal latches (chain guard + "
                   "the FIX-R4 latched write-time helper)");
        TEST_CHECK(iss.find("original-user engine.version write failed") != std::string::npos &&
                       iss.find("original-user components.json write failed") != std::string::npos,
                   "R2-02: both original-user record-write failures latch");
        TEST_CHECK(iss.find("original-user components.json merge refused") != std::string::npos,
                   "R2-02: a refused components merge latches (the record was not advanced)");
        TEST_CHECK(iss.find("ShouldInstallEngineHost: elevated engine-store chain reparse "
                            "refusal (host)") != std::string::npos &&
                       iss.find("ShouldInstallEngineWorker: elevated engine-store chain reparse "
                                "refusal (worker)") != std::string::npos &&
                       iss.find("ShouldInstallEngineWorkerManifest: elevated engine-store chain "
                                "reparse refusal (manifest)") != std::string::npos,
                   "R2-02: the three elevated chain refusals latch too");
    }

    // ---- 261108_0002 (FIX-R3, round-2 P3/R2-04): probe cache honesty -------
    if (!iss.empty()) {
        const std::string probe = body_of("function EngineBundledHashProbe(");
        TEST_CHECK(!probe.empty(), "R2-04: the bundled-hash probe body is locatable");
        const size_t host_latch = probe.find("EngineBundledHashHostProbed := True;");
        const size_t extract = probe.find("ExtractTemporaryFile(FileName);");
        TEST_CHECK(host_latch != std::string::npos && extract != std::string::npos &&
                       host_latch < extract,
                   "R2-04: the probe latches the ATTEMPT before extracting, so a failed probe is "
                   "not retried by later decision callers");
        TEST_CHECK(iss.find("EngineBundledHashManifestProbed: Boolean;") != std::string::npos &&
                       probe.find("EngineBundledHashManifestProbed := True;") != std::string::npos &&
                       probe.find("EngineBundledHashManifest := Result") != std::string::npos,
                   "R2-04: the manifest rides the same probe-once cache (comment and behaviour "
                   "now agree: every cached file is probed at most once per run)");
        // The success path must not re-latch (only the value is stored).
        size_t host_latch_count = 0;
        for (size_t at = probe.find("EngineBundledHashHostProbed := True;");
             at != std::string::npos;
             at = probe.find("EngineBundledHashHostProbed := True;", at + 1)) {
            ++host_latch_count;
        }
        TEST_CHECK(host_latch_count == 1,
                   "R2-04: the host probe latch is set exactly once in the body (before the "
                   "attempt), not again on success");
        TEST_CHECK(iss.find("'261108_0002: WARNING could not extract the bundled copy of '") !=
                       std::string::npos,
                   "R2-04: a failed probe still says 'UNAVAILABLE' (status 0 semantics kept - "
                   "no defect claim, no re-copy storm)");
    }

    // ---- 261108_0002 (FIX-R4, round-2 HIGH-2): uninstall store-tree guard ----
    // The pre-FIX-R4 guard walked only the PREFIXES of `\Emebala\Common`, so a
    // junction planted at the `engine` or `models` CHILD (or at any file below
    // them) was never tested, and the elevated delete list resolved through it.
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function DirTreeHasReparsePoint(const Dir: String): Boolean;") !=
                       std::string::npos,
                   "HIGH-2: a recursive descendant reparse walk exists");
        const std::string tree_body = body_of("function DirTreeHasReparsePoint(");
        TEST_CHECK(!tree_body.empty() &&
                       tree_body.find("(FindRec.Attributes and "
                                      "FILE_ATTRIBUTE_REPARSE_POINT) <> 0") != std::string::npos &&
                       tree_body.find("DirTreeHasReparsePoint(Child)") != std::string::npos,
                   "HIGH-2: the walk tests the LINK's own attributes at every depth "
                   "(FindFirst/FindNext do not follow the reparse point)");
        TEST_CHECK(iss.find("function StoreTreeRefused(const Site, CommonDir: String): Boolean;") !=
                       std::string::npos,
                   "HIGH-2: one store-tree guard serves the uninstall cleanup");
        const std::string store_tree = body_of("function StoreTreeRefused(");
        TEST_CHECK(!store_tree.empty() &&
                       store_tree.find("COMMON_STORE_SUFFIX + '\\engine'") !=
                           std::string::npos &&
                       store_tree.find("COMMON_STORE_SUFFIX + '\\models'") !=
                           std::string::npos &&
                       store_tree.find("DirTreeHasReparsePoint(CommonDir + '\\engine')") !=
                           std::string::npos &&
                       store_tree.find("DirTreeHasReparsePoint(CommonDir + '\\models')") !=
                           std::string::npos,
                   "HIGH-2: the guard covers the root, BOTH children and every "
                   "descendant (the 12-file delete list can no longer resolve through a "
                   "planted child junction)");
        const std::string cleanup = body_of("procedure CleanupSharedEngineStore(");
        TEST_CHECK(!cleanup.empty() &&
                       cleanup.find("if StoreTreeRefused('CleanupSharedEngineStore', "
                                    "CommonDir) then") != std::string::npos,
                   "HIGH-2: the uninstall cleanup consults that guard BEFORE any delete");
        TEST_CHECK(!cleanup.empty() &&
                       cleanup.find("if StoreTreeRefused(") <
                           cleanup.find("DeleteOwnedFile(EngineDir + '\\' + "
                                        "ENGINE_HOST_FILENAME)"),
                   "HIGH-2: the guard precedes the first owned-file delete");
        TEST_CHECK(!cleanup.empty() &&
                       cleanup.find("if StoreTreeRefused(") <
                           cleanup.find("RemoveBundledItemFiles(Items[I], ModelsDir)"),
                   "HIGH-2: the guard precedes the models bundled-file delete loop");
        TEST_CHECK(cleanup.find("the ENTIRE shared store is preserved (261108_0002 FIX-R4 "
                                "HIGH-2)") != std::string::npos,
                   "HIGH-2: a hit preserves the whole store (fail-closed direction kept)");
    }

    // ---- 261108_0002 (FIX-R4, MEDIUM): write-time re-test at every [Code]
    // store write. FIX-R3 covered the [Files] entries with a BeforeInstall hook;
    // the [Code] writers (FileCopy / SaveStringToFile / registry writer) had
    // only decision-time checks, which CopyFileW/CreateFile then raced.
    if (!iss.empty()) {
        TEST_CHECK(iss.find("function StoreWriteRefused(const Site, Base, DestPath: String): "
                            "Boolean;") != std::string::npos,
                   "MEDIUM: one write-time refusal helper exists");
        const std::string write_helper = body_of("function StoreWriteRefused(");
        TEST_CHECK(!write_helper.empty() &&
                       write_helper.find("StoreChainRefused(Base, DestPath)") !=
                           std::string::npos &&
                       write_helper.find("LatchEngineStoreVerifyFailed(") != std::string::npos,
                   "MEDIUM: the helper re-tests the FULL chain INCLUDING the leaf "
                   "(StoreChainRefused's trailing full-path attribute test) and latches");
        const char* const write_sites[] = {
            "StoreWriteRefused('WriteEngineVersionFile/pre-write',",
            "StoreWriteRefused('WriteComponentsFile/pre-write',",
            "StoreWriteRefused('WriteOriginalUserEngineVersionFile/pre-write',",
            "StoreWriteRefused('WriteOriginalUserComponentsFile/pre-write',",
            "StoreWriteRefused('DeployOwnedEngineStoreForOriginalUser/pre-copy',",
            "StoreWriteRefused('WriteRegistryFile/create-pre-write',",
            "StoreWriteRefused('WriteRegistryFile/create-pre-write-tmp',",
            "StoreWriteRefused('WriteRegistryFile/merge-pre-write',",
            "StoreWriteRefused('WriteRegistryFile/merge-pre-write-tmp',",
        };
        for (const char* site : write_sites) {
            TEST_CHECK(iss.find(site) != std::string::npos,
                       std::string("MEDIUM: write-time refusal wired at ") + site);
        }
        // Each write site must be checked BEFORE its primitive.
        struct SiteOrder { const char* site; const char* primitive; };
        const SiteOrder orders[] = {
            {"StoreWriteRefused('WriteEngineVersionFile/pre-write',",
             "SaveStringToFile(VersionPath, ENGINE_BUNDLED_VERSION, False)"},
            {"StoreWriteRefused('WriteComponentsFile/pre-write',",
             "SaveStringToFile(ComponentsPath, OutJson, False)"},
            {"StoreWriteRefused('DeployOwnedEngineStoreForOriginalUser/pre-copy',",
             "FileCopy(SrcPath, DestPath, False)"},
        };
        for (const SiteOrder& o : orders) {
            const size_t a = iss.find(o.site);
            const size_t b = iss.find(o.primitive);
            TEST_CHECK(a != std::string::npos && b != std::string::npos && a < b,
                       std::string("MEDIUM: the write-time check precedes ") + o.primitive);
        }
        // MED-1(b): the two refusals the reviewer found silent now latch.
        const std::string absorb = body_of("procedure AbsorbLegacyWorkerFileName(");
        TEST_CHECK(!absorb.empty() &&
                       absorb.find("LatchEngineStoreVerifyFailed('AbsorbLegacyWorkerFileName:") !=
                           std::string::npos,
                   "MED-1b: the legacy-absorb chain refusal latches");
        TEST_CHECK(!absorb.empty() &&
                       absorb.find("ParentChainReparsePoint(ExpandConstant('{localappdata}'), "
                                   "LegacyPath, Offender)") != std::string::npos,
                   "MED-1b: the absorb re-tests the PARENT CHAIN before its delete/rename "
                   "(a junctioned parent can no longer redirect the fixed legacy name)");
        const std::string asr = body_of("function ShouldInstallEngineWorkerAsr(): Boolean;");
        TEST_CHECK(!asr.empty() &&
                       asr.find("LatchEngineStoreVerifyFailed('ShouldInstallEngineWorkerAsr:") !=
                           std::string::npos,
                   "MED-1b: the asr Check's chain refusal latches");
    }

    // ---- 261108_0002 (FIX-R4, MED-1a): hash-unavailable DEPLOYS ANYWAY ------
    if (!iss.empty()) {
        TEST_CHECK(iss.find("no bundled hash for ' + Names[I] + '; not deployed to the "
                            "original-user store.") == std::string::npos,
                   "MED-1a: the old 'no bundled hash => not deployed' skip is gone");
        TEST_CHECK(iss.find("deploying UNVERIFIED anyway (the record is NOT advanced; the "
                            "next run re-verifies).") != std::string::npos,
                   "MED-1a: a failed probe still deploys the engine the app reads, and says "
                   "it is unverified");
        TEST_CHECK(iss.find("deployed UNVERIFIED (no bundled hash); its record is not "
                            "advanced.") != std::string::npos,
                   "MED-1a: an unverified deployment stamps nothing (self-heals next run)");
        const std::string deploy = body_of("procedure DeployOwnedEngineStoreForOriginalUser(");
        TEST_CHECK(!deploy.empty() &&
                       deploy.find("if Hashes[I] = '' then\n        Log('261108_0002 (FIX-R4) "
                                   "MED-1a: no bundled hash") != std::string::npos,
                   "MED-1a: the deploy branch runs BEFORE the hash check (no early skip)");
    }

    // ---- 261108_0002 (FIX-R4, MED-3): the DelTree guard covers descendants ---
    if (!iss.empty()) {
        const size_t deltree = iss.find("DelTree(LocalAppData");
        const size_t desc_guard = iss.find("DirTreeHasReparsePoint(LocalAppData)");
        TEST_CHECK(deltree != std::string::npos && desc_guard != std::string::npos &&
                       desc_guard < deltree,
                   "MED-3: the user-data recursive delete is refused when ANY descendant is "
                   "a reparse point (not just the chain to the directory)");
        TEST_CHECK(iss.find("the elevated cleanup is refused and the directory is left "
                            "untouched (261029_0001 R3 F-R3-01 / 261108_0002 FIX-R4 MED-3)") !=
                       std::string::npos,
                   "MED-3: the refusal log keeps the fail-closed contract explicit");
    }

    std::cout << "  bt9 installer upgrade safety: "
              << (g_failed_count - failures_before) << " new failures" << std::endl;
}
