// ---------------------------------------------------------------------------
// M7 A-7 + A-8 (session 260922_0001, DEC-003/DEC-004): family-shared
// "EmebalaSetup" installer mutex gate & ARP DisplayName prefix contract
// (TestM7SetupGateMutex).
//
// A-7 (plan v2.1 결정 기록 A-7): all Emebala-family installers write the same
// per-user common store, so every family setup/uninstall entry point must
// take the advisory Local\EmebalaSetup gate before touching it. Real runtime
// behavior needs a physical install (user-verified domain), so this suite
// pins the STRUCTURE in installer/setup.iss, following the A-1/A-2 .inc
// ResolveRepoFile source-pin precedent:
//   (a) the exact mutex name constant (Local\ namespace — per-session scope
//       rationale lives in the setup.iss comment next to it);
//   (b) BOTH event entry points wired: InitializeSetup and InitializeUninstall
//       abort through FamilySetupGateAcquire;
//   (c) the acquire helper is CheckForMutexes -> CreateMutex with the
//       chained-mode {param:SL5} child guard (the master gates; the elevated
//       child of the SAME install inherits, so it must not self-abort);
//   (d) honest localized messaging: the gate message keys exist for the exact
//       11-language F3 policy set (the other 21 languages fall back to
//       english — same documented policy as SharedEngineKeptInUse*).
//
// A-8 (plan v2.1 결정 기록 A-8): the family-detection contract is the ARP
// DisplayName prefix "Emebala" (7 chars). Pins: [Setup] AppName carries the
// prefix (ARP DisplayName "Emebala Chat" is derived from it), and the
// RegistryHasOtherEmebalaApp scanner keeps matching exactly that prefix while
// excluding this app's own AppId-derived subkey — the same rule the
// Listener/Reader installers must satisfy ("Emebala Listener").
// The uninstall-contract gate (tools/check_uninstall_contract.py CHECK 10)
// re-derives (a)-(c) independently.
// ---------------------------------------------------------------------------

