// ===========================================================================
// 261116_0003 P9b-2 (language-menu redesign, plan §3 D5 + §4 Phase 3): the
// "내 언어" group on the six menu surfaces.
//
// What this suite pins, and why it can be pinned headlessly at all:
//
//   (1) the COMMAND BANDS. The menus themselves are Win32 (TrackPopupMenuEx,
//       GUI thread, no window may be opened here), so the id layout is exposed
//       as pure constexpr functions in the two headers (tray.hpp /
//       tooltip.hpp) and pinned twice: the compile-time half lives next to the
//       band declarations (static_asserts in tray.cpp / tooltip.cpp), the
//       runtime half below proves the arithmetic, the round trip and the
//       cross-surface disjointness.
//   (2) ENGINE AWARENESS. BuildLanguageMenuView is the single filtering
//       authority (P9a); this suite pins the consumption contract the menus
//       depend on (local view never carries a group, Google only servable
//       codes, the free-form engines everything, Auto == Google) and that the
//       group can never exceed the band's per-surface capacity.
//   (3) DISPATCH. The (surface, role, index) decode plus the payload form
//       (UserLanguageStoredName), and the source-scan proof that each surface's
//       arm maps to its own callback.
//   (4) GHOST. The registry half stays the shared palette predicate; the user
//       half asks the merge (a code-less entry has no LanguageInfo row), and a
//       pinned user entry under a local view still shows the truthful ghost.
//   (5) THE APPLY PATH. A "내 언어" payload validates through the 6-argument
//       PlanLanguageSync (main.cpp passes config.user_languages), while an
//       unregistered token and the AUTO target keep refusing - and the
//       5-argument overload keeps refusing a code-less user payload, which is
//       the proof the allowance is scoped to the one wired call site.
//
// The menu RENDERING itself (row order, header, check marks on screen) is a
// manual-QA badge item: no window is created here.
// ===========================================================================
void TestP9b2UserLanguageMenu() {
    std::cout << "[RUN] Testing 261116_0003 P9b-2 user-language menu group..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) the command bands ---------------------------------------------
    // Compile-time constants compared at RUNTIME go through these helpers, so
    // TEST_CHECK's `if` is never a constant condition (MSVC C4127; the test TUs
    // stay warning-clean apart from the documented vram_gate/p5 exemptions).
    // The values are pinned twice on purpose: the static_asserts in tray.cpp /
    // tooltip.cpp are the compile-time half, this is the runtime half - a
    // renumbering that still compiles must fail HERE.
    const auto eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
    const auto lt = [](std::size_t lhs, std::size_t rhs) { return lhs < rhs; };
    const auto gt = [](std::size_t lhs, std::size_t rhs) { return lhs > rhs; };

    // kMaxUserLanguages is P9a's registry cap and the only bound the merge can
    // produce (BuildLanguageMenuView stops at it), so the band sizes below are
    // exact, not estimates.
    TEST_CHECK(eq(kMaxUserLanguages, 64),
               "P9b-2: the user-row band math is pinned against the P9a cap (64)");
    TEST_CHECK(eq(kTrayUserLangBase, 2609) &&
                   eq(kTrayUserLangStride, static_cast<UINT>(kMaxUserLanguages)) &&
                   eq(kTrayUserLangSurfaceCount, 4),
               "P9b-2: the tray band starts at 2609 with one 64-id slot per surface");
    TEST_CHECK(eq(kTrayUserLangBandEnd, 2865),
               "P9b-2: the tray band's exclusive end is 2609 + 4 * 64 = 2865");

    // The flat layout, surface by surface (the exact ids the menus append).
    TEST_CHECK(eq(TrayUserLangCommand(TrayUserLangSurface::TypingSource, 0), 2609) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::TypingSource, 63), 2672) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::TypingTarget, 0), 2673) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::TypingTarget, 63), 2736) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::DragSource, 0), 2737) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::DragSource, 63), 2800) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::DragTarget, 0), 2801) &&
                   eq(TrayUserLangCommand(TrayUserLangSurface::DragTarget, 63), 2864),
               "P9b-2: the four tray surfaces own 2609..2864 in 64-id slots");
    // Non-collision with every existing tray band: the P6 special rows end at
    // 2608 and the highest registry-band id any menu can emit is the drag
    // target's exclusive end 2589. 2600 (drag swap) and the 2301..2337 UI
    // language rows are all below 2609 by construction; the compile-time half
    // names each band. Here the runtime assertion is the one fact the
    // dispatcher relies on: no id in the new band equals a pre-P9b-2 id.
    TEST_CHECK(gt(kTrayUserLangBase, 2608) && gt(kTrayUserLangBase, 2589),
               "P9b-2: the tray band starts above the P6 ghost block and every registry band");

    // Round trip for every id in both bands, and the boundary rejections: an
    // id one below the base / one at the exclusive end decodes as "not mine".
    {
        bool round_trip = true;
        for (int s = 0; s < 4; ++s) {
            for (std::size_t i = 0; i < kMaxUserLanguages; ++i) {
                const UINT cmd = TrayUserLangCommand(static_cast<TrayUserLangSurface>(s), i);
                const TrayUserLangCommandRef ref = DecodeTrayUserLangCommand(cmd);
                if (!ref.decoded || static_cast<int>(ref.surface) != s ||
                    ref.user_index != i) {
                    round_trip = false;
                }
            }
        }
        TEST_CHECK(round_trip,
                   "P9b-2: every (surface, user index) command round-trips through the decoder");
        TEST_CHECK(!DecodeTrayUserLangCommand(kTrayUserLangBase - 1).decoded &&
                       !DecodeTrayUserLangCommand(2600).decoded &&
                       !DecodeTrayUserLangCommand(2100).decoded &&
                       !DecodeTrayUserLangCommand(kTrayUserLangBandEnd).decoded &&
                       !DecodeTrayUserLangCommand(0).decoded,
                   "P9b-2: ids outside the tray user band never decode as a user row");
    }

    TEST_CHECK(eq(kTooltipCmdUserSrcBase, 2900) && eq(kTooltipCmdUserTgtBase, 2964) &&
                   eq(kTooltipCmdUserBandEnd, 3028),
               "P9b-2: the tooltip band runs 2900..3027, source slot then target slot");
    TEST_CHECK(eq(TooltipUserLangCommand(TooltipUserLangRole::Source, 0), 2900) &&
                   eq(TooltipUserLangCommand(TooltipUserLangRole::Source, 63), 2963) &&
                   eq(TooltipUserLangCommand(TooltipUserLangRole::Target, 0), 2964) &&
                   eq(TooltipUserLangCommand(TooltipUserLangRole::Target, 63), 3027),
               "P9b-2: the two tooltip roles own one 64-id slot each");
    {
        bool round_trip = true;
        for (int r = 0; r < 2; ++r) {
            for (std::size_t i = 0; i < kMaxUserLanguages; ++i) {
                const UINT cmd = TooltipUserLangCommand(static_cast<TooltipUserLangRole>(r), i);
                const TooltipUserLangCommandRef ref = DecodeTooltipUserLangCommand(cmd);
                if (!ref.decoded || static_cast<int>(ref.role) != r || ref.user_index != i) {
                    round_trip = false;
                }
            }
        }
        TEST_CHECK(round_trip,
                   "P9b-2: every (role, user index) command round-trips through the tooltip decoder");
        TEST_CHECK(!DecodeTooltipUserLangCommand(kTooltipCmdUserSrcBase - 1).decoded &&
                       !DecodeTooltipUserLangCommand(2701).decoded &&
                       !DecodeTooltipUserLangCommand(2704).decoded &&
                       !DecodeTooltipUserLangCommand(kTooltipCmdUserBandEnd).decoded &&
                       !DecodeTooltipUserLangCommand(1).decoded,
                   "P9b-2: ids outside the tooltip user band never decode (registry rows and the "
                   "P6b special rows stay unreadable as user rows)");
    }
    // Cross-surface disjointness: each surface's dispatcher must not be able to
    // claim the other's rows even if a future change ever shared a dispatch
    // chain. The tray band ends at 2864, the tooltip band starts at 2900, and
    // neither decoder accepts the other's ids.
    TEST_CHECK(lt(kTrayUserLangBandEnd, kTooltipCmdUserSrcBase) &&
                   !DecodeTooltipUserLangCommand(TrayUserLangCommand(
                                                    TrayUserLangSurface::DragTarget,
                                                    kMaxUserLanguages - 1))
                        .decoded &&
                   !DecodeTrayUserLangCommand(
                        TooltipUserLangCommand(TooltipUserLangRole::Source, 0))
                        .decoded &&
                   !DecodeTrayUserLangCommand(
                        TooltipUserLangCommand(TooltipUserLangRole::Target, 0))
                        .decoded,
               "P9b-2: the tray and tooltip user bands are numerically disjoint");

    // ---- 2) engine awareness: the merge is the only filtering authority ----
    UserLanguage ko;
    ko.native_name = "한국어";
    ko.english_name = "Korean";
    ko.code = "KO";
    UserLanguage bs;
    bs.native_name = "Bosanski";
    bs.english_name = "Bosnian";
    bs.code = "BS";
    UserLanguage twi;
    twi.native_name = "Twi"; // no code: free-form engines only (D5)
    const std::vector<UserLanguage> users = {ko, bs, twi};

    const LanguageMenuView local_src = BuildLanguageMenuView(LanguageEngine::HyMt2, true, users);
    const LanguageMenuView google_src = BuildLanguageMenuView(LanguageEngine::Google, true, users);
    const LanguageMenuView openai_src = BuildLanguageMenuView(LanguageEngine::OpenAi, true, users);
    const LanguageMenuView gguf_src = BuildLanguageMenuView(LanguageEngine::UserGguf, true, users);
    const LanguageMenuView auto_src = BuildLanguageMenuView(LanguageEngine::Auto, true, users);

    TEST_CHECK(local_src.user_group.empty(),
               "P9b-2: the local Hy-MT2 view never carries a \"내 언어\" group (D7)");
    TEST_CHECK(google_src.user_group.size() == 2 &&
                   google_src.user_group[0].user == &users[0] &&
                   google_src.user_group[1].user == &users[1],
               "P9b-2: the Google view offers only the Google-servable entries (KO, BS)");
    TEST_CHECK(openai_src.user_group.size() == 3 && gguf_src.user_group.size() == 3,
               "P9b-2: the free-form engines offer every registered entry (code-less included)");
    TEST_CHECK(auto_src.user_group.size() == google_src.user_group.size() &&
                   auto_src.registry.size() == google_src.registry.size(),
               "P9b-2: Auto resolves to the Google MENU view (the tray's index-0 idiom)");
    TEST_CHECK(local_src.user_group.size() <= kMaxUserLanguages &&
                   google_src.user_group.size() <= kMaxUserLanguages &&
                   openai_src.user_group.size() <= kMaxUserLanguages,
               "P9b-2: the merge never produces more rows than one band slot holds");

    // Capacity proof: 70 registered code-less entries collapse to the P9a cap,
    // so the per-surface slot can never overflow and every row id stays inside
    // its 64-id slot.
    {
        std::vector<UserLanguage> many;
        for (int i = 0; i < 70; ++i) {
            UserLanguage entry;
            entry.native_name = "Twi" + std::to_string(i);
            many.push_back(entry);
        }
        const LanguageMenuView capped =
            BuildLanguageMenuView(LanguageEngine::OpenAi, true, many);
        TEST_CHECK(capped.user_group.size() == kMaxUserLanguages,
                   "P9b-2: a 70-entry registry is capped at kMaxUserLanguages by the merge");
        bool in_slot = true;
        for (std::size_t i = 0; i < capped.user_group.size(); ++i) {
            const UINT cmd = TrayUserLangCommand(TrayUserLangSurface::DragTarget, i);
            if (cmd >= kTrayUserLangBandEnd) {
                in_slot = false;
            }
        }
        TEST_CHECK(in_slot,
                   "P9b-2: the capped group's last row id stays inside the band");
    }

    // ---- 3) dispatch mapping + payload form --------------------------------
    // The decode is the mapping; the payload is the entry's STORED name in
    // every role. The source-scan block in (6) proves each decoded surface
    // reaches its own callback.
    {
        const UINT cmd = TrayUserLangCommand(TrayUserLangSurface::DragTarget, 1);
        const TrayUserLangCommandRef ref = DecodeTrayUserLangCommand(cmd);
        TEST_CHECK(ref.decoded && ref.surface == TrayUserLangSurface::DragTarget &&
                       ref.user_index == 1,
                   "P9b-2: the drag-target slot decodes to (DragTarget, 1)");
        const LanguageMenuView drag_tgt =
            BuildLanguageMenuView(LanguageEngine::Google, false, users);
        TEST_CHECK(ref.user_index < drag_tgt.user_group.size() &&
                       UserLanguageStoredName(*drag_tgt.user_group[ref.user_index].user) ==
                           "Bosnian",
                   "P9b-2: the decoded index selects the same row the menu built, and the "
                   "payload is its stored name (English name when present)");
    }
    {
        // A code-less entry's stored form is its native name (no English field).
        const LanguageMenuView openai_view =
            BuildLanguageMenuView(LanguageEngine::OpenAi, false, users);
        std::string twi_payload;
        for (const LanguageMenuRow& row : openai_view.user_group) {
            if (row.user != nullptr && row.user->native_name == "Twi") {
                twi_payload = UserLanguageStoredName(*row.user);
            }
        }
        TEST_CHECK(twi_payload == "Twi",
                   "P9b-2: a code-less entry's dispatch payload is its native name");
    }

    // ---- 3b) the registry rows' check-mark rule ---------------------------
    // NormalizeLanguageCode maps EVERY unresolvable token to "AUTO", and since
    // P9b-2 a code-less user pin is exactly such a token. The shared rule keeps
    // the normalized comparison for concrete rows but lets the RAW token decide
    // the AUTO row, so one setting never lights two rows (the AUTO row and the
    // pin's own "내 언어" row).
    TEST_CHECK(IsRegistryMenuRowCurrent("AUTO", "AUTO", "Auto Detect") &&
                   IsRegistryMenuRowCurrent("AUTO", "AUTO", "AUTO") &&
                   IsRegistryMenuRowCurrent("AUTO", "AUTO", ""),
               "P9b-2: a real AUTO pin checks the AUTO row");
    TEST_CHECK(!IsRegistryMenuRowCurrent("AUTO", "AUTO", "Twi") &&
                   !IsRegistryMenuRowCurrent("AUTO", "AUTO", "Elvish"),
               "P9b-2: an unresolvable pin (which normalizes to AUTO) does NOT check the AUTO row");
    TEST_CHECK(IsRegistryMenuRowCurrent("KO", "KO", "Korean") &&
                   IsRegistryMenuRowCurrent("KO", "KO", "KO") &&
                   !IsRegistryMenuRowCurrent("KO", "EN", "Korean"),
               "P9b-2: concrete registry rows keep the plain normalized comparison");

    // ---- 3c) fix R14-02: the check-mark pin round-trips the planner --------
    // The planner canonicalizes a pick REGISTRY-FIRST (config.cpp
    // CanonicalPlannerToken): a pick of a native-only entry (code KO resolved,
    // no English name) persists the REGISTRY spelling "Korean", which appears
    // on the entry nowhere. The merge then moves the claimed KO registry row
    // into the group, so a spelling-only lookup lights ZERO rows. The shared
    // pin-to-row resolution (CurrentUserLanguageRow) closes the round trip
    // through the pin's resolved code, and the registry rule's user_row_checked
    // argument keeps one setting at one check mark.
    UserLanguage native_only;
    native_only.native_name = "한국어"; // no English name: the config-load shape
    native_only.code = "KO";           // an explicit "code" key is trusted verbatim
    const std::vector<UserLanguage> native_only_users = {native_only};
    const UserLanguage* const native_only_row = &native_only_users[0];

    // The six menu surfaces as their headless equivalents: the tray's four
    // submenus share two role views (source = include_auto, target = not), and
    // the tooltip's two dropdowns build those same two views through the same
    // merge. This lambda mirrors the render loops exactly: a user row is
    // checked by pointer identity against the shared resolution, a registry row
    // by the shared rule fed with that answer.
    struct P9b2Surface {
        const char* name;
        bool source_role;
    };
    const P9b2Surface kP9b2Surfaces[6] = {
        {"tray typing source", true},  {"tray typing target", false},
        {"tray drag source", true},    {"tray drag target", false},
        {"tooltip source", true},      {"tooltip target", false},
    };
    const auto count_marks = [](const LanguageMenuView& view, std::string_view pin,
                                int& user_marks, int& registry_marks) {
        user_marks = 0;
        registry_marks = 0;
        const UserLanguage* current_user = CurrentUserLanguageRow(view, pin);
        const std::string normalized = NormalizeLanguageCode(pin);
        const bool user_row_checked = current_user != nullptr;
        for (const LanguageMenuRow& row : view.user_group) {
            if (row.user != nullptr && row.user == current_user) {
                ++user_marks;
            }
        }
        for (const LanguageInfo* row : view.registry) {
            if (row != nullptr &&
                IsRegistryMenuRowCurrent(row->code, normalized, pin, user_row_checked)) {
                ++registry_marks;
            }
        }
    };

    // The post-pick half: the dispatch payload is UserLanguageStoredName (the
    // native name here) and the 6-argument planner rewrites it.
    const std::string native_only_payload = UserLanguageStoredName(native_only);
    LanguageSyncPlan native_pick = PlanLanguageSync(
        LanguageContext::Type, "Auto Detect", "English", native_only_payload,
        std::string_view{}, native_only_users);
    TEST_CHECK(native_only_payload == "한국어" && native_pick.valid &&
                   native_pick.source_language == "Korean",
               "P9b-2 fix R14-02: a native-only pick persists the REGISTRY name_en "
               "(\"Korean\"), not the entry's stored form (the root cause)");
    TEST_CHECK(FindUserLanguage(native_only_users, native_pick.source_language) == nullptr,
               "P9b-2 fix R14-02: the spelling set alone cannot see that pin (the entry has no "
               "English name and the registry spelling is not one of its own)");
    {
        // The TARGET role persists through the same canonicalization (the
        // planner is context- and role-agnostic), which is what puts the
        // tooltip target dropdown and the drag-pair target menus on the same
        // round trip as the source role.
        LanguageSyncPlan target_pick = PlanLanguageSync(
            LanguageContext::Drag, "English", "Auto Detect", std::string_view{},
            native_only_payload, native_only_users);
        TEST_CHECK(target_pick.valid && target_pick.target_language == "Korean",
                   "P9b-2 fix R14-02: a TARGET-role pick persists the same registry spelling, so "
                   "the drag-pair and tooltip-target surfaces round-trip identically");
    }
    {
        const LanguageMenuView google_view =
            BuildLanguageMenuView(LanguageEngine::Google, /*include_auto=*/true, native_only_users);
        TEST_CHECK(CurrentUserLanguageRow(google_view, native_pick.source_language) ==
                       native_only_row,
                   "P9b-2 fix R14-02: the shared resolution finds the entry again through the "
                   "pin's resolved code");
        TEST_CHECK(CurrentUserLanguageRow(google_view, "한국어") == native_only_row &&
                       CurrentUserLanguageRow(google_view, "KO") == native_only_row,
                   "P9b-2 fix R14-02: the entry's own spellings (native name, resolved code) keep "
                   "lighting the same row");
    }
    // DoD 1: the post-pick reopen. "Korean" is the persisted pin; each of the
    // six menus must light exactly the entry's own row (never zero, never two).
    {
        bool one_row_everywhere = true;
        for (int s = 0; s < 6; ++s) {
            const LanguageMenuView view = BuildLanguageMenuView(
                LanguageEngine::Google, kP9b2Surfaces[s].source_role, native_only_users);
            int user_marks = 0;
            int registry_marks = 0;
            count_marks(view, native_pick.source_language, user_marks, registry_marks);
            if (user_marks != 1 || registry_marks != 0 ||
                CurrentUserLanguageRow(view, native_pick.source_language) != native_only_row) {
                std::cout << "[INFO] P9b-2 fix R14-02: surface " << kP9b2Surfaces[s].name
                          << " user_marks=" << user_marks
                          << " registry_marks=" << registry_marks << std::endl;
                one_row_everywhere = false;
            }
        }
        TEST_CHECK(one_row_everywhere,
                   "P9b-2 fix R14-02: the reopened \"Korean\" pin lights EXACTLY ONE row, the "
                   "entry's own \"내 언어\" row, on all six menus (cloud view)");
    }
    // DoD 2: the AUTO collision. A user entry stored exactly as "Auto Detect" is
    // a legal free-form entry, and the raw-token AUTO arm would ALSO accept the
    // pin, lighting the entry's row and the AUTO registry row. One setting, so
    // one mark: the user row wins and the AUTO row stays dark.
    UserLanguage auto_named;
    auto_named.native_name = "Auto Detect";
    const std::vector<UserLanguage> auto_named_users = {auto_named};
    {
        const LanguageMenuView view = BuildLanguageMenuView(
            LanguageEngine::OpenAi, /*include_auto=*/true, auto_named_users);
        int user_marks = 0;
        int registry_marks = 0;
        count_marks(view, "Auto Detect", user_marks, registry_marks);
        TEST_CHECK(user_marks == 1 && registry_marks == 0,
                   "P9b-2 fix R14-02: a user entry named exactly \"Auto Detect\" lights ONLY its "
                   "own row (the AUTO row stays dark)");
    }
    TEST_CHECK(!IsRegistryMenuRowCurrent("AUTO", "AUTO", "Auto Detect", /*user_row_checked=*/true) &&
                   IsRegistryMenuRowCurrent("AUTO", "AUTO", "Auto Detect") &&
                   IsRegistryMenuRowCurrent("KO", "KO", "Korean", /*user_row_checked=*/true),
               "P9b-2 fix R14-02: the 4-argument form only withdraws the AUTO mark (the "
               "3-argument behavior and every concrete row are unchanged)");
    {
        // A genuine AUTO pin, with no such user entry, still lights exactly the
        // AUTO row: the flag must never suppress the truthful setting.
        const LanguageMenuView view = BuildLanguageMenuView(LanguageEngine::Google, true, users);
        int user_marks = 0;
        int registry_marks = 0;
        count_marks(view, "Auto Detect", user_marks, registry_marks);
        TEST_CHECK(user_marks == 0 && registry_marks == 1,
                   "P9b-2 fix R14-02: a genuine AUTO pin still lights EXACTLY ONE row (the AUTO "
                   "row) when no user entry claims it");
    }
    // The resolution answers over the RENDERED group only, and it keeps the
    // FindUserLanguage spelling set for every spelling a pin can carry.
    {
        const LanguageMenuView google_view =
            BuildLanguageMenuView(LanguageEngine::Google, true, users);
        const LanguageMenuView free_form_view =
            BuildLanguageMenuView(LanguageEngine::UserGguf, true, users);
        TEST_CHECK(CurrentUserLanguageRow(google_view, "Twi") == nullptr &&
                       CurrentUserLanguageRow(free_form_view, "Twi") == &users[2],
                   "P9b-2 fix R14-02: the mark is resolved over the RENDERED rows (a code-less "
                   "entry is unmarkable where the merge does not draw it)");
        bool spelling_agreement = true;
        const char* const pins[] = {"한국어", "Korean", "KO", "Bosnian", "BS", "Twi"};
        for (const char* pin : pins) {
            if (CurrentUserLanguageRow(free_form_view, pin) != FindUserLanguage(users, pin)) {
                spelling_agreement = false;
            }
        }
        TEST_CHECK(spelling_agreement,
                   "P9b-2 fix R14-02: the spelling arm keeps the FindUserLanguage semantics "
                   "(stored/native/English/code) on a view that renders every entry");
        TEST_CHECK(CurrentUserLanguageRow(google_view, "") == nullptr &&
                       CurrentUserLanguageRow(google_view, "Elvish") == nullptr,
                   "P9b-2 fix R14-02: an empty or unregistered pin resolves to no user row");
    }

    // ---- 3d) fix R14-03: the TARGET dropdown uses the shared rule -----------
    // The legacy target check was a raw `name_en == pin || code == pin`
    // comparison, so a native, alias or case-variant spelling that the tray's
    // drag-target menu lights showed NO check mark in the tooltip's target
    // dropdown. Both surfaces now run ONE rule over the same target-role view.
    {
        const LanguageMenuView local_tgt =
            BuildLanguageMenuView(LanguageEngine::HyMt2, /*include_auto=*/false, users);
        const LanguageInfo* ko_row = nullptr;
        for (const LanguageInfo* row : local_tgt.registry) {
            if (row != nullptr && row->code == "KO") {
                ko_row = row;
            }
        }
        const auto legacy_target_current = [](const LanguageInfo& row, std::string_view pin) {
            return row.name_en == pin || row.code == pin;
        };
        TEST_CHECK(ko_row != nullptr,
                   "P9b-2 fix R14-03: the local target view carries the KO registry row");
        TEST_CHECK(ko_row != nullptr &&
                       IsRegistryMenuRowCurrent(ko_row->code, NormalizeLanguageCode("한국어"),
                                                "한국어") &&
                       IsRegistryMenuRowCurrent(ko_row->code, NormalizeLanguageCode("korean"),
                                                "korean") &&
                       !legacy_target_current(*ko_row, "한국어") &&
                       !legacy_target_current(*ko_row, "korean"),
                   "P9b-2 fix R14-03: a native or case-variant pin NOW lights the target row the "
                   "legacy raw comparison missed");
        int user_marks = 0;
        int registry_marks = 0;
        count_marks(local_tgt, "한국어", user_marks, registry_marks);
        TEST_CHECK(user_marks == 0 && registry_marks == 1,
                   "P9b-2 fix R14-03: the shared rule lights exactly that one registry row, the "
                   "same single mark the tray's drag-target menu shows");
        // The same target-role view and rule with a registry pin (no user entry
        // claims it): one mark, so the two surfaces cannot disagree.
        const std::vector<UserLanguage> no_users;
        const LanguageMenuView google_tgt =
            BuildLanguageMenuView(LanguageEngine::Google, false, no_users);
        count_marks(google_tgt, "Korean", user_marks, registry_marks);
        TEST_CHECK(user_marks == 0 && registry_marks == 1,
                   "P9b-2 fix R14-03: the target-role view marks exactly one row for a registry "
                   "pin spelling both surfaces read");
    }

    // ---- 4) the pinned-absent ghost ---------------------------------------
    // (a) a user entry whose code resolved keeps the SHARED registry path: the
    //     pin canonicalizes to the stored name, NormalizeLanguageCode maps it
    //     back to the code, and the palette predicate answers from the engine
    //     view (exactly like a registry pin).
    {
        language_palette::PickerInit probe;
        probe.engine = LanguageEngine::HyMt2;
        probe.role = language_palette::PickerRole::Target;
        probe.source_code = "AUTO";
        probe.target_code = NormalizeLanguageCode("Bosnian");
        TEST_CHECK(probe.target_code == "BS",
                   "P9b-2: a code-carrying user pin normalizes to its registry code");
        const LanguageInfo* ghost =
            language_palette::PickerState{probe}.PinnedAbsentLanguage();
        TEST_CHECK(ghost != nullptr && ghost->code == "BS",
                   "P9b-2: a user entry pinned outside the local view ghosts through the SHARED "
                   "registry predicate (not a second capability check)");
        // Under the free-form engines the same entry IS offered, so no ghost.
        language_palette::PickerInit offered = probe;
        offered.engine = LanguageEngine::UserGguf;
        TEST_CHECK(language_palette::PickerState{offered}.PinnedAbsentLanguage() == nullptr,
                   "P9b-2: the same pin is NOT a ghost where the merge offers it");
    }
    // (b) a code-less entry has no LanguageInfo row, so the shared predicate
    //     cannot express it ("Twi" even normalizes to AUTO). The user half
    //     answers from the authoritative merge instead.
    {
        language_palette::PickerInit probe;
        probe.engine = LanguageEngine::HyMt2;
        probe.role = language_palette::PickerRole::Target;
        probe.source_code = "AUTO";
        probe.target_code = NormalizeLanguageCode("Twi");
        TEST_CHECK(probe.target_code == "AUTO" &&
                       language_palette::PickerState{probe}.PinnedAbsentLanguage() == nullptr,
                   "P9b-2: a code-less pin is invisible to the registry predicate (no row)");
        const UserLanguage* ghost = PinnedAbsentUserLanguage(local_src, users, "Twi");
        TEST_CHECK(ghost != nullptr && ghost->native_name == "Twi",
                   "P9b-2: a code-less user pin ghosts under the local view via the merge");
        const LanguageMenuView google_tgt =
            BuildLanguageMenuView(LanguageEngine::Google, false, users);
        TEST_CHECK(PinnedAbsentUserLanguage(google_tgt, users, "Twi") != nullptr,
                   "P9b-2: the same pin ghosts under Google (Google cannot serve it either)");
        TEST_CHECK(PinnedAbsentUserLanguage(openai_src, users, "Twi") == nullptr,
                   "P9b-2: no ghost where the merge offers the entry");
    }
    // (c) an entry whose resolved registry row IS in the view is not a ghost:
    //     the local view serves KO from its own registry section.
    TEST_CHECK(PinnedAbsentUserLanguage(local_src, users, "Korean") == nullptr &&
                   PinnedAbsentUserLanguage(local_src, users, "KO") == nullptr,
               "P9b-2: a user entry offered through its registry code is not a ghost "
               "(stored-name and code spellings agree)");
    // (d) an unregistered pin is never a user ghost.
    TEST_CHECK(PinnedAbsentUserLanguage(local_src, users, "Elvish") == nullptr &&
                   PinnedAbsentUserLanguage(local_src, users, "") == nullptr,
               "P9b-2: an unregistered or empty pin produces no user ghost");
    // (e) identity contract: a view built from a DIFFERENT vector cannot be
    //     queried with this one (the pointers differ), which is exactly why the
    //     tray keeps one `users` alive across the menu's dispatch.
    {
        std::vector<UserLanguage> copy = users;
        const LanguageMenuView from_copy =
            BuildLanguageMenuView(LanguageEngine::OpenAi, true, copy);
        TEST_CHECK(PinnedAbsentUserLanguage(from_copy, users, "Twi") != nullptr,
                   "P9b-2: the user half answers from the merge's own rows (a foreign vector's "
                   "rows are not matched by identity)");
    }
    // (f) fix R14-01: the SUPPRESSED state. Under engine AUTO + a live local
    //     host, a code-carrying Hy-MT2-only pin (BO) names a language the
    //     AUTO menu view (Google, the tray's TrayLanguageEngineFor mapping)
    //     does not offer, so the registry half DOES find a ghost, and the
    //     shared gate suppresses it because the pair is locally reliable.
    //     The user half is gated on the PRE-SUPPRESSION null, so the ghost
    //     contract renders NOTHING, exactly like an identical registry pin.
    //     The decision components are pinned here; the menu wiring itself is
    //     pinned structurally in (7).
    {
        language_palette::PickerInit probe;
        probe.engine = LanguageEngine::Google; // what the tray passes for AUTO
        probe.role = language_palette::PickerRole::Target;
        probe.source_code = "AUTO";
        probe.target_code = "BO";
        const LanguageInfo* registry_ghost =
            language_palette::PickerState{probe}.PinnedAbsentLanguage();
        TEST_CHECK(registry_ghost != nullptr && registry_ghost->code == "BO",
                   "P9b-2 fix R14-01: under AUTO (Google menu view) the registry half DOES "
                   "find the BO ghost (Tibetan is Hy-MT2-only)");
        TEST_CHECK(language_palette::SuppressPinnedAbsentGhost(probe, 0, true, true),
                   "P9b-2 fix R14-01: AUTO + live host + reliable pair SUPPRESSES that ghost "
                   "(the shared predicate's own verdict, the P6b AUTO->BO row)");
        TEST_CHECK(LocalPairReliable("AUTO", "BO"),
                   "P9b-2 fix R14-01: AUTO->BO is a locally reliable pair (the suppression "
                   "precondition)");
        UserLanguage tibetan;
        tibetan.native_name = "Tibetan";
        tibetan.english_name = "Tibetan";
        tibetan.code = "BO";
        const std::vector<UserLanguage> bo_users = {tibetan};
        const LanguageMenuView auto_tgt =
            BuildLanguageMenuView(LanguageEngine::Auto, false, bo_users);
        TEST_CHECK(auto_tgt.user_group.empty() &&
                       PinnedAbsentUserLanguage(auto_tgt, bo_users, "Tibetan") == &bo_users[0],
                   "P9b-2 fix R14-01: ungated, the user half WOULD re-render the ghost (BO is "
                   "absent from the AUTO view): the pre-suppression gate is the only thing "
                   "keeping the suppressed state silent");
    }
    // (g) fix R14-04: a duplicate-code pair. The merge's code-level dedupe
    //     keeps only the FIRST entry (PR9-01, "first occurrence of a code
    //     wins"), so a pin resolving to the dropped twin must not ghost while
    //     the language is offered as the surviving entry's row.
    {
        UserLanguage fr_a;
        fr_a.native_name = "Français";
        fr_a.english_name = "French";
        fr_a.code = "FR";
        UserLanguage fr_b;
        fr_b.native_name = "French Canadian";
        fr_b.english_name = "French";
        fr_b.code = "FR";
        const std::vector<UserLanguage> dup_users = {fr_a, fr_b};
        const LanguageMenuView dup_view =
            BuildLanguageMenuView(LanguageEngine::Google, true, dup_users);
        TEST_CHECK(dup_view.user_group.size() == 1 &&
                       dup_view.user_group[0].user == &dup_users[0],
                   "P9b-2 fix R14-04: the merge keeps only the first entry of a duplicate code");
        TEST_CHECK(FindUserLanguage(dup_users, "French Canadian") == &dup_users[1],
                   "P9b-2 fix R14-04: the native-name pin resolves to the DROPPED twin");
        TEST_CHECK(PinnedAbsentUserLanguage(dup_view, dup_users, "French Canadian") == nullptr,
                   "P9b-2 fix R14-04: a pin on the dropped twin renders NO ghost: the code "
                   "match sees the language offered as the surviving entry's row");
        TEST_CHECK(PinnedAbsentUserLanguage(dup_view, dup_users, "French") == nullptr,
                   "P9b-2 fix R14-04: a pin on the surviving entry stays ghost-free (identity)");
        const LanguageMenuView openai_dup =
            BuildLanguageMenuView(LanguageEngine::OpenAi, true, dup_users);
        TEST_CHECK(openai_dup.user_group.size() == 1 &&
                       PinnedAbsentUserLanguage(openai_dup, dup_users, "French Canadian") ==
                           nullptr,
                   "P9b-2 fix R14-04: the code-based check holds on the free-form views too");
    }

    // ---- 5) the apply path: 6-argument PlanLanguageSync wired in main.cpp --
    // The payload a "내 언어" row sends is validated against the registry the
    // coordinator now passes. This is the end-to-end headless proof: pick ->
    // payload -> plan -> persisted form.
    {
        LanguageSyncPlan plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect",
                                                 "English", std::string_view{}, "Twi", users);
        TEST_CHECK(plan.valid && plan.target_language == "Twi",
                   "P9b-2: a registered code-less user pick is VALID through the 6-argument "
                   "planner and persists the stored name");
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view{}, "Bosnian", users);
        TEST_CHECK(plan.valid && plan.target_language == "Bosnian",
                   "P9b-2: a code-carrying user pick validates through the same call");
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English", "Twi",
                                std::string_view{}, users);
        TEST_CHECK(plan.valid && plan.source_language == "Twi" &&
                       plan.target_language == "English",
                   "P9b-2: a user pick in the SOURCE role sets only the source");
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "Twi",
                                std::string_view{}, "Twi", users);
        TEST_CHECK(plan.valid && !plan.changed,
                   "P9b-2: a no-op re-pick of a user pin reports changed == false (no disk churn)");
    }
    // Refusals keep their exact current behavior.
    {
        LanguageSyncPlan plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect",
                                                 "English", std::string_view{}, "Elvish", users);
        TEST_CHECK(!plan.valid && plan.target_language == "English",
                   "P9b-2: an unregistered token is still refused (echoes the current pair)");
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view{}, "Auto Detect", users);
        TEST_CHECK(!plan.valid,
                   "P9b-2: the AUTO-target refusal is unchanged (AUTO stays source-only)");
        // Scope proof: the allowance exists only where the registry is passed.
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view{}, "Twi");
        TEST_CHECK(!plan.valid,
                   "P9b-2: the 5-argument overload still refuses a code-less user payload "
                   "(the allowance is scoped to the wired call site)");
    }

    // ---- 6) row form -------------------------------------------------------
    TEST_CHECK(FormatUserLanguageMenuLabel(ko) == "한국어 (Korean)",
               "P9b-2: a user row reads native (English) - no code prefix");
    TEST_CHECK(FormatUserLanguageMenuLabel(bs) == "Bosanski (Bosnian)",
               "P9b-2: the English name rides in parentheses when present");
    TEST_CHECK(FormatUserLanguageMenuLabel(twi) == "Twi",
               "P9b-2: an entry without an English name renders the native name only");
    TEST_CHECK(FormatUserLanguageMenuLabel(bs).rfind("BS", 0) != 0,
               "P9b-2: the user row never carries the registry form's code prefix");

    // Fix R14-05: the label the menu APIs receive escapes the mnemonic marker.
    // AppendMenuW consumes '&', so a user-typed name containing it must reach
    // the API doubled to render the literal character; the stored name (and
    // therefore the UserLanguageStoredName dispatch payload) stays verbatim,
    // so the row cannot read different from the name it dispatches.
    UserLanguage amp;
    amp.native_name = "AT & T";
    amp.english_name = "Ampersand & Co";
    amp.code = "AM";
    TEST_CHECK(FormatUserLanguageMenuLabel(amp) == "AT && T (Ampersand && Co)",
               "P9b-2 fix R14-05: an ampersand in the user-typed name reaches the menu "
               "label doubled (AppendMenuW renders the literal stored character)");
    TEST_CHECK(UserLanguageStoredName(amp) == "Ampersand & Co",
               "P9b-2 fix R14-05: the dispatch payload stays the verbatim stored name "
               "(the escape is display-only, in the one label formatter)");
    UserLanguage amp_native;
    amp_native.native_name = "Tom & Jerry";
    TEST_CHECK(FormatUserLanguageMenuLabel(amp_native) == "Tom && Jerry",
               "P9b-2 fix R14-05: the escape holds for the native-name-only row form");

    // ---- 7) structural pins (the menus themselves are Win32/GUI-only) ------
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

        std::string tray_src;
        std::string tray_hdr;
        std::string tooltip_src;
        std::string tooltip_hdr;
        std::string main_src;
        const bool readable = read_src(L"src\\ui\\tray.cpp", tray_src) &&
                              read_src(L"src\\ui\\tray.hpp", tray_hdr) &&
                              read_src(L"src\\ui\\tooltip.cpp", tooltip_src) &&
                              read_src(L"src\\ui\\tooltip.hpp", tooltip_hdr) &&
                              read_src(L"src\\main.cpp", main_src);
        // Fix R14-08: an unresolvable source is a FAILURE, never a silent
        // [SKIP] that still prints suite PASS (verification integrity,
        // charter §3d). The resolution is anchored: ResolveRepoFile probes
        // the exe directory and its parents, the CWD and its parents, and
        // the engine repo fallback, so these sources are reachable from the
        // repo root and every standard build/run layout.
        TEST_CHECK(readable,
                   "P9b-2 fix R14-08: the structural-pin sources resolve via the anchored "
                   "ResolveRepoFile (an unresolvable source fails loudly, never skips)");
        if (readable) {
            const std::string main_flat = squeeze(main_src);

            // The band declarations live in the headers (the only place both
            // surfaces' constants are visible to this suite).
            TEST_CHECK(tray_hdr.find("kTrayUserLangBase = 2609") != std::string::npos &&
                           tray_hdr.find("DecodeTrayUserLangCommand") != std::string::npos &&
                           tray_hdr.find("FormatUserLanguageMenuLabel") != std::string::npos &&
                           tray_hdr.find("PinnedAbsentUserLanguage") != std::string::npos,
                       "P9b-2: tray.hpp declares the band, the decoder and the two row helpers");
            TEST_CHECK(tooltip_hdr.find("kTooltipCmdUserSrcBase = 2900") != std::string::npos &&
                           tooltip_hdr.find("DecodeTooltipUserLangCommand") != std::string::npos &&
                           tooltip_hdr.find("struct MenuEngineResolution") != std::string::npos &&
                           tooltip_hdr.find("std::vector<UserLanguage> user_languages;") !=
                               std::string::npos,
                       "P9b-2: tooltip.hpp declares its own band, decoder and resolver payload");

            // Both tray language menus (source and target views, reused by the
            // type and drag pairs) come from the MERGE now, and the group is
            // rendered only when non-empty, under one header row.
            TEST_CHECK(count_of(tray_src, "BuildLanguageMenuView(menu_engine,") == 2,
                       "P9b-2: both tray views are BuildLanguageMenuView merges (the type pair "
                       "and the drag pair share them)");
            TEST_CHECK(tray_src.find("!merged.user_group.empty()") != std::string::npos &&
                           tray_src.find("I18n::Get(StringId::UserLangsGroup)") != std::string::npos &&
                           tray_src.find("MF_STRING | MF_DISABLED, 0") != std::string::npos,
                       "P9b-2: the tray group renders only when non-empty, under the "
                       "non-selectable UserLangsGroup header (id 0)");
            // Order: search row -> ghost -> group header/rows -> registry rows.
            // Pinned by source position inside the single builder.
            {
                const std::size_t search_pos =
                    tray_src.find("I18n::Get(StringId::MenuSearchLangs)");
                const std::size_t ghost_pos = tray_src.find(
                    "language_palette::PickerState{ghost_probe}.PinnedAbsentLanguage()");
                const std::size_t group_pos =
                    tray_src.find("I18n::Get(StringId::UserLangsGroup)");
                const std::size_t registry_pos =
                    tray_src.find("for (size_t i = 0; i < merged.registry.size(); ++i)");
                TEST_CHECK(search_pos != std::string::npos && ghost_pos != std::string::npos &&
                               group_pos != std::string::npos &&
                               registry_pos != std::string::npos && search_pos < ghost_pos &&
                               ghost_pos < group_pos && group_pos < registry_pos,
                           "P9b-2: the tray group sits BELOW the search/ghost rows and ABOVE the "
                           "registry rows (the P6 rows keep their positions)");
            }
            TEST_CHECK(count_of(tray_src, "PinnedAbsentUserLanguage(merged, users, current_raw)") == 1 &&
                           tray_src.find("} else if (!registry_ghost_found)") !=
                               std::string::npos &&
                           count_of(tray_src, "language_palette::SuppressPinnedAbsentGhost(") == 1,
                       "P9b-2: the tray keeps ONE shared ghost gate and adds the user half as its "
                       "else branch (no second suppression path, no double ghost row)");
            // Fix R14-01, structural half: the user half must be gated on the
            // PRE-SUPPRESSION null. The capture has to precede the shared gate
            // call, and the else branch must test the capture, not the
            // post-gate pointer (a post-gate null would re-render the ghost
            // the shared gate just suppressed).
            {
                const std::size_t capture_pos =
                    tray_src.find("const bool registry_ghost_found = ghost != nullptr;");
                const std::size_t gate_pos =
                    tray_src.find("language_palette::SuppressPinnedAbsentGhost(");
                const std::size_t else_pos = tray_src.find("} else if (!registry_ghost_found)");
                TEST_CHECK(capture_pos != std::string::npos && gate_pos != std::string::npos &&
                               else_pos != std::string::npos && capture_pos < gate_pos &&
                               gate_pos < else_pos,
                           "P9b-2 fix R14-01: the tray captures the pre-suppression null BEFORE "
                           "the shared gate and the user half answers only on that null");
            }
            TEST_CHECK(tray_src.find("switch (user_cmd.surface)") != std::string::npos &&
                           count_of(tray_src, "case TrayUserLangSurface::") == 4,
                       "P9b-2: the tray dispatch maps all four surfaces explicitly");
            TEST_CHECK(count_of(tray_src, "UserLanguageStoredName(") == 1 &&
                           tray_src.find("callbacks_.on_select_drag_target_lang(name)") !=
                               std::string::npos,
                       "P9b-2: the tray user-row payload is the stored name, routed per surface");

            // The tooltip: two dropdowns, two groups, two user ghosts, the
            // stored-name payload in both dispatch arms.
            TEST_CHECK(count_of(tooltip_src, "BuildLanguageMenuView(menu_engine.engine,") == 2,
                       "P9b-2: both tooltip dropdowns build from the merge");
            TEST_CHECK(count_of(tooltip_src, "I18n::Get(StringId::UserLangsGroup)") == 2,
                       "P9b-2: both tooltip dropdowns carry the group header");
            TEST_CHECK(count_of(tooltip_src, "PinnedAbsentUserLanguage(") == 2 &&
                           count_of(tooltip_src, "UserLanguageStoredName(") == 2,
                       "P9b-2: both tooltip dropdowns have the user ghost and the stored-name "
                       "payload");
            TEST_CHECK(tooltip_src.find("TooltipUserLangCommand(TooltipUserLangRole::Source, i)") !=
                               std::string::npos &&
                           tooltip_src.find("TooltipUserLangCommand(TooltipUserLangRole::Target, i)") !=
                               std::string::npos,
                       "P9b-2: each tooltip dropdown appends its own role slot");
            // Fix R14-01 on the tooltip mirrors: both dropdowns carry the same
            // pre-suppression capture and the same gated else branch as the
            // tray (each capture sits before its own shared gate call).
            TEST_CHECK(count_of(tooltip_src,
                                "const bool registry_ghost_found = ghost != nullptr;") == 2 &&
                           count_of(tooltip_src, "} else if (!registry_ghost_found)") == 2,
                       "P9b-2 fix R14-01: both tooltip dropdowns gate the user half on the "
                       "pre-suppression null");
            {
                const std::size_t capture_pos =
                    tooltip_src.find("const bool registry_ghost_found = ghost != nullptr;");
                const std::size_t gate_pos =
                    tooltip_src.find("language_palette::SuppressPinnedAbsentGhost(");
                TEST_CHECK(capture_pos != std::string::npos && gate_pos != std::string::npos &&
                               capture_pos < gate_pos &&
                               tooltip_src.find("const bool registry_ghost_found", gate_pos) !=
                                   std::string::npos,
                           "P9b-2 fix R14-01: each tooltip capture precedes its shared gate "
                           "(the find-with-offset proves the second dropdown is gated too)");
            }
            {
                const std::size_t search_pos = tooltip_src.find("kTooltipCmdSearchSrc,");
                const std::size_t group_pos =
                    tooltip_src.find("I18n::Get(StringId::UserLangsGroup)");
                const std::size_t registry_pos =
                    tooltip_src.find("for (size_t i = 0; i < src_langs.size(); ++i)");
                TEST_CHECK(search_pos != std::string::npos && group_pos != std::string::npos &&
                               registry_pos != std::string::npos && search_pos < group_pos &&
                               group_pos < registry_pos,
                           "P9b-2: the tooltip group sits below the search row and above the "
                           "registry rows");
            }

            // Fix R14-02, structural half: ONE shared pin-to-row resolution is
            // declared in the header next to the row helpers, and every surface
            // calls it and hands its answer to the registry rule. No surface may
            // re-derive the match from FindUserLanguage on its own.
            TEST_CHECK(tray_hdr.find("CurrentUserLanguageRow") != std::string::npos,
                       "P9b-2 fix R14-02: the shared pin-to-row resolution is declared in "
                       "tray.hpp next to the user-row helpers");
            TEST_CHECK(count_of(tray_src, "CurrentUserLanguageRow(merged, current_raw)") == 1 &&
                           count_of(tray_src, "current, current_raw, user_row_checked)") == 1 &&
                           tray_src.find("FindUserLanguage(users, current_raw)") ==
                               std::string::npos,
                       "P9b-2 fix R14-02: the tray's four submenus use the shared resolution, "
                       "feed the registry rule, and no longer call FindUserLanguage directly");
            TEST_CHECK(count_of(tooltip_src, "CurrentUserLanguageRow(") == 2 &&
                           count_of(tooltip_src, "user_row_checked))") == 2,
                       "P9b-2 fix R14-02: both tooltip dropdowns use the shared resolution and "
                       "feed the registry rule");
            // Fix R14-03, structural half: the target loop runs the shared call
            // the source loop runs, and the legacy raw comparison is gone.
            {
                const std::size_t tgt_loop =
                    tooltip_src.find("for (size_t i = 0; i < target_langs.size(); ++i)");
                TEST_CHECK(tooltip_src.find("target_langs[i]->name_en == pThis->target_lang_") ==
                                   std::string::npos &&
                               count_of(tooltip_src, "IsRegistryMenuRowCurrent(") == 2 &&
                               tgt_loop != std::string::npos &&
                               tooltip_src.find("IsRegistryMenuRowCurrent(target_langs[i]->code",
                                                tgt_loop) != std::string::npos,
                           "P9b-2 fix R14-03: the tooltip target loop uses the shared registry "
                           "rule and the legacy raw comparison is gone");
            }

            // Fix R14-05, structural half: the ONE label formatter doubles the
            // mnemonic marker ('&' -> "&&") for the menu API, and every surface
            // consumes that one definition (the render sites are pinned above:
            // one tray builder + two tooltip dropdowns), so the escaped form
            // reaches all six menu surfaces with no surface-local variant.
            TEST_CHECK(count_of(tray_src, "if (ch == '&')") == 1,
                       "P9b-2 fix R14-05: the label formatter doubles '&' in its one body "
                       "(no surface-local escape can drift from the shared definition)");
            // Fix R14-06, structural half: both "내 언어" dispatch arms record
            // the user-typed stored name only behind the REQ-003 content gate,
            // and each gate precedes its own content line; the else branch
            // keeps the shape-only len= field, so no verbatim name is emitted
            // when diag_log_content is off.
            TEST_CHECK(count_of(tooltip_src, "if (diag::ContentLoggingEnabled())") == 2 &&
                           count_of(tooltip_src, "src_menu_select_user lang=%s") == 1 &&
                           count_of(tooltip_src, "src_menu_select_user len=%zu") == 1 &&
                           count_of(tooltip_src, "tgt_menu_select_user lang=%s") == 1 &&
                           count_of(tooltip_src, "tgt_menu_select_user len=%zu") == 1,
                       "P9b-2 fix R14-06: both user dispatch arms gate the stored-name log "
                       "behind ContentLoggingEnabled and keep a shape-only len= line");
            {
                const std::size_t src_gate_pos =
                    tooltip_src.find("if (diag::ContentLoggingEnabled())");
                const std::size_t src_content_pos =
                    tooltip_src.find("src_menu_select_user lang=%s");
                const std::size_t tgt_gate_pos =
                    tooltip_src.find("if (diag::ContentLoggingEnabled())", src_gate_pos + 1);
                const std::size_t tgt_content_pos =
                    tooltip_src.find("tgt_menu_select_user lang=%s");
                TEST_CHECK(src_gate_pos != std::string::npos &&
                               src_content_pos != std::string::npos &&
                               src_gate_pos < src_content_pos &&
                               tgt_gate_pos != std::string::npos &&
                               tgt_content_pos != std::string::npos &&
                               tgt_gate_pos < tgt_content_pos,
                           "P9b-2 fix R14-06: the ContentLoggingEnabled gate precedes each "
                           "stored-name log line (the gate, not the sink master switch, "
                           "owns the content field)");
            }

            // The apply path: the coordinator passes the registry to the
            // 6-argument planner, and both resolvers carry the registry copy.
            TEST_CHECK(main_flat.find("PlanLanguageSync(ctx,cur_src,cur_tgt,req_src,req_tgt,"
                                      "config.user_languages)") != std::string::npos,
                       "P9b-2: main.cpp's ApplyLanguageChange passes config.user_languages to the "
                       "6-argument PlanLanguageSync");
            TEST_CHECK(main_flat.find("view.user_languages=config.user_languages;") !=
                               std::string::npos &&
                           main_flat.find("resolution.user_languages=config.user_languages;") !=
                               std::string::npos,
                       "P9b-2: both menu resolvers carry the registry copy (derive-at-open)");
            // No registry-only planner call may survive in the coordinator: the
            // 5-argument form's exact tail must be gone from main.cpp.
            TEST_CHECK(main_src.find("ctx, cur_src, cur_tgt, req_src, req_tgt)") ==
                               std::string::npos,
                       "P9b-2: the coordinator's only planner call is the 6-argument form (the "
                       "5-argument tail is gone, so no surface can bypass the registry)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9b-2 user-language menu group passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9b-2 user-language menu group: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
