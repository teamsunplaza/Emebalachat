// ===========================================================================
// 261116_0003 P9b-3 U1 (language-menu redesign, plan §5 P9): the CONFIG WRITE
// SURFACE for user-defined languages - the base the add-language dialog (U2),
// the probe runner (U3) and the badge surfaces (U4) write through.
//
// Pinned here, headlessly:
//   (1) PR9-11 creation validation: the reserved AUTO spellings, registry
//       duplicates and user-side duplicates are refused, a clean name is
//       accepted, and the refusal folds exactly like the storage sanitizer.
//   (2) PR9-12 locked mutators: add / update / remove plus the three probe
//       record verbs, including the refusal ORDER (carrier -> exists -> name
//       verdict -> capacity) and the source-scan proof that no other mutation
//       site of the registry survives anywhere under src/.
//   (3) The forward-compat carrier predicate: armed by a newer-schema block,
//       disarmed by a current one, and every write verb refuses while armed
//       (the edit would be DISCARDED on save - config.hpp carrier contract).
//   (4) PR10-07 write-seam caps: the canonical probe key, evidence and
//       checked-at are capped at the kMax* constants on the LIVE path (the
//       load-path caps are P9a's PR9-05), and an over-long model can no longer
//       split one logical key across two cache slots.
//   (5) The UTC checked-at helper (the config layer had no UTC formatter).
//   (6) Round trip: entries + probes recorded through the new verbs keep their
//       verdicts through ToJsonString/FromJsonString and a real
//       SaveToFile/LoadFromFile cycle.
//   (7) F-1 (R15-10): the probe-cache READ seam canonicalises its key through
//       that same builder, so a raw over-cap model (openai.model / the
//       user-.gguf registry id reach it uncapped) reads the verdict the writer
//       stored, while an already-canonical key stays a fixed point.
//
// The dialog itself (window, inline refusal message rendering) stays a
// manual-QA badge item: no window is created here (AGENTS.md §10 headless rule).
// ===========================================================================
void TestP9b3UserLanguageWriteSurface() {
    std::cout << "[RUN] Testing 261116_0003 P9b-3 U1 user-language write surface..."
              << std::endl;
    const int failures_before = g_failed_count;

    // Constants compared at RUNTIME (kMax*, band sizes) go through these
    // helpers so TEST_CHECK's `if` is never a constant condition (MSVC C4127
    // discipline, same as the P9b-2 suite).
    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
    const auto lt = [](std::size_t lhs, std::size_t rhs) { return lhs < rhs; };
    const auto gt = [](std::size_t lhs, std::size_t rhs) { return lhs > rhs; };

    // ---- (1) PR9-11 creation validation ------------------------------------
    {
        const LanguageInfo* auto_row = FindLanguageByCode("AUTO");
        TEST_CHECK(auto_row != nullptr, "P9b-3: the registry carries the AUTO row");
        // The reserved set IS the AUTO row's three spellings: a rename of that
        // row cannot silently desynchronise the refusal.
        TEST_CHECK(auto_row != nullptr &&
                       ValidateUserLanguageName(auto_row->name_en, "", {}) ==
                           UserLanguageNameVerdict::Reserved &&
                       ValidateUserLanguageName(auto_row->name_native, "", {}) ==
                           UserLanguageNameVerdict::Reserved &&
                       ValidateUserLanguageName(auto_row->code, "", {}) ==
                           UserLanguageNameVerdict::Reserved,
                   "PR9-11: the reserved set is exactly the AUTO row's English/native/code "
                   "spellings");
        TEST_CHECK(ValidateUserLanguageName("Auto Detect", "", {}) ==
                           UserLanguageNameVerdict::Reserved &&
                       ValidateUserLanguageName("AUTO", "", {}) ==
                           UserLanguageNameVerdict::Reserved &&
                       ValidateUserLanguageName("자동 감지", "", {}) ==
                           UserLanguageNameVerdict::Reserved,
                   "PR9-11: the three literal reserved spellings are refused");
        TEST_CHECK(ValidateUserLanguageName("  auto detect  ", "", {}) ==
                           UserLanguageNameVerdict::Reserved &&
                       ValidateUserLanguageName("", "auto", {}) ==
                           UserLanguageNameVerdict::Reserved,
                   "PR9-11: surrounding whitespace is trimmed and the fold is ASCII "
                   "case-insensitive on BOTH typed fields");
        TEST_CHECK(ValidateUserLanguageName("Auto\xE2\x80\x8B Detect", "", {}) ==
                           UserLanguageNameVerdict::Reserved,
                   "PR9-11: the PR9-03/PR10-08 invisible-character strip runs BEFORE the "
                   "reserved check (a ZWSP cannot smuggle the spelling through)");

        TEST_CHECK(ValidateUserLanguageName("Korean", "", {}) ==
                           UserLanguageNameVerdict::Duplicate &&
                       ValidateUserLanguageName("한국어", "", {}) ==
                           UserLanguageNameVerdict::Duplicate &&
                       ValidateUserLanguageName("ko", "", {}) ==
                           UserLanguageNameVerdict::Duplicate &&
                       ValidateUserLanguageName("", "Korean", {}) ==
                           UserLanguageNameVerdict::Duplicate,
                   "PR9-11: a name colliding with a registry row's English/native/code "
                   "spelling is refused");

        std::vector<UserLanguage> existing;
        existing.push_back(MakeUserLanguage("Samplelandic", ""));
        TEST_CHECK(ValidateUserLanguageName("samplelandic", "", existing) ==
                           UserLanguageNameVerdict::Duplicate &&
                       ValidateUserLanguageName("", "SAMPLELANDIC", existing) ==
                           UserLanguageNameVerdict::Duplicate,
                   "PR9-11: a name already owned by a registered user entry is refused "
                   "(the FindUserLanguage spelling set)");
        TEST_CHECK(ValidateUserLanguageName("Samplelandic", "", existing, 0) ==
                           UserLanguageNameVerdict::Accepted,
                   "PR9-11: an update may keep its OWN current name (ignore_index)");
        TEST_CHECK(ValidateUserLanguageName("Samplelandic", "", existing, 1) ==
                           UserLanguageNameVerdict::Duplicate,
                   "PR9-11: ... while any other index still sees that name as a duplicate");

        TEST_CHECK(FindLanguageByName("Samplelandic") == nullptr &&
                       FindLanguageByCode("Samplelandic") == nullptr,
                   "P9b-3: the clean-name fixture is genuinely outside the registry");
        TEST_CHECK(ValidateUserLanguageName("Samplelandic", "", {}) ==
                           UserLanguageNameVerdict::Accepted &&
                       ValidateUserLanguageName("Samplelandic B", "Samplelandic Bee",
                                                existing) == UserLanguageNameVerdict::Accepted,
                   "PR9-11: a name outside the registry and the existing entries is accepted");
        TEST_CHECK(ValidateUserLanguageName("", "", {}) == UserLanguageNameVerdict::Accepted,
                   "PR9-11: an empty native name is not a NAME verdict (the write verbs "
                   "refuse it with RejectedEmptyName)");
        // Backward compatibility: the BUILDER stays permissive - P9a's
        // MakeUserLanguage pins (and the merge's AUTO-trap pin) depend on it,
        // so the refusal lives at the write seam only.
        TEST_CHECK(MakeUserLanguage("Auto Detect", "").native_name == "Auto Detect",
                   "PR9-11: MakeUserLanguage still builds an AUTO-named entry (refusal is a "
                   "write-seam rule, not a schema rule)");
    }

    // ---- (2) the forward-compat carrier predicate --------------------------
    {
        AppConfig fresh;
        TEST_CHECK(!fresh.HasUserLanguagesForwardCompatCarrier(),
                   "P9b-3: a default config carries no forward-compat carrier");

        AppConfig current_version;
        TEST_CHECK(current_version.FromJsonString(
                       "{\"user_languages_version\": 1, "
                       "\"user_languages\": [{\"native_name\": \"Samplelandic\"}]}") &&
                       !current_version.HasUserLanguagesForwardCompatCarrier() &&
                       current_version.user_languages.size() == 1,
                   "P9b-3: a current-schema block parses and leaves the carrier disarmed");

        AppConfig newer;
        TEST_CHECK(newer.FromJsonString(
                       "{\"user_languages_version\": 2, "
                       "\"user_languages\": [{\"native_name\": \"Samplelandic\"}]}") &&
                       newer.HasUserLanguagesForwardCompatCarrier() &&
                       newer.user_languages.empty(),
                   "P9b-3: a newer-schema block arms the carrier and freezes the in-memory "
                   "registry");

        const ProbeEvaluation carrier_eval = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Habari ya asubuhi", kScriptMaskLatin);
        TEST_CHECK(newer.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                           UserLanguageWriteResult::RefusedForwardCompatCarrier &&
                       newer.RemoveUserLanguage(0) ==
                           UserLanguageWriteResult::RefusedForwardCompatCarrier &&
                       newer.UpdateUserLanguage(0, MakeUserLanguage("Samplelandic", "")) ==
                           UserLanguageWriteResult::RefusedForwardCompatCarrier &&
                       newer.RecordUserLanguageProbeResultFor("Samplelandic", "openai", "m",
                                                             carrier_eval,
                                                             "2026-11-18T00:00:00Z") ==
                           UserLanguageWriteResult::RefusedForwardCompatCarrier &&
                       newer.user_languages.empty(),
                   "P9b-3: while the carrier is armed every write verb refuses (the edit would "
                   "be discarded on save); Remove(0) refuses as CARRIER, not as not-found");
        TEST_CHECK(newer.ToJsonString().find(
                       "\"user_languages\": [{\"native_name\": \"Samplelandic\"}]") !=
                       std::string::npos,
                   "P9b-3: the refused edits did not disturb the preserved verbatim block");
        TEST_CHECK(newer.FromJsonString("{\"user_languages\": []}") &&
                       !newer.HasUserLanguagesForwardCompatCarrier() &&
                       newer.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3: a current-schema load disarms the carrier and writes apply again");
    }

    // ---- (3) add / update / remove -----------------------------------------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.user_languages.size() == 1 &&
                       cfg.user_languages[0].native_name == "Samplelandic",
                   "PR9-12: AddUserLanguage appends a valid entry");
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("samplelandic", "")) ==
                           UserLanguageWriteResult::RejectedDuplicateName &&
                       cfg.user_languages.size() == 1,
                   "PR9-11: a duplicate add is refused and the registry is untouched");
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("AUTO", "")) ==
                           UserLanguageWriteResult::RejectedReservedName &&
                       cfg.AddUserLanguage(MakeUserLanguage("Korean", "")) ==
                           UserLanguageWriteResult::RejectedDuplicateName,
                   "PR9-11: the add verb enforces the reserved and duplicate verdicts");
        TEST_CHECK(cfg.AddUserLanguage(UserLanguage{}) ==
                           UserLanguageWriteResult::RejectedEmptyName,
                   "PR9-12: an unnamed entry is refused (native_name is the required field)");

        // The write seam re-applies the storage cleaning, so a hand-built entry
        // cannot smuggle controls or bidi marks past the dialog's own path.
        UserLanguage dirty;
        dirty.native_name = "B\x01" "emba";
        TEST_CHECK(cfg.AddUserLanguage(dirty) == UserLanguageWriteResult::Applied &&
                       cfg.user_languages.back().native_name == "Bemba",
                   "PR9-12: the write seam strips C0 controls from a hand-built entry");

        UserLanguage renamed = cfg.user_languages[0];
        renamed.english_name = "Samplelandic English";
        TEST_CHECK(cfg.UpdateUserLanguage(0, renamed) == UserLanguageWriteResult::Applied &&
                       cfg.user_languages[0].english_name == "Samplelandic English",
                   "PR9-12: UpdateUserLanguage replaces at the index");
        const UserLanguage keep = cfg.user_languages[0];
        TEST_CHECK(cfg.UpdateUserLanguage(0, keep) == UserLanguageWriteResult::Applied,
                   "PR9-11: an update may keep the entry's own name (no self-duplicate)");
        UserLanguage reserved_update = keep;
        reserved_update.native_name = "Auto Detect";
        TEST_CHECK(cfg.UpdateUserLanguage(0, reserved_update) ==
                           UserLanguageWriteResult::RejectedReservedName &&
                       cfg.user_languages[0].native_name == "Samplelandic",
                   "PR9-11: an update onto a reserved spelling is refused and the stored entry "
                   "is untouched");
        UserLanguage registry_update = keep;
        registry_update.native_name = "Korean";
        TEST_CHECK(cfg.UpdateUserLanguage(0, registry_update) ==
                           UserLanguageWriteResult::RejectedDuplicateName,
                   "PR9-11: ... as is an update onto a registry row's spelling");
        TEST_CHECK(cfg.UpdateUserLanguage(9, keep) == UserLanguageWriteResult::RejectedNotFound &&
                       cfg.RemoveUserLanguage(9) == UserLanguageWriteResult::RejectedNotFound,
                   "PR9-12: an out-of-range index is refused by both verbs");
        const std::size_t before_remove = cfg.user_languages.size();
        TEST_CHECK(cfg.RemoveUserLanguage(0) == UserLanguageWriteResult::Applied &&
                       eq(cfg.user_languages.size(), before_remove - 1) &&
                       cfg.user_languages[0].native_name == "Bemba",
                   "PR9-12: RemoveUserLanguage erases at the index (the next entry shifts up)");

        // Capacity is the registry bound the band math is pinned against.
        AppConfig full;
        bool adds_ok = true;
        for (std::size_t i = 0; i < kMaxUserLanguages; ++i) {
            UserLanguage entry;
            entry.native_name = "Freeform " + std::to_string(i);
            entry.script_mask = kScriptMaskLatin;
            if (full.AddUserLanguage(entry) != UserLanguageWriteResult::Applied) {
                adds_ok = false;
            }
        }
        TEST_CHECK(adds_ok && eq(full.user_languages.size(), kMaxUserLanguages),
                   "PR9-12: kMaxUserLanguages entries can be added");
        UserLanguage overflow;
        overflow.native_name = "Freeform overflow";
        TEST_CHECK(full.AddUserLanguage(overflow) == UserLanguageWriteResult::RejectedCapacity &&
                       eq(full.user_languages.size(), kMaxUserLanguages),
                   "PR9-12: the entry past kMaxUserLanguages is refused");
    }

    // ---- (4) probe record verbs + PR10-07 write-seam caps ------------------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3: the probe fixture entry is registered");
        const ProbeEvaluation ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Habari ya asubuhi", kScriptMaskLatin);
        TEST_CHECK(ok.status == UserLanguageProbeStatus::Supported,
                   "P9b-3: the probe fixture evaluates to Supported (evaluator sanity)");
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Samplelandic", "openai", "gpt-4o-mini",
                                                       ok, "2026-11-18T01:02:03Z") ==
                       UserLanguageWriteResult::Applied,
                   "PR9-12: the result verb records through the locked seam");
        {
            const UserLanguageProbe* probe =
                FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-4o-mini");
            TEST_CHECK(probe != nullptr &&
                           probe->status == UserLanguageProbeStatus::Supported &&
                           probe->checked_at == "2026-11-18T01:02:03Z" &&
                           probe->evidence.find("nonempty=1") != std::string::npos,
                       "P9b-3: verdict, stamp and evidence land in the cache slot");
            TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor(
                           "Samplelandic", "openai", "gpt-4o-mini",
                           UserLanguageProbeStatus::Unsupported, "2026-11-18T01:03:00Z") ==
                               UserLanguageWriteResult::Applied,
                       "PR9-12: the override verb records the user's answer");
            const UserLanguageProbe* overridden =
                FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-4o-mini");
            TEST_CHECK(overridden != nullptr &&
                           EffectiveUserLanguageProbeStatus(*overridden) ==
                               UserLanguageProbeStatus::Unsupported &&
                           DisplayUserLanguageProbeStatus(*overridden) ==
                               UserLanguageProbeStatus::UserOverridden,
                       "P9b-3: the override wins both the effective and the display fold");
            const UserLanguageProbe* cleared =
                FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-4o-mini");
            TEST_CHECK(cfg.ClearUserLanguageProbeOverrideFor("Samplelandic", "openai",
                                                            "gpt-4o-mini") ==
                               UserLanguageWriteResult::Applied &&
                           cleared != nullptr &&
                           EffectiveUserLanguageProbeStatus(
                               *FindUserLanguageProbe(cfg.user_languages[0], "openai",
                                                      "gpt-4o-mini")) ==
                               UserLanguageProbeStatus::Supported,
                       "P9b-3: clearing the override restores the cached verdict");
        }
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Nobody", "openai", "m", ok,
                                                       "2026-11-18T01:04:00Z") ==
                           UserLanguageWriteResult::RejectedNotFound &&
                       cfg.RecordUserLanguageProbeOverrideFor(
                           "Nobody", "openai", "m", UserLanguageProbeStatus::Supported,
                           "2026-11-18T01:04:00Z") ==
                           UserLanguageWriteResult::RejectedNotFound &&
                       cfg.ClearUserLanguageProbeOverrideFor("Nobody", "openai", "m") ==
                           UserLanguageWriteResult::RejectedNotFound,
                   "P9b-3: an unknown stored name is refused by all three probe verbs");
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Samplelandic", "", "m", ok,
                                                       "2026-11-18T01:05:00Z") ==
                           UserLanguageWriteResult::RejectedMissingProbeKey,
                   "P9b-3: an empty engine id is refused (the engine IS the cache key)");
        TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor(
                       "Samplelandic", "openai", "m", UserLanguageProbeStatus::Unknown,
                       "2026-11-18T01:05:00Z") ==
                           UserLanguageWriteResult::RejectedInvalidStatus &&
                       cfg.RecordUserLanguageProbeOverrideFor(
                           "Samplelandic", "openai", "m", UserLanguageProbeStatus::Checking,
                           "2026-11-18T01:05:00Z") ==
                           UserLanguageWriteResult::RejectedInvalidStatus,
                   "P9b-3: an override is an ANSWER, not a state (Only Supported/Unsupported)");

        // PR10-07: the canonical key and the two recorded payload fields are
        // capped on the LIVE path. Each block below is deleted by one mutation
        // probe (see the P9b-3 U1 report).
        const std::string long_engine(64, 'e');
        const std::string long_model(400, 'm');
        const UserLanguageProbeKey capped = MakeUserLanguageProbeKey(long_engine, long_model);
        TEST_CHECK(eq(capped.engine.size(), kMaxUserLanguageProbeEngineBytes) &&
                       eq(capped.model.size(), kMaxUserLanguageProbeModelBytes),
                   "PR10-07: the canonical probe key caps engine and model at their kMax "
                   "constants");
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Samplelandic", long_engine, long_model,
                                                       ok, "2026-11-18T01:06:00Z") ==
                       UserLanguageWriteResult::Applied,
                   "PR10-07: the record verb accepts an over-cap key");
        const std::size_t probes_after_first = cfg.user_languages[0].probes.size();
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Samplelandic", long_engine, long_model,
                                                       ok, "2026-11-18T01:07:00Z") ==
                           UserLanguageWriteResult::Applied &&
                       eq(cfg.user_languages[0].probes.size(), probes_after_first),
                   "PR10-07: an over-long key hits its OWN capped slot (the key cannot split)");
        {
            const UserLanguageProbe* capped_probe =
                FindUserLanguageProbe(cfg.user_languages[0], capped.engine, capped.model);
            TEST_CHECK(capped_probe != nullptr &&
                           eq(capped_probe->model.size(), kMaxUserLanguageProbeModelBytes),
                       "PR10-07: the STORED model is the capped form");
        }
        ProbeEvaluation fat = ok;
        fat.evidence.assign(400, 'e');
        const std::string long_stamp(48, 'c');
        const std::string long_override_stamp(48, 'd');
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Samplelandic", "openai", "fat", fat,
                                                       long_stamp) ==
                       UserLanguageWriteResult::Applied,
                   "PR10-07: the result seam accepts an over-cap evidence/stamp payload");
        {
            const UserLanguageProbe* fat_probe =
                FindUserLanguageProbe(cfg.user_languages[0], "openai", "fat");
            TEST_CHECK(fat_probe != nullptr &&
                           eq(fat_probe->evidence.size(),
                              kMaxUserLanguageProbeEvidenceBytes) &&
                           eq(fat_probe->checked_at.size(),
                              kMaxUserLanguageProbeCheckedAtBytes),
                       "PR10-07: the result seam caps evidence + checked_at at their kMax "
                       "constants");
        }
        TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor(
                       "Samplelandic", "openai", "fat", UserLanguageProbeStatus::Supported,
                       long_override_stamp) == UserLanguageWriteResult::Applied,
                   "PR10-07: the override seam accepts the same over-cap stamp");
        {
            const UserLanguageProbe* override_probe =
                FindUserLanguageProbe(cfg.user_languages[0], "openai", "fat");
            TEST_CHECK(override_probe != nullptr &&
                           eq(override_probe->checked_at.size(),
                              kMaxUserLanguageProbeCheckedAtBytes),
                       "PR10-07: the override seam caps checked_at at its kMax constant");
        }

        // The probe-slot cap is reported, never silently dropped (the cache
        // holds kMaxUserLanguageProbes keys; an existing key keeps recording).
        AppConfig cache;
        TEST_CHECK(cache.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3: the probe-cache fixture entry is registered");
        bool slots_ok = true;
        for (std::size_t i = 0; i < kMaxUserLanguageProbes; ++i) {
            if (cache.RecordUserLanguageProbeResultFor(
                    "Samplelandic", "openai", "model " + std::to_string(i), ok,
                    "2026-11-18T02:00:00Z") != UserLanguageWriteResult::Applied) {
                slots_ok = false;
            }
        }
        TEST_CHECK(slots_ok && eq(cache.user_languages[0].probes.size(),
                                  kMaxUserLanguageProbes),
                   "P9b-3: kMaxUserLanguageProbes cache keys can be recorded");
        TEST_CHECK(cache.RecordUserLanguageProbeResultFor("Samplelandic", "openai", "one more",
                                                          ok, "2026-11-18T02:01:00Z") ==
                           UserLanguageWriteResult::RejectedCapacity &&
                       cache.RecordUserLanguageProbeResultFor("Samplelandic", "openai",
                                                              "model 0", ok,
                                                              "2026-11-18T02:02:00Z") ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3: a NEW key on a full cache is reported (not dropped), while an "
                   "existing key still records");

        // The UTC stamp helper: the config layer had none (diag_logger's
        // TimestampNow is LOCAL and does not leave that module).
        const std::string now = UserLanguageProbeTimestampNowUtc();
        bool stamp_shape_ok = eq(now.size(), 20);
        for (std::size_t i = 0; stamp_shape_ok && i < now.size(); ++i) {
            const char ch = now[i];
            if (i == 4 || i == 7) {
                stamp_shape_ok = ch == '-';
            } else if (i == 10) {
                stamp_shape_ok = ch == 'T';
            } else if (i == 13 || i == 16) {
                stamp_shape_ok = ch == ':';
            } else if (i == 19) {
                stamp_shape_ok = ch == 'Z';
            } else {
                stamp_shape_ok = ch >= '0' && ch <= '9';
            }
        }
        TEST_CHECK(stamp_shape_ok && lt(now.size(), kMaxUserLanguageProbeCheckedAtBytes + 1),
                   "P9b-3: the UTC stamp helper emits the documented ISO-8601 shape inside the "
                   "checked_at cap");
    }

    // ---- (5) round trip through the new verbs ------------------------------
    {
        const std::string stamp = UserLanguageProbeTimestampNowUtc();
        const ProbeEvaluation supported = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Habari ya asubuhi", kScriptMaskLatin);
        const ProbeEvaluation unsupported = EvaluateProbeResult(
            kUserLanguageProbeSentence, kUserLanguageProbeSentence, kScriptMaskLatin);

        AppConfig cfg;
        const bool registered =
            cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                UserLanguageWriteResult::Applied &&
            cfg.AddUserLanguage(MakeUserLanguage("Samplelandic B", "")) ==
                UserLanguageWriteResult::Applied;
        TEST_CHECK(registered && eq(cfg.user_languages.size(), 2),
                   "P9b-3: two entries register through the write seam");
        const bool recorded =
            cfg.RecordUserLanguageProbeResultFor("Samplelandic", "openai", "gpt-4o-mini",
                                                 supported, stamp) ==
                UserLanguageWriteResult::Applied &&
            cfg.RecordUserLanguageProbeOverrideFor("Samplelandic B", "google", "",
                                                   UserLanguageProbeStatus::Unsupported,
                                                   stamp) == UserLanguageWriteResult::Applied &&
            cfg.RecordUserLanguageProbeResultFor("Samplelandic B", "user_gguf",
                                                 "my-model", unsupported, stamp) ==
                UserLanguageWriteResult::Applied;
        TEST_CHECK(recorded,
                   "P9b-3: three probe records land through the probe verbs (result, override, "
                   "a second engine key)");

        const std::string json = cfg.ToJsonString();
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(json) && eq(reloaded.user_languages.size(), 2),
                   "P9b-3: the emitted document re-parses with both entries");
        const UserLanguageProbe* reloaded_result =
            FindUserLanguageProbe(reloaded.user_languages[0], "openai", "gpt-4o-mini");
        const UserLanguageProbe* reloaded_override =
            FindUserLanguageProbe(reloaded.user_languages[1], "google", "");
        const UserLanguageProbe* reloaded_second =
            FindUserLanguageProbe(reloaded.user_languages[1], "user_gguf", "my-model");
        const UserLanguageProbe* written_result =
            FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-4o-mini");
        TEST_CHECK(reloaded_result != nullptr &&
                       reloaded_result->status == UserLanguageProbeStatus::Supported &&
                       reloaded_result->override_status == UserLanguageProbeStatus::Unknown &&
                       reloaded_result->checked_at == stamp &&
                       written_result != nullptr &&
                       reloaded_result->evidence == written_result->evidence,
                   "P9b-3: the probe verdict, stamp and evidence survive save -> reload");
        TEST_CHECK(reloaded_override != nullptr &&
                       EffectiveUserLanguageProbeStatus(*reloaded_override) ==
                           UserLanguageProbeStatus::Unsupported &&
                       DisplayUserLanguageProbeStatus(*reloaded_override) ==
                           UserLanguageProbeStatus::UserOverridden,
                   "P9b-3: the override survives and still wins after reload");
        TEST_CHECK(reloaded_second != nullptr &&
                       reloaded_second->status == UserLanguageProbeStatus::Unsupported &&
                       reloaded_second->override_status ==
                           UserLanguageProbeStatus::Unknown,
                   "P9b-3: a second (engine, model) key stays a separate cache slot");

        std::error_code ec;
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path(ec) /
            "emebalachat_p9b3_write_surface.json";
        std::filesystem::remove(tmp, ec); // stale leftovers must not mask a save failure
        TEST_CHECK(cfg.SaveToFile(tmp), "P9b-3: SaveToFile writes the block");
        AppConfig from_disk;
        TEST_CHECK(from_disk.LoadFromFile(tmp) && eq(from_disk.user_languages.size(), 2) &&
                       from_disk.user_languages[0].native_name == "Samplelandic",
                   "P9b-3: LoadFromFile reads both entries back");
        const UserLanguageProbe* disk_probe =
            FindUserLanguageProbe(from_disk.user_languages[0], "openai", "gpt-4o-mini");
        TEST_CHECK(disk_probe != nullptr &&
                       disk_probe->status == UserLanguageProbeStatus::Supported &&
                       disk_probe->checked_at == stamp,
                   "P9b-3: the on-disk round trip preserves the verdict and its stamp");
        std::filesystem::remove(tmp, ec); // our own artifact
    }

    // ---- (6) structural pin: the only writers of the registry --------------
    {
        const auto read_src = [](const std::filesystem::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };
        const auto count_of = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };
        const auto line_at = [](const std::string& text, std::size_t pos) {
            std::size_t start = text.rfind('\n', pos);
            start = (start == std::string::npos) ? 0 : start + 1;
            std::size_t end = text.find('\n', pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            return text.substr(start, end - start);
        };
        const auto trim_ascii = [](std::string text) {
            const auto ws = [](char ch) {
                return ch == ' ' || ch == '\t' || ch == '\r';
            };
            while (!text.empty() && ws(text.front())) {
                text.erase(text.begin());
            }
            while (!text.empty() && ws(text.back())) {
                text.pop_back();
            }
            return text;
        };
        // A member write begins a statement/expression: the byte before it must
        // not be an identifier character or '.', so "raw_user_languages = ..."
        // and "view.user_languages = ..." (a COPY into another struct, in
        // main.cpp) are not mistaken for registry mutations.
        const auto is_write_context = [](const std::string& text, std::size_t pos) {
            if (pos == 0) {
                return true;
            }
            const char prev = text[pos - 1];
            const bool ident = (prev >= 'a' && prev <= 'z') ||
                               (prev >= 'A' && prev <= 'Z') ||
                               (prev >= '0' && prev <= '9') || prev == '_' || prev == '.';
            return !ident;
        };
        const char* const needles[] = {
            "user_languages.clear()",      "user_languages = ",
            "user_languages.push_back(",   "user_languages.emplace_back(",
            "user_languages.erase(",       "user_languages.insert(",
            "user_languages.assign(",      "user_languages.resize(",
            "user_languages.pop_back(",    "user_languages.swap(",
            "user_languages[",
        };

        const std::filesystem::path src_dir = ResolveRepoFile(L"src");
        std::vector<std::filesystem::path> files;
        if (!src_dir.empty()) {
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(src_dir, ec)) {
                if (ec) {
                    break;
                }
                std::error_code fec;
                if (!entry.is_regular_file(fec)) {
                    continue;
                }
                const std::string ext = entry.path().extension().string();
                if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".inl") {
                    files.push_back(entry.path());
                }
            }
        }
        TEST_CHECK(!files.empty() && gt(files.size(), static_cast<std::size_t>(30)),
                   "PR9-12: the write-surface scan covers the whole src tree");

        const std::filesystem::path config_cpp = ResolveRepoFile(L"src\\config.cpp");
        std::string config_src;
        const bool config_read = !config_cpp.empty() && read_src(config_cpp, config_src);
        TEST_CHECK(config_read, "PR9-12: src/config.cpp resolves for the structural pin");
        if (config_read) {
            const std::size_t begin_marker = config_src.find("P9b-3 PR9-12 BEGIN");
            const std::size_t end_marker = config_src.find("P9b-3 PR9-12 END");
            const bool markers_ok = begin_marker != std::string::npos &&
                                    end_marker != std::string::npos &&
                                    lt(begin_marker, end_marker);
            TEST_CHECK(markers_ok,
                       "PR9-12: the write-surface region markers exist and are ordered");
            const std::string region =
                markers_ok ? config_src.substr(begin_marker, end_marker - begin_marker)
                           : std::string();
            // The pin is anchored to real code, never vacuously true: all six
            // verbs (and the carrier accessor) are DEFINED inside the region.
            TEST_CHECK(region.find("AppConfig::AddUserLanguage(") != std::string::npos &&
                           region.find("AppConfig::UpdateUserLanguage(") != std::string::npos &&
                           region.find("AppConfig::RemoveUserLanguage(") != std::string::npos &&
                           region.find("AppConfig::RecordUserLanguageProbeResultFor(") !=
                               std::string::npos &&
                           region.find("AppConfig::RecordUserLanguageProbeOverrideFor(") !=
                               std::string::npos &&
                           region.find("AppConfig::ClearUserLanguageProbeOverrideFor(") !=
                               std::string::npos &&
                           region.find("AppConfig::HasUserLanguagesForwardCompatCarrier") !=
                               std::string::npos,
                       "PR9-12: the six write verbs + the carrier accessors are defined inside "
                       "the marked region");
            // PR9-12's actual lock: the public carrier accessor + every write
            // verb takes the SAME mutex_ the whole-config serialiser takes. One
            // guard per function, no more and no fewer (a verb that forgot the
            // lock, or a second nested guard, fails here).
            TEST_CHECK(eq(count_of(region, "std::lock_guard<std::mutex> lock(mutex_);"), 7),
                       "PR9-12: every write verb and the carrier accessor takes mutex_ (7 "
                       "guards inside the region)");

            std::size_t outside = 0;
            std::size_t block_writes = 0;
            std::string outside_detail;
            for (const char* needle : needles) {
                const std::string text(needle);
                for (std::size_t pos = config_src.find(text); pos != std::string::npos;
                     pos = config_src.find(text, pos + text.size())) {
                    if (!is_write_context(config_src, pos)) {
                        continue;
                    }
                    if (markers_ok && pos > begin_marker && pos < end_marker) {
                        ++block_writes;
                        continue;
                    }
                    const std::string line = trim_ascii(line_at(config_src, pos));
                    // The load path (FromJsonString, already under mutex_) is the
                    // one documented writer outside the region: these two exact
                    // statements and nothing else.
                    const bool load_path = line == "user_languages.clear();" ||
                                           line == "user_languages = std::move(parsed);";
                    if (!load_path) {
                        ++outside;
                        outside_detail += line + " | ";
                    }
                }
            }
            TEST_CHECK(eq(outside, 0),
                       "PR9-12: no direct user_languages mutation survives outside the locked "
                       "mutators and the two load-path statements");
            TEST_CHECK(gt(block_writes, static_cast<std::size_t>(2)),
                       "PR9-12: the marked region really carries the mutations (the pin cannot "
                       "pass vacuously)");
            TEST_CHECK(eq(count_of(config_src, "user_languages.clear();"), 1) &&
                           eq(count_of(config_src, "user_languages = std::move(parsed);"), 1),
                       "PR9-12: the whitelisted load-path writes are exactly the two known "
                       "statements");
            if (outside != 0) {
                std::cerr << "  [write-surface] " << outside_detail << std::endl;
            }
        }

        std::size_t other_hits = 0;
        std::string other_detail;
        for (const std::filesystem::path& path : files) {
            if (path.filename() == "config.cpp") {
                continue; // covered by the region scan above
            }
            std::string text;
            if (!read_src(path, text)) {
                ++other_hits;
                other_detail += path.filename().string() + " (unreadable) | ";
                continue;
            }
            for (const char* needle : needles) {
                const std::string text_needle(needle);
                for (std::size_t pos = text.find(text_needle); pos != std::string::npos;
                     pos = text.find(text_needle, pos + text_needle.size())) {
                    if (!is_write_context(text, pos)) {
                        continue;
                    }
                    ++other_hits;
                    other_detail += path.filename().string() + ": " +
                                    trim_ascii(line_at(text, pos)) + " | ";
                }
            }
        }
        TEST_CHECK(eq(other_hits, 0),
                   "PR9-12: no file outside src/config.cpp mutates the registry (the UI, "
                   "main.cpp, hook and engine only read it)");
        if (other_hits != 0) {
            std::cerr << "  [write-surface] " << other_detail << std::endl;
        }
    }

    // ---- (7) F-1 (R15-10): the READ seam canonicalises the probe key --------
    // PR10-07 capped the key on the WRITE side only, while the readers compared
    // the raw arguments. The model half reaches every reader UNCAPPED
    // (openai.model / the user-.gguf registry id), so an over-cap model recorded
    // its verdict in a slot no lookup could find: the first-use probe re-fired on
    // every pick (one engine call per pick against the user's quota) and no
    // badge/manager row could ever show the verdict. One accessor serves every
    // read surface, so this is pinned through it: raw key in, stored verdict out.
    {
        AppConfig read_cfg;
        TEST_CHECK(read_cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 F-1: the read-seam fixture entry is registered");
        const ProbeEvaluation read_ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Habari ya asubuhi", kScriptMaskLatin);
        // Both halves over their caps (48 > 32 engine, 417 > 256 model). The
        // model shape mirrors a real config value ("org/...").
        const std::string raw_read_engine(48, 'g');
        const std::string raw_read_model = std::string("org/") + std::string(413, 'm');
        TEST_CHECK(gt(raw_read_engine.size(), kMaxUserLanguageProbeEngineBytes) &&
                       gt(raw_read_model.size(), kMaxUserLanguageProbeModelBytes),
                   "P9b-3 F-1: the fixture key halves really exceed their kMax constants");
        TEST_CHECK(read_cfg.RecordUserLanguageProbeResultFor(
                       "Samplelandic", raw_read_engine, raw_read_model, read_ok,
                       "2026-11-18T03:00:00Z") == UserLanguageWriteResult::Applied,
                   "P9b-3 F-1: the write seam records the verdict under the over-cap key");
        const UserLanguageProbe* raw_read =
            FindUserLanguageProbe(read_cfg.user_languages[0], raw_read_engine, raw_read_model);
        TEST_CHECK(raw_read != nullptr &&
                       raw_read->status == UserLanguageProbeStatus::Supported &&
                       raw_read->checked_at == "2026-11-18T03:00:00Z",
                   "P9b-3 F-1: the RAW over-cap key reads its verdict back (the reader "
                   "canonicalises exactly like the writer)");
        TEST_CHECK(raw_read != nullptr &&
                       eq(raw_read->model.size(), kMaxUserLanguageProbeModelBytes) &&
                       eq(raw_read->engine.size(), kMaxUserLanguageProbeEngineBytes),
                   "P9b-3 F-1: the slot the raw read lands on is the CAPPED one");
        // Idempotence: the canonical form is a fixed point of the key builder, and
        // reading it (what the write verbs' capacity checks do) returns the SAME
        // slot, so the already-canonical internal call sites keep their verdicts.
        const UserLanguageProbeKey read_key =
            MakeUserLanguageProbeKey(raw_read_engine, raw_read_model);
        const UserLanguageProbeKey read_key_again =
            MakeUserLanguageProbeKey(read_key.engine, read_key.model);
        TEST_CHECK(read_key_again.engine == read_key.engine &&
                       read_key_again.model == read_key.model,
                   "P9b-3 F-1: the canonical probe key is a fixed point of its own builder "
                   "(re-canonicalising never moves the slot)");
        TEST_CHECK(raw_read != nullptr &&
                       FindUserLanguageProbe(read_cfg.user_languages[0], read_key.engine,
                                             read_key.model) == raw_read,
                   "P9b-3 F-1: an already-canonical key reads the SAME slot (the write verbs' "
                   "internal checks are unaffected)");
        // No over-match: a different over-cap model has a different capped form.
        const std::string other_read_model = std::string("org/") + std::string(413, 'n');
        TEST_CHECK(FindUserLanguageProbe(read_cfg.user_languages[0], raw_read_engine,
                                         other_read_model) == nullptr,
                   "P9b-3 F-1: a different over-cap model still reads nothing (the reader "
                   "does not over-match)");
        // The real path the raw model meets: the file stores the capped slot while
        // the live config keeps the raw model, so the reloaded cache must answer it.
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(read_cfg.ToJsonString()) &&
                       eq(reloaded.user_languages.size(), static_cast<std::size_t>(1)),
                   "P9b-3 F-1: the probe-cache fixture round-trips through the config JSON");
        const UserLanguageProbe* reloaded_read =
            reloaded.user_languages.empty()
                ? nullptr
                : FindUserLanguageProbe(reloaded.user_languages[0], raw_read_engine,
                                        raw_read_model);
        TEST_CHECK(reloaded_read != nullptr &&
                       reloaded_read->status == UserLanguageProbeStatus::Supported &&
                       eq(reloaded_read->model.size(), kMaxUserLanguageProbeModelBytes),
                   "P9b-3 F-1: the saved-then-reloaded cache answers the raw over-cap key");
        // The capacity gate keeps its old verdicts now that its reader
        // canonicalises: at kMaxUserLanguageProbes an over-cap key whose capped
        // form is already cached still records, a brand-new one is still refused.
        AppConfig full_read_cfg;
        TEST_CHECK(full_read_cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 F-1: the full-cache fixture entry is registered");
        const std::string capped_tail(kMaxUserLanguageProbeModelBytes, 'z');
        const std::string raw_tail = capped_tail + std::string(120, 'z');
        bool full_ok = true;
        for (std::size_t i = 0; i < kMaxUserLanguageProbes; ++i) {
            const std::string slot =
                (i + 1 == kMaxUserLanguageProbes) ? capped_tail
                                                  : "model " + std::to_string(i);
            if (full_read_cfg.RecordUserLanguageProbeResultFor(
                    "Samplelandic", "openai", slot, read_ok,
                    "2026-11-18T03:10:00Z") != UserLanguageWriteResult::Applied) {
                full_ok = false;
            }
        }
        TEST_CHECK(full_ok && eq(full_read_cfg.user_languages[0].probes.size(),
                                 kMaxUserLanguageProbes),
                   "P9b-3 F-1: the full-cache fixture reaches kMaxUserLanguageProbes slots");
        TEST_CHECK(full_read_cfg.RecordUserLanguageProbeResultFor(
                       "Samplelandic", "openai", raw_tail, read_ok,
                       "2026-11-18T03:11:00Z") == UserLanguageWriteResult::Applied,
                   "P9b-3 F-1: at the cap, an over-cap key that caps onto a cached slot still "
                   "records (the gate's reader is idempotent)");
        TEST_CHECK(full_read_cfg.RecordUserLanguageProbeResultFor(
                       "Samplelandic", "openai", std::string(300, 'q'), read_ok,
                       "2026-11-18T03:12:00Z") == UserLanguageWriteResult::RejectedCapacity,
                   "P9b-3 F-1: at the cap, a genuinely new over-cap key is still "
                   "RejectedCapacity (no over-match)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-3 U1 user-language write surface passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-3 U1 user-language write surface: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// 261116_0003 P9b-3 U2 (language-menu redesign, plan §5 P9): the ADD-LANGUAGE
// DIALOG's pure half. Appended to U1's suite file (U1's checks above are
// untouched): the config write surface U1 landed is what this dialog writes
// through, so the two halves pin the same contract from both ends.
//
// Pinned here, headlessly (no window is created - AGENTS.md §10; the modal
// loop stays a manual-QA badge item, the same split P9b-2 recorded at :31-32):
//   (A) the dialog TEMPLATE: item count, header geometry, the 4-byte
//       alignment contract, every control id, the EDIT/BUTTON style bits, and
//       that every caption is its landed P9b-1 StringId.
//   (B) the refusal message: the "{n}" token is present exactly once in the
//       landed string and is substituted exactly once by the CLEANED typed
//       name - for each reserved AUTO spelling and for the duplicate case.
//   (C) the live derivation through the injectable D5 locale seam: a fake
//       source resolves a deterministic code, an unknown name yields "" plus
//       the no-code notice path, and an untouched form shows neither.
//   (D) bidi neutralization at RENDER time: a typed name carrying RLO /
//       isolates / a soft hyphen renders stripped in every derived form,
//       including the refusal line built from it.
//   (E) the outcome fold: kAdded ONLY for Applied; every refusal reason keeps
//       the dialog open with its own block code (reserved/duplicate keep the
//       refusal copy, the only block with landed words).
//   (F) the unit boundary read off the source: the dialog applies through the
//       locked mutator, consults the forward-compat carrier before offering
//       the affordance, performs NO file I/O, and does not restate the
//       sanitizer strip set.
// ===========================================================================
#include "../src/ui/user_language_dialog.hpp"

namespace p9b3u2_detail {

// The injectable D5 stage-2 source (LocaleLanguageSource is a plain function
// pointer, so the fake is a free function). Same fixture shape as the p9a
// suite: a name only the locale table knows (it resolves through the locale's
// primary subtag) plus the empty table as the "no stage 2" control.
std::vector<LocaleLanguageName> FakeLocaleSource() {
    return {
        {"fi-FI", "Samplelandic", "samplelandic"},
        {"ko-KR", "Korean", "한국어"},
    };
}

std::vector<LocaleLanguageName> EmptyLocaleSource() { return {}; }

struct RowExpect {
    WORD id;
    WORD cls;
    short x;
    short y;
    short cx;
    short cy;
    DWORD style;
    std::wstring title;
};

WORD ReadWord(const BYTE* p, size_t off) {
    return static_cast<WORD>(p[off] | (static_cast<WORD>(p[off + 1]) << 8));
}

DWORD ReadDword(const BYTE* p, size_t off) {
    return static_cast<DWORD>(ReadWord(p, off)) |
           (static_cast<DWORD>(ReadWord(p, off + 2)) << 16);
}

std::wstring ReadString(const BYTE* p, size_t* off) {
    std::wstring s;
    for (;;) {
        const wchar_t ch = static_cast<wchar_t>(ReadWord(p, *off));
        *off += 2;
        if (ch == L'\0') break;
        s += ch;
    }
    return s;
}

std::size_t CountOf(const std::wstring& haystack, const std::wstring& needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::wstring::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace p9b3u2_detail

void TestP9b3UserLanguageDialog() {
    std::cout << "[RUN] Testing 261116_0003 P9b-3 U2 add-language dialog..."
              << std::endl;
    const int failures_before = g_failed_count;

    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };

    namespace uld = emebalachat::user_language_dialog;
    using emebalachat::UserLanguageDialogOutcome;
    using uld::UserLanguageAddBlock;
    using uld::UserLanguageAddDisposition;
    using uld::UserLanguageCandidate;

    // ---- (A) the dialog template -------------------------------------------
    // The suite pins captions per locale, so both are set and restored
    // (suite-state hygiene: the p9a/p9b2/p9b3-U1 suites run before this one).
    const UiLocale initial_locale = I18n::GetCurrentLocale();
    {
        using namespace p9b3u2_detail;
        I18n::SetLocale(UiLocale::English);
        const std::wstring dialog_title = I18n::Get(StringId::UserLangAddTitle);
        TemplateBuilder tb;
        tb.Begin(dialog_title, uld::kDialogWidthDlu, uld::kDialogHeightDlu,
                 uld::kDialogItemCount);
        uld::BuildUserLanguageTemplate(tb);

        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        TEST_CHECK(base != nullptr, "P9b-3 U2 (A): the template buffer is non-null");
        if (base != nullptr) {
            // Header: style, dwExtendedStyle, cdit, x, y, cx, cy (18 bytes). The
            // cdit slot (offset 8) is the REQ-048 P2 root-cause pin: the loader
            // reads cx as the item count when cdit does not sit there.
            const WORD cdit = ReadWord(base, 8);
            TEST_CHECK(cdit == uld::kDialogItemCount,
                       "P9b-3 U2 (A): header cdit == kDialogItemCount (12 items)");
            TEST_CHECK(ReadWord(base, 14) == static_cast<WORD>(uld::kDialogWidthDlu) &&
                           ReadWord(base, 16) ==
                               static_cast<WORD>(uld::kDialogHeightDlu),
                       "P9b-3 U2 (A): dialog geometry matches the exposed DLU pair");
            TEST_CHECK(ReadDword(base, 0) != 0,
                       "P9b-3 U2 (A): style DWORD present at offset 0");

            // Production item set, walked exactly as the Windows dialog parser
            // consumes it: 4-byte item alignment, 18-byte fixed part,
            // {0xFFFF, ordinal} class array, title string, creation-data WORD.
            const DWORD LBL = WS_CHILD | WS_VISIBLE;
            const DWORD EDT =
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
            const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            const DWORD ADD = BTN | BS_DEFPUSHBUTTON;
            const WORD STATIC_CLS = 0x0082;
            const WORD EDIT_CLS = 0x0081;
            const WORD BTN_CLS = 0x0080;
            const RowExpect rows[] = {
                {uld::kIdNativeLabel, STATIC_CLS, 8, 6, 244, 9, LBL,
                 I18n::Get(StringId::UserLangNativeNameLabel)},
                {uld::kIdNativeEdit, EDIT_CLS, 8, 19, 244, 12, EDT, L""},
                {uld::kIdEnglishLabel, STATIC_CLS, 8, 35, 244, 9, LBL,
                 I18n::Get(StringId::UserLangEnglishNameLabel)},
                {uld::kIdEnglishEdit, EDIT_CLS, 8, 48, 244, 12, EDT, L""},
                {uld::kIdCodeLabel, STATIC_CLS, 8, 64, 116, 9, LBL,
                 I18n::Get(StringId::UserLangCodeLabel)},
                {uld::kIdCodeValue, STATIC_CLS, 128, 64, 124, 9, LBL, L""},
                {uld::kIdScriptLabel, STATIC_CLS, 8, 78, 116, 9, LBL,
                 I18n::Get(StringId::UserLangScriptLabel)},
                {uld::kIdScriptValue, STATIC_CLS, 128, 78, 124, 9, LBL, L""},
                {uld::kIdNoCodeNotice, STATIC_CLS, 8, 92, 244, 26, LBL, L""},
                {uld::kIdRefusal, STATIC_CLS, 8, 120, 244, 20, LBL, L""},
                {uld::kIdAdd, BTN_CLS, 108, 146, 64, 14, ADD,
                 I18n::Get(StringId::UserLangAdd)},
                {IDCANCEL, BTN_CLS, 180, 146, 64, 14, BTN,
                 I18n::Get(StringId::UserLangCancel)},
            };
            const std::size_t expected_rows = sizeof(rows) / sizeof(rows[0]);
            TEST_CHECK(eq(expected_rows, static_cast<std::size_t>(cdit)),
                       "P9b-3 U2 (A): the expectation table covers every emitted item");

            size_t off = 18;
            off += 2; // menu (none)
            off += 2; // windowClass (none)
            const std::wstring walked_title = ReadString(base, &off);
            TEST_CHECK(walked_title == dialog_title,
                       "P9b-3 U2 (A): the dialog title round-trips (UserLangAddTitle)");
            const WORD pointsize = ReadWord(base, off);
            off += 2;
            const std::wstring font = ReadString(base, &off);
            TEST_CHECK(pointsize == 9 && font == L"Segoe UI",
                       "P9b-3 U2 (A): 9pt Segoe UI shell font");

            bool ids_ok = true;
            bool geometry_ok = true;
            bool styles_ok = true;
            bool titles_ok = true;
            bool alignment_ok = true;
            bool add_default_ok = false;
            for (WORD i = 0; i < cdit && i < static_cast<WORD>(expected_rows); ++i) {
                off += (4 - off % 4) % 4; // DWORD-align relative to the template
                if (off % 4 != 0) {
                    alignment_ok = false;
                }
                const RowExpect& e = rows[i];
                if (ReadWord(base, off + 16) != e.id) {
                    ids_ok = false;
                }
                const short x = static_cast<short>(ReadWord(base, off + 8));
                const short y = static_cast<short>(ReadWord(base, off + 10));
                const short cx = static_cast<short>(ReadWord(base, off + 12));
                const short cy = static_cast<short>(ReadWord(base, off + 14));
                if (x != e.x || y != e.y || cx != e.cx || cy != e.cy) {
                    geometry_ok = false;
                }
                if (ReadDword(base, off) != e.style) {
                    styles_ok = false;
                }
                if (e.id == uld::kIdAdd &&
                    ReadDword(base, off) == (BTN | BS_DEFPUSHBUTTON)) {
                    add_default_ok = true;
                }
                // Class array: 0xFFFF prefix WORD + the system-class ordinal.
                if (ReadWord(base, off + 18) != 0xFFFF ||
                    ReadWord(base, off + 20) != e.cls) {
                    ids_ok = false;
                }
                size_t title_off = off + 22; // 18-byte fixed part + class array
                const std::wstring title = ReadString(base, &title_off);
                if (title != e.title) {
                    titles_ok = false;
                }
                if (ReadWord(base, title_off) != 0) {
                    styles_ok = false; // creation-data count WORD must be 0
                }
                off = title_off + 2;
            }
            TEST_CHECK(alignment_ok,
                       "P9b-3 U2 (A): every item starts 4-byte aligned (DLGTEMPLATE "
                       "contract)");
            TEST_CHECK(ids_ok,
                       "P9b-3 U2 (A): control ids + class ordinals match the production set");
            TEST_CHECK(geometry_ok,
                       "P9b-3 U2 (A): item x/y/cx/cy match the production geometry");
            TEST_CHECK(styles_ok,
                       "P9b-3 U2 (A): exact style DWORDs (both EDITs carry ES_AUTOHSCROLL, "
                       "creation data 0)");
            TEST_CHECK(titles_ok,
                       "P9b-3 U2 (A): every caption is its landed P9b-1 StringId");
            TEST_CHECK(add_default_ok,
                       "P9b-3 U2 (A): Add is the default push button");
            TEST_CHECK(off == tb.size(),
                       "P9b-3 U2 (A): the parser walk consumed exactly size() bytes");
        }

        // The same template under a second locale: no caption may fall back to
        // the English default (that would be a missing 37-locale designator).
        I18n::SetLocale(UiLocale::Korean);
        TemplateBuilder tb_ko;
        tb_ko.Begin(I18n::Get(StringId::UserLangAddTitle), uld::kDialogWidthDlu,
                    uld::kDialogHeightDlu, uld::kDialogItemCount);
        uld::BuildUserLanguageTemplate(tb_ko);
        const std::wstring ko_bytes(
            reinterpret_cast<const wchar_t*>(tb_ko.Get()),
            tb_ko.size() / sizeof(wchar_t));
        TEST_CHECK(!I18n::Get(StringId::UserLangAdd).empty() &&
                       ko_bytes.find(I18n::Get(StringId::UserLangNativeNameLabel)) !=
                           std::wstring::npos &&
                       ko_bytes.find(I18n::Get(StringId::UserLangAdd)) !=
                           std::wstring::npos,
                   "P9b-3 U2 (A): the Korean template carries the Korean captions");
        I18n::SetLocale(UiLocale::English);

        // The dialog renders exactly these landed ids: no new string was needed
        // for U2 (the P9b-1 copy inventory covers the whole form).
        const StringId used_ids[] = {
            StringId::UserLangAddTitle,         StringId::UserLangNativeNameLabel,
            StringId::UserLangEnglishNameLabel, StringId::UserLangCodeLabel,
            StringId::UserLangScriptLabel,      StringId::UserLangAdd,
            StringId::UserLangCancel,           StringId::UserLangNoCodeNotice,
            StringId::UserLangNameReserved,     StringId::UserLangScriptNone,
        };
        bool copy_ok = true;
        for (const StringId id : used_ids) {
            if (I18n::Get(id).empty()) {
                copy_ok = false;
            }
        }
        TEST_CHECK(copy_ok && eq(sizeof(used_ids) / sizeof(used_ids[0]),
                                 static_cast<std::size_t>(10)),
                   "P9b-3 U2 (A): all ten StringIds the dialog renders are non-empty (the "
                   "P9b-1 copy covers the form)");
        I18n::SetLocale(initial_locale);
    }

    // ---- (B) the refusal message + the "{n}" token -------------------------
    {
        using namespace p9b3u2_detail;
        I18n::SetLocale(UiLocale::English);
        const std::wstring tmpl = I18n::Get(StringId::UserLangNameReserved);
        TEST_CHECK(eq(CountOf(tmpl, L"{n}"), 1),
                   "P9b-3 U2 (B): the landed UserLangNameReserved carries the {n} token "
                   "exactly once");

        const char* const reserved[] = {"Auto Detect", "AUTO", "자동 감지"};
        bool reserved_pins = true;
        for (const char* spelling : reserved) {
            const std::wstring name = ToUtf16(std::string(spelling));
            const uld::UserLanguageRefusal refusal =
                uld::BuildUserLanguageNameRefusal(UserLanguageNameVerdict::Reserved,
                                                  std::string(spelling));
            // Anchored to the verdict U1 pins for this spelling, so the loop
            // cannot pass vacuously on a fixture the config layer let through.
            const bool refused_by_config =
                ValidateUserLanguageName(std::string(spelling), "", {}) ==
                UserLanguageNameVerdict::Reserved;
            if (!refused_by_config || !refusal.refuse ||
                eq(CountOf(refusal.message, name), 0) ||
                !eq(CountOf(refusal.message, name), 1) ||
                refusal.message.find(L"{n}") != std::wstring::npos ||
                refusal.message != uld::SubstituteNameTokenOnce(tmpl, name)) {
                reserved_pins = false;
                std::cerr << "  [refusal] reserved spelling not rendered once: " << spelling
                          << std::endl;
            }
        }
        TEST_CHECK(reserved_pins,
                   "P9b-3 U2 (B): each reserved AUTO spelling reaches the landed refusal copy "
                   "exactly once and no {n} survives");

        // The duplicate case shares the same landed sentence ("{n}" is already a
        // language name): ONE string answers both PR9-11 verdicts.
        const std::wstring dup_name = ToUtf16(std::string("Korean"));
        const uld::UserLanguageRefusal duplicate =
            uld::BuildUserLanguageNameRefusal(UserLanguageNameVerdict::Duplicate, "Korean");
        TEST_CHECK(ValidateUserLanguageName("Korean", "", {}) ==
                           UserLanguageNameVerdict::Duplicate &&
                       duplicate.refuse && eq(CountOf(duplicate.message, dup_name), 1) &&
                       duplicate.message == uld::SubstituteNameTokenOnce(tmpl, dup_name),
                   "P9b-3 U2 (B): the duplicate verdict renders the same copy with the "
                   "duplicate name substituted once");

        // Accepted says nothing (no fabricated complaint on a clean name).
        const uld::UserLanguageRefusal accepted =
            uld::BuildUserLanguageNameRefusal(UserLanguageNameVerdict::Accepted,
                                              "Samplelandic");
        TEST_CHECK(!accepted.refuse && accepted.message.empty(),
                   "P9b-3 U2 (B): an accepted name produces no refusal line");

        // "Exactly once" is a property of the helper, not of the tables: a
        // two-token template keeps its second token, a token-less one is
        // returned unchanged (never a half-built sentence).
        TEST_CHECK(uld::SubstituteNameTokenOnce(L"A {n} B {n}", L"X") == L"A X B {n}" &&
                       uld::SubstituteNameTokenOnce(L"no token", L"X") == L"no token",
                   "P9b-3 U2 (B): the substitution replaces the FIRST token only and leaves "
                   "a token-less template alone");
        I18n::SetLocale(initial_locale);
    }

    // ---- (C) live derivation through the injectable D5 locale seam ---------
    {
        using namespace p9b3u2_detail;
        // Stage 2 answers: "Samplelandic" is known only to the fake locale
        // table, whose primary subtag "fi" IS a registry row.
        const UserLanguageCandidate known = uld::DeriveUserLanguageCandidate(
            "Samplelandic", "", {}, &FakeLocaleSource);
        TEST_CHECK(known.code == "FI" && known.name_present && !known.no_code,
                   "P9b-3 U2 (C): the fake locale source resolves a deterministic code (FI)");
        TEST_CHECK(known.english_name == "Finnish",
                   "P9b-3 U2 (C): the resolved code back-fills the stored English name "
                   "(PR9-07) while the EDIT is left as typed");
        TEST_CHECK(known.script_token == UserLanguageScriptToken(known.script_mask) &&
                       known.script_token == "latin",
                   "P9b-3 U2 (C): the script row is the token of the entry's own mask "
                   "(name inference wins over the code's script)");

        // The SAME name with no stage 2 collapses to the no-code outcome: proof
        // that the seam is really the resolver and not a constant.
        const UserLanguageCandidate no_seam = uld::DeriveUserLanguageCandidate(
            "Samplelandic", "", {}, &EmptyLocaleSource);
        TEST_CHECK(no_seam.code.empty() && no_seam.no_code,
                   "P9b-3 U2 (C): an empty locale table yields no code + the no-code notice "
                   "path");

        // An unknown name: "" code, notice on, English left empty.
        const UserLanguageCandidate unknown_name =
            uld::DeriveUserLanguageCandidate("Zorblaxian", "", {}, &FakeLocaleSource);
        TEST_CHECK(FindLanguageByName("Zorblaxian") == nullptr &&
                       unknown_name.code.empty() && unknown_name.no_code &&
                       unknown_name.english_name.empty() &&
                       unknown_name.verdict == UserLanguageNameVerdict::Accepted,
                   "P9b-3 U2 (C): an unresolvable name reports no code, no back-fill and no "
                   "PR9-11 refusal");

        // An untouched form shows NEITHER the notice nor the script-failure row:
        // the pins above would otherwise make the notice appear at dialog open.
        const UserLanguageCandidate blank =
            uld::DeriveUserLanguageCandidate("", "", {}, &FakeLocaleSource);
        TEST_CHECK(!blank.name_present && !blank.no_code && blank.native_name.empty() &&
                       blank.code.empty() && blank.script_token.empty(),
                   "P9b-3 U2 (C): the empty form derives nothing (no notice, no script "
                   "token, no code)");

        // Fail-open: a name with no script class keeps mask 0, so the window
        // renders UserLangScriptNone rather than guessing a script.
        const UserLanguageCandidate no_script =
            uld::DeriveUserLanguageCandidate("---", "", {}, &FakeLocaleSource);
        TEST_CHECK(no_script.name_present && no_script.script_mask == 0 &&
                       no_script.script_token.empty() &&
                       !I18n::Get(StringId::UserLangScriptNone).empty(),
                   "P9b-3 U2 (C): a name with no classifiable script renders the landed "
                   "UserLangScriptNone value");

        // The live (default-seam) overload still resolves through the alias
        // matrix - a machine-independent assertion.
        const UserLanguageCandidate live =
            uld::DeriveUserLanguageCandidate("Korean", "", {});
        TEST_CHECK(live.code == "KO" && live.verdict == UserLanguageNameVerdict::Duplicate,
                   "P9b-3 U2 (C): the default-seam overload resolves through the alias stage "
                   "and reports the duplicate verdict");

        // The two derivation sources the requirement names ARE the sources the
        // candidate carries: the code equals the injectable ladder's own answer,
        // and a classifiable name's mask equals the inference the row names.
        const UserLanguageResolution direct =
            ResolveUserLanguageCode("Samplelandic", "", &FakeLocaleSource);
        const UserLanguageCandidate plain = uld::DeriveUserLanguageCandidate(
            "Samplelandic", "", {}, &EmptyLocaleSource);
        TEST_CHECK(known.code == direct.code && direct.code == "FI",
                   "P9b-3 U2 (C): the code row IS ResolveUserLanguageCode's answer through the "
                   "injectable 3-argument seam");
        TEST_CHECK(plain.script_mask == InferUserLanguageScript("Samplelandic") &&
                       plain.script_mask != 0 &&
                       plain.script_token == UserLanguageScriptToken(plain.script_mask) &&
                       plain.script_token == "latin",
                   "P9b-3 U2 (C): the script row IS InferUserLanguageScript's mask named by "
                   "UserLanguageScriptToken");
    }

    // ---- (D) bidi neutralization at RENDER time ----------------------------
    // PR9-03's recorded P9b obligation: storage strips the marks, and the
    // dialog's preview/refusal rows must render the SAME cleaned form (a raw
    // RLO/isolate could otherwise reorder the line that reports the refusal).
    {
        using namespace p9b3u2_detail;
        const UserLanguageCandidate rlo = uld::DeriveUserLanguageCandidate(
            "Sam\xE2\x80\xAEplelandic", "", {}, &EmptyLocaleSource);
        TEST_CHECK(rlo.native_name == "Samplelandic" &&
                       rlo.native_name.find('\xE2') == std::string::npos,
                   "P9b-3 U2 (D): U+202E (RLO) is stripped from the rendered name");

        const UserLanguageCandidate isolates = uld::DeriveUserLanguageCandidate(
            "\xE2\x81\xA7He\xE2\x81\xA8", "", {}, &EmptyLocaleSource);
        TEST_CHECK(isolates.native_name == "He",
                   "P9b-3 U2 (D): U+2067/U+2068 isolates are stripped from the rendered name");

        const UserLanguageCandidate soft_hyphen = uld::DeriveUserLanguageCandidate(
            "Med\xC2\xADical", "", {}, &EmptyLocaleSource);
        TEST_CHECK(soft_hyphen.native_name == "Medical",
                   "P9b-3 U2 (D): U+00AD (soft hyphen) is stripped from the rendered name");

        const UserLanguageCandidate clean_control =
            uld::DeriveUserLanguageCandidate("Samplelandic", "", {}, &EmptyLocaleSource);
        TEST_CHECK(clean_control.native_name == "Samplelandic",
                   "P9b-3 U2 (D): clean text passes through unchanged (the strip is targeted, "
                   "not a blanket ASCII filter)");

        // The refusal line renders the CLEANED form: a reserved spelling
        // smuggled past the fold with a zero-width space is refused AND shown
        // stripped, so the message names exactly what was compared.
        const UserLanguageCandidate dirty_reserved = uld::DeriveUserLanguageCandidate(
            "Auto\xE2\x80\x8B Detect", "", {}, &EmptyLocaleSource);
        const uld::UserLanguageRefusal dirty_refusal = uld::BuildUserLanguageNameRefusal(
            dirty_reserved.verdict, dirty_reserved.native_name);
        TEST_CHECK(dirty_reserved.native_name == "Auto Detect" &&
                       dirty_reserved.verdict == UserLanguageNameVerdict::Reserved &&
                       dirty_refusal.refuse &&
                       eq(CountOf(dirty_refusal.message, L"Auto Detect"), 1) &&
                       dirty_refusal.message.find(L"\u200B") == std::wstring::npos &&
                       dirty_refusal.message.find(ToUtf16("Auto\xE2\x80\x8B Detect")) ==
                           std::wstring::npos,
                   "P9b-3 U2 (D): the refusal line renders the cleaned name (no U+200B, no "
                   "raw typed bytes)");
    }

    // ---- (E) the outcome fold ---------------------------------------------
    // kAdded ONLY for Applied. Every other reason keeps the dialog OPEN; only
    // NameTaken has landed copy, so only it must also surface the refusal line.
    {
        using namespace p9b3u2_detail;
        const UserLanguageAddDisposition applied =
            uld::FoldUserLanguageAddResult(UserLanguageWriteResult::Applied);
        TEST_CHECK(applied.close_dialog &&
                       applied.outcome == UserLanguageDialogOutcome::kAdded &&
                       applied.block == UserLanguageAddBlock::None,
                   "P9b-3 U2 (E): Applied is the only outcome that closes the dialog as kAdded");

        const UserLanguageAddDisposition reserved = uld::FoldUserLanguageAddResult(
            UserLanguageWriteResult::RejectedReservedName);
        const UserLanguageAddDisposition duplicate = uld::FoldUserLanguageAddResult(
            UserLanguageWriteResult::RejectedDuplicateName);
        TEST_CHECK(!reserved.close_dialog && !duplicate.close_dialog &&
                       reserved.outcome == UserLanguageDialogOutcome::kCancelled &&
                       duplicate.outcome == UserLanguageDialogOutcome::kCancelled &&
                       reserved.block == UserLanguageAddBlock::NameTaken &&
                       duplicate.block == UserLanguageAddBlock::NameTaken &&
                       reserved.name_verdict == UserLanguageNameVerdict::Reserved &&
                       duplicate.name_verdict == UserLanguageNameVerdict::Duplicate,
                   "P9b-3 U2 (E): reserved and duplicate refusals keep the dialog open and "
                   "carry their own verdict for the copy");

        const UserLanguageAddDisposition empty = uld::FoldUserLanguageAddResult(
            UserLanguageWriteResult::RejectedEmptyName);
        const UserLanguageAddDisposition capacity = uld::FoldUserLanguageAddResult(
            UserLanguageWriteResult::RejectedCapacity);
        const UserLanguageAddDisposition carrier = uld::FoldUserLanguageAddResult(
            UserLanguageWriteResult::RefusedForwardCompatCarrier);
        TEST_CHECK(!empty.close_dialog && !capacity.close_dialog &&
                       !carrier.close_dialog &&
                       empty.block == UserLanguageAddBlock::EmptyName &&
                       capacity.block == UserLanguageAddBlock::Capacity &&
                       carrier.block == UserLanguageAddBlock::ForwardCompatStored &&
                       empty.name_verdict == UserLanguageNameVerdict::Accepted &&
                       capacity.name_verdict == UserLanguageNameVerdict::Accepted &&
                       carrier.name_verdict == UserLanguageNameVerdict::Accepted,
                   "P9b-3 U2 (E): empty / capacity / carrier keep the dialog open with no "
                   "refusal copy (no landed words for them)");

        // End to end through the REAL mutator: the fold reads the verb's actual
        // result, so the mapping above cannot drift from the config layer.
        AppConfig cfg;
        const UserLanguageAddDisposition first = uld::FoldUserLanguageAddResult(
            cfg.AddUserLanguage(MakeUserLanguage("Samplelandic B", "", &EmptyLocaleSource)));
        const UserLanguageAddDisposition again = uld::FoldUserLanguageAddResult(
            cfg.AddUserLanguage(MakeUserLanguage("Samplelandic B", "", &EmptyLocaleSource)));
        const UserLanguageAddDisposition auto_named = uld::FoldUserLanguageAddResult(
            cfg.AddUserLanguage(MakeUserLanguage("Auto Detect", "", &EmptyLocaleSource)));
        TEST_CHECK(first.close_dialog && first.outcome == UserLanguageDialogOutcome::kAdded &&
                       eq(cfg.user_languages.size(), 1) &&
                       !again.close_dialog &&
                       again.block == UserLanguageAddBlock::NameTaken &&
                       !auto_named.close_dialog &&
                       auto_named.block == UserLanguageAddBlock::NameTaken &&
                       auto_named.name_verdict == UserLanguageNameVerdict::Reserved &&
                       eq(cfg.user_languages.size(), 1),
                   "P9b-3 U2 (E): the real mutator drives the fold (add = kAdded, duplicate + "
                   "reserved = open, registry untouched)");

        AppConfig carrier_cfg;
        TEST_CHECK(carrier_cfg.FromJsonString(
                       "{\"user_languages_version\": 2, \"user_languages\": "
                       "[{\"native_name\": \"Samplelandic\"}]}") &&
                       uld::FoldUserLanguageAddResult(carrier_cfg.AddUserLanguage(
                                                          MakeUserLanguage("Samplelandic B", "",
                                                                           &EmptyLocaleSource)))
                               .block == UserLanguageAddBlock::ForwardCompatStored,
                   "P9b-3 U2 (E): the forward-compat carrier reaches the dialog as its own "
                   "block code (edit would be discarded on save)");
    }

    // ---- (F) the unit boundary, read off the source ------------------------
    {
        using namespace p9b3u2_detail;
        const std::filesystem::path cpp =
            ResolveRepoFile(L"src\\ui\\user_language_dialog.cpp");
        const std::filesystem::path hpp =
            ResolveRepoFile(L"src\\ui\\user_language_dialog.hpp");
        const std::string source = cpp.empty() ? std::string() : ReadFileBytes(cpp);
        const std::string header = hpp.empty() ? std::string() : ReadFileBytes(hpp);
        TEST_CHECK(!source.empty() && !header.empty(),
                   "P9b-3 U2 (F): both new module files resolve for the boundary scan");
        if (!source.empty()) {
            TEST_CHECK(source.find("AddUserLanguage(") != std::string::npos,
                       "P9b-3 U2 (F): the add goes through the LOCKED mutator "
                       "(AppConfig::AddUserLanguage)");
            TEST_CHECK(source.find("HasUserLanguagesForwardCompatCarrier") !=
                               std::string::npos,
                       "P9b-3 U2 (F): the forward-compat carrier is consulted before the "
                       "affordance is offered (PR9-12 carrier rule)");
            TEST_CHECK(source.find("SaveToFile") == std::string::npos,
                       "P9b-3 U2 (F): the dialog performs NO file I/O (the caller persists)");
            TEST_CHECK(source.find("SanitizeUserLanguageField(") == std::string::npos &&
                           source.find("CapUserLanguageField(") == std::string::npos,
                       "P9b-3 U2 (F): the strip set is not restated (the cleaned form comes "
                       "from the public MakeUserLanguage choke point)");
            TEST_CHECK(source.find("std::lock_guard") == std::string::npos,
                       "P9b-3 U2 (F): no lock is duplicated in the UI layer (the mutator owns "
                       "the lock)");
            TEST_CHECK(source.find("MakeUserLanguage(") != std::string::npos &&
                           source.find("FoldUserLanguageAddResult(") != std::string::npos,
                       "P9b-3 U2 (F): the entry is built by the public builder and its result "
                       "routed through the fold");
            // Exactly ONE window-creating call: the modal loop stays the only
            // GUI entry, so a test never reaches it.
            std::size_t dialog_boxes = 0;
            for (std::size_t pos = source.find("DialogBoxIndirectParamW");
                 pos != std::string::npos;
                 pos = source.find("DialogBoxIndirectParamW", pos + 1)) {
                ++dialog_boxes;
            }
            TEST_CHECK(eq(dialog_boxes, 1),
                       "P9b-3 U2 (F): exactly one DialogBoxIndirectParamW (the single GUI "
                       "entry)");
        }
        if (!header.empty()) {
            TEST_CHECK(header.find("UserLanguageDialogOutcome ShowUserLanguageDialogEx") !=
                               std::string::npos &&
                           header.find("void BuildUserLanguageTemplate") !=
                               std::string::npos,
                       "P9b-3 U2 (F): the public surface later units call is declared "
                       "(ShowUserLanguageDialogEx + BuildUserLanguageTemplate)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-3 U2 add-language dialog passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-3 U2 add-language dialog: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}


// ===========================================================================
// 261116_0003 P9b-3 U3 (language-menu redesign, plan §5 P9): the CAPABILITY-
// PROBE RUNNER + FIRST-USE TRIGGER. Appended to U1/U2's suite file (every
// check above is untouched): U1's locked record verb is what the runner calls,
// and U2's dialog is one of the surfaces that will pick a probed target.
//
// Pinned here, headlessly (no window, no network - the transport is a fake
// std::function, the FakeLocaleSource idiom):
//   (a) the fold + the record path: a fake seam's translated sentence folds to
//       Supported, a same-as-input or empty output folds to Unsupported, a
//       throwing transport (engine failure) still records Unsupported and
//       never crashes; the seam receives the fixed sentence, the engine key,
//       the model and the stored name.
//   (b) the trigger condition: fires exactly once while Effective == Unknown
//       and no override exists; silent with a verdict, an override, a registry
//       target, the Google view, the local engine, Auto, or an in-flight key;
//       the user-.gguf engine fires under its own (engine, registry-id) key.
//   (c) Checking is runtime-only: mid-flight the persisted document carries
//       no probe state at all, the verdict + stamp survive a save/reload round
//       trip, and a hand-written "checking" token reads back as Unknown (the
//       P9a reader pin re-asserted as the U3 regression guard).
//   (d) the unit boundary, read off the sources: the single record site goes
//       through the LOCKED verb (never the raw free verb), Checking never
//       enters the runner, no file I/O, no surface headers, and the main.cpp
//       trigger/wiring exists exactly once.
// ===========================================================================
#include "../src/user_language_probe.hpp"

namespace p9b3u3_detail {

// The fake-seam observation fixture (which arguments the transport received).
struct SeenArgs {
    std::string sentence;
    std::string engine_id;
    std::string model;
    std::string target_name;
    int calls = 0;
};

// fix-list-round15 R15-24: the probe worker is OWNED and JOINABLE since
// R15-11 (a std::thread in the service's workers_ vector, never detached);
// it delivers by value + wake-up post, and the suite has no window pump, so
// it polls the drain until the outcome lands (bounded).
inline std::size_t WaitDrain(emebalachat::UserLanguageProbeService& service,
                             std::size_t want) {
    std::size_t got = 0;
    for (int i = 0; i < 10000 && got < want; ++i) {
        got += service.DrainOutcomes();
        if (got < want) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return got;
}

} // namespace p9b3u3_detail

void TestP9b3UserLanguageProbeRunner() {
    std::cout << "[RUN] Testing 261116_0003 P9b-3 U3 user-language probe runner..."
              << std::endl;
    const int failures_before = g_failed_count;

    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };

    // ---- (a) the fold + the record path through the locked verb ------------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U3 (a): the probe fixture entry registers");
        cfg.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService service(cfg, nullptr);
        p9b3u3_detail::SeenArgs seen;
        service.SetTransportForTest(
            [&seen](std::string_view sentence, std::string_view engine_id,
                    std::string_view model, std::string_view target_name) -> std::wstring {
                ++seen.calls;
                seen.sentence = std::string(sentence);
                seen.engine_id = std::string(engine_id);
                seen.model = std::string(model);
                seen.target_name = std::string(target_name);
                return L"Jó reggelt, elküldöm a jelentést.";
            });
        TEST_CHECK(service.RunOneBlockingForTest(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 U3 (a): the synchronous test path runs");

        const UserLanguageProbe* probe =
            FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-test");
        TEST_CHECK(probe != nullptr &&
                       probe->status == UserLanguageProbeStatus::Supported &&
                       probe->override_status == UserLanguageProbeStatus::Unknown &&
                       eq(probe->checked_at.size(), 20) &&
                       probe->evidence.find("nonempty=1") != std::string::npos &&
                       probe->evidence.find("differs=1") != std::string::npos,
                   "P9b-3 U3 (a): a translated sentence folds to Supported and records "
                   "through the locked verb with a UTC stamp + shape-only evidence");
        TEST_CHECK(seen.calls == 1 && seen.sentence == kUserLanguageProbeSentence &&
                       seen.engine_id == "openai" && seen.model == "gpt-test" &&
                       seen.target_name == "Samplelandic",
                   "P9b-3 U3 (a): the seam receives the fixed sentence, the engine key, "
                   "the OpenAI model and the stored name");

        // Same-as-input: the engine echoed the prompt instead of translating.
        service.SetTransportForTest(
            [](std::string_view, std::string_view, std::string_view,
               std::string_view) -> std::wstring {
                return ToUtf16(std::string(kUserLanguageProbeSentence));
            });
        TEST_CHECK(service.RunOneBlockingForTest(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 U3 (a): the re-run path executes");
        const UserLanguageProbe* echoed =
            FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-test");
        TEST_CHECK(echoed != nullptr &&
                       echoed->status == UserLanguageProbeStatus::Unsupported &&
                       echoed->evidence.find("differs=0") != std::string::npos,
                   "P9b-3 U3 (a): a same-as-input output folds to Unsupported");

        // Empty output: recorded, never a crash.
        service.SetTransportForTest([](std::string_view, std::string_view,
                                       std::string_view, std::string_view) -> std::wstring {
            return L"";
        });
        TEST_CHECK(service.RunOneBlockingForTest(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 U3 (a): an empty-output run completes the record path");
        const UserLanguageProbe* empty_probe =
            FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-test");
        TEST_CHECK(empty_probe != nullptr &&
                       empty_probe->status == UserLanguageProbeStatus::Unsupported &&
                       empty_probe->evidence.find("reason=empty") != std::string::npos,
                   "P9b-3 U3 (a): an empty output folds to Unsupported with the failure "
                   "recorded in the evidence");

        // Engine failure (a throwing transport): the runner catches, folds to
        // Unsupported, records, and never crashes.
        service.SetTransportForTest([](std::string_view, std::string_view,
                                       std::string_view, std::string_view) -> std::wstring {
            throw std::runtime_error("simulated engine failure");
        });
        TEST_CHECK(service.RunOneBlockingForTest(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 U3 (a): a throwing transport still completes the record path");
        const UserLanguageProbe* failed_probe =
            FindUserLanguageProbe(cfg.user_languages[0], "openai", "gpt-test");
        TEST_CHECK(failed_probe != nullptr &&
                       failed_probe->status == UserLanguageProbeStatus::Unsupported &&
                       failed_probe->evidence.find("reason=empty") != std::string::npos,
                   "P9b-3 U3 (a): an engine failure folds to Unsupported with the failure "
                   "recorded - never a crash");

        // The fold only ever yields a storable verdict.
        const ProbeEvaluation any_fold = FoldUserLanguageProbeOutput("anything", 0);
        TEST_CHECK(any_fold.status == UserLanguageProbeStatus::Supported ||
                       any_fold.status == UserLanguageProbeStatus::Unsupported,
                   "P9b-3 U3 (a): the fold never yields Unknown/Checking (Checking is "
                   "runtime-only)");

        // The user-.gguf key carries the registry id as its model half.
        cfg.user_model_id = "my-model";
        p9b3u3_detail::SeenArgs gguf_seen;
        service.SetTransportForTest(
            [&gguf_seen](std::string_view, std::string_view engine_id,
                         std::string_view model, std::string_view target_name)
                -> std::wstring {
                ++gguf_seen.calls;
                gguf_seen.engine_id = std::string(engine_id);
                gguf_seen.model = std::string(model);
                gguf_seen.target_name = std::string(target_name);
                return L"Qapla'qu' jatlh.";
            });
        TEST_CHECK(service.RunOneBlockingForTest(LanguageEngine::UserGguf, "Samplelandic"),
                   "P9b-3 U3 (a): the user-.gguf engine runs the same path");
        TEST_CHECK(gguf_seen.calls == 1 && gguf_seen.engine_id == "user_gguf" &&
                       gguf_seen.model == "my-model" &&
                       gguf_seen.target_name == "Samplelandic",
                   "P9b-3 U3 (a): the user-.gguf key carries the registry id as its model");
        const UserLanguageProbe* gguf_probe =
            FindUserLanguageProbe(cfg.user_languages[0], "user_gguf", "my-model");
        TEST_CHECK(gguf_probe != nullptr &&
                       gguf_probe->status == UserLanguageProbeStatus::Supported,
                   "P9b-3 U3 (a): the user-.gguf verdict lands in its own cache slot");

        // The never-probed engines refuse the synchronous path too.
        TEST_CHECK(!service.RunOneBlockingForTest(LanguageEngine::Google, "Samplelandic") &&
                       !service.RunOneBlockingForTest(LanguageEngine::HyMt2, "Samplelandic") &&
                       !service.RunOneBlockingForTest(LanguageEngine::Auto, "Samplelandic") &&
                       !service.RunOneBlockingForTest(LanguageEngine::OpenAi, "Nobodyish"),
                   "P9b-3 U3 (a): the sync path refuses Google/HyMt2/Auto and unknown names");
    }

    // ---- (b) the trigger condition ------------------------------------------
    {
        // The pure predicate across the engine set and the answer states.
        UserLanguage entry;
        entry.native_name = "Samplelandic";
        entry.script_mask = kScriptMaskLatin;
        UserLanguageProbe answered;
        answered.engine = "openai";
        answered.model = "gpt-test";
        answered.status = UserLanguageProbeStatus::Supported;
        UserLanguageProbe overridden = answered;
        overridden.status = UserLanguageProbeStatus::Unknown;
        overridden.override_status = UserLanguageProbeStatus::Unsupported;

        TEST_CHECK(!UserLanguageProbeFirstUseWanted(LanguageEngine::Google, &entry,
                                                    nullptr, false) &&
                       !UserLanguageProbeFirstUseWanted(LanguageEngine::HyMt2, &entry,
                                                        nullptr, false) &&
                       !UserLanguageProbeFirstUseWanted(LanguageEngine::Auto, &entry,
                                                        nullptr, false),
                   "P9b-3 U3 (b): Google (publisher-declared), the local built-in and Auto "
                   "never fire");
        TEST_CHECK(UserLanguageProbeFirstUseWanted(LanguageEngine::OpenAi, &entry,
                                                   nullptr, false) &&
                       UserLanguageProbeFirstUseWanted(LanguageEngine::UserGguf, &entry,
                                                       nullptr, false),
                   "P9b-3 U3 (b): the evidence-judging engines fire on an unanswered entry");
        TEST_CHECK(!UserLanguageProbeFirstUseWanted(LanguageEngine::OpenAi, nullptr,
                                                    nullptr, false),
                   "P9b-3 U3 (b): a registry target (no user entry) never fires");
        TEST_CHECK(!UserLanguageProbeFirstUseWanted(LanguageEngine::OpenAi, &entry,
                                                    &answered, false),
                   "P9b-3 U3 (b): a cached verdict suppresses the trigger");
        TEST_CHECK(!UserLanguageProbeFirstUseWanted(LanguageEngine::OpenAi, &entry,
                                                    &overridden, false),
                   "P9b-3 U3 (b): a user override suppresses the trigger");
        TEST_CHECK(!UserLanguageProbeFirstUseWanted(LanguageEngine::OpenAi, &entry,
                                                    nullptr, true),
                   "P9b-3 U3 (b): an in-flight probe suppresses a second launch");
        TEST_CHECK(UserLanguageEngineId(LanguageEngine::Google) == "google" &&
                       UserLanguageEngineId(LanguageEngine::OpenAi) == "openai" &&
                       UserLanguageEngineId(LanguageEngine::UserGguf) == "user_gguf" &&
                       UserLanguageEngineId(LanguageEngine::HyMt2).empty() &&
                       UserLanguageEngineId(LanguageEngine::Auto).empty(),
                   "P9b-3 U3 (b): the D7 baseline the gate relies on (HyMt2/Auto -> \"\")");

        // End to end through the service: exactly one engine call, then silence.
        AppConfig trig;
        TEST_CHECK(trig.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied &&
                       trig.AddUserLanguage(MakeUserLanguage("Bembish", "")) ==
                           UserLanguageWriteResult::Applied &&
                       trig.AddUserLanguage(MakeUserLanguage("Retestic", "")) ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3 U3 (b): three fixture entries register");
        trig.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService service(trig, nullptr);
        int calls = 0;
        service.SetTransportForTest(
            [&calls](std::string_view, std::string_view, std::string_view,
                     std::string_view) -> std::wstring {
                ++calls;
                return L"Elküldöm a jelentést.";
            });
        TEST_CHECK(service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic") &&
                       eq(p9b3u3_detail::WaitDrain(service, 1),
                          static_cast<std::size_t>(1)) &&
                       calls == 1,
                   "P9b-3 U3 (b): the first use fires exactly one engine call");
        TEST_CHECK(!service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic") &&
                       calls == 1,
                   "P9b-3 U3 (b): the cached verdict suppresses a re-fire");
        const std::string stamp = UserLanguageProbeTimestampNowUtc();
        TEST_CHECK(trig.RecordUserLanguageProbeOverrideFor(
                       "Bembish", "openai", "gpt-test",
                       UserLanguageProbeStatus::Unsupported, stamp) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U3 (b): the override fixture records");
        TEST_CHECK(!service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Bembish") &&
                       calls == 1,
                   "P9b-3 U3 (b): an override answers the question (no probe)");
        TEST_CHECK(!service.MaybeTriggerForPick(LanguageEngine::Google, "Retestic") &&
                       !service.MaybeTriggerForPick(LanguageEngine::HyMt2, "Retestic") &&
                       !service.MaybeTriggerForPick(LanguageEngine::Auto, "Retestic") &&
                       calls == 1,
                   "P9b-3 U3 (b): the Google view, the local engine and Auto never trigger");
        TEST_CHECK(FindUserLanguageProbe(trig.user_languages[2], "google", "") == nullptr &&
                       FindUserLanguageProbe(trig.user_languages[2], "openai",
                                             "gpt-test") == nullptr,
                   "P9b-3 U3 (b): a suppressed trigger writes nothing");
        trig.user_model_id = "my-model";
        TEST_CHECK(service.MaybeTriggerForPick(LanguageEngine::UserGguf, "Retestic") &&
                       eq(p9b3u3_detail::WaitDrain(service, 1),
                          static_cast<std::size_t>(1)) &&
                       calls == 2,
                   "P9b-3 U3 (b): the user-.gguf first use fires under its own key");
        TEST_CHECK(!service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Nobodyish"),
                   "P9b-3 U3 (b): an unregistered target never fires");
    }

    // ---- (c) Checking is runtime-only + the round trip ----------------------
    {
        AppConfig chk;
        TEST_CHECK(chk.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U3 (c): the round-trip fixture registers");
        chk.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService service(chk, nullptr);
        std::atomic<bool> release{false};
        std::atomic<int> calls{0};
        service.SetTransportForTest(
            [&release, &calls](std::string_view, std::string_view, std::string_view,
                               std::string_view) -> std::wstring {
                ++calls;
                while (!release.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return L"Elküldöm a jelentést.";
            });
        TEST_CHECK(service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 U3 (c): the blocking probe launches");
        for (int i = 0; i < 5000 && calls.load(std::memory_order_relaxed) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_CHECK(calls.load(std::memory_order_relaxed) == 1 &&
                       service.IsInFlight("Samplelandic", "openai", "gpt-test"),
                   "P9b-3 U3 (c): the runtime state reads in flight (U4's badge folds this "
                   "to Checking)");
        TEST_CHECK(!service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic") &&
                       calls.load(std::memory_order_relaxed) == 1,
                   "P9b-3 U3 (c): the in-flight key coalesces a second trigger");
        const std::string mid_json = chk.ToJsonString();
        TEST_CHECK(mid_json.find("checking") == std::string::npos,
                   "P9b-3 U3 (c): while a probe is in flight the persisted document "
                   "carries no Checking token - the runtime state never serializes");

        release.store(true, std::memory_order_relaxed);
        TEST_CHECK(eq(p9b3u3_detail::WaitDrain(service, 1), static_cast<std::size_t>(1)) &&
                       !service.IsInFlight("Samplelandic", "openai", "gpt-test"),
                   "P9b-3 U3 (c): the completed probe drains and frees the slot");
        const UserLanguageProbe* done =
            FindUserLanguageProbe(chk.user_languages[0], "openai", "gpt-test");
        const std::string post_json = chk.ToJsonString();
        TEST_CHECK(done != nullptr && done->status == UserLanguageProbeStatus::Supported &&
                       post_json.find("checking") == std::string::npos,
                   "P9b-3 U3 (c): the verdict lands and the document still holds no "
                   "Checking token");
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(post_json) &&
                       eq(reloaded.user_languages.size(), static_cast<std::size_t>(1)),
                   "P9b-3 U3 (c): the post-probe document re-parses");
        const UserLanguageProbe* reloaded_probe =
            FindUserLanguageProbe(reloaded.user_languages[0], "openai", "gpt-test");
        TEST_CHECK(reloaded_probe != nullptr &&
                       reloaded_probe->status == UserLanguageProbeStatus::Supported &&
                       reloaded_probe->checked_at == done->checked_at,
                   "P9b-3 U3 (c): the verdict + stamp survive the save/reload round trip "
                   "and Checking never appears in it");

        // The reader contract (P9a-pinned, re-asserted as the U3 regression
        // guard): a hand-written "checking" token reads back as Unknown, so a
        // crashed run cannot strand a language in Checking forever.
        AppConfig hand;
        TEST_CHECK(hand.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Samplelandic\", "
                       "\"script\": \"latin\", \"probes\": [{\"engine\": \"openai\", "
                       "\"model\": \"m\", \"status\": \"checking\"}]}]}") &&
                       eq(hand.user_languages.size(), static_cast<std::size_t>(1)),
                   "P9b-3 U3 (c): a hand-written block with a stored \"checking\" parses");
        const UserLanguageProbe* hand_probe =
            FindUserLanguageProbe(hand.user_languages[0], "openai", "m");
        TEST_CHECK(hand_probe != nullptr &&
                       hand_probe->status == UserLanguageProbeStatus::Unknown,
                   "P9b-3 U3 (c): the persisted reader maps \"checking\" back to Unknown");
    }

    // ---- (d) the unit boundary, read off the sources ------------------------
    {
        using p9b3u2_detail::ReadFileBytes; // U2's helper above
        const std::filesystem::path cpp = ResolveRepoFile(L"src\\user_language_probe.cpp");
        const std::filesystem::path hpp = ResolveRepoFile(L"src\\user_language_probe.hpp");
        const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
        const std::string cpp_src = cpp.empty() ? std::string() : ReadFileBytes(cpp);
        const std::string hpp_src = hpp.empty() ? std::string() : ReadFileBytes(hpp);
        const std::string main_src = main_cpp.empty() ? std::string() : ReadFileBytes(main_cpp);
        TEST_CHECK(!cpp_src.empty() && !hpp_src.empty() && !main_src.empty(),
                   "P9b-3 U3 (d): the runner sources + main.cpp resolve for the scan");

        const auto count_of_str = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };
        // The open-paren needle cannot match the locked verb's name (the
        // "For(" breaks the prefix), so this is exactly the raw free verb.
        TEST_CHECK(eq(count_of_str(cpp_src, "RecordUserLanguageProbeResultFor("), 1),
                   "P9b-3 U3 (d): exactly one record site, through the LOCKED verb "
                   "AppConfig::RecordUserLanguageProbeResultFor");
        TEST_CHECK(cpp_src.find("RecordUserLanguageProbeResult(") == std::string::npos,
                   "P9b-3 U3 (d): the raw free verb is never called (the PR9-12 lock is "
                   "the only write path)");
        TEST_CHECK(hpp_src.find("UserLanguageProbeStatus::Checking") == std::string::npos &&
                       cpp_src.find("UserLanguageProbeStatus::Checking") == std::string::npos,
                   "P9b-3 U3 (d): the runner never references Checking - the runtime "
                   "state cannot leak into a record");
        TEST_CHECK(cpp_src.find("SaveToFile") == std::string::npos,
                   "P9b-3 U3 (d): the runner performs no file I/O (the recorded callback "
                   "persists)");
        TEST_CHECK(cpp_src.find("#include \"ui/tray.hpp\"") == std::string::npos &&
                       cpp_src.find("#include \"ui/tooltip.hpp\"") == std::string::npos &&
                       cpp_src.find("StringId::") == std::string::npos &&
                       hpp_src.find("#include \"ui/tray.hpp\"") == std::string::npos &&
                       hpp_src.find("#include \"ui/tooltip.hpp\"") == std::string::npos &&
                       hpp_src.find("StringId::") == std::string::npos,
                   "P9b-3 U3 (d): no surface headers and no StringIds (the badge/tray/"
                   "tooltip copy is U4's scope)");
        TEST_CHECK(eq(count_of_str(cpp_src, "kUserLanguageProbeSentence"), 2),
                   "P9b-3 U3 (d): the probe sends the fixed sentence - exactly the two "
                   "launch sites, no other payload source");
        TEST_CHECK(eq(count_of_str(main_src, "MaybeTriggerForPick("), 1) &&
                       main_src.find("kMsgUserLanguageProbeDone") != std::string::npos &&
                       main_src.find("DrainOutcomes(") != std::string::npos,
                   "P9b-3 U3 (d): the first-use trigger is wired into the coordinator "
                   "exactly once and the completion drain into the controller WndProc");
    }

    // ---- (e) fix-list-round15 R15-11 (unit F-2): joinable workers + the
    // bounded shutdown join. The probe worker was a fire-and-forget
    // std::thread(...).detach() whose lambda dereferenced the
    // TranslationManager - a cancel-ignoring request could outlive the
    // warn-only WaitInferenceIdle and destroy the engine under the live
    // worker (use-after-free at exit). Pinned from the sources (the F-1
    // idiom) plus one live fold through the REAL join path.
    {
        using p9b3u2_detail::ReadFileBytes;
        const auto lt = [](std::size_t lhs, std::size_t rhs) { return lhs < rhs; };
        const std::filesystem::path cpp = ResolveRepoFile(L"src\\user_language_probe.cpp");
        const std::filesystem::path hpp = ResolveRepoFile(L"src\\user_language_probe.hpp");
        const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
        const std::string cpp_src = cpp.empty() ? std::string() : ReadFileBytes(cpp);
        const std::string hpp_src = hpp.empty() ? std::string() : ReadFileBytes(hpp);
        const std::string main_src = main_cpp.empty() ? std::string() : ReadFileBytes(main_cpp);
        TEST_CHECK(!cpp_src.empty() && !hpp_src.empty() && !main_src.empty(),
                   "P9b-3 F-2 R15-11: the runner sources + main.cpp resolve for the scan");

        // (a)-1: the probe worker is OWNED and JOINABLE - no detach survives.
        TEST_CHECK(cpp_src.find(".detach()") == std::string::npos &&
                       hpp_src.find(".detach()") == std::string::npos,
                   "R15-11 (a): the probe worker is never detached (no .detach() anywhere "
                   "in the runner)");
        TEST_CHECK(hpp_src.find("std::vector<std::thread> workers_") != std::string::npos &&
                       cpp_src.find("workers_.push_back(") != std::string::npos,
                   "R15-11 (a): the service OWNS the joinable threads (member vector + the "
                   "spawn site pushes into it)");
        TEST_CHECK(hpp_src.find("ShutdownJoinWorkers(std::chrono::milliseconds bound)") !=
                       std::string::npos,
                   "R15-11 (a): the bounded shutdown join is part of the service contract");
        TEST_CHECK(cpp_src.find("state_->workers_cv.wait_for(") != std::string::npos &&
                       cpp_src.find(".join()") != std::string::npos,
                   "R15-11 (a): the shutdown path WAITS (bounded cv wait) then joins the "
                   "owned threads");

        // (a)-2: the shutdown block joins BEFORE the engine-teardown
        // confirmation (and long before the engine leaves scope).
        const std::size_t join_pos = main_src.find("probe_service.ShutdownJoinWorkers(");
        const std::size_t idle_pos = main_src.find("engine.WaitInferenceIdle(2000)");
        TEST_CHECK(join_pos != std::string::npos && idle_pos != std::string::npos &&
                       lt(join_pos, idle_pos),
                   "R15-11 (a): the probe join runs in the shutdown block BEFORE the "
                   "engine-teardown confirmation");

        // (a)-3: a bound expiry escalates to the safe terminal state
        // (TerminateProcess - the engine is never destroyed under the worker).
        TEST_CHECK(cpp_src.find("::TerminateProcess(::GetCurrentProcess(), 1)") !=
                       std::string::npos,
                   "R15-11 (a): the bound-expiry path escalates to TerminateProcess "
                   "instead of a use-after-free");

        // (a)-4: the in-flight bookkeeping stays exact across the join - the
        // worker signals completion only AFTER its outcome is queued, so the
        // post-join drain frees every slot.
        const std::size_t push_pos = cpp_src.find("state->outcomes.push_back(");
        const std::size_t dec_pos = cpp_src.find("--state->active_workers;");
        TEST_CHECK(push_pos != std::string::npos && dec_pos != std::string::npos &&
                       lt(push_pos, dec_pos),
                   "R15-11 (a): the outcome is queued BEFORE the worker decrements the "
                   "active count (the post-join drain is exhaustive)");

        // (a)-5, live: an in-flight probe blocks on the fake transport; the
        // bounded join returns only after the engine call ran to completion;
        // the drain then records and frees the slot (in-flight exact). The
        // service destructs below with every worker already joined - the
        // destructor backstop is a no-op, never an escalation.
        AppConfig join_cfg;
        TEST_CHECK(join_cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 F-2 R15-11: the join fixture entry registers");
        join_cfg.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService join_service(join_cfg, nullptr);
        std::atomic<int> join_calls{0};
        join_service.SetTransportForTest(
            [&join_calls](std::string_view, std::string_view, std::string_view,
                          std::string_view) -> std::wstring {
                ++join_calls;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return L"Elküldöm a jelentést.";
            });
        TEST_CHECK(join_service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic"),
                   "R15-11 (a): the join fixture probe launches");
        join_service.ShutdownJoinWorkers(std::chrono::milliseconds(2000));
        TEST_CHECK(join_calls.load() == 1,
                   "R15-11 (a): ShutdownJoinWorkers returned only after the in-flight "
                   "engine call ran to completion");
        TEST_CHECK(eq(join_service.DrainOutcomes(), static_cast<std::size_t>(1)) &&
                       !join_service.IsInFlight("Samplelandic", "openai", "gpt-test"),
                   "R15-11 (a): the post-join drain records the verdict and frees the "
                   "in-flight slot (bookkeeping exact)");

        // (a)-6 fix-list-round15 R15-20 (unit F-3a): the shutdown language-sync
        // drain is itself a probe PRODUCER - the coordinator it calls reaches
        // MaybeTriggerForPick (main.cpp:2513) and the service's retired latch
        // flips only AFTER the completion drain - so the worker join must sit
        // AFTER that drain loop and BEFORE the completion drain. The pre-fix
        // order (join -> loop) left a drain-spawned worker unjoined while the
        // shutdown comments claimed the drain was exhaustive; moving the call
        // back before the loop kills this check. Landmarks: the drain loop's
        // own first line and the service's shutdown drain, which is the LAST
        // DrainOutcomes call in main.cpp (the WndProc publishes it earlier).
        const std::size_t join_call_pos =
            main_src.find("probe_service.ShutdownJoinWorkers(");
        const std::size_t sync_drain_pos = main_src.find(
            "for (const emebalachat::LanguageSyncRequest& req : "
            "emebalachat::DrainLanguageSyncQueue())");
        const std::size_t outcome_drain_pos = main_src.rfind("probe_service.DrainOutcomes(");
        TEST_CHECK(sync_drain_pos != std::string::npos && join_call_pos != std::string::npos &&
                       outcome_drain_pos != std::string::npos &&
                       lt(sync_drain_pos, join_call_pos) && lt(join_call_pos, outcome_drain_pos),
                   "R15-20 (a): the probe join sits AFTER the shutdown language-sync drain "
                   "and BEFORE the probe-outcome drain (a drain-spawned probe is joined too)");

        // (a)-7: the causal chain that order exists for - the drained loop calls
        // the language coordinator, and the coordinator calls
        // MaybeTriggerForPick. Pinning both links keeps (a)-6 from going vacuous
        // unnoticed if either call is ever removed.
        const bool order_found = sync_drain_pos != std::string::npos &&
                                 join_call_pos != std::string::npos &&
                                 lt(sync_drain_pos, join_call_pos);
        const std::string drain_body =
            order_found ? main_src.substr(sync_drain_pos, join_call_pos - sync_drain_pos)
                        : std::string();
        const std::size_t trigger_pos = main_src.find("probe_service.MaybeTriggerForPick(");
        TEST_CHECK(trigger_pos != std::string::npos && lt(trigger_pos, sync_drain_pos) &&
                       drain_body.find("emebalachat::g_apply_language_change(") !=
                           std::string::npos,
                   "R15-20 (a): the drained loop drives the coordinator that starts the probe "
                   "(drain -> g_apply_language_change -> MaybeTriggerForPick)");

        // (a)-8: the sweep's join -> drain -> retire triangle: retiring before
        // the outcome drain would turn every queued verdict into a discard, so
        // the drain must run first (drain-before-retire is the house rule).
        const std::size_t retire_pos = main_src.find("probe_service.Retire();");
        TEST_CHECK(retire_pos != std::string::npos && lt(outcome_drain_pos, retire_pos),
                   "R15-20 (a): the service retires only AFTER the outcome drain (the "
                   "join -> drain -> retire triangle the sweep comment states)");
    }

    // ---- (f) fix-list-round15 R15-12 (unit F-2): the lang_sync DIAG line is
    // content-gated. The line names the planner's four pair fields, which can
    // BE user-typed stored names while the master sink is ON and the content
    // gate is OFF - the R14-06 class. Pinned: the line sits inside a
    // ContentLoggingEnabled gate with the shape-only len= fallback (the
    // tooltip menu_select_user idiom); ctx/valid/changed ride both branches.
    {
        using p9b3u2_detail::ReadFileBytes;
        const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
        const std::string main_src = main_cpp.empty() ? std::string() : ReadFileBytes(main_cpp);
        TEST_CHECK(!main_src.empty(),
                   "P9b-3 F-2 R15-12: main.cpp resolves for the lang_sync gate scan");
        const std::size_t sync_pos =
            main_src.find("lang_sync ctx=%s valid=%d changed=%d pair %s/%s");
        TEST_CHECK(sync_pos != std::string::npos,
                   "R15-12: the lang_sync pair record survives (now inside the gate)");
        const std::size_t window = sync_pos > 1200 ? sync_pos - 1200 : 0;
        const std::string before = main_src.substr(window, sync_pos - window);
        TEST_CHECK(before.find("if (diag::ContentLoggingEnabled())") != std::string::npos &&
                       before.find("R15-12") != std::string::npos,
                   "R15-12 (b): the lang_sync DIAG line sits inside the "
                   "diag::ContentLoggingEnabled() gate (the R14-06 pattern)");
        TEST_CHECK(main_src.find("pair_len=%zu/%zu") != std::string::npos &&
                       main_src.find("(req_len %zu/%zu)") != std::string::npos,
                   "R15-12 (b): the else branch keeps the shape-only len= fallback");
        // ctx/valid/changed ride BOTH branches: the shared prefix appears
        // exactly twice (the gated line + the fallback line).
        std::size_t sync_lines = 0;
        for (std::size_t pos = main_src.find("lang_sync ctx=%s valid=%d changed=%d pair");
             pos != std::string::npos;
             pos = main_src.find("lang_sync ctx=%s valid=%d changed=%d pair",
                                 pos + std::string("lang_sync ctx=%s").size())) {
            ++sync_lines;
        }
        TEST_CHECK(eq(sync_lines, static_cast<std::size_t>(2)),
                   "R15-12 (b): the non-content fields (ctx/valid/changed) ride BOTH "
                   "branches (the gated line + the shape-only fallback)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-3 U3 user-language probe runner passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-3 U3 user-language probe runner: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// 261116_0003 P9b-3 U4b (language-menu redesign, plan §5 P9 + D6/PR9-06): the
// user-language MANAGER dialog. Appended to U1/U2/U3's suite file (every check
// above is untouched): the manager renders U1's cached verdicts, drives U3's
// runner and applies U1's locked verbs.
//
// Pinned here, headlessly (no window - AGENTS.md §10, the p9b2/p9b3 manual-QA
// split):
//   (a) the dialog template: item count, 4-byte alignment, control ids /
//       geometry / styles / captions, parser walk consumes size() exactly.
//   (b) the row fold for all five UserLanguageProbeStatus reaches + the
//       override-active marker + the no-probe engine context (Google / local /
//       Auto: empty badge, every action disabled).
//   (c) the evidence render uses the cached shape-only string VERBATIM.
//   (d) the outcome fold: Applied -> kChanged (per verb result), every refusal
//       -> unchanged, a retest launch -> unchanged, no verb -> kClosed.
//   (e) the unit boundary read off the sources: the locked verbs (never the raw
//       free verbs, never a registry mutation), U3's RequestRetest, no
//       SetRecordedCallback hijack of the caller's slot, no file I/O, and no
//       StringId outside the landed P9b-1 set.
//   (f) the fold consumed through the REAL runner: in flight -> 확인 중 and a
//       disabled retest, the verdict -> the cached badge once it lands.
// ===========================================================================
#include "../src/ui/user_language_manager_dialog.hpp"

namespace p9b3u4b_detail {

using emebalachat::ProbeEvaluation;
using emebalachat::UserLanguage;
using emebalachat::UserLanguageProbe;
using emebalachat::UserLanguageProbeStatus;

// A probe evaluation fixture: only the fields the record verb stores matter
// here (status + evidence); the check bits are set consistently so a future
// evaluator-driven fixture cannot silently disagree.
ProbeEvaluation Eval(UserLanguageProbeStatus status, const std::string& evidence) {
    ProbeEvaluation e;
    e.status = status;
    e.non_empty = status == UserLanguageProbeStatus::Supported;
    e.differs = status == UserLanguageProbeStatus::Supported;
    e.script_ok = status == UserLanguageProbeStatus::Supported;
    e.evidence = evidence;
    return e;
}

// The shape-only evidence grammar the P9a evaluator emits (never user text).
const char* kEvidenceSupported =
    "nonempty=1 differs=1 script=1 expected=latin detected=latin";
const char* kEvidenceUnsupported = "nonempty=1 differs=0 script=na";

std::size_t CountOfStr(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

bool IsIdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

} // namespace p9b3u4b_detail

void TestP9b3UserLanguageManagerDialog() {
    std::cout << "[RUN] Testing 261116_0003 P9b-3 U4b user-language manager dialog..."
              << std::endl;
    const int failures_before = g_failed_count;

    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
    const auto gt = [](std::size_t lhs, std::size_t rhs) { return lhs > rhs; };

    namespace umd = emebalachat::user_language_manager_dialog;
    using emebalachat::LanguageEngine;
    using emebalachat::UserLanguageManagerOutcome;
    using emebalachat::UserLanguageWriteResult;
    using umd::UserLanguageManagerRow;

    // ---- (a) the dialog template -------------------------------------------
    // Captions are pinned per locale, so English is set for the walk and the
    // previous locale is restored (suite-state hygiene, the U2 precedent).
    const UiLocale initial_locale = I18n::GetCurrentLocale();
    {
        using namespace p9b3u2_detail; // ReadWord / ReadDword / ReadString / RowExpect
        I18n::SetLocale(UiLocale::English);
        const std::wstring dialog_title = I18n::Get(StringId::UserLangsGroup);
        TemplateBuilder tb;
        tb.Begin(dialog_title, umd::kDialogWidthDlu, umd::kDialogHeightDlu,
                 umd::kDialogItemCount);
        umd::BuildUserLanguageManagerTemplate(tb);

        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        TEST_CHECK(base != nullptr,
                   "P9b-3 U4b (a): the manager template buffer is non-null");
        if (base != nullptr) {
            // Header: style, dwExtendedStyle, cdit, x, y, cx, cy (18 bytes).
            const WORD cdit = ReadWord(base, 8);
            TEST_CHECK(cdit == umd::kDialogItemCount,
                       "P9b-3 U4b (a): header cdit == kDialogItemCount (10 items)");
            TEST_CHECK(ReadWord(base, 14) ==
                               static_cast<WORD>(umd::kDialogWidthDlu) &&
                           ReadWord(base, 16) ==
                               static_cast<WORD>(umd::kDialogHeightDlu),
                       "P9b-3 U4b (a): dialog geometry matches the exposed DLU pair");
            TEST_CHECK(ReadDword(base, 0) != 0,
                       "P9b-3 U4b (a): style DWORD present at offset 0");

            const DWORD LBL = WS_CHILD | WS_VISIBLE;
            const DWORD LIST = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                               WS_VSCROLL | LBS_NOTIFY;
            const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            const DWORD CHK = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
            const WORD STATIC_CLS = 0x0082;
            const WORD BTN_CLS = 0x0080;
            const WORD LIST_CLS = 0x0083;
            const RowExpect rows[] = {
                {umd::kIdList, LIST_CLS, 8, 6, 404, 170, LIST, L""},
                {umd::kIdBadge, STATIC_CLS, 8, 180, 404, 10, LBL, L""},
                {umd::kIdEvidenceLabel, STATIC_CLS, 8, 194, 64, 9, LBL,
                 I18n::Get(StringId::UserLangProbeEvidenceLabel)},
                {umd::kIdEvidence, STATIC_CLS, 76, 194, 336, 22, LBL, L""},
                {umd::kIdRetest, BTN_CLS, 8, 222, 92, 14, BTN,
                 I18n::Get(StringId::UserLangProbeRetest)},
                {umd::kIdOverrideSupported, BTN_CLS, 104, 222, 132, 14, BTN,
                 I18n::Get(StringId::UserLangOverrideSupported)},
                {umd::kIdOverrideUnsupported, BTN_CLS, 240, 222, 132, 14, BTN,
                 I18n::Get(StringId::UserLangOverrideUnsupported)},
                {umd::kIdOverrideMarker, BTN_CLS, 8, 242, 232, 12, CHK,
                 I18n::Get(StringId::UserLangOverrideActive)},
                {umd::kIdDisclaimer, STATIC_CLS, 8, 260, 404, 10, LBL,
                 I18n::Get(StringId::UserLangSupportNotQuality)},
                {IDCANCEL, BTN_CLS, 348, 278, 64, 14, BTN,
                 I18n::Get(StringId::GgufManagerClose)},
            };
            const std::size_t expected_rows = sizeof(rows) / sizeof(rows[0]);
            TEST_CHECK(eq(expected_rows, static_cast<std::size_t>(cdit)),
                       "P9b-3 U4b (a): the expectation table covers every emitted item");

            size_t off = 18;
            off += 2; // menu (none)
            off += 2; // windowClass (none)
            const std::wstring walked_title = ReadString(base, &off);
            TEST_CHECK(walked_title == dialog_title,
                       "P9b-3 U4b (a): the title is the landed UserLangsGroup copy");
            const WORD pointsize = ReadWord(base, off);
            off += 2;
            const std::wstring font = ReadString(base, &off);
            TEST_CHECK(pointsize == 9 && font == L"Segoe UI",
                       "P9b-3 U4b (a): 9pt Segoe UI shell font");

            bool ids_ok = true;
            bool geometry_ok = true;
            bool styles_ok = true;
            bool titles_ok = true;
            bool alignment_ok = true;
            for (WORD i = 0; i < cdit && i < static_cast<WORD>(expected_rows); ++i) {
                off += (4 - off % 4) % 4; // DWORD-align relative to the template
                if (off % 4 != 0) {
                    alignment_ok = false;
                }
                const RowExpect& e = rows[i];
                if (ReadWord(base, off + 16) != e.id) {
                    ids_ok = false;
                }
                const short x = static_cast<short>(ReadWord(base, off + 8));
                const short y = static_cast<short>(ReadWord(base, off + 10));
                const short cx = static_cast<short>(ReadWord(base, off + 12));
                const short cy = static_cast<short>(ReadWord(base, off + 14));
                if (x != e.x || y != e.y || cx != e.cx || cy != e.cy) {
                    geometry_ok = false;
                }
                if (ReadDword(base, off) != e.style) {
                    styles_ok = false;
                }
                // Class array: 0xFFFF prefix WORD + the system-class ordinal.
                if (ReadWord(base, off + 18) != 0xFFFF ||
                    ReadWord(base, off + 20) != e.cls) {
                    ids_ok = false;
                }
                size_t title_off = off + 22; // 18-byte fixed part + class array
                const std::wstring title = ReadString(base, &title_off);
                if (title != e.title) {
                    titles_ok = false;
                }
                if (ReadWord(base, title_off) != 0) {
                    styles_ok = false; // creation-data count WORD must be 0
                }
                off = title_off + 2;
            }
            TEST_CHECK(alignment_ok,
                       "P9b-3 U4b (a): every item starts 4-byte aligned");
            TEST_CHECK(ids_ok,
                       "P9b-3 U4b (a): control ids + class ordinals match the "
                       "production set (LISTBOX 0x0083 included)");
            TEST_CHECK(geometry_ok,
                       "P9b-3 U4b (a): item x/y/cx/cy match the production geometry");
            TEST_CHECK(styles_ok,
                       "P9b-3 U4b (a): exact style DWORDs (LBS_NOTIFY list, "
                       "BS_AUTOCHECKBOX marker, creation data 0)");
            TEST_CHECK(titles_ok,
                       "P9b-3 U4b (a): every caption is its landed P9b-1 string "
                       "(and Close reuses the generic landed label)");
            TEST_CHECK(off == tb.size(),
                       "P9b-3 U4b (a): the parser walk consumed exactly size() bytes");
        }
    }
    I18n::SetLocale(initial_locale);

    // ---- (b)(c) the row fold ------------------------------------------------
    {
        const std::string kModel = "gpt-test";
        AppConfig cfg;
        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U4b (b): the row fixture registers");
        // A second entry whose STORED name differs from its native spelling: one
        // row must show the stored label, never the native bytes.
        UserLanguage bilingual = MakeUserLanguage("Samplelandic", "");
        bilingual.native_name = "Samplelandic (native)";
        bilingual.english_name = "Samplelandic EN";
        TEST_CHECK(cfg.AddUserLanguage(bilingual) == UserLanguageWriteResult::Applied &&
                       eq(cfg.user_languages.size(), static_cast<std::size_t>(2)),
                   "P9b-3 U4b (b): the native/stored pair registers");

        // (b.1) Unknown: nothing judged yet -> no badge, actions available.
        {
            const std::vector<UserLanguageManagerRow> rows = umd::FoldUserLanguageManagerRows(
                cfg.user_languages, LanguageEngine::OpenAi, kModel, nullptr, true);
            TEST_CHECK(eq(rows.size(), static_cast<std::size_t>(2)) &&
                           rows[0].stored_name == "Samplelandic" &&
                           rows[0].label == L"Samplelandic" &&
                           rows[1].label == L"Samplelandic EN" &&
                           rows[1].list_text == L"Samplelandic EN",
                       "P9b-3 U4b (b): a row is labelled by UserLanguageStoredName, never "
                       "the native spelling");
            TEST_CHECK(!rows[0].badge_shown && rows[0].badge_text.empty() &&
                           !rows[0].override_active && !rows[0].in_flight &&
                           rows[0].engine_judges,
                       "P9b-3 U4b (b): Unknown draws NO badge (the only unlabelled "
                       "display state)");
            TEST_CHECK(rows[0].retest_enabled && rows[0].override_supported_enabled &&
                           rows[0].override_unsupported_enabled &&
                           !rows[0].clear_override_enabled,
                       "P9b-3 U4b (b): Unknown still offers 다시 시험 and both override "
                       "directions (no override to clear)");
            TEST_CHECK(rows[0].evidence.empty() && rows[0].evidence_line.empty(),
                       "P9b-3 U4b (c): no cached evidence renders nothing, not a filler");
        }

        // (b.2) Supported, recorded through U1's LOCKED verb.
        const std::string stamp = UserLanguageProbeTimestampNowUtc();
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor(
                       "Samplelandic", "openai", kModel,
                       p9b3u4b_detail::Eval(UserLanguageProbeStatus::Supported,
                                             p9b3u4b_detail::kEvidenceSupported),
                       stamp) == UserLanguageWriteResult::Applied,
                   "P9b-3 U4b (b): the Supported fixture records");
        {
            const std::vector<UserLanguageManagerRow> rows = umd::FoldUserLanguageManagerRows(
                cfg.user_languages, LanguageEngine::OpenAi, kModel, nullptr, true);
            const std::wstring expected =
                std::wstring(umd::kBadgeSupportedPrefix) +
                I18n::Get(StringId::UserLangProbeSupported);
            TEST_CHECK(rows[0].badge_shown &&
                           rows[0].badge_id == StringId::UserLangProbeSupported &&
                           rows[0].badge_text == expected,
                       "P9b-3 U4b (b): Supported renders the check prefix + the landed "
                       "UserLangProbeSupported copy");
            TEST_CHECK(rows[0].list_text ==
                           rows[0].label + umd::kListBadgeSeparator + rows[0].badge_text,
                       "P9b-3 U4b (b): the list row carries the badge inline after the "
                       "stored label");
            const std::wstring evidence =
                ToUtf16(std::string(p9b3u4b_detail::kEvidenceSupported));
            TEST_CHECK(rows[0].evidence == evidence &&
                           rows[0].evidence_line ==
                               I18n::Get(StringId::UserLangProbeEvidenceLabel) +
                                   umd::kEvidenceSeparator + evidence,
                       "P9b-3 U4b (c): the evidence line is the cached shape-only string "
                       "VERBATIM under UserLangProbeEvidenceLabel");
            TEST_CHECK(rows[0].retest_enabled && !rows[0].clear_override_enabled &&
                           !rows[0].override_active,
                       "P9b-3 U4b (b): a cached verdict keeps 다시 시험 enabled and the "
                       "clear-override action inert");
        }

        // (b.3) Unsupported.
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor(
                       "Samplelandic", "openai", kModel,
                       p9b3u4b_detail::Eval(UserLanguageProbeStatus::Unsupported,
                                             p9b3u4b_detail::kEvidenceUnsupported),
                       stamp) == UserLanguageWriteResult::Applied,
                   "P9b-3 U4b (b): the Unsupported fixture records");
        {
            const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, false, true);
            TEST_CHECK(row.badge_shown &&
                           row.badge_id == StringId::UserLangProbeUnsupported &&
                           row.badge_text ==
                               std::wstring(umd::kBadgeUnsupportedPrefix) +
                                   I18n::Get(StringId::UserLangProbeUnsupported),
                       "P9b-3 U4b (b): Unsupported renders the cross prefix + the hedge "
                       "copy (UserLangProbeUnsupported)");
        }

        // (b.4) Checking - the RUNTIME state (U3 IsInFlight), which turns the
        // retest action off so a second check cannot stack on the first.
        {
            const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, true, true);
            TEST_CHECK(row.in_flight && row.badge_shown &&
                           row.badge_id == StringId::UserLangProbeChecking &&
                           row.badge_text == I18n::Get(StringId::UserLangProbeChecking),
                       "P9b-3 U4b (b): an in-flight key renders 확인 중 with no verdict "
                       "prefix");
            TEST_CHECK(!row.retest_enabled && row.override_supported_enabled &&
                           row.override_unsupported_enabled,
                       "P9b-3 U4b (b): while checking, 다시 시험 is disabled but the "
                       "final-authority answers stay available");
        }
        // (b.4b) Checking - a slot that READS Checking (only the load path maps a
        // stored "checking" to Unknown, so a defensive render must exist).
        {
            UserLanguage stored_checking = MakeUserLanguage("Samplelandic", "");
            UserLanguageProbe slot;
            slot.engine = "openai";
            slot.model = kModel;
            slot.status = UserLanguageProbeStatus::Checking;
            stored_checking.probes.push_back(slot);
            const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                stored_checking, LanguageEngine::OpenAi, kModel, false, true);
            TEST_CHECK(row.badge_shown &&
                           row.badge_id == StringId::UserLangProbeChecking &&
                           !row.in_flight && row.retest_enabled,
                       "P9b-3 U4b (b): a persisted Checking slot renders 확인 중 without "
                       "pretending a probe is running");
        }

        // (b.5) UserOverridden - the marker + the direction.
        TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor(
                       "Samplelandic", "openai", kModel,
                       UserLanguageProbeStatus::Unsupported, stamp) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U4b (b): the override fixture records through the locked verb");
        {
            const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, false, true);
            const std::wstring expected =
                std::wstring(umd::kBadgeUnsupportedPrefix) +
                I18n::Get(StringId::UserLangProbeUnsupported) +
                umd::kBadgeMarkerSeparator +
                I18n::Get(StringId::UserLangOverrideActive);
            TEST_CHECK(row.override_active && row.badge_shown &&
                           row.badge_text == expected &&
                           row.badge_text.find(I18n::Get(StringId::UserLangOverrideActive)) !=
                               std::wstring::npos,
                       "P9b-3 U4b (b): UserOverridden appends the landed "
                       "UserLangOverrideActive marker to the override direction");
            TEST_CHECK(row.clear_override_enabled,
                       "P9b-3 U4b (b): an active override enables the clear action");
            // The direction flip (Supported) must move the prefix + copy together.
            TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor(
                           "Samplelandic", "openai", kModel,
                           UserLanguageProbeStatus::Supported, stamp) ==
                           UserLanguageWriteResult::Applied,
                       "P9b-3 U4b (b): the direction flip records");
            const UserLanguageManagerRow flipped = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, false, true);
            TEST_CHECK(flipped.badge_text ==
                           std::wstring(umd::kBadgeSupportedPrefix) +
                               I18n::Get(StringId::UserLangProbeSupported) +
                               umd::kBadgeMarkerSeparator +
                               I18n::Get(StringId::UserLangOverrideActive),
                       "P9b-3 U4b (b): the Supported direction renders the check prefix "
                       "plus the marker");
            // PR9-06: a probe run never retires the user's answer.
            TEST_CHECK(cfg.RecordUserLanguageProbeResultFor(
                           "Samplelandic", "openai", kModel,
                           p9b3u4b_detail::Eval(UserLanguageProbeStatus::Unsupported,
                                                 p9b3u4b_detail::kEvidenceUnsupported),
                           stamp) == UserLanguageWriteResult::Applied,
                       "P9b-3 U4b (b): a re-record after the override applies");
            const UserLanguageManagerRow after = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, false, true);
            TEST_CHECK(after.override_active &&
                           after.badge_text.find(I18n::Get(
                               StringId::UserLangOverrideActive)) != std::wstring::npos,
                       "P9b-3 U4b (b): the marker survives a probe record (PR9-06: the "
                       "user is the final authority)");
        }

        // (b.6) The no-probe engine context: Google (publisher-declared), the
        // local engine and Auto (no probe row, UserLanguageEngineId "").
        {
            const LanguageEngine no_probe[] = {LanguageEngine::Google,
                                               LanguageEngine::HyMt2,
                                               LanguageEngine::Auto};
            bool all_empty = true;
            bool all_disabled = true;
            for (LanguageEngine engine : no_probe) {
                const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                    cfg.user_languages[0], engine, "", false, true);
                if (row.badge_shown || !row.badge_text.empty() || row.engine_judges) {
                    all_empty = false;
                }
                if (row.retest_enabled || row.override_supported_enabled ||
                    row.override_unsupported_enabled || row.clear_override_enabled) {
                    all_disabled = false;
                }
            }
            TEST_CHECK(all_empty,
                       "P9b-3 U4b (b): a no-probe engine context shows NO badge (the "
                       "publisher-declared state has no landed copy)");
            TEST_CHECK(all_disabled,
                       "P9b-3 U4b (b): every action stays disabled when the engine does "
                       "not judge by evidence");
        }

        // (b.7) The carrier gate: a preserved newer-schema block disables every
        // write action but keeps the cached badge readable (PR9-12).
        {
            const UserLanguageManagerRow row = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, kModel, false, false);
            TEST_CHECK(row.badge_shown && !row.retest_enabled &&
                           !row.override_supported_enabled &&
                           !row.override_unsupported_enabled &&
                           !row.clear_override_enabled,
                       "P9b-3 U4b (b): writes_allowed=false greys every action while the "
                       "verdict stays visible");
        }

        // (b.8) The model half: an explicit model addresses its own slot (D6
        // caches per engine + model), and the resolver falls back to the config.
        {
            const UserLanguageManagerRow other = umd::FoldUserLanguageManagerRow(
                cfg.user_languages[0], LanguageEngine::OpenAi, "other-model", false, true);
            TEST_CHECK(!other.badge_shown && other.evidence.empty(),
                       "P9b-3 U4b (b): a different model half reads a different cache slot "
                       "(no badge, no evidence)");
            AppConfig keys;
            keys.openai.model = "gpt-config";
            keys.user_model_id = "reg-id";
            TEST_CHECK(umd::ResolveUserLanguageManagerModel(LanguageEngine::OpenAi, keys,
                                                            L"") == "gpt-config" &&
                           umd::ResolveUserLanguageManagerModel(LanguageEngine::UserGguf,
                                                                keys, L"") == "reg-id" &&
                           umd::ResolveUserLanguageManagerModel(LanguageEngine::OpenAi, keys,
                                                                L"gpt-explicit") ==
                               "gpt-explicit" &&
                           umd::ResolveUserLanguageManagerModel(LanguageEngine::HyMt2, keys,
                                                                L"").empty(),
                       "P9b-3 U4b (b): an empty model argument falls back to the runner's "
                       "own key derivation (openai.model / user_model_id)");
        }
    }

    // ---- (d) the outcome fold ----------------------------------------------
    {
        using umd::FoldUserLanguageManagerOutcome;
        using umd::FoldUserLanguageManagerRetest;
        using umd::FoldUserLanguageManagerVerbResult;
        TEST_CHECK(FoldUserLanguageManagerVerbResult(UserLanguageWriteResult::Applied)
                           .changed &&
                       !FoldUserLanguageManagerVerbResult(UserLanguageWriteResult::Applied)
                            .refused,
                   "P9b-3 U4b (d): Applied is the only changing verb result");
        const UserLanguageWriteResult refusals[] = {
            UserLanguageWriteResult::RefusedForwardCompatCarrier,
            UserLanguageWriteResult::RejectedEmptyName,
            UserLanguageWriteResult::RejectedReservedName,
            UserLanguageWriteResult::RejectedDuplicateName,
            UserLanguageWriteResult::RejectedCapacity,
            UserLanguageWriteResult::RejectedNotFound,
            UserLanguageWriteResult::RejectedMissingProbeKey,
            UserLanguageWriteResult::RejectedInvalidStatus,
        };
        bool all_refused = true;
        for (UserLanguageWriteResult result : refusals) {
            const umd::UserLanguageManagerActionResult action =
                FoldUserLanguageManagerVerbResult(result);
            if (action.changed || !action.refused) {
                all_refused = false;
            }
        }
        TEST_CHECK(all_refused,
                   "P9b-3 U4b (d): every refusal maps to no change (the dialog stays "
                   "open, the caller gets no persist)");
        TEST_CHECK(!FoldUserLanguageManagerRetest(true).changed &&
                       !FoldUserLanguageManagerRetest(true).refused &&
                       !FoldUserLanguageManagerRetest(false).changed &&
                       FoldUserLanguageManagerRetest(false).refused,
                   "P9b-3 U4b (d): a retest launch is never kChanged (U3 records + "
                   "persists the verdict later)");
        TEST_CHECK(FoldUserLanguageManagerOutcome(true) ==
                           UserLanguageManagerOutcome::kChanged &&
                       FoldUserLanguageManagerOutcome(false) ==
                           UserLanguageManagerOutcome::kClosed,
                   "P9b-3 U4b (d): any applied verb -> kChanged; a cancelled dialog with "
                   "no applied verb -> kClosed");
    }

    // ---- (e) the unit boundary, read off the sources ------------------------
    {
        using p9b3u2_detail::ReadFileBytes;
        const std::filesystem::path cpp =
            ResolveRepoFile(L"src\\ui\\user_language_manager_dialog.cpp");
        const std::filesystem::path hpp =
            ResolveRepoFile(L"src\\ui\\user_language_manager_dialog.hpp");
        const std::string cpp_src = cpp.empty() ? std::string() : ReadFileBytes(cpp);
        const std::string hpp_src = hpp.empty() ? std::string() : ReadFileBytes(hpp);
        TEST_CHECK(!cpp_src.empty() && !hpp_src.empty(),
                   "P9b-3 U4b (e): both manager sources resolve for the scan");
        if (!cpp_src.empty() && !hpp_src.empty()) {
            using p9b3u4b_detail::CountOfStr;
            // The locked write surface, and only that.
            TEST_CHECK(eq(CountOfStr(cpp_src, "RecordUserLanguageProbeOverrideFor("), 1) &&
                           eq(CountOfStr(cpp_src,
                                         "ClearUserLanguageProbeOverrideFor("), 1),
                       "P9b-3 U4b (e): every override write goes through U1's LOCKED "
                       "verbs (one call site each: the direction is a parameter)");
            TEST_CHECK(cpp_src.find("RecordUserLanguageProbeOverride(") ==
                               std::string::npos &&
                           cpp_src.find("ClearUserLanguageProbeOverride(") ==
                               std::string::npos,
                       "P9b-3 U4b (e): the raw free verbs are never called (the PR9-12 "
                       "lock is the only write path)");
            TEST_CHECK(cpp_src.find("RecordUserLanguageProbeResultFor(") ==
                               std::string::npos,
                       "P9b-3 U4b (e): the dialog never records a verdict - that is U3's "
                       "single record site");
            // No registry mutation: reads only (GUI-thread read rule).
            TEST_CHECK(cpp_src.find("user_languages.push_back") == std::string::npos &&
                           cpp_src.find("user_languages.erase") == std::string::npos &&
                           cpp_src.find("user_languages.insert") == std::string::npos &&
                           cpp_src.find("user_languages.clear") == std::string::npos &&
                           cpp_src.find("user_languages =") == std::string::npos,
                       "P9b-3 U4b (e): the registry vector is read, never mutated "
                       "directly");
            TEST_CHECK(eq(CountOfStr(cpp_src, "RequestRetest("), 1) &&
                           cpp_src.find("IsInFlight(") != std::string::npos &&
                           cpp_src.find("DrainOutcomes(") != std::string::npos,
                       "P9b-3 U4b (e): 다시 시험 goes through U3's RequestRetest, and the "
                       "refresh reads the runner's in-flight set + drain");
            TEST_CHECK(cpp_src.find("SetRecordedCallback") == std::string::npos,
                       "P9b-3 U4b (e): the dialog never claims the runner's single-slot "
                       "recorded callback (the caller's persist callback owns it)");
            TEST_CHECK(cpp_src.find("SaveToFile") == std::string::npos,
                       "P9b-3 U4b (e): the dialog performs no file I/O (kChanged is the "
                       "caller's persist cue)");
            TEST_CHECK(cpp_src.find("UserLanguageStoredName(") != std::string::npos &&
                           cpp_src.find("DisplayUserLanguageProbeStatus(") !=
                               std::string::npos &&
                           cpp_src.find("UserLanguageProbeEngineJudgedByEvidence(") !=
                               std::string::npos,
                       "P9b-3 U4b (e): the row uses the stored name, the landed display "
                       "fold and U3's evidence-engine predicate (no re-implementation)");
            TEST_CHECK(cpp_src.find("#include \"ui/tray.hpp\"") == std::string::npos &&
                           cpp_src.find("#include \"ui/tooltip.hpp\"") == std::string::npos,
                       "P9b-3 U4b (e): no tray/tooltip surface is touched (the wiring "
                       "unit owns those)");
            // No new StringIds: every id referenced anywhere in the module belongs to
            // the landed P9b-1 / generic set.
            const std::string landed[] = {
                "UserLangsGroup",
                "UserLangProbeChecking",
                "UserLangProbeSupported",
                "UserLangProbeUnsupported",
                "UserLangProbeEvidenceLabel",
                "UserLangProbeRetest",
                "UserLangOverrideSupported",
                "UserLangOverrideUnsupported",
                "UserLangOverrideActive",
                "UserLangSupportNotQuality",
                "GgufManagerClose",
            };
            std::size_t foreign_ids = 0;
            const std::string combined = cpp_src + hpp_src;
            for (std::size_t pos = combined.find("StringId::"); pos != std::string::npos;
                 pos = combined.find("StringId::", pos + 10)) {
                std::string id;
                for (std::size_t i = pos + 10;
                     i < combined.size() && p9b3u4b_detail::IsIdentifierChar(combined[i]);
                     ++i) {
                    id.push_back(combined[i]);
                }
                bool known = false;
                for (const std::string& allowed : landed) {
                    if (id == allowed) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    ++foreign_ids;
                }
            }
            TEST_CHECK(eq(foreign_ids, static_cast<std::size_t>(0)) &&
                           gt(CountOfStr(combined, "StringId::"),
                              static_cast<std::size_t>(0)),
                       "P9b-3 U4b (e): the module references only landed P9b-1 / generic "
                       "StringIds (no new copy was invented)");
            TEST_CHECK(hpp_src.find("ShowUserLanguageManagerDialogEx(") !=
                               std::string::npos &&
                           hpp_src.find("UserLanguageManagerOutcome") !=
                               std::string::npos,
                       "P9b-3 U4b (e): the header exposes the mandated entry point + "
                       "outcome enum");
        }
    }

    // ---- (f) the fold consumed through the REAL runner ----------------------
    {
        AppConfig rt;
        TEST_CHECK(rt.AddUserLanguage(MakeUserLanguage("Retestic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 U4b (f): the runner fixture registers");
        rt.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService service(rt, nullptr);
        std::atomic<bool> release{false};
        std::atomic<int> calls{0};
        service.SetTransportForTest(
            [&release, &calls](std::string_view, std::string_view, std::string_view,
                               std::string_view) -> std::wstring {
                ++calls;
                while (!release.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return L"Elküldöm a jelentést.";
            });
        TEST_CHECK(service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Retestic"),
                   "P9b-3 U4b (f): the first-use probe launches");
        for (int i = 0; i < 5000 && calls.load(std::memory_order_relaxed) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            const std::vector<UserLanguageManagerRow> rows = umd::FoldUserLanguageManagerRows(
                rt.user_languages, LanguageEngine::OpenAi, "gpt-test", &service, true);
            TEST_CHECK(rows.size() == 1 && rows[0].in_flight && rows[0].badge_shown &&
                           rows[0].badge_id == StringId::UserLangProbeChecking &&
                           !rows[0].retest_enabled,
                       "P9b-3 U4b (f): the rows fold reads U3's in-flight set and renders "
                       "확인 중 with 다시 시험 disabled");
        }
        release.store(true, std::memory_order_relaxed);
        TEST_CHECK(eq(p9b3u3_detail::WaitDrain(service, 1), static_cast<std::size_t>(1)),
                   "P9b-3 U4b (f): the verdict drains through U3's marshal");
        {
            const std::vector<UserLanguageManagerRow> rows = umd::FoldUserLanguageManagerRows(
                rt.user_languages, LanguageEngine::OpenAi, "gpt-test", &service, true);
            TEST_CHECK(rows.size() == 1 && !rows[0].in_flight && rows[0].badge_shown &&
                           rows[0].badge_id == StringId::UserLangProbeSupported &&
                           rows[0].retest_enabled &&
                           !rows[0].evidence.empty() &&
                           rows[0].evidence_line.find(rows[0].evidence) !=
                               std::wstring::npos,
                       "P9b-3 U4b (f): after the drain the row shows the cached verdict + "
                       "its evidence and 다시 시험 is available again");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-3 U4b user-language manager dialog passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-3 U4b user-language manager dialog: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}


// ===========================================================================
// 261116_0003 P9b-3 U4a (language-menu redesign, plan §5 P9): the probe BADGE
// on the "내 언어" menu rows + the two tray dialog rows.
//
// Pinned here, headlessly:
//   (a) the ONE shared badge fold (UserLanguageMenuBadgeSuffix, tray.hpp) for
//       all five display states + the override marker + the no-probe context
//       + the runtime in-flight flag, composed from the manager dialog's OWN
//       public landmarks (no drift between the menu rows and the manager
//       list), and the label composition on top of FormatUserLanguageMenuLabel
//       (the &-escape stays the single label form; the badge carries no '&').
//   (b) the six-surface parity source-scan: all six menu surfaces (the tray's
//       four submenus through the one build_language_menu + the tooltip's two
//       dropdowns) compose label + fold adjacently — a bare-label bypass on
//       any surface trips the count (the load-bearing check).
//   (c) the two tray rows (2865/2866): ids, labels, dispatch arms, the
//       deferred-open band (WM_APP + 0x504/0x505), the persist-on-outcome
//       lambdas and the resolver key wiring in main.cpp.
//   (d) the band asserts after the extension (both directions pinned).
//
// No window is created (AGENTS.md §10); the rendered badge glyphs need one
// manual-QA pass, same split as U2/U4b.
// ===========================================================================
void TestP9b3UserLanguageBadgeRows() {
    std::cout << "[RUN] Testing 261116_0003 P9b-3 U4a user-language badge rows + tray wiring..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace uld = emebalachat::user_language_manager_dialog;

    // Constants compared at RUNTIME go through these helpers so TEST_CHECK's
    // `if` is never a constant condition (MSVC C4127 discipline, same as the
    // P9b-2/P9b-3 suites above).
    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
    const auto lt = [](std::size_t lhs, std::size_t rhs) { return lhs < rhs; };

    // ---- (a) the ONE shared badge fold ------------------------------------
    {
        AppConfig cfg;
        // The production key halves: the runner writes its slots under
        // UserLanguageEngineId(engine) + UserLanguageProbeModelFor(engine, cfg).
        cfg.openai.model = "gpt-badge";
        const std::string engine_id = UserLanguageEngineId(LanguageEngine::OpenAi);
        const std::string model = UserLanguageProbeModelFor(LanguageEngine::OpenAi, cfg);
        TEST_CHECK(engine_id == "openai" && model == "gpt-badge",
                   "P9b-3 U4a (a): the fixture key is the production (engine id, model) pair");

        TEST_CHECK(cfg.AddUserLanguage(MakeUserLanguage("Badgeok", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("Badgebad", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("BadgeoverU", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("BadgeoverS", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("Badgecheckslot", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("Badgefree", "")) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.AddUserLanguage(MakeUserLanguage("AT & T", "")) ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3 U4a (a): the badge fixture entries register");

        const ProbeEvaluation ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Habari ya asubuhi", kScriptMaskLatin);
        const ProbeEvaluation bad = EvaluateProbeResult(kUserLanguageProbeSentence, "",
                                                        kScriptMaskLatin);
        TEST_CHECK(ok.status == UserLanguageProbeStatus::Supported &&
                       bad.status == UserLanguageProbeStatus::Unsupported,
                   "P9b-3 U4a (a): the evaluator fixture sanity (Supported / Unsupported)");

        const std::string stamp = "2026-11-18T02:00:00Z";
        TEST_CHECK(cfg.RecordUserLanguageProbeResultFor("Badgeok", engine_id, model, ok,
                                                        stamp) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.RecordUserLanguageProbeResultFor("Badgebad", engine_id, model, bad,
                                                          stamp) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.RecordUserLanguageProbeResultFor("AT & T", engine_id, model, ok,
                                                          stamp) ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3 U4a (a): verdicts record under the production key");
        TEST_CHECK(cfg.RecordUserLanguageProbeOverrideFor("BadgeoverU", engine_id, model,
                                                          UserLanguageProbeStatus::Unsupported,
                                                          stamp) ==
                           UserLanguageWriteResult::Applied &&
                       cfg.RecordUserLanguageProbeOverrideFor("BadgeoverS", engine_id, model,
                                                            UserLanguageProbeStatus::Supported,
                                                            stamp) ==
                           UserLanguageWriteResult::Applied,
                   "P9b-3 U4a (a): both override directions record");

        const UserLanguage* ok_entry = FindUserLanguage(cfg.user_languages, "Badgeok");
        const UserLanguage* bad_entry = FindUserLanguage(cfg.user_languages, "Badgebad");
        const UserLanguage* over_u = FindUserLanguage(cfg.user_languages, "BadgeoverU");
        const UserLanguage* over_s = FindUserLanguage(cfg.user_languages, "BadgeoverS");
        const UserLanguage* chk_entry = FindUserLanguage(cfg.user_languages, "Badgecheckslot");
        const UserLanguage* free_entry = FindUserLanguage(cfg.user_languages, "Badgefree");
        const UserLanguage* amp_entry = FindUserLanguage(cfg.user_languages, "AT & T");
        TEST_CHECK(ok_entry && bad_entry && over_u && over_s && chk_entry && free_entry &&
                       amp_entry,
                   "P9b-3 U4a (a): every fixture entry resolves");

        // The record seam coerces every verdict to Supported/Unsupported
        // (config.cpp RecordUserLanguageProbeResult), so a persisted slot can
        // only READ Checking when it bypassed both writers — fabricate the
        // in-memory slot directly (UserLanguage::probes is public) to exercise
        // the fold's defensive arm, the same shape U4b's (b.4) pins on the
        // manager fold.
        UserLanguage* chk_mutable = nullptr;
        for (UserLanguage& e : cfg.user_languages) {
            if (UserLanguageStoredName(e) == "Badgecheckslot") {
                chk_mutable = &e;
                break;
            }
        }
        TEST_CHECK(chk_mutable != nullptr,
                   "P9b-3 U4a (a): the Checking-slot fixture entry is found for the "
                   "direct-slot fabrication");
        if (chk_mutable != nullptr) {
            UserLanguageProbe slot;
            slot.engine = engine_id;
            slot.model = model;
            slot.status = UserLanguageProbeStatus::Checking;
            chk_mutable->probes.push_back(std::move(slot));
        }

        // The expected strings compose from the PRODUCTION pieces (the manager
        // dialog's public landmarks + the live locale strings), never copies.
        const std::wstring sep = uld::kListBadgeSeparator; // " - "
        const std::wstring supported = std::wstring(uld::kBadgeSupportedPrefix) +
                                       I18n::Get(StringId::UserLangProbeSupported);
        const std::wstring unsupported = std::wstring(uld::kBadgeUnsupportedPrefix) +
                                         I18n::Get(StringId::UserLangProbeUnsupported);
        const std::wstring checking = I18n::Get(StringId::UserLangProbeChecking);
        const std::wstring marker = I18n::Get(StringId::UserLangOverrideActive);

        TEST_CHECK(UserLanguageMenuBadgeSuffix(*ok_entry, engine_id, model, false) ==
                       sep + supported,
                   "P9b-3 U4a (a): Supported renders the separator + the check prefix + "
                   "UserLangProbeSupported");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*bad_entry, engine_id, model, false) ==
                       sep + unsupported,
                   "P9b-3 U4a (a): Unsupported renders the separator + the cross prefix + "
                   "UserLangProbeUnsupported");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*ok_entry, engine_id, model, true) ==
                       sep + checking,
                   "P9b-3 U4a (a): in-flight renders Checking WITHOUT a prefix and BEATS a "
                   "cached verdict (runtime-only state)");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*over_u, engine_id, model, true) ==
                       sep + checking,
                   "P9b-3 U4a (a): in-flight beats an active override too (the U4b fold order)");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*over_u, engine_id, model, false) ==
                       sep + unsupported + uld::kBadgeMarkerSeparator + marker,
                   "P9b-3 U4a (a): the unsupported override renders the direction + the "
                   "UserLangOverrideActive marker");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*over_s, engine_id, model, false) ==
                       sep + supported + uld::kBadgeMarkerSeparator + marker,
                   "P9b-3 U4a (a): the supported override renders the check direction + the "
                   "marker");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*chk_entry, engine_id, model, false) ==
                       sep + checking,
                   "P9b-3 U4a (a): a persisted slot that reads Checking renders like the "
                   "runtime state (defensive arm, as the manager fold)");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*free_entry, engine_id, model, false).empty(),
                   "P9b-3 U4a (a): Unknown (no cache record) draws NO badge");
        TEST_CHECK(UserLanguageMenuBadgeSuffix(*ok_entry, "", "", false).empty() &&
                       UserLanguageMenuBadgeSuffix(*ok_entry, "", "", true).empty() &&
                       UserLanguageMenuBadgeSuffix(*over_u, "", "", false).empty(),
                   "P9b-3 U4a (a): the no-probe engine context (empty engine id) renders NO "
                   "suffix — verdicts, overrides and in-flight are all gated");
        {
            // The label composition: FormatUserLanguageMenuLabel stays the SINGLE
            // label form (the &-escape lives only there) and the fold composes on
            // top; the badge text itself carries no '&'.
            const std::wstring suffix = UserLanguageMenuBadgeSuffix(*amp_entry, engine_id,
                                                                    model, false);
            const std::wstring full = ToUtf16(FormatUserLanguageMenuLabel(*amp_entry)) + suffix;
            TEST_CHECK(full.find(L"AT && T") == 0 && full.find(suffix) != std::wstring::npos &&
                           suffix.find(L'&') == std::wstring::npos,
                       "P9b-3 U4a (a): the badge appends AFTER the &-escaped label and the "
                       "badge text carries no '&'");
        }
    }

    // ---- (b)(c)(d) structural pins (the menus are Win32/GUI-only) ----------
    {
        const auto read_src = [](const wchar_t* rel, std::string& out) {
            const std::filesystem::path p = ResolveRepoFile(rel);
            if (p.empty()) {
                return false;
            }
            std::ifstream in(p, std::ios::binary);
            if (!in) {
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };
        const auto squeeze = [](const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (const char ch : text) {
                if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                    out.push_back(ch);
                }
            }
            return out;
        };
        const auto count_of = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };

        std::string tray_src, tray_hdr, tooltip_src, tooltip_hdr, main_src;
        const bool readable = read_src(L"src\\ui\\tray.cpp", tray_src) &&
                              read_src(L"src\\ui\\tray.hpp", tray_hdr) &&
                              read_src(L"src\\ui\\tooltip.cpp", tooltip_src) &&
                              read_src(L"src\\ui\\tooltip.hpp", tooltip_hdr) &&
                              read_src(L"src\\main.cpp", main_src);
        // R14-08 discipline: an unresolvable source is a FAILURE, never a
        // silent skip that still prints suite PASS.
        TEST_CHECK(readable,
                   "P9b-3 U4a: the structural-pin sources resolve via the anchored "
                   "ResolveRepoFile (an unresolvable source fails loudly, never skips)");
        if (!readable) {
            if (g_failed_count == failures_before) {
                std::cout << "[PASS] 261116_0003 P9b-3 U4a user-language badge rows + tray "
                             "wiring passed."
                          << std::endl;
            } else {
                std::cout << "[FAIL] 261116_0003 P9b-3 U4a user-language badge rows + tray "
                             "wiring: "
                          << (g_failed_count - failures_before) << " check(s) failed."
                          << std::endl;
            }
            return;
        }
        const std::string tray_sq = squeeze(tray_src);
        const std::string tooltip_sq = squeeze(tooltip_src);
        const std::string main_sq = squeeze(main_src);

        // ONE fold: declared once in tray.hpp, defined once in tray.cpp.
        TEST_CHECK(count_of(tray_hdr, "UserLanguageMenuBadgeSuffix(const UserLanguage& language") ==
                           1 &&
                       count_of(tray_src,
                                "std::wstring UserLanguageMenuBadgeSuffix(const UserLanguage& "
                                "language") == 1,
                   "P9b-3 U4a (b): the badge fold has exactly ONE declaration (tray.hpp) and "
                   "ONE definition (tray.cpp)");
        TEST_CHECK(tray_src.find("namespace uld = user_language_manager_dialog;") !=
                           std::string::npos &&
                       tray_src.find("uld::kBadgeSupportedPrefix") != std::string::npos &&
                       tray_src.find("uld::kBadgeMarkerSeparator") != std::string::npos &&
                       tray_src.find("uld::kListBadgeSeparator") != std::string::npos,
                   "P9b-3 U4a (b): the fold composes from the manager dialog's OWN public "
                   "landmarks (the menu rows and the manager list cannot drift)");

        // Six-surface parity: every user-row rendering composes the &-escaped
        // label and the fold ADJACENTLY. The tray's four submenus share the one
        // build_language_menu (1 call site); the tooltip's two dropdowns carry
        // one call site each (2). A bare-label bypass on ANY surface breaks the
        // adjacency count while keeping the label count — the load-bearing pin.
        TEST_CHECK(count_of(tray_sq, "FormatUserLanguageMenuLabel(*entry)") == 1 &&
                       count_of(tray_sq,
                                "FormatUserLanguageMenuLabel(*entry))+UserLanguageMenuBadgeSuf"
                                "fix(*entry,view.user_lang_probe_engine,") == 1,
                   "P9b-3 U4a (b): the tray's FOUR submenus render their user rows through "
                   "the shared fold, keyed by the resolver view (one builder, one call)");
        TEST_CHECK(count_of(tooltip_sq, "FormatUserLanguageMenuLabel(*entry)") == 2 &&
                       count_of(tooltip_sq,
                                "FormatUserLanguageMenuLabel(*entry))+UserLanguageMenuBadgeSuf"
                                "fix(*entry,menu_engine.user_lang_probe_engine,") == 2,
                   "P9b-3 U4a (b): BOTH tooltip dropdowns render their user rows through the "
                   "shared fold, keyed by the dropdown's own resolver view");
        TEST_CHECK(count_of(tooltip_src, "UserLanguageMenuBadgeSuffix(") == 2 &&
                       count_of(main_src, "UserLanguageMenuBadgeSuffix(") == 0,
                   "P9b-3 U4a (b): the tooltip holds the two CALL sites only (no fold "
                   "definition) and main.cpp never re-implements the badge");
        TEST_CHECK(tooltip_src.find("UserLangProbeChecking") == std::string::npos &&
                       tooltip_src.find("UserLangOverrideActive") == std::string::npos,
                   "P9b-3 U4a (b): no second badge path — the badge copy is referenced only "
                   "inside the ONE fold (tooltip.cpp names none of it)");
        TEST_CHECK(count_of(tray_sq, "FormatUserLanguageMenuLabel(*user_ghost)") == 1 &&
                       count_of(tooltip_sq, "FormatUserLanguageMenuLabel(*user_ghost)") == 2,
                   "P9b-3 U4a (b): the pinned-absent user ghost keeps the bare label form "
                   "everywhere (display-only badge, no ghost change)");

        // (c) the two tray rows: ids, labels, dispatch, deferred-open band,
        // persist-on-outcome, resolver wiring.
        TEST_CHECK(tray_src.find("ID_TRAY_USER_LANG_ADD = kTrayUserLangBandEnd") !=
                           std::string::npos &&
                       tray_src.find("ID_TRAY_USER_LANG_MANAGE = kTrayUserLangBandEnd + 1") !=
                           std::string::npos,
                   "P9b-3 U4a (c): the two rows take 2865 (= kTrayUserLangBandEnd) and 2866 "
                   "(band end + 1) — the first free ids after the user band");
        TEST_CHECK(count_of(tray_src, "ID_TRAY_USER_LANG_ADD,") == 1 &&
                       count_of(tray_src, "ID_TRAY_USER_LANG_MANAGE,") == 1,
                   "P9b-3 U4a (c): each row is appended to the main menu exactly once");
        TEST_CHECK(tray_src.find("I18n::Get(StringId::UserLangAddTitle) + L\"\\u2026\"") !=
                           std::string::npos &&
                       tray_src.find("I18n::Get(StringId::UserLangsGroup) + L\"\\u2026\"") !=
                           std::string::npos,
                   "P9b-3 U4a (c): the add row reuses the landed UserLangAddTitle and the "
                   "manage row reuses the group's own name (no new StringIds; the ellipsis "
                   "marks the dialog-opening row)");
        TEST_CHECK(tray_src.find("cmd == ID_TRAY_USER_LANG_ADD") != std::string::npos &&
                       tray_src.find("cmd == ID_TRAY_USER_LANG_MANAGE") != std::string::npos,
                   "P9b-3 U4a (c): both rows have dispatch arms");
        TEST_CHECK(count_of(tray_sq, "::PostMessageW(hOwner_,WM_APP+0x504,0,0)") == 1 &&
                       count_of(tray_sq, "::PostMessageW(hOwner_,WM_APP+0x505,0,0)") == 1,
                   "P9b-3 U4a (c): both dispatch arms DEFER the modal open to the controller "
                   "(the kMsgOpenGgufManager REQ-047 D3 contract)");
        TEST_CHECK(count_of(main_sq, "kMsgOpenUserLangAdd=WM_APP+0x504;") == 1 &&
                       count_of(main_sq, "kMsgOpenUserLangManager=WM_APP+0x505;") == 1 &&
                       main_src.find("if (msg == kMsgOpenUserLangAdd)") != std::string::npos &&
                       main_src.find("if (msg == kMsgOpenUserLangManager)") !=
                           std::string::npos,
                   "P9b-3 U4a (c): the 0x504/0x505 wake-up band is unique and both "
                   "ControllerWndProc arms exist (0x503 is the palette, 0x600 the probe)");
        {
            // The persist-on-outcome lambdas: the dialogs perform NO file I/O, so
            // the caller persists on kAdded / kChanged and refreshes the tray.
            const std::size_t add_pos = main_src.find(
                "g_open_user_lang_add = [&config, &refresh_tray]()");
            const std::size_t add_end =
                add_pos == std::string::npos ? std::string::npos : main_src.find("};", add_pos);
            const std::string add_body =
                (add_pos == std::string::npos || add_end == std::string::npos)
                    ? std::string()
                    : main_src.substr(add_pos, add_end - add_pos);
            TEST_CHECK(!add_body.empty() &&
                           add_body.find("ShowUserLanguageDialogEx") != std::string::npos &&
                           add_body.find("UserLanguageDialogOutcome::kAdded") !=
                               std::string::npos &&
                           add_body.find("SaveToFile") != std::string::npos &&
                           add_body.find("refresh_tray") != std::string::npos,
                       "P9b-3 U4a (c): the add lambda persists through SaveToFile and "
                       "refreshes the tray on kAdded only (kCancelled does nothing)");
            const std::size_t man_pos = main_src.find(
                "g_open_user_lang_manager = [&config, &probe_service, &refresh_tray]()");
            const std::size_t man_end =
                man_pos == std::string::npos ? std::string::npos : main_src.find("};", man_pos);
            const std::string man_body =
                (man_pos == std::string::npos || man_end == std::string::npos)
                    ? std::string()
                    : main_src.substr(man_pos, man_end - man_pos);
            TEST_CHECK(!man_body.empty() &&
                           man_body.find("ShowUserLanguageManagerDialogEx") !=
                               std::string::npos &&
                           man_body.find("TrayLanguageEngineFor") != std::string::npos &&
                           man_body.find("UserLanguageProbeModelFor") != std::string::npos &&
                           man_body.find("&probe_service") != std::string::npos &&
                           man_body.find("UserLanguageManagerOutcome::kChanged") !=
                               std::string::npos &&
                           man_body.find("SaveToFile") != std::string::npos &&
                           man_body.find("refresh_tray") != std::string::npos,
                       "P9b-3 U4a (c): the manager lambda passes the SAME engine + model "
                       "resolution the badge rows read (one verdict everywhere) and the "
                       "runner for retest; it persists on kChanged only");
        }
        TEST_CHECK(main_src.find("g_open_user_lang_add = nullptr;") != std::string::npos &&
                       main_src.find("g_open_user_lang_manager = nullptr;") !=
                           std::string::npos,
                   "P9b-3 U4a (c): both coordinators retire at the shutdown sweep (a late "
                   "wake-up can never call into destroyed state)");

        // (d) the band asserts after the extension, both directions.
        TEST_CHECK(eq(kTrayUserLangBandEnd, 2865) && lt(kTrayUserLangBandEnd + 1, 2900) &&
                       eq(kTooltipCmdUserSrcBase, 2900),
                   "P9b-3 U4a (d): 2865/2866 sit between the user band end and the tooltip "
                   "band start at 2900");
        TEST_CHECK(tray_src.find("ID_TRAY_USER_LANG_ADD == kTrayUserLangBandEnd") !=
                           std::string::npos &&
                       tray_src.find("ID_TRAY_USER_LANG_MANAGE < 2900") != std::string::npos,
                   "P9b-3 U4a (d): the tray assert block re-pins the rows' contiguity with "
                   "the band end and the headroom to the tooltip band");
        TEST_CHECK(tooltip_src.find("kTooltipCmdUserSrcBase > kTrayUserLangBandEnd + 1") !=
                       std::string::npos,
                   "P9b-3 U4a (d): the tooltip assert block re-pins its user band ABOVE the "
                   "two new tray rows");

        // The resolver key fields ride the derive-at-open seam on BOTH surfaces.
        TEST_CHECK(tray_hdr.find("std::string user_lang_probe_engine;") != std::string::npos &&
                       tray_hdr.find("std::function<bool(const UserLanguage&)> "
                                     "user_lang_probe_in_flight;") != std::string::npos &&
                       tooltip_hdr.find("std::string user_lang_probe_engine;") !=
                           std::string::npos &&
                       tooltip_hdr.find("std::function<bool(const UserLanguage&)> "
                                        "user_lang_probe_in_flight;") != std::string::npos,
                   "P9b-3 U4a (d): both resolver views carry the badge key + the in-flight "
                   "seam (empty = the conservative unwired default)");
        TEST_CHECK(main_src.find("view.user_lang_probe_engine =") != std::string::npos &&
                       main_src.find("view.user_lang_probe_in_flight =") !=
                           std::string::npos &&
                       main_src.find("resolution.user_lang_probe_engine =") !=
                           std::string::npos &&
                       main_src.find("resolution.user_lang_probe_in_flight =") !=
                           std::string::npos,
                   "P9b-3 U4a (d): main.cpp populates BOTH resolver lambdas from the same "
                   "persisted snapshot (derive-at-open, FALSE/unwired-safe defaults)");

        // 261116_0003 P9b-3 F-1 (R15-12, orchestrator MICRO): the tray_update DIAG
        // line's pair fields can BE user-typed stored names, so the line sits
        // behind diag::ContentLoggingEnabled() with the shape-only len= fallback
        // (the R14-06 idiom, same gate as menu_select_user and lang_sync). The
        // fallback literal exists ONLY in the gated else branch: un-gating the
        // line deletes it and the count drops to one.
        TEST_CHECK(tray_src.find("tray_update active=%d engine=%s src_len=%zu") !=
                           std::string::npos &&
                       count_of(tray_src, "tray_update active=%d engine=%s") == 2,
                   "P9b-3 F-1 (R15-12): the tray_update DIAG pair fields are gated on "
                   "diag::ContentLoggingEnabled() with the shape-only len= fallback (both "
                   "branches carry the tag; un-gating deletes the fallback)");
        // fix-list-round15 R15-22 (unit F-3b): the branch-structure pin above
        // survives a gate bypass — rewriting the condition to `if (true)`
        // keeps BOTH literals (the pair record + the len= fallback) and the
        // tag count. Mirror the lang_sync (f) pin's gate-condition scan: the
        // content line must sit inside a real diag::ContentLoggingEnabled()
        // gate (the same R14-06 window as tray.cpp's own comment block).
        const std::size_t tray_upd_pos =
            tray_src.find("tray_update active=%d engine=%s src=%s tgt=%s drag_src=%s "
                          "drag_tgt=%s auto_send=%d icon_changed=%d");
        TEST_CHECK(tray_upd_pos != std::string::npos,
                   "P9b-3 F-3b R15-22: the tray_update pair record survives for the "
                   "gate-condition scan");
        const std::size_t tray_upd_window = tray_upd_pos > 1200 ? tray_upd_pos - 1200 : 0;
        const std::string tray_upd_before =
            tray_src.substr(tray_upd_window, tray_upd_pos - tray_upd_window);
        TEST_CHECK(tray_upd_before.find("if (diag::ContentLoggingEnabled())") !=
                       std::string::npos,
                   "P9b-3 F-3b R15-22: the tray_update content line sits inside a real "
                   "diag::ContentLoggingEnabled() gate — a condition bypass (`if (true)`) "
                   "removes the gate string and fails this pin even though both branch "
                   "literals survive");

        // ---- fix-list-round15 R15-23 (unit F-3b): the main.cpp DIAG
        // content-gate family -------------------------------------------
        // (a) the LangSync/001 individual gate. The R15-12 round gated the
        // lang_sync DIAG_LOG below it but left this DIAG_F printing the RAW
        // refused src/tgt (user-typed planner tokens) on the unconditional
        // stderr channel. Pinned like the (f) lang_sync pin: inside a real
        // ContentLoggingEnabled gate, with the shape-only src_len/tgt_len
        // fallback; ctx rides both branches (the site tag appears twice).
        const std::size_t ls1_pos = main_src.find(
            "MAIN/LangSync/001: refused unresolvable language request (ctx=%s, src='%s', "
            "tgt='%s')");
        TEST_CHECK(ls1_pos != std::string::npos,
                   "P9b-3 F-3b R15-23 (a): the LangSync/001 pair record survives (now "
                   "inside the gate)");
        const std::size_t ls1_window = ls1_pos > 1200 ? ls1_pos - 1200 : 0;
        const std::string ls1_before =
            ls1_pos == std::string::npos
                ? std::string()
                : main_src.substr(ls1_window, ls1_pos - ls1_window);
        TEST_CHECK(ls1_pos != std::string::npos &&
                       ls1_before.find("if (diag::ContentLoggingEnabled())") !=
                           std::string::npos,
                   "P9b-3 F-3b R15-23 (a): LangSync/001 sits inside the "
                   "diag::ContentLoggingEnabled() gate (un-gating deletes the gate "
                   "string and fails this pin)");
        TEST_CHECK(main_src.find("src_len=%zu, tgt_len=%zu") != std::string::npos,
                   "P9b-3 F-3b R15-23 (a): the else branch keeps the shape-only "
                   "src_len/tgt_len fallback");
        TEST_CHECK(count_of(main_src,
                            "MAIN/LangSync/001: refused unresolvable language request") == 2,
                   "P9b-3 F-3b R15-23 (a): ctx rides BOTH branches (the site tag appears "
                   "on the gated line + the shape-only fallback)");

        // (b) the family invariant, three layers:
        //   1. enumerated set — every site from the F-3b sweep: the content
        //      tag exists, a real gate precedes it (1200-char lookback, the
        //      lang_sync pin's window), and its shape-only _len= fallback
        //      exists.
        //   2. generic token sweep — EVERY DIAG_LOG/DIAG_F call in main.cpp
        //      whose format carries a pair/src/tgt/lang/pick/model_path
        //      content token must follow a ContentLoggingEnabled gate; a
        //      future family addition without a gate fails here immediately.
        //   3. gate-count ratchet — 21 = the pre-existing lang_sync gate +
        //      the 20 enumerated F-3b sites, so any future gated addition
        //      forces an explicit table update (the enumeration stays
        //      honest, the count cannot drift silently).
        const std::pair<const char*, const char*> kGatedDiagSites[] = {
            {"MAIN/LangSync/001: refused unresolvable language request (ctx=%s, src='%s', "
             "tgt='%s')",
             "src_len=%zu, tgt_len=%zu"},
            {"config loaded=%d ui_language=%s engine=%s model_path=%s", "model_path_len=%zu"},
            {"back_translate_skip status=%d fwd_len=%zu back_tgt=%s", "back_tgt_len=%zu"},
            {"back_translate_done fwd_len=%zu back_len=%zu orig_len=%zu matched=%d back_tgt=%s",
             "back_tgt_len=%zu"},
            {"back_translate_enqueue fwd_len=%zu orig_len=%zu back_tgt=%s fwd_tgt=%s",
             "fwd_tgt_len=%zu"},
            {"drag_target_pinned tgt=%s (F2 user-explicit pick)",
             "drag_target_pinned tgt_len=%zu (F2 user-explicit pick)"},
            {"cycle_confirm tgt=%s ctx=%s", "cycle_confirm tgt_len=%zu ctx=%s"},
            {"lang_reset ctx=both pair %s/%s %s/%s ui=auto",
             "lang_reset ctx=both pair_len=%zu/%zu %zu/%zu ui=auto"},
            {"drag_swap_apply %s/%s -> %s/%s", "drag_swap_apply len=%zu/%zu -> %zu/%zu"},
            {"pivot_temporary ctx=drag path=icon persisted_tgt=%s pivot_tgt=%s persist=0",
             "persisted_tgt_len=%zu pivot_tgt_len=%zu persist=0"},
            {"drag_src path=icon mode=%s persisted=%s detected=%s eff=%s pivot=%d tgt=%s "
             "usr_explicit=%d",
             "persisted_len=%zu detected_len=%zu eff_len=%zu"},
            {"drag_bypass path=icon detected=%s tgt=%s",
             "drag_bypass path=icon detected_len=%zu tgt_len=%zu"},
            {"retranslate_worker_run gen=%llu src_len=%zu tgt=%s",
             "retranslate_worker_run gen=%llu src_len=%zu tgt_len=%zu"},
            {"retranslate_src gen=%llu mode=%s persisted=%s carried=%s eff=%s pivot=%d tgt=%s "
             "usr_explicit=%d",
             "persisted_len=%zu carried_len=%zu eff_len=%zu"},
            {"pivot_temporary ctx=drag path=dbl_ctrl_c persisted_tgt=%s pivot_tgt=%s persist=0",
             "persisted_tgt_len=%zu pivot_tgt_len=%zu persist=0"},
            {"drag_src path=dbl_ctrl_c mode=%s persisted=%s detected=%s eff=%s pivot=%d "
             "tgt=%s usr_explicit=%d",
             "persisted_len=%zu detected_len=%zu eff_len=%zu"},
            {"drag_bypass path=dbl_ctrl_c detected=%s tgt=%s",
             "drag_bypass path=dbl_ctrl_c detected_len=%zu tgt_len=%zu"},
            {"retranslate_enqueued gen=%llu tgt=%s",
             "retranslate_enqueued gen=%llu tgt_len=%zu"},
            {"tooltip_src lang=%s applied=%d gen=%llu",
             "tooltip_src lang_len=%zu applied=%d gen=%llu"},
            {"retranslate_enqueued_src gen=%llu pick=%s tgt=%s",
             "retranslate_enqueued_src gen=%llu pick_len=%zu tgt_len=%zu"},
        };
        for (const auto& site : kGatedDiagSites) {
            const std::size_t tag_pos = main_src.find(site.first);
            bool site_ok = tag_pos != std::string::npos;
            if (site_ok) {
                const std::size_t w = tag_pos > 1200 ? tag_pos - 1200 : 0;
                const std::string before = main_src.substr(w, tag_pos - w);
                site_ok = before.find("if (diag::ContentLoggingEnabled())") !=
                              std::string::npos &&
                          main_src.find(site.second) != std::string::npos;
            }
            TEST_CHECK(site_ok,
                       "P9b-3 F-3b R15-23 (b): the enumerated DIAG family site carries the "
                       "content gate + its shape-only fallback: " +
                           std::string(site.first));
        }
        TEST_CHECK(eq(sizeof(kGatedDiagSites) / sizeof(kGatedDiagSites[0]),
                      static_cast<std::size_t>(20)),
                   "P9b-3 F-3b R15-23 (b): the enumerated family set holds exactly the 20 "
                   "sites from the F-3b sweep");
        const char* kDiagContentTokens[] = {"src=%s",     "tgt=%s",
                                            "src='%s'",   "tgt='%s'",
                                            "persisted=%s",
                                            "detected=%s",
                                            "eff=%s",
                                            "persisted_tgt=%s",
                                            "pivot_tgt=%s",
                                            "back_tgt=%s",
                                            "fwd_tgt=%s",
                                            "carried=%s",
                                            "lang=%s",
                                            "pick=%s",
                                            "model_path=%s",
                                            "drag=%s",
                                            "type=%s",
                                            "pair %s"};
        std::size_t ungated_content_sites = 0;
        for (const char* marker : {"DIAG_LOG(", "DIAG_F("}) {
            for (std::size_t pos = main_src.find(marker); pos != std::string::npos;
                 pos = main_src.find(
                     marker, pos + std::char_traits<char>::length(marker))) {
                // Bound the call at its terminating ';' (no main.cpp DIAG
                // format/argument contains one) so a NEIGHBORING gated call's
                // tokens cannot leak into this call's window — the first
                // sweep draft used a fixed 1200-char window and false-
                // flagged the ungated shape-only back_translate_skip_disabled
                // line sitting ~300 chars before a gated content line.
                const std::size_t call_end = main_src.find(";", pos);
                const std::string call =
                    main_src.substr(pos, call_end == std::string::npos
                                             ? std::string::npos
                                             : call_end - pos);
                bool carries_content = false;
                for (const char* token : kDiagContentTokens) {
                    if (call.find(token) != std::string::npos) {
                        carries_content = true;
                        break;
                    }
                }
                if (!carries_content) {
                    continue;
                }
                const std::size_t w = pos > 1200 ? pos - 1200 : 0;
                const std::string before = main_src.substr(w, pos - w);
                if (before.find("if (diag::ContentLoggingEnabled())") == std::string::npos) {
                    ++ungated_content_sites;
                }
            }
        }
        TEST_CHECK(eq(ungated_content_sites, static_cast<std::size_t>(0)),
                   "P9b-3 F-3b R15-23 (b): every main.cpp DIAG call whose format carries a "
                   "pair/src/tgt/lang/pick/model_path token sits inside a "
                   "ContentLoggingEnabled gate (zero ungated content sites)");
        TEST_CHECK(eq(count_of(main_src, "if (diag::ContentLoggingEnabled())"),
                      static_cast<std::size_t>(21)),
                   "P9b-3 F-3b R15-23 (b): the gate count is exactly 21 (the pre-existing "
                   "lang_sync gate + the 20 enumerated F-3b family sites — a new gated "
                   "site forces an explicit table update, keeping the enumeration honest)");
    }

    // ---- (g) fix-list-round15 R18 (unit F-4): the probe service's shutdown-
    // INTENT latch. A probe spawned by the shutdown language-sync drain after
    // engine.RequestCancel() short-circuits inside Translate to Canceled+empty;
    // the transport maps that to {}, EvaluateProbeResult folds empty output to
    // Unsupported(reason=empty) and the locked verb persists it - so a language
    // that was never really probed stays "unsupported" until a manual retest.
    // The latch stops NEW spawns at both seams; the R15-20 join/drain order is
    // untouched (the exhaustive claim holds a fortiori - the drain can no
    // longer produce work).
    {
        // (g)-1 control, live: WITHOUT the latch the same fixture fires, so the
        // declines pinned next cannot be vacuous. Joined + drained like (a)-5,
        // so the destructor backstop stays a no-op.
        AppConfig pre_latch_cfg;
        TEST_CHECK(pre_latch_cfg.AddUserLanguage(MakeUserLanguage("Latchexia", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 F-4 R18 (g): the control entry registers");
        pre_latch_cfg.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService pre_latch_service(pre_latch_cfg, nullptr);
        std::atomic<int> control_calls{0};
        pre_latch_service.SetTransportForTest(
            [&control_calls](std::string_view, std::string_view, std::string_view,
                             std::string_view) -> std::wstring {
                ++control_calls;
                return L"Sample output.";
            });
        TEST_CHECK(pre_latch_service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Latchexia"),
                   "P9b-3 F-4 R18 (g): the first-use trigger fires while no shutdown "
                   "intent is latched (control - the declines below are not vacuous)");
        pre_latch_service.ShutdownJoinWorkers(std::chrono::milliseconds(2000));
        TEST_CHECK(eq(pre_latch_service.DrainOutcomes(), static_cast<std::size_t>(1)) &&
                       control_calls.load() == 1,
                   "P9b-3 F-4 R18 (g): the control probe ran once through the real join "
                   "+ drain path");

        // (g)-2, live: with the intent latched BOTH seams decline - no worker,
        // no in-flight slot, no transport call, no drained completion and no
        // persisted verdict (the false-Unsupported write is impossible).
        AppConfig latch_cfg;
        TEST_CHECK(latch_cfg.AddUserLanguage(MakeUserLanguage("Samplelandic", "")) ==
                       UserLanguageWriteResult::Applied,
                   "P9b-3 F-4 R18 (g): the latch fixture entry registers");
        latch_cfg.openai.model = "gpt-test";
        emebalachat::UserLanguageProbeService latch_service(latch_cfg, nullptr);
        std::atomic<int> latch_calls{0};
        latch_service.SetTransportForTest(
            [&latch_calls](std::string_view, std::string_view, std::string_view,
                           std::string_view) -> std::wstring {
                ++latch_calls;
                return L"Sample output.";
            });
        latch_service.BeginShutdown();
        TEST_CHECK(!latch_service.MaybeTriggerForPick(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 F-4 R18 (g): MaybeTriggerForPick declines once the shutdown "
                   "intent is latched");
        TEST_CHECK(!latch_service.RequestRetest(LanguageEngine::OpenAi, "Samplelandic"),
                   "P9b-3 F-4 R18 (g): RequestRetest declines once the shutdown intent is "
                   "latched");
        latch_service.ShutdownJoinWorkers(std::chrono::milliseconds(2000));
        // The drain runs UNCONDITIONALLY (a side effect inside an && chain
        // would be skipped the moment an earlier operand fails, which would
        // hide the persisted-verdict consequence below).
        const std::size_t latched_drained = latch_service.DrainOutcomes();
        TEST_CHECK(latch_calls.load() == 0 &&
                       !latch_service.IsInFlight("Samplelandic", "openai", "gpt-test") &&
                       eq(latched_drained, static_cast<std::size_t>(0)),
                   "P9b-3 F-4 R18 (g): no worker started, no in-flight slot and no "
                   "completion queued after the latch");
        TEST_CHECK(!latch_cfg.user_languages.empty() &&
                       latch_cfg.user_languages.front().probes.empty(),
                   "P9b-3 F-4 R18 (g): no probe verdict was persisted (the false "
                   "Unsupported record that would suppress the first-use trigger)");

        // (g)-3, from the sources: both spawn seams consult the latch BESIDE the
        // retired gate (the latch must not replace it - retired alone discards a
        // late completion, which is a different contract).
        using p9b3u2_detail::ReadFileBytes;
        const std::filesystem::path probe_cpp = ResolveRepoFile(L"src\\user_language_probe.cpp");
        const std::filesystem::path probe_hpp = ResolveRepoFile(L"src\\user_language_probe.hpp");
        const std::filesystem::path latch_main_cpp = ResolveRepoFile(L"src\\main.cpp");
        const std::string probe_cpp_src =
            probe_cpp.empty() ? std::string() : ReadFileBytes(probe_cpp);
        const std::string probe_hpp_src =
            probe_hpp.empty() ? std::string() : ReadFileBytes(probe_hpp);
        const std::string latch_main_src =
            latch_main_cpp.empty() ? std::string() : ReadFileBytes(latch_main_cpp);
        TEST_CHECK(!probe_cpp_src.empty() && !probe_hpp_src.empty() &&
                       !latch_main_src.empty(),
                   "P9b-3 F-4 R18 (g): the probe sources + main.cpp resolve for the scan");

        // Body of one service method: from its definition to the next closing
        // brace at column 0 (the seam bodies are flat).
        const auto seam_body = [](const std::string& src, const char* signature) {
            const std::size_t pos = src.find(signature);
            if (pos == std::string::npos) {
                return std::string();
            }
            const std::size_t end = src.find("\n}\n", pos);
            return src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        };
        const std::string pick_body =
            seam_body(probe_cpp_src, "UserLanguageProbeService::MaybeTriggerForPick(");
        const std::string retest_body =
            seam_body(probe_cpp_src, "UserLanguageProbeService::RequestRetest(");
        TEST_CHECK(!pick_body.empty() &&
                       pick_body.find("shutdown_intent") != std::string::npos &&
                       pick_body.find("state_->retired") != std::string::npos,
                   "P9b-3 F-4 R18 (g): MaybeTriggerForPick gates on the shutdown intent "
                   "AND keeps the retired latch");
        TEST_CHECK(!retest_body.empty() &&
                       retest_body.find("shutdown_intent") != std::string::npos &&
                       retest_body.find("state_->retired") != std::string::npos,
                   "P9b-3 F-4 R18 (g): RequestRetest gates on the shutdown intent AND "
                   "keeps the retired latch");
        TEST_CHECK(probe_hpp_src.find("bool shutdown_intent = false;") != std::string::npos &&
                       probe_hpp_src.find("void BeginShutdown();") != std::string::npos &&
                       probe_cpp_src.find("void UserLanguageProbeService::BeginShutdown()") !=
                           std::string::npos,
                   "P9b-3 F-4 R18 (g): the latch lives in SharedState beside retired and "
                   "BeginShutdown is part of the service contract");

        // (g)-4, the set-point order: latched INSIDE the shutdown sequence -
        // after engine.RequestCancel() and BEFORE the language-sync drain whose
        // accepted-plan path can spawn (and before the bounded join, which then
        // only has to cover probes started earlier).
        const auto count_of_latch = [](const std::string& haystack,
                                       const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };
        const std::size_t latch_set_pos = latch_main_src.find("probe_service.BeginShutdown();");
        const std::size_t cancel_latch_pos = latch_main_src.find("engine.RequestCancel();");
        const std::size_t spawn_drain_pos = latch_main_src.find(
            "for (const emebalachat::LanguageSyncRequest& req : "
            "emebalachat::DrainLanguageSyncQueue())");
        const std::size_t shutdown_join_pos =
            latch_main_src.find("probe_service.ShutdownJoinWorkers(");
        const std::size_t probe_retire_pos = latch_main_src.find("probe_service.Retire();");
        TEST_CHECK(latch_set_pos != std::string::npos &&
                       count_of_latch(latch_main_src, "probe_service.BeginShutdown();") == 1 &&
                       cancel_latch_pos != std::string::npos &&
                       lt(cancel_latch_pos, latch_set_pos),
                   "P9b-3 F-4 R18 (g): exactly one set-point, inside the shutdown sequence "
                   "after engine.RequestCancel()");
        TEST_CHECK(spawn_drain_pos != std::string::npos && lt(latch_set_pos, spawn_drain_pos),
                   "P9b-3 F-4 R18 (g): the latch is set BEFORE the language-sync drain "
                   "whose coordinator can spawn a probe");
        TEST_CHECK(shutdown_join_pos != std::string::npos &&
                       lt(latch_set_pos, shutdown_join_pos) &&
                       probe_retire_pos != std::string::npos &&
                       lt(latch_set_pos, probe_retire_pos),
                   "P9b-3 F-4 R18 (g): the latch precedes the bounded join and the retire, "
                   "so the join covers only probes started before the exit intent");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-3 U4a user-language badge rows + tray wiring "
                     "passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-3 U4a user-language badge rows + tray wiring: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
