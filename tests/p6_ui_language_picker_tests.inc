// ===========================================================================
// 261119_0001 P6-U1..U5 (UI-language-89 phase 2, design of record:
// docs/261119_0001_session_ui-language-89/architect-pick.md - the "Pool
// build" one-walk pick and the "AUTO in pool" pick): the UI-locale picker
// POOL emitted by the same single join walk that owns the selector
// (I18n::GetSupportedUiLocalePickerPool, src/i18n.cpp).
//
// This suite pins the POOL'S DATA CONTRACT as it stands after the whole P6
// unit landed (U1 data source, U2 mode, U3 window layer, U4 tray shape, U5
// controller dispatch). It is consumed in production by
// PickerState::RefreshHits on the TrayUiLanguage surface, and no part of it
// is staging-only any more:
//   - pool size == selector size + 1 (the AUTO row; derived, never a literal
//     count, so phase-3 authored=true flips scale the pin);
//   - pool row 0 is the kAllLanguages AUTO registry row;
//   - pool[1..] is POINTER-IDENTICAL to each selector entry's registry row,
//     in the selector's exact display order (single join owner: the two
//     views cannot drift);
//   - every pool row is a member row of the immutable registry statics
//     (pointer containment, process lifetime);
//   - the authenticity gate bounds the pool exactly like the selector: an
//     authored=false mapping row contributes NO pool row (vacuous-safe once
//     all 89 are authored);
//   - the suite never opens a window: the picker's only window-creating
//     entry (emebalachat::ShowLanguageSearchPalette) and the deferred-open
//     latch are never named as calls here, and zero windows of the picker
//     class exist while this suite runs (p5 section (10) idiom mirrored).
//
// The sections append in unit order: U1 pool (1)-(6), U2 the UI-language
// mode's keyboard contract (sections 1-11), which reads no palette source and
// opens no window, U3 the SECTION B source pins (the window layer's UI-mode
// rendering, the p5 section (10) idiom), U4 the tray shape, and U5 the
// controller dispatch; they read the .cpp/.hpp sources and still open no
// window - the whole file stays inside the section (6) self-call pin.
// ===========================================================================
void TestP6UiLanguagePicker() {
    std::cout << "[RUN] Testing 261119_0001 P6-U1..U5 UI-language picker "
                 "(pool + mode + window + tray + dispatch)..."
              << std::endl;
    const int failures_before = g_failed_count;

    const std::vector<const LanguageInfo*>& pool = GetSupportedUiLocalePickerPool();
    const std::vector<UiLocaleEntry>& selector = GetSupportedUiLocales();

    // ---- 1) size: the pool is the selector plus exactly the AUTO row ----
    {
        TEST_CHECK(pool.size() == selector.size() + 1,
                   "P6-U1 (1): the pool is the selector plus exactly the AUTO row "
                   "(size derived from GetSupportedUiLocales, no literal count)");
    }

    // ---- 2) row 0 is the registry AUTO row ------------------------------
    {
        TEST_CHECK(!pool.empty() && pool[0] != nullptr && pool[0]->code == "AUTO",
                   "P6-U1 (2): pool row 0 is the kAllLanguages AUTO registry row");
    }

    // ---- 3) order + pointer identity: pool[1..] walks the selector's
    //         registry rows in the selector's exact display order ----------
    {
        bool order_ok = pool.size() == selector.size() + 1;
        for (std::size_t i = 0; order_ok && i < selector.size(); ++i) {
            const LanguageInfo* expected =
                FindLanguageByCode(I18n::LocaleToString(selector[i].locale));
            order_ok = expected != nullptr && pool[i + 1] == expected;
        }
        TEST_CHECK(order_ok,
                   "P6-U1 (3): pool[1..] is pointer-identical to each selector entry's "
                   "registry row (FindLanguageByCode(LocaleToString(locale))), in selector order");
    }

    // ---- 4) containment: every pool row lives in the immutable registry --
    {
        const std::vector<LanguageInfo>& registry = GetSupportedLanguages();
        bool contained = true;
        for (const LanguageInfo* row : pool) {
            bool found = false;
            if (row != nullptr) {
                for (const LanguageInfo& lang : registry) {
                    if (row == &lang) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) contained = false;
        }
        TEST_CHECK(contained,
                   "P6-U1 (4): every pool row is a member row of GetSupportedLanguages() "
                   "(pointer into the registry statics, process lifetime)");
    }

    // ---- 5) the authenticity gate bounds the pool exactly like the selector
    {
        // Vacuous-safe by construction: when every mapping row is authored
        // (phase-3 end state) the loop body never runs and the check holds
        // trivially; until then every authored=false row must be absent.
        bool gate_ok = true;
        for (const LocaleMapping& m : GetLocaleMappings()) {
            if (m.authored) continue;
            std::string up(m.config_code);
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (const LanguageInfo* row : pool) {
                if (row != nullptr && row->code == up) gate_ok = false;
            }
        }
        TEST_CHECK(gate_ok,
                   "P6-U1 (5): no authored=false mapping row contributes a pool row "
                   "(vacuous-safe once all 89 are authored)");

        // The same gate feeds both views: the authored mapping rows and the
        // selector are one set, so the pool is exactly that set plus AUTO.
        std::size_t authored = 0;
        for (const LocaleMapping& m : GetLocaleMappings()) {
            if (m.authored) ++authored;
        }
        TEST_CHECK(authored == selector.size(),
                   "P6-U1 (5): the authored mapping rows and the selector are the same set "
                   "(one gate bounds both views)");
    }

    // ---- 6) headless proof: this suite never opens the picker window -----
    {
        auto read_repo_text = [](const wchar_t* rel) {
            std::string out;
            const std::filesystem::path path = ResolveRepoFile(rel);
            if (path.empty()) {
                return out;
            }
            std::ifstream in(path, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            }
            return out;
        };
        const std::string self_src = read_repo_text(L"tests\\p6_ui_language_picker_tests.inc");
        TEST_CHECK(!self_src.empty(),
                   "P6-U1 (6): this suite's own source is readable for the self-call pin");

        // The needles are assembled at runtime so this file cannot match
        // itself (the p5 section (10) idiom).
        const std::string show_entry = std::string("ShowLanguageSearch") + "Palette(";
        const std::string deferred_entry = std::string("PostDeferredLanguage") + "PaletteOpen(";
        TEST_CHECK(self_src.find(show_entry) == std::string::npos &&
                       self_src.find(deferred_entry) == std::string::npos,
                   "P6-U1 (6): the suite never calls the window/deferred-open entries");

        // Runtime half: zero windows of the picker class exist now. The
        // class name is duplicated here ON PURPOSE (the .cpp keeps it in an
        // anonymous namespace); the pin fails loudly if it is ever renamed.
        TEST_CHECK(::FindWindowW(L"Emebalachat_LanguageSearchPalette", nullptr) == nullptr,
                   "P6-U1 (6): no picker window exists during the headless suite");
    }

    // ===================================================================
    // 261119_0001 P6-U2 (UI-language-89 phase 2, architect-pick.md "Mode
    // discriminator" + unit "U2 SECTION A mode + comment sweep"): the
    // picker's UI-LANGUAGE MODE, driven headlessly.
    //
    // ONE additive surface value (PickerSurface::TrayUiLanguage) swaps the
    // engine view for GetSupportedUiLocalePickerPool() and refuses the Tab
    // flip. Everything else - movement, commit, Esc, double-click, ClearQuery,
    // preselection - is the SAME code the p5 suite pins on the translation
    // surfaces, so the checks below re-prove that contract on the UI pool
    // (the translation picker itself is pinned by p5 and by section (11) here).
    //
    // No check below opens a window. The suite's own self-call pin in section
    // (6) above scans this WHOLE file, so the appended code is covered too.
    // ===================================================================
    using language_palette::PickerCommand;
    using language_palette::PickerInit;
    using language_palette::PickerResolution;
    using language_palette::PickerRole;
    using language_palette::PickerState;
    using language_palette::PickerSurface;

    // The U2 fixture: the tray's UI-language submenu, source slot seeded with
    // the persisted UI locale exactly as the config holds it (a registry
    // spelling: "ko", "ZH-CN", "AUTO", or "" for unset).
    const auto ui_init = [](const char* source_code) {
        PickerInit init;
        init.surface = PickerSurface::TrayUiLanguage;
        init.role = PickerRole::Source;
        init.engine = LanguageEngine::Auto;
        init.source_code = source_code;
        init.target_code.clear();
        return init;
    };
    // p5 section (1) containment idiom: every returned hit must come from the
    // pool the state searched.
    const auto all_hits_in_pool = [](const PickerState& state) {
        for (const LanguageSearchHit& hit : state.hits()) {
            bool found = false;
            for (const LanguageInfo* row : state.pool()) {
                if (row == hit.language) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    };

    // ---- 1) empty query: the whole UI-locale pool, in pool order ----------
    {
        PickerState state(ui_init("ko"));
        const std::vector<const LanguageInfo*>& ui_pool = GetSupportedUiLocalePickerPool();
        // The pool SOURCE is the mode switch: the same address the accessor
        // hands out, never an engine view (a second source would drift).
        TEST_CHECK(&state.pool() == &GetSupportedUiLocalePickerPool(),
                   "P6-ui (1): the UI-language mode searches GetSupportedUiLocalePickerPool(), "
                   "not an engine view");
        bool order_ok = !ui_pool.empty() && state.hits().size() == ui_pool.size();
        bool tier_ok = true;
        for (std::size_t i = 0; order_ok && i < state.hits().size(); ++i) {
            if (state.hits()[i].language != ui_pool[i]) {
                order_ok = false;
            }
            if (state.hits()[i].tier != AliasTier::None) {
                tier_ok = false;
            }
        }
        TEST_CHECK(order_ok,
                   "P6-ui (1): an empty query returns the whole pool row-for-row in pool order");
        TEST_CHECK(tier_ok,
                   "P6-ui (1): every empty-query hit carries tier None (the unfiltered marker)");
        TEST_CHECK(!state.hits().empty() && state.hits()[0].language != nullptr &&
                       state.hits()[0].language->code == "AUTO",
                   "P6-ui (1): the list starts on the registry AUTO row (pool row 0)");
        TEST_CHECK(all_hits_in_pool(state),
                   "P6-ui (1): every hit is a pool row (containment)");
        TEST_CHECK(!state.Filtering(),
                   "P6-ui (1): an empty query is not 'filtering'");
    }

    // ---- 2) preselection: the seeded current locale seats itself ----------
    {
        const struct {
            const char* seeded;
            const char* expected_code;
        } cases[] = {
            {"ko", "KO"},
            {"zh-CN", "ZH-CN"},
            {"AUTO", "AUTO"},
            // 261119_0001 FIX-R1 R1-06: the PRODUCTION spelling of the auto
            // value is lowercase ("auto" is what config.json persists and what
            // the tray's Auto row dispatches); FindLanguageByCode is
            // ASCII-case-insensitive, so it must seat the AUTO row either way.
            {"auto", "AUTO"},
        };
        bool seats_ok = true;
        for (const auto& c : cases) {
            PickerState state(ui_init(c.seeded));
            const int seat = state.selection();
            if (seat < 0 || static_cast<std::size_t>(seat) >= state.hits().size() ||
                state.hits()[static_cast<std::size_t>(seat)].language->code != c.expected_code) {
                seats_ok = false;
            }
        }
        TEST_CHECK(seats_ok,
                   "P6-ui (2): the seeded current locale preselects its own pool row "
                   "(ko -> KO, zh-CN -> ZH-CN, AUTO/auto -> row 0) — FindLanguageByCode is "
                   "ASCII-case-insensitive, so the registry spelling resolves unchanged");

        PickerState unset_code(ui_init(""));
        TEST_CHECK(unset_code.selection() == 0,
                   "P6-ui (2): an unset current locale ('') seats the selection at row 0 (AUTO)");

        // 261119_0001 FIX-R1 R1-06: the two spellings a RAW config value can
        // carry that the pool cannot resolve. Both seat at row 0 (never -1),
        // which is exactly why the tray canonicalizes at menu-build time
        // (LocaleToString(StringToLocale(ui_language_))) instead of feeding
        // the raw bytes into PickerInit::source_code.
        {
            const char* kUnresolvable[] = {
                "zh_cn",   // REQ-037 legacy underscore spelling (StringToLocale
                           // accepts it, FindLanguageByCode does not)
                "klingon", // junk: no registry row and no name_en/name_native
            };
            bool fallback_ok = true;
            for (const char* seeded : kUnresolvable) {
                PickerState state(ui_init(seeded));
                if (state.selection() != 0 || state.hits().empty() ||
                    state.hits()[0].language->code != "AUTO") {
                    fallback_ok = false;
                }
            }
            TEST_CHECK(fallback_ok,
                       "P6-ui (2): an unresolvable seed spelling (legacy 'zh_cn', junk) seats "
                       "at row 0 (AUTO), never -1 — the reason the tray canonicalizes first");

            // ...and the canonicalization the tray performs DOES resolve them,
            // so the production wiring never hits the fallback above.
            const struct {
                const char* raw;
                const char* canonical;
                const char* expected_code;
            } kCanonicalized[] = {
                {"auto", "auto", "AUTO"},
                {"zh_cn", "zh-CN", "ZH-CN"},
                {"klingon", "en", "EN"},
            };
            bool canonical_ok = true;
            for (const auto& c : kCanonicalized) {
                const std::string canonical =
                    I18n::LocaleToString(I18n::StringToLocale(c.raw));
                PickerState state(ui_init(canonical.c_str()));
                if (canonical != c.canonical || state.selection() < 0 ||
                    static_cast<std::size_t>(state.selection()) >= state.hits().size() ||
                    state.hits()[static_cast<std::size_t>(state.selection())].language->code !=
                        c.expected_code) {
                    canonical_ok = false;
                }
            }
            TEST_CHECK(canonical_ok,
                       "P6-ui (2): the tray's build-time seed (LocaleToString(StringToLocale(raw))) "
                       "maps auto/zh_cn/junk to auto/zh-CN/en and each seats its own pool row "
                       "(junk lands on EN, never on a gated code)");
        }

        // A current locale the pool does NOT hold (a gate-withheld row while
        // phase 3 is still authoring, or a free-form value) seats at row 0 —
        // never -1. Derived at runtime, so the pin survives the phase-3
        // authored=true flips (vacuous-safe: falls back to an unknown code).
        std::string absent_code = "Klingon"; // not a registry row at all
        for (const LocaleMapping& m : GetLocaleMappings()) {
            if (!m.authored) {
                absent_code = m.config_code;
                break;
            }
        }
        PickerState absent(ui_init(absent_code.c_str()));
        TEST_CHECK(absent.selection() == 0 && !absent.hits().empty(),
                   "P6-ui (2): a current locale outside the pool seats at row 0 (never -1)");
    }

    // ---- 3) live filter: the CEO's five Korean queries + an exonym ---------
    {
        PickerState state(ui_init("ko"));
        const char* korean_queries[] = {"korean", "한국어", "koreanisch", "한글", "KO"};
        bool all_best = true;
        for (const char* query : korean_queries) {
            if (!state.SetQuery(query)) {
                all_best = false;
                continue;
            }
            if (state.hits()[0].language->code != "KO" || state.selection() != 0) {
                all_best = false;
            }
        }
        TEST_CHECK(all_best,
                   "P6-ui (3): korean/한국어/koreanisch/한글/KO each rank KO first and preselect it");

        // An EXONYM-tier hit proves the cross-locale matrix is live in this
        // mode: "스페인어" is the KOREAN-column CLDR exonym of Spanish (ES's
        // own code/native/English aliases cannot match it).
        TEST_CHECK(state.SetQuery("스페인어") && !state.hits().empty() &&
                       state.hits()[0].language->code == "ES" &&
                       state.hits()[0].tier == AliasTier::Exonym,
                   "P6-ui (3): a Korean-column exonym query ranks Spanish first at Exonym tier");

        TEST_CHECK(!state.SetQuery("qqqq") && state.hits().empty() &&
                       state.selection() == -1 && !state.HasHits(),
                   "P6-ui (3): a no-match query empties the list, seats nothing (-1) and "
                   "reports HasHits() == false");
    }

    // ---- 4) ↑↓ wraparound, including the multi-step normalization probe ---
    {
        PickerState state(ui_init("ko"));
        const int n = static_cast<int>(state.hits().size());
        TEST_CHECK(n > 2 && n == static_cast<int>(GetSupportedUiLocalePickerPool().size()),
                   "P6-ui (4): the empty-query list is the whole UI pool (>= 3 rows)");
        state.SelectRow(n - 1);
        const PickerResolution down = state.Apply(PickerCommand::MoveDown);
        TEST_CHECK(down.changed && state.selection() == 0,
                   "P6-ui (4): ↓ from the last row wraps to row 0");
        const PickerResolution up = state.Apply(PickerCommand::MoveUp);
        TEST_CHECK(up.changed && state.selection() == n - 1,
                   "P6-ui (4): ↑ from row 0 wraps to the last row");
        state.SelectRow(0);
        state.MoveSelection(+3);
        TEST_CHECK(state.selection() == 3, "P6-ui (4): a +3 step lands 3 rows down");
        state.MoveSelection(-2 * n);
        TEST_CHECK(state.selection() == 3, "P6-ui (4): a full-multiple step is a no-op");
        state.MoveSelection(-5);
        TEST_CHECK(state.selection() == (3 - 5 + n) % n,
                   "P6-ui (4): a negative step wraps through the list");

        state.SetQuery("qqqq");
        const PickerResolution down_empty = state.Apply(PickerCommand::MoveDown);
        const PickerResolution up_empty = state.Apply(PickerCommand::MoveUp);
        TEST_CHECK(!down_empty.changed && !up_empty.changed && state.selection() == -1,
                   "P6-ui (4): ↑/↓ on an empty result list change nothing and keep the seat at -1");
    }

    // ---- 5) Enter commits the selected row, payload byte-identical --------
    {
        PickerState state(ui_init("ko"));
        const int seat = state.selection();
        TEST_CHECK(seat >= 0 && static_cast<std::size_t>(seat) < state.hits().size(),
                   "P6-ui (5): the Enter pin has a seated row to commit");
        const LanguageInfo* seated = state.selected();
        const PickerResolution commit = state.Apply(PickerCommand::Commit);
        TEST_CHECK(commit.committed && commit.row != nullptr && commit.row == seated,
                   "P6-ui (5): Enter commits the SELECTED row (pointer identity)");
        TEST_CHECK(std::string(PickerState::PayloadOf(commit.row)) == commit.row->name_en,
                   "P6-ui (5): the payload is the registry name_en bytes, untouched");
        TEST_CHECK(PickerState::PayloadOf(commit.row) == "Korean",
                   "P6-ui (5): the seeded 'ko' seat commits Korean");

        // Mutation probe: after a MoveDown, Enter must commit the MOVED-TO row.
        PickerState moved(ui_init("ko"));
        moved.Apply(PickerCommand::MoveDown);
        const int moved_to = moved.selection();
        TEST_CHECK(moved_to >= 0 && static_cast<std::size_t>(moved_to) < moved.hits().size(),
                   "P6-ui (5): the post-MoveDown selection is inside the hit list before indexing");
        const LanguageInfo* moved_row =
            (moved_to >= 0 && static_cast<std::size_t>(moved_to) < moved.hits().size())
                ? moved.hits()[static_cast<std::size_t>(moved_to)].language
                : nullptr;
        TEST_CHECK(moved_row != nullptr && moved_row != seated &&
                       !moved.hits().empty() && moved_row != moved.hits()[0].language,
                   "P6-ui (5): the MoveDown left the seeded seat (mutation-visible setup)");
        const PickerResolution moved_commit = moved.Apply(PickerCommand::Commit);
        TEST_CHECK(moved_commit.committed && moved_commit.row == moved_row,
                   "P6-ui (5): Enter after MoveDown commits the moved-to row, not hits_[0]");

        PickerState empty(ui_init("ko"));
        empty.SetQuery("qqqq");
        const PickerResolution enter_empty = empty.Apply(PickerCommand::Commit);
        TEST_CHECK(!enter_empty.committed && !enter_empty.cancelled &&
                       enter_empty.row == nullptr,
                   "P6-ui (5): Enter on an empty result list commits nothing and cancels nothing");
    }

    // ---- 6) Esc yields no commit ------------------------------------------
    {
        PickerState state(ui_init("ko"));
        state.SetQuery("korean");
        const int selection_before = state.selection();
        const std::string query_before = state.query();
        const PickerResolution cancel = state.Apply(PickerCommand::Cancel);
        TEST_CHECK(cancel.cancelled && !cancel.committed && cancel.row == nullptr,
                   "P6-ui (6): Esc cancels with no committed row");
        TEST_CHECK(state.selection() == selection_before && state.query() == query_before,
                   "P6-ui (6): Esc mutates nothing (the state is inert)");
    }

    // ---- 7) Tab is a NO-OP in UI mode (one role, one pool) ----------------
    {
        PickerState state(ui_init("ko"));
        const std::vector<const LanguageInfo*>* pool_addr = &state.pool();
        const std::vector<LanguageSearchHit> hits_before = state.hits();
        const int selection_before = state.selection();
        const std::string query_before = state.query();
        const PickerResolution flip = state.Apply(PickerCommand::FlipRole);
        bool rows_identical = state.hits().size() == hits_before.size();
        for (std::size_t i = 0; rows_identical && i < hits_before.size(); ++i) {
            if (state.hits()[i].language != hits_before[i].language ||
                state.hits()[i].tier != hits_before[i].tier) {
                rows_identical = false;
            }
        }
        TEST_CHECK(!flip.changed,
                   "P6-ui (7): Tab in UI mode reports NO change (the mode has one role)");
        TEST_CHECK(state.role() == PickerRole::Source && &state.pool() == pool_addr &&
                       rows_identical && state.selection() == selection_before &&
                       state.query() == query_before,
                   "P6-ui (7): the refused flip leaves role/pool/hits/order/selection/query "
                   "untouched (same pool pointer, same rows, same seat)");

        // ...and again with a live query in the box.
        TEST_CHECK(state.SetQuery("korean") && !state.hits().empty() &&
                       state.hits()[0].language->code == "KO",
                   "P6-ui (7): the live-query flip pin is non-vacuous (KO ranks first)");
        const std::vector<LanguageSearchHit> filtered_before = state.hits();
        const PickerResolution flip_filtered = state.Apply(PickerCommand::FlipRole);
        TEST_CHECK(!flip_filtered.changed && state.query() == "korean" &&
                       state.hits().size() == filtered_before.size() &&
                       !state.hits().empty() &&
                       state.hits()[0].language == filtered_before[0].language &&
                       state.selection() == 0 && state.role() == PickerRole::Source,
                   "P6-ui (7): with a live query the flip is still a no-op (same hits, same seat)");
    }

    // ---- 8) double-click commits the clicked row --------------------------
    {
        PickerState state(ui_init("ko"));
        const int total = static_cast<int>(state.hits().size());
        TEST_CHECK(total >= 3,
                   "P6-ui (8): the double-click pin is non-vacuous (>= 3 pool rows)");
        const int clicked = total - 2;
        const bool index_ok = clicked >= 0 && clicked < total;
        const LanguageInfo* clicked_row =
            index_ok ? state.hits()[static_cast<std::size_t>(clicked)].language : nullptr;
        TEST_CHECK(clicked_row != nullptr && clicked_row != state.selected(),
                   "P6-ui (8): the clicked row differs from the selection (mutation-visible)");
        const PickerResolution click = state.Apply(PickerCommand::ActivateRow, clicked);
        TEST_CHECK(click.committed && click.row == clicked_row,
                   "P6-ui (8): a double-click commits the row AT the clicked index");
        TEST_CHECK(!state.Apply(PickerCommand::ActivateRow, total).committed &&
                       !state.Apply(PickerCommand::ActivateRow, -1).committed,
                   "P6-ui (8): an out-of-range double-click (== size, -1) commits nothing");
    }

    // ---- 9) ClearQuery restores the pool and the seeded seat --------------
    {
        PickerState state(ui_init("ko"));
        const int ko_seat = state.selection();
        TEST_CHECK(state.SetQuery("korean") && state.hits().size() < state.pool().size(),
                   "P6-ui (9): the ClearQuery pin starts from a FILTERED list");
        state.ClearQuery();
        TEST_CHECK(!state.Filtering() && state.query().empty() &&
                       state.hits().size() == state.pool().size() &&
                       state.selection() == ko_seat && state.selected() != nullptr &&
                       state.selected()->code == "KO",
                   "P6-ui (9): ClearQuery restores the whole pool and reseats the seeded "
                   "current locale (KO)");
    }

    // ---- 10) default-init protection: the translation picker is unchanged -
    {
        PickerInit def; // TrayTyping x Source x Auto - the production default
        PickerState state(def);
        const std::vector<const LanguageInfo*>& engine_view =
            GetLanguagesForEngine(LanguageEngine::HyMt2, true);
        TEST_CHECK(&state.pool() == &engine_view,
                   "P6-ui (10): a default PickerInit still searches the engine view "
                   "(the UI branch did not capture the translation surfaces)");
        TEST_CHECK(state.hits().size() == engine_view.size() && !engine_view.empty() &&
                       state.hits()[0].language == engine_view[0],
                   "P6-ui (10): the default-init list is the engine view row-for-row");
        const PickerResolution flip = state.Apply(PickerCommand::FlipRole);
        TEST_CHECK(flip.changed && state.role() == PickerRole::Target,
                   "P6-ui (10): Tab still flips a translation surface (the UI guard never leaks)");
    }

    // ---- 11) gate membership: the authored gate bounds the picker ---------
    {
        PickerState state(ui_init("ko"));
        bool gate_ok = true;
        for (const LocaleMapping& m : GetLocaleMappings()) {
            if (m.authored) continue;
            std::string upper(m.config_code);
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            state.SetQuery(m.config_code);
            for (const LanguageSearchHit& hit : state.hits()) {
                // No gated locale may ever come back as ITSELF...
                if (hit.language->code == upper) {
                    gate_ok = false;
                }
            }
            // ...and the filtered list still contains only pool rows (the query
            // TEXT may legitimately match another authored language, which is
            // why the pin is row-identity, not "zero hits").
            if (!all_hits_in_pool(state)) {
                gate_ok = false;
            }
        }
        TEST_CHECK(gate_ok,
                   "P6-ui (11): no authored=false locale can surface from the UI pool "
                   "(vacuous-safe when every mapping row is authored)");

        const char* spread[] = {"korean", "ko", "zh", "cyrillic", "arabic", "francais",
                                "tieng viet"};
        bool contained = true;
        for (const char* query : spread) {
            state.SetQuery(query);
            if (!all_hits_in_pool(state)) {
                contained = false;
            }
        }
        TEST_CHECK(contained,
                   "P6-ui (11): every hit is a pool row across 7 spread queries");
    }

    // ===================================================================
    // 261119_0001 P6-U3 (UI-language-89 phase 2, architect-pick.md unit "U3
    // SECTION B window"): the picker's WIN32 LAYER renders the UI mode.
    //
    // SECTION A (the pure mode) is exercised headlessly by sections 1-11
    // above; this section pins the WINDOW layer's SOURCE in the p5 section
    // (10) idiom. The suite still opens no window: the only runtime proof is
    // the standing "zero picker windows" FindWindowW check in section (6),
    // which scans this WHOLE file (so the appended code is covered by the
    // self-call pin too) and still passes.
    //
    //   - kPickerSlotCount grew 3 -> 4: the deferred-open latch owns one slot
    //     per PickerSurface value and TrayUiLanguage == 3 needs its own (the
    //     slot index IS the enum value);
    //   - RowText's UI branch renders the selector's MenuUiLanguageAuto label
    //     on the AUTO pool row; every other row keeps the pinned shape;
    //   - UpdateHeader's UI branch titles the header MenuUiLanguage and HIDES
    //     the engine chip (SW_HIDE) — NO second CountPhrase /
    //     LanguageSearchCount call exists (the P7 chip call-site counts must
    //     still hold: exactly one of each in the whole TU);
    //   - VK_TAB consumes the FlipRole resolution and repaints only when
    //     `changed` (the UI-mode no-op must not churn the list);
    //   - the p5 section (10) landmarks survive verbatim;
    //   - echo of the p5 section (10) header-purity pin: no forbidden Win32
    //     token leaked into SECTION A of the header.
    // ===================================================================
    {
        auto read_repo_text = [](const wchar_t* rel) {
            std::string out;
            const std::filesystem::path path = ResolveRepoFile(rel);
            if (path.empty()) {
                return out;
            }
            std::ifstream in(path, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            }
            return out;
        };
        const std::string cpp_src = read_repo_text(L"src\\ui\\language_search_palette.cpp");
        const std::string header_src = read_repo_text(L"src\\ui\\language_search_palette.hpp");
        TEST_CHECK(!cpp_src.empty() && !header_src.empty(),
                   "P6-U3 (B): the palette .cpp/.hpp sources are readable for the "
                   "SECTION B pins");

        // ---- (a) the deferred-open latch owns a slot for TrayUiLanguage ----
        TEST_CHECK(cpp_src.find("kPickerSlotCount = 4") != std::string::npos,
                   "P6-U3 (B): kPickerSlotCount is 4 (TrayUiLanguage gained the latch "
                   "a slot; the slot index is the enum value)");
        TEST_CHECK(cpp_src.find("idx >= kPickerSlotCount") != std::string::npos,
                   "P6-U3 (B): PickerSlotFor's bounds check is unchanged (the array "
                   "grew, the guard did not)");

        // ---- (b) RowText: the AUTO pool row renders the selector's label ---
        const std::size_t row_fn = cpp_src.find("std::wstring RowText");
        const std::size_t row_end = cpp_src.find("\n}\n", row_fn);
        const std::string row_body =
            cpp_src.substr(row_fn, row_end == std::string::npos
                                       ? std::string::npos
                                       : row_end - row_fn);
        TEST_CHECK(row_fn != std::string::npos &&
                       row_body.find("StringId::MenuUiLanguageAuto") != std::string::npos &&
                       row_body.find("language.code == \"AUTO\"") != std::string::npos,
                   "P6-U3 (B): RowText's UI branch renders MenuUiLanguageAuto on the "
                   "AUTO pool row");
        TEST_CHECK(row_body.find("ToUtf16(language.code) + L\" - \" + "
                                 "I18n::GetLanguageDisplayName(language.code)") !=
                       std::string::npos,
                   "P6-U3 (B): every non-AUTO row keeps the existing code-prefix + "
                   "display-name shape verbatim");

        // ---- (c) UpdateHeader: UI branch titles + hides the chip ------------
        const std::size_t hdr_fn = cpp_src.find("void UpdateHeader(PaletteWindow& w)");
        const std::size_t hdr_end = cpp_src.find("\n}\n", hdr_fn);
        const std::string hdr_body =
            cpp_src.substr(hdr_fn, hdr_end == std::string::npos
                                       ? std::string::npos
                                       : hdr_end - hdr_fn);
        TEST_CHECK(hdr_fn != std::string::npos &&
                       hdr_body.find("StringId::MenuUiLanguage)") != std::string::npos &&
                       hdr_body.find("ShowWindow(w.chip, SW_HIDE)") != std::string::npos,
                   "P6-U3 (B): UpdateHeader's UI branch titles the header "
                   "MenuUiLanguage and hides the engine chip (SW_HIDE)");
        // The P7/P9b-3 chip arms must survive verbatim inside the same body.
        TEST_CHECK(hdr_body.find("== LanguageEngine::Auto") != std::string::npos &&
                       hdr_body.find("CountPhrase(w.state.pool().size())") !=
                           std::string::npos &&
                       hdr_body.find("L\" · \"") != std::string::npos,
                   "P6-U3 (B): UpdateHeader's translation-mode chip arms are intact "
                   "(Auto/concrete branch, pool-size count, separator)");
        // ...and no second count call leaked anywhere in the TU (the P7
        // call-site counts, re-derived here so this suite stands alone).
        const auto count_of = [&cpp_src](const char* needle) {
            std::size_t n = 0;
            for (std::size_t at = cpp_src.find(needle); at != std::string::npos;
                 at = cpp_src.find(needle, at + 1)) {
                ++n;
            }
            return n;
        };
        TEST_CHECK(count_of("CountPhrase(w.state.pool().size())") == 1 &&
                       count_of("I18n::Get(StringId::LanguageSearchCount)") == 1,
                   "P6-U3 (B): exactly one pool-size CountPhrase call and one "
                   "LanguageSearchCount read (the hidden chip added no second call)");
        TEST_CHECK(count_of("StringId::MenuUiLanguageAuto") == 1 &&
                       count_of("StringId::MenuUiLanguage)") == 1,
                   "P6-U3 (B): MenuUiLanguageAuto is read exactly once (RowText) and "
                   "MenuUiLanguage exactly once (UpdateHeader)");

        // ---- (d) VK_TAB consumes the resolution, repaints on change only ----
        const std::size_t tab_at = cpp_src.find("case VK_TAB");
        const std::size_t tab_end = cpp_src.find("return true;", tab_at);
        const std::string tab_body =
            cpp_src.substr(tab_at, tab_end == std::string::npos
                                       ? std::string::npos
                                       : tab_end - tab_at);
        TEST_CHECK(tab_at != std::string::npos &&
                       tab_body.find("PickerCommand::FlipRole") != std::string::npos &&
                       tab_body.find("resolution.changed") != std::string::npos &&
                       tab_body.find("UpdateHeader(w);") != std::string::npos &&
                       tab_body.find("SyncList(w);") != std::string::npos,
                   "P6-U3 (B): VK_TAB consumes the FlipRole resolution and repaints "
                   "(UpdateHeader + SyncList) only when `changed`");

        // ---- (e) the p5 section (10) landmarks survive verbatim -------------
        TEST_CHECK(cpp_src.find("Emebalachat_LanguageSearchPalette") != std::string::npos,
                   "P6-U3 (B): the pinned palette class name survives (p5 (10) idiom)");
        TEST_CHECK(cpp_src.find("CreateWindowExW") != std::string::npos &&
                       cpp_src.find("GetMessageW") != std::string::npos,
                   "P6-U3 (B): the WS_POPUP CreateWindowExW path and the hand-rolled "
                   "GetMessageW modal loop survive (p5 (10) landmarks)");
        TEST_CHECK(cpp_src.find("result.surface = w.state.surface();") !=
                           std::string::npos &&
                       cpp_src.find("result.role = w.state.role();") != std::string::npos,
                   "P6-U3 (B): the committed role/surface are still read at commit "
                   "time (p5 (10) pin)");

        // ---- (f) echo of the p5 (10) header-purity pin ----------------------
        // SECTION A of the header must stay Win32-free: the window layer owns
        // every window-creating call. Same strip-comments-then-scan idiom as
        // p5 section (10), re-derived so this suite stands alone.
        const std::string marker = "SECTION B — the Win32 window (never";
        const std::size_t split = header_src.find(marker);
        TEST_CHECK(split != std::string::npos,
                   "P6-U3 (B): the header carries the SECTION A / SECTION B seam "
                   "marker (the echo pin's boundary)");
        const std::string pure_section =
            split == std::string::npos ? std::string() : header_src.substr(0, split);
        const std::size_t logic_ns = header_src.find("namespace language_palette {");
        const std::size_t logic_class = header_src.find("class PickerState {");
        TEST_CHECK(logic_ns != std::string::npos && logic_class != std::string::npos &&
                       logic_ns < logic_class && split != std::string::npos &&
                       logic_class < split,
                   "P6-U3 (B): the scanned SECTION A prefix still contains the whole "
                   "logic layer (namespace + PickerState)");
        const auto strip_comments = [](const std::string& source) {
            std::string out;
            out.reserve(source.size());
            std::size_t i = 0;
            while (i < source.size()) {
                if (source[i] == '"' || source[i] == '\'') {
                    const char quote = source[i];
                    out.push_back(source[i++]);
                    while (i < source.size()) {
                        if (source[i] == '\\' && i + 1 < source.size()) {
                            out.push_back(source[i++]);
                            out.push_back(source[i++]);
                            continue;
                        }
                        const char c = source[i++];
                        out.push_back(c);
                        if (c == quote) {
                            break;
                        }
                    }
                    continue;
                }
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                    const std::size_t nl = source.find('\n', i);
                    i = (nl == std::string::npos) ? source.size() : nl;
                    continue;
                }
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                    const std::size_t close = source.find("*/", i + 2);
                    i = (close == std::string::npos) ? source.size() : close + 2;
                    continue;
                }
                out.push_back(source[i++]);
            }
            return out;
        };
        const std::string pure_code = strip_comments(pure_section);
        const char* forbidden[] = {"CreateWindow", "RegisterClass", "ShowWindow",
                                   "GetMessage",   "PostMessage",   "SendMessage",
                                   "DialogBox",    "SetWindowPos",  "UpdateWindow"};
        bool pure_is_win32_free = true;
        for (const char* token : forbidden) {
            if (pure_code.find(token) != std::string::npos) {
                pure_is_win32_free = false;
            }
        }
        TEST_CHECK(pure_is_win32_free,
                   "P6-U3 (B): SECTION A (the logic layer) contains no window, "
                   "message-loop or control-message call — the U3 window work did "
                   "not leak a Win32 token into the header");
    }

    // ===================================================================
    // 261119_0001 P6-U4 (UI-language-89 phase 2, architect-pick.md unit "U4
    // tray shape"): the tray's UI-language submenu is a TWO-ROW LAUNCHER - the
    // inline Auto row plus ONE "검색…" row that opens the shared picker - and
    // the flat endonym band is retired. Pinned by SOURCE SCAN (the tray TU is
    // GUI code and this suite links core only), in the p5 section (10) idiom:
    //   - the retired band token appears NOWHERE (code or comment) and no
    //     UI-language RANGE dispatch arm survives;
    //   - ID_TRAY_UILANG_SEARCH owns 2301 and BOTH re-pinned static_asserts
    //     name it (the two 37-locale pins are gone);
    //   - the build block keeps the Auto row + its persisted-value check mark
    //     and appends the separator + MenuSearchLangs row;
    //   - the submenu title stays plain for auto and composes the active
    //     locale's endonym from selector data (no new StringId);
    //   - PickerSurface::TrayUiLanguage is latched at build time with the
    //     canonical config-code spelling and the inert Auto engine;
    //   - the dispatch arm is the P6 deferred-open shape verbatim (PR6-03e
    //     SetLastError hygiene + cause-neutral diagnostic).
    // No window is opened here; section (6) scans this WHOLE file, so the
    // appended code stays inside the headless self-call pin.
    // ===================================================================
    {
        auto read_repo_text = [](const wchar_t* rel) {
            std::string out;
            const std::filesystem::path path = ResolveRepoFile(rel);
            if (path.empty()) {
                return out;
            }
            std::ifstream in(path, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            }
            return out;
        };
        const std::string tray_src = read_repo_text(L"src\\ui\\tray.cpp");
        const std::string tray_hdr = read_repo_text(L"src\\ui\\tray.hpp");
        TEST_CHECK(!tray_src.empty() && !tray_hdr.empty(),
                   "P6-U4 (C): the tray .cpp/.hpp sources are readable for the two-row "
                   "launcher pins");

        // ---- (a) the flat band is fully retired --------------------------
        const std::string retired_band_id = std::string("ID_TRAY_UILANG_") + "BASE";
        TEST_CHECK(tray_src.find(retired_band_id) == std::string::npos,
                   "P6-U4 (C): the retired flat UI-language band token is gone from tray.cpp "
                   "(code and comments)");
        TEST_CHECK(tray_src.find("cmd >= ID_TRAY_UILANG_") == std::string::npos,
                   "P6-U4 (C): no UI-language RANGE dispatch arm remains (both rows dispatch "
                   "by equality)");
        TEST_CHECK(tray_hdr.find("Auto + 37 locales") == std::string::npos,
                   "P6-U4 (C): tray.hpp no longer documents the flat Auto + 37 layout");

        // ---- (b) the new row id and BOTH re-pinned static_asserts --------
        TEST_CHECK(tray_src.find("ID_TRAY_UILANG_SEARCH = 2301") != std::string::npos,
                   "P6-U4 (C): ID_TRAY_UILANG_SEARCH owns the retired band's first slot "
                   "(2301), directly above Auto");
        TEST_CHECK(tray_src.find("ID_TRAY_UILANG_SEARCH == ID_TRAY_UILANG_AUTO + 1") !=
                           std::string::npos &&
                       tray_src.find("ID_TRAY_UILANG_SEARCH + 1 <= "
                                     "SystemTray::kCmdThemeFluent") != std::string::npos,
                   "P6-U4 (C): the adjacency assert binds the searched row to the theme band "
                   "2381..2383 (the real neighbour, not the far drag band)");
        TEST_CHECK(tray_src.find("kTrayUserLangBase > ID_TRAY_UILANG_SEARCH &&") !=
                       std::string::npos,
                   "P6-U4 (C): the P9b-2 band assert reads the searched row instead of the "
                   "retired 37-locale literal");

        // ---- (c) the build block: Auto + separator + 검색… ---------------
        const std::size_t build_pos = tray_src.find("HMENU hUiLangMenu = ::CreatePopupMenu();");
        const std::size_t build_end = tray_src.find("uiLangTitle.c_str()", build_pos);
        const std::string build_block =
            (build_pos == std::string::npos || build_end == std::string::npos)
                ? std::string()
                : tray_src.substr(build_pos, build_end - build_pos);
        TEST_CHECK(!build_block.empty() &&
                       build_block.find("ID_TRAY_UILANG_AUTO") != std::string::npos &&
                       build_block.find("StringId::MenuUiLanguageAuto") != std::string::npos &&
                       build_block.find("autoChecked ? MF_CHECKED : MF_UNCHECKED") !=
                           std::string::npos,
                   "P6-U4 (C): the Auto row and its persisted-value check mark survive in the "
                   "build block");
        TEST_CHECK(build_block.find("MF_SEPARATOR, 0, nullptr") != std::string::npos &&
                       build_block.find("ID_TRAY_UILANG_SEARCH") != std::string::npos &&
                       build_block.find("StringId::MenuSearchLangs") != std::string::npos,
                   "P6-U4 (C): the build block appends separator + the MenuSearchLangs '검색…' "
                   "row (the four translation search rows' label idiom)");
        TEST_CHECK(build_block.find("static_cast<UINT>(i)") == std::string::npos,
                   "P6-U4 (C): the flat per-row ID loop is gone from the build block");
        TEST_CHECK(build_block.find(
                       "std::wstring uiLangTitle = I18n::Get(StringId::MenuUiLanguage);") !=
                           std::string::npos &&
                       build_block.find("I18n::LocaleToString(entry.locale)") !=
                           std::string::npos &&
                       build_block.find("uiLangTitle += entry.native_name;") != std::string::npos,
                   "P6-U4 (C): the submenu title stays plain for auto and gains the active "
                   "locale's endonym from the selector data when a concrete one is active");

        // ---- (d) the UI-language picker request is latched at build ------
        TEST_CHECK(tray_src.find("PickerSurface::TrayUiLanguage") != std::string::npos &&
                       tray_src.find("ui_lang_req.engine = LanguageEngine::Auto;") !=
                           std::string::npos &&
                       tray_src.find("I18n::LocaleToString(I18n::StringToLocale("
                                     "autoChecked ? \"auto\" : ui_language_))") !=
                           std::string::npos,
                   "P6-U4 (C): the UI-language PickerInit is latched at build time with the "
                   "canonical config-code spelling, the menu's own auto predicate (empty == "
                   "auto) and the inert Auto engine");

        // ---- (e) the dispatch arm is the P6 deferred-open shape ----------
        const std::size_t arm_pos = tray_src.find("cmd == ID_TRAY_UILANG_SEARCH");
        const std::size_t arm_end = tray_src.find("} else if (", arm_pos);
        const std::string arm_body =
            (arm_pos == std::string::npos || arm_end == std::string::npos)
                ? std::string()
                : tray_src.substr(arm_pos, arm_end - arm_pos);
        // Assembled at runtime: section (6) scans this file for the literal
        // call token, and this pin must not trip its own self-call check.
        const std::string deferred_call =
            std::string("PostDeferredLanguage") + "PaletteOpen(hOwner_, ui_lang_req)";
        TEST_CHECK(!arm_body.empty() &&
                       arm_body.find("::SetLastError(ERROR_SUCCESS)") != std::string::npos &&
                       arm_body.find(deferred_call) != std::string::npos,
                   "P6-U4 (C): the search-row arm is the P6 deferred-open shape verbatim "
                   "(PR6-03e SetLastError hygiene + the latched request posted by value)");
        TEST_CHECK(arm_body.find("deferred language-picker open post failed (GLE %lu)") !=
                           std::string::npos &&
                       arm_body.find("coalesced or rejected") != std::string::npos,
                   "P6-U4 (C): the arm keeps the cause-neutral diagnostic (a coalesced post is "
                   "never mislabelled as a failure)");
        TEST_CHECK(tray_src.find(
                       "cmd == ID_TRAY_UILANG_AUTO && callbacks_.on_select_ui_language") !=
                           std::string::npos &&
                       tray_src.find("callbacks_.on_select_ui_language(\"auto\");") !=
                           std::string::npos,
                   "P6-U4 (C): the Auto dispatch arm is unchanged (still dispatches \"auto\")");
        TEST_CHECK(tray_hdr.find("title's endonym suffix") != std::string::npos,
                   "P6-U4 (C): tray.hpp documents the title suffix as the current-locale "
                   "signal that replaced the retired checkmarked rows");
    }

    // ===================================================================
    // 261119_0001 P6-U5 (UI-language-89 phase 2, architect-pick.md unit "U5
    // controller dispatch"): the CONSUMER for the tray's third picker surface
    // (PickerSurface::TrayUiLanguage) is wired in main.cpp's
    // kMsgOpenLanguagePalette controller arm, and the committed REGISTRY code
    // is canonicalized through the EXISTING UI-language apply path (one
    // planner, one persist, one refresh). Pinned by SOURCE SCAN (the p5
    // section (10) idiom; the controller arm is GUI code and this suite links
    // core only):
    //   - the seam g_apply_ui_language is DECLARED beside g_apply_language_change,
    //     ASSIGNED from wWinMain's on_select_ui_language body, CLEARED at
    //     shutdown with the other coordinators, and CONSUMED inside the arm;
    //   - the PR6-42 order pin survives: LanguagePickerShowing() is checked
    //     BEFORE any latch take;
    //   - the drain polls all THREE tray-owned slots (TrayTyping, TrayTooltip,
    //     TrayUiLanguage, in that order);
    //   - the ownership guard accepts TrayUiLanguage alongside the two
    //     translation surfaces (a foreign surface is still refused before any
    //     picker open);
    //   - the commit branch is gated on picked.committed (a cancel dispatches
    //     nothing) and hands the committed REGISTRY code to the seam VERBATIM
    //     (261119_0001 FIX-R1 R1-07) - PlanUiLocaleChange owns validation and
    //     canonicalization, so the picked spelling reaches the planner
    //     unchanged and an unknown code is refused there instead of being
    //     coerced to "en" at this site;
    //   - the translation dispatch below EXCLUDES the UI-language surface
    //     (261119_0001 FIX-R1 R1-03): one commit can never fire both the UI
    //     apply and the language-change callback.
    // No window is opened here; section (6) scans this WHOLE file, so the
    // appended code stays inside the headless self-call pin (the two forbidden
    // window/deferred-open call tokens are never named below).
    // ===================================================================
    {
        auto read_repo_text = [](const wchar_t* rel) {
            std::string out;
            const std::filesystem::path path = ResolveRepoFile(rel);
            if (path.empty()) {
                return out;
            }
            std::ifstream in(path, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            }
            return out;
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
        const std::string main_src = read_repo_text(L"src\\main.cpp");
        TEST_CHECK(!main_src.empty(),
                   "P6-U5 (D): main.cpp is readable for the controller-dispatch pins");

        // ---- (a) the seam: declared, assigned, cleared ---------------------
        TEST_CHECK(main_src.find(
                       "std::function<void(std::string_view)> g_apply_ui_language;") !=
                       std::string::npos,
                   "P6-U5 (D): g_apply_ui_language is declared beside g_apply_language_change");
        TEST_CHECK(main_src.find("emebalachat::g_apply_ui_language = "
                                 "trayCallbacks.on_select_ui_language;") != std::string::npos,
                   "P6-U5 (D): the seam is assigned from wWinMain's existing "
                   "on_select_ui_language body (one planner, one persist, one refresh)");
        TEST_CHECK(main_src.find("emebalachat::g_apply_ui_language = nullptr;") !=
                       std::string::npos,
                   "P6-U5 (D): the seam is cleared at shutdown with the other coordinators");

        // ---- (b) the arm window (the P6c windowing idiom, re-derived) -------
        const std::size_t arm_at = main_src.find("if (msg == kMsgOpenLanguagePalette) {");
        const std::string arm_window =
            arm_at == std::string::npos ? std::string() : main_src.substr(arm_at, 16384);
        const std::size_t next_arm = arm_window.find("\n    if (msg == ", 8);
        const std::string arm_body =
            next_arm == std::string::npos ? arm_window : arm_window.substr(0, next_arm);
        const std::string arm_flat = squeeze(arm_body);

        // ---- (c) the PR6-42 order pin: the showing guard precedes the takes -
        const std::size_t showing_at = arm_body.find("LanguagePickerShowing()");
        const std::size_t first_take_at = arm_body.find("TakeDeferredLanguagePaletteOpen(");
        TEST_CHECK(showing_at != std::string::npos && first_take_at != std::string::npos &&
                       showing_at < first_take_at,
                   "P6-U5 (D): the kMsgOpenLanguagePalette arm exists and LanguagePickerShowing() "
                   "is checked BEFORE any latch take (the PR6-42 nested-delivery rule)");

        // ---- (d) the drain polls all three tray-owned slots, in order ------
        const std::size_t take_typing = arm_flat.find(
            "TakeDeferredLanguagePaletteOpen(language_palette::PickerSurface::TrayTyping)");
        const std::size_t take_tooltip = arm_flat.find(
            "TakeDeferredLanguagePaletteOpen(language_palette::PickerSurface::TrayTooltip)");
        const std::size_t take_uilang = arm_flat.find(
            "TakeDeferredLanguagePaletteOpen(language_palette::PickerSurface::TrayUiLanguage)");
        TEST_CHECK(take_typing != std::string::npos && take_tooltip != std::string::npos &&
                       take_uilang != std::string::npos && take_typing < take_tooltip &&
                       take_tooltip < take_uilang,
                   "P6-U5 (D): the drain polls all three tray-owned slots "
                   "(TrayTyping, TrayTooltip, TrayUiLanguage, in that order)");

        // ---- (e) the ownership guard accepts the third surface -------------
        const std::size_t mismatch_at = arm_flat.find("result=surface_mismatch");
        const std::string guard_window =
            mismatch_at == std::string::npos
                ? std::string()
                : arm_flat.substr(mismatch_at > 800 ? mismatch_at - 800 : 0, 800);
        TEST_CHECK(mismatch_at != std::string::npos &&
                       guard_window.find(
                           "init->surface!=language_palette::PickerSurface::TrayUiLanguage") !=
                           std::string::npos,
                   "P6-U5 (D): the ownership guard names TrayUiLanguage (the guard diagnostic "
                   "survives; a foreign surface is still refused before any picker open)");

        // ---- (f) the commit branch: committed-gated + VERBATIM dispatch -----
        TEST_CHECK(arm_flat.find("picked.committed&&picked.surface==language_palette::"
                                 "PickerSurface::TrayUiLanguage") != std::string::npos,
                   "P6-U5 (D): the UI-language commit branch is gated on picked.committed "
                   "(a cancel / click-outside dismissal dispatches nothing)");
        // 261119_0001 FIX-R1 R1-07: the dispatch passes the picked code through
        // UNCHANGED. The old LocaleToString(StringToLocale(...)) coercion ran
        // StringToLocale's English fallback first, so an unknown code reached
        // the planner as "en" and its refuse-unknown leg was bypassed.
        TEST_CHECK(arm_flat.find("g_apply_ui_language(picked.code)") != std::string::npos,
                   "P6-U5 (D): the commit dispatches picked.code VERBATIM through "
                   "g_apply_ui_language (the planner, not this site, canonicalizes)");
        TEST_CHECK(arm_flat.find("LocaleToString(I18n::StringToLocale(picked.code))") ==
                           std::string::npos &&
                       arm_flat.find("StringToLocale(picked.code)") == std::string::npos,
                   "P6-U5 (D): the arm performs NO locale coercion on the picked code (a "
                   "coercing site would re-open the English-fallback bypass the planner's "
                   "refusal closes)");

        // ---- (g) R1-03: the translation dispatch EXCLUDES the UI surface ----
        // The exclusion is what stops ONE commit from firing both
        // g_apply_ui_language and g_apply_language_change. Deleting the `!=`
        // (or flipping it to `==`) would double-dispatch on every UI-language
        // commit and silently write a translation pair, so it is pinned
        // explicitly and not merely implied by branch (f) above.
        TEST_CHECK(arm_flat.find("picked.committed&&picked.surface!=language_palette::"
                                 "PickerSurface::TrayUiLanguage") != std::string::npos,
                   "P6-U5 (D): the translation dispatch is gated on "
                   "picked.surface != TrayUiLanguage (single-commit, single-dispatch)");
        {
            // Order pin: the exclusion gate opens AFTER the UI branch closed,
            // i.e. the UI branch sits closer to the commit than the
            // translation branch, so the UI code is consumed before the
            // translation path can see the same commit.
            const std::size_t ui_branch = arm_flat.find(
                "picked.committed&&picked.surface==language_palette::"
                "PickerSurface::TrayUiLanguage");
            const std::size_t tr_branch = arm_flat.find(
                "picked.committed&&picked.surface!=language_palette::"
                "PickerSurface::TrayUiLanguage");
            TEST_CHECK(ui_branch != std::string::npos && tr_branch != std::string::npos &&
                           ui_branch < tr_branch &&
                           arm_flat.find("g_apply_language_change(", ui_branch) !=
                               std::string::npos,
                       "P6-U5 (D): the UI branch precedes the translation branch, so a UI "
                       "commit is consumed before the translation dispatch is considered");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261119_0001 P6-U1..U5 UI-language picker "
                     "(pool + mode + window + tray + dispatch) completed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261119_0001 P6-U1..U5 UI-language picker "
                     "(pool + mode + window + tray + dispatch): "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