void TestM7SetupGateMutex() {
    std::cout << "[RUN] Testing M7 A-7/A-8 family setup-gate mutex & naming..."
              << std::endl;
    const int failures_before = g_failed_count;

    std::string iss_src;
    {
        const std::filesystem::path p = ResolveRepoFile(L"installer\\setup.iss");
        TEST_CHECK(!p.empty(), "pin: setup.iss resolves");
        std::ifstream in(p, std::ios::binary);
        TEST_CHECK(static_cast<bool>(in), "pin: setup.iss readable");
        std::ostringstream ss;
        ss << in.rdbuf();
        iss_src = ss.str();
    }

    // ---- (a) A-7: exact family-shared mutex name constant -------------------
    TEST_CHECK(iss_src.find("FAMILY_SETUP_MUTEX = 'Local\\EmebalaSetup';") !=
                   std::string::npos,
               "A-7 (a): Family mutex constant is exactly Local\\EmebalaSetup");

    // ---- (b) A-7: both entry points wired through the gate ------------------
    {
        const size_t isetup = iss_src.find("function InitializeSetup(): Boolean;");
        const size_t iunin = iss_src.find("function InitializeUninstall(): Boolean;");
        TEST_CHECK(isetup != std::string::npos,
                   "A-7 (b): InitializeSetup event implemented");
        TEST_CHECK(iunin != std::string::npos,
                   "A-7 (b): InitializeUninstall event implemented");
        // Each event body's first statement must abort through the gate
        // helper (search only forward from each definition).
        TEST_CHECK(isetup != std::string::npos &&
                       iss_src.find("Result := not FamilySetupGateAcquire();", isetup) < iunin,
                   "A-7 (b): InitializeSetup aborts via FamilySetupGateAcquire");
        TEST_CHECK(iunin != std::string::npos &&
                       iss_src.find("Result := not FamilySetupGateAcquire();", iunin) !=
                           std::string::npos,
                   "A-7 (b): InitializeUninstall aborts via FamilySetupGateAcquire");
    }

    // ---- (c) A-7: acquire helper — check/create + chained-child guard -------
    {
        const size_t helper = iss_src.find("function FamilySetupGateAcquire(): Boolean;");
        TEST_CHECK(helper != std::string::npos, "A-7 (c): gate helper defined");
        if (helper != std::string::npos) {
            const size_t helper_end = iss_src.find("\nend;", helper);
            const std::string body = helper_end != std::string::npos
                                         ? iss_src.substr(helper, helper_end - helper)
                                         : iss_src.substr(helper);
            TEST_CHECK(body.find("CheckForMutexes(FAMILY_SETUP_MUTEX)") !=
                           std::string::npos,
                       "A-7 (c): gate checks the shared mutex before creating it");
            TEST_CHECK(body.find("CreateMutex(FAMILY_SETUP_MUTEX)") !=
                           std::string::npos,
                       "A-7 (c): gate acquires the mutex on the free path");
            TEST_CHECK(body.find("{param:SL5}") != std::string::npos,
                       "A-7 (c): chained-mode elevated child is exempt (master gated)");
            // Order matters: the SL5 exemption must precede the check, or the
            // elevated child would abort on the master's own mutex.
            TEST_CHECK(body.find("{param:SL5}") < body.find("CheckForMutexes("),
                       "A-7 (c): SL5 exemption is evaluated BEFORE the mutex check");
        }
    }

    // ---- (d) A-7: 11-language honest-notice coverage (F3 policy set) --------
    {
        static const char* kLangs[] = {"english", "korean", "japanese",
            "chinesesimplified", "chinesetraditional", "spanish", "portuguese",
            "brazilianportuguese", "french", "german", "italian"};
        int title_hits = 0, body_hits = 0;
        for (const char* lang : kLangs) {
            const std::string tkey = std::string(lang) + ".FamilySetupGateTitle=";
            const std::string bkey = std::string(lang) + ".FamilySetupGateBody=";
            title_hits += iss_src.find(tkey) != std::string::npos ? 1 : 0;
            body_hits += iss_src.find(bkey) != std::string::npos ? 1 : 0;
        }
        TEST_CHECK(title_hits == 11, "A-7 (d): gate title localized for all 11 policy languages");
        TEST_CHECK(body_hits == 11, "A-7 (d): gate body localized for all 11 policy languages");
    }

    // ---- A-8: DisplayName prefix contract -----------------------------------
    TEST_CHECK(iss_src.find("AppName=Emebala Chat") != std::string::npos,
               "A-8: [Setup] AppName carries the \"Emebala\" prefix (ARP DisplayName)");
    TEST_CHECK(iss_src.find("SameText(Copy(DisplayName, 1, 7), 'Emebala')") !=
                   std::string::npos,
               "A-8: family scanner matches the exact 7-char \"Emebala\" prefix");
    // The scanner must still exclude THIS app's own AppId-derived ARP subkey
    // (an in-progress uninstall must not count itself as "another family app").
    {
        const size_t scan = iss_src.find("function RegistryHasOtherEmebalaApp(RootKey: Integer): Boolean;");
        const size_t scan_end = scan != std::string::npos ? iss_src.find("\nend;", scan) : std::string::npos;
        const std::string scan_body = (scan != std::string::npos && scan_end != std::string::npos)
                                          ? iss_src.substr(scan, scan_end - scan)
                                          : std::string();
        TEST_CHECK(scan_body.find("'{E3B7A1C4-8D2F-4A6E-9C1B-5F0D3E8A7B2C}_is1'") !=
                       std::string::npos,
                   "A-8: own-AppId ARP subkey stays excluded from the family scan");
    }

    std::cout << "  m7 setup-gate mutex/naming: "
              << (g_failed_count - failures_before) << " new failures" << std::endl;
}
