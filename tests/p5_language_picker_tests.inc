// ===========================================================================
// 261116_0003 P5 (language-menu redesign, plan §3 D3 + §4 "Search" + §5 P5):
// the shared searchable language picker's PURE LOGIC.
//
// This suite exercises ONLY language_palette::* (header-inline, zero Win32).
// It never constructs the popup: the only window-creating entry in the module
// (emebalachat::ShowLanguageSearchPalette) is not called here, and section (10)
// proves that structurally AND at runtime (zero windows of the picker class
// exist before and after this suite).
//
// Covered: the engine-view filter (a cloud-only language can never come back
// from a Hy-MT2 pool), best-match preselection (the CEO's five Korean queries),
// ↑↓ wraparound, Enter/double-click resolution with a byte-identical name_en
// payload, Esc, the Tab source↔target flip including the VIEW swap, the
// empty-query state (whole pool, registry order), the AUTO row's role
// asymmetry, the pinned-absent ghost predicate, and the two new i18n strings
// across all 37 UI locales.
// ===========================================================================
void TestP5LanguagePicker() {
    std::cout << "[RUN] Testing 261116_0003 P5 language search picker logic..." << std::endl;
    const int failures_before = g_failed_count;

    using language_palette::PickerCommand;
    using language_palette::PickerInit;
    using language_palette::PickerResolution;
    using language_palette::PickerRole;
    using language_palette::PickerState;
    using language_palette::PickerSurface;

    const auto indexOfHit = [](const std::vector<LanguageSearchHit>& hits, const char* code) {
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (hits[i].language->code == code) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const auto containsHit = [&](const std::vector<LanguageSearchHit>& hits, const char* code) {
        return indexOfHit(hits, code) >= 0;
    };
    // Every returned row must come from the pool the state searched (the
    // matcher's containment contract, checked through the picker's own view).
    const auto allHitsInPool = [](const PickerState& state) {
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

    // ---- 1) engine-view filtering: a cloud-only language never leaks into a
    //         local (Hy-MT2) pool, and the same query does find it in a cloud
    //         pool. HU (Hungarian) is a cloud-only row (P2 D7); "magyar" is its
    //         native name (a query that can only match through the alias table).
    {
        PickerInit init;
        init.surface = PickerSurface::TrayTyping;
        init.role = PickerRole::Target;
        init.engine = LanguageEngine::HyMt2;
        init.target_code = "Korean";
        PickerState local(init);
        TEST_CHECK(local.pool().size() == GetLanguagesForEngine(LanguageEngine::HyMt2, false).size(),
                   "P5 (1): the target role searches the engine's TARGET view (AUTO dropped)");
        TEST_CHECK(!local.SetQuery("hungarian") && local.hits().empty(),
                   "P5 (1): 'hungarian' returns nothing from the Hy-MT2 view (HU is cloud-only)");
        TEST_CHECK(!local.SetQuery("magyar") && local.hits().empty(),
                   "P5 (1): 'magyar' (HU's native name) returns nothing from the Hy-MT2 view");
        TEST_CHECK(!local.SetQuery("swahili") && local.hits().empty(),
                   "P5 (1): a language the registry does not hold returns nothing");
        TEST_CHECK(allHitsInPool(local),
                   "P5 (1): the Hy-MT2 view never returns a row outside its pool");

        PickerInit cloud = init;
        cloud.engine = LanguageEngine::OpenAi;
        PickerState online(cloud);
        TEST_CHECK(online.SetQuery("hungarian") && online.hits()[0].language->code == "HU",
                   "P5 (1): the same query hits HU first in the OpenAI-compatible view");
        TEST_CHECK(online.SetQuery("magyar") && online.hits()[0].language->code == "HU",
                   "P5 (1): the native-name query hits HU in the cloud view too");

        // Containment across a spread of queries in both views (a ranking bug
        // that appended out-of-pool rows would show up here).
        const char* queries[] = {"korean", "ko", "zh", "cyrillic", "arabic", "francais", "tieng viet"};
        bool contained = true;
        for (const char* query : queries) {
            online.SetQuery(query);
            if (!allHitsInPool(online)) {
                contained = false;
            }
            local.SetQuery(query);
            if (!allHitsInPool(local)) {
                contained = false;
            }
        }
        TEST_CHECK(contained,
                   "P5 (1): every hit is a pool row across 7 queries x both engine views");
    }

    // ---- 2) best-match preselection (hits[0], never re-sorted) -------------
    // The CEO's five Korean query styles all rank KO first (P4 pin) AND the
    // picker preselects hits[0] on every keystroke.
    {
        PickerInit init;
        init.role = PickerRole::Source;
        init.engine = LanguageEngine::HyMt2;
        init.source_code = "Auto Detect";
        PickerState state(init);
        const char* korean_queries[] = {"한국어", "korean", "koreanisch", "한글", "KO"};
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
                   "P5 (2): 한국어/korean/koreanisch/한글/KO each preselect KO at row 0");
        // A keystroke always resets the selection to the best match, even when
        // the user had arrowed away from row 0. 261116_0003 PR5-01(a): the pin
        // ran on "korean" — a SINGLE hit in the Hy-MT2 view — so the MoveDown
        // could never leave row 0 and a SetQuery that kept the previous
        // selection passed the pin too. Re-pointed at "chinese" (the ZH-CN /
        // ZH-TW rows match it in the Hy-MT2 view): the MoveDown really moves,
        // and the reseat is visible against it.
        state.SetQuery("chinese");
        TEST_CHECK(state.hits().size() >= 2,
                   "P5 (2): the re-query pin is non-vacuous (a multi-hit query, >=2 hits)");
        state.Apply(PickerCommand::MoveDown);
        TEST_CHECK(state.selection() == 1,
                   "P5 (2): the setup MoveDown really moved off row 0 (multi-hit list)");
        state.SetQuery("chinese");
        TEST_CHECK(state.selection() == 0,
                   "P5 (2): a re-query reseats the selection at row 0 (best match)");
    }

    // ---- 3) ↑↓ with wraparound --------------------------------------------
    {
        PickerInit init;
        init.role = PickerRole::Target;
        init.engine = LanguageEngine::HyMt2;
        init.target_code = "Korean";
        PickerState state(init);
        const int n = static_cast<int>(state.hits().size());
        TEST_CHECK(n == 38 && n > 2, "P5 (3): the empty query lists the whole 38-row Hy-MT2 target pool");
        const int ko = indexOfHit(state.hits(), "KO");
        TEST_CHECK(state.selection() == ko && ko >= 0,
                   "P5 (3): the empty state preselects the role's CURRENT language (KO)");

        // 261116_0003 PR5-01(d): the not-in-view -> row-0 start-seat fallback,
        // pinned on a MULTI-hit list (the flip-side pin in section 6 runs on a
        // 1-row filtered list, where every seat equals row 0 and the fallback
        // is invisible). "Hungarian" is a cloud-only pin under the Hy-MT2
        // engine: the whole 38-row target pool cannot contain it, so
        // construction must seat the selection at row 0 — never -1, never
        // another row.
        {
            PickerInit absent;
            absent.role = PickerRole::Target;
            absent.engine = LanguageEngine::HyMt2;
            absent.target_code = "Hungarian";
            PickerState absent_state(absent);
            TEST_CHECK(absent_state.hits().size() >= 2,
                       "P5 (3): the start-seat fallback pin is non-vacuous (multi-hit pool)");
            TEST_CHECK(absent_state.selection() == 0,
                       "P5 (3): a current language absent from a MULTI-hit view seats the "
                       "start selection at row 0 (not -1, not another row)");
        }

        // ↓ from the last row wraps to row 0; ↑ from row 0 wraps to the last.
        state.SelectRow(n - 1);
        const PickerResolution down = state.Apply(PickerCommand::MoveDown);
        TEST_CHECK(down.changed && state.selection() == 0,
                   "P5 (3): ↓ from the last row wraps to row 0");
        const PickerResolution up = state.Apply(PickerCommand::MoveUp);
        TEST_CHECK(up.changed && state.selection() == n - 1,
                   "P5 (3): ↑ from row 0 wraps to the last row");
        // A multi-step move normalizes modulo the size (no out-of-range index).
        state.SelectRow(0);
        state.MoveSelection(+3);
        TEST_CHECK(state.selection() == 3, "P5 (3): a +3 step lands 3 rows down");
        state.MoveSelection(-2 * n);
        TEST_CHECK(state.selection() == 3, "P5 (3): a full-multiple step is a no-op");
        state.MoveSelection(-5);
        TEST_CHECK(state.selection() == (3 - 5 + n) % n,
                   "P5 (3): a negative step wraps through the list");
        // A short result list: ↑↓ stay inside it and return to the same row.
        // 261116_0003 PR5-01(a): re-pointed at a MULTI-hit query — on the old
        // single-hit "korean" list, wraparound and clamping coincide, so a
        // MoveSelection that clamped instead of wrapping passed this pin too.
        state.SetQuery("chinese");
        TEST_CHECK(state.hits().size() >= 2,
                   "P5 (3): the short-list setup resolves to a multi-hit list (>=2 hits)");
        const int short_rows = static_cast<int>(state.hits().size());
        const int seat = state.selection();
        state.MoveUp();
        const int after_up = state.selection();
        state.MoveDown();
        TEST_CHECK(after_up >= 0 && after_up < short_rows && state.selection() == seat,
                   "P5 (3): ↑ then ↓ returns to the same row inside a short result list");
        // No rows at all: the selection is -1 and the moves report no change.
        TEST_CHECK(!state.SetQuery("zzzzz-no-such-language") && state.selection() == -1,
                   "P5 (3): an empty result list has no selection (-1)");
        const PickerResolution move_on_empty = state.Apply(PickerCommand::MoveDown);
        TEST_CHECK(!move_on_empty.changed && state.selection() == -1,
                   "P5 (3): ↓ on an empty result list changes nothing");
    }

    // ---- 4) Enter commits the preselected hit, name_en byte-identical ------
    {
        PickerInit init;
        init.role = PickerRole::Target;
        init.engine = LanguageEngine::HyMt2;
        init.target_code = "English";
        PickerState state(init);
        state.SetQuery("korean");
        const LanguageInfo* registry_ko = FindLanguageByCode("KO");
        TEST_CHECK(registry_ko != nullptr, "P5 (4): the KO registry row resolves");
        const PickerResolution commit = state.Apply(PickerCommand::Commit);
        TEST_CHECK(commit.committed && commit.row != nullptr,
                   "P5 (4): Enter commits the preselected row");
        TEST_CHECK(commit.row == registry_ko,
                   "P5 (4): the committed row IS the registry row (pointer identity)");
        TEST_CHECK(PickerState::PayloadOf(commit.row) == "Korean",
                   "P5 (4): the payload is the registry name_en verbatim");
        TEST_CHECK(std::string(PickerState::PayloadOf(commit.row)) ==
                       std::string(registry_ko->name_en),
                   "P5 (4): payload bytes are byte-identical to the registry row's name_en "
                   "(the tray's on_select_*_lang contract)");
        TEST_CHECK(PickerState::PayloadOf(nullptr).empty(),
                   "P5 (4): a null row yields an empty payload (nothing dispatched)");
        // 261116_0003 P5 mutation-probe hardening: the single-hit case above
        // cannot see a "commit hits_[1]" mutation (hits_[1] does not exist), so
        // the commit-row selection is pinned again on a MULTI-hit query where
        // row 0 != row 1. "chinese" matches ZH-CN and ZH-TW inside the HyMt2
        // view; Enter must commit the preselection (row 0), never the runner-up.
        {
            PickerInit multi;
            multi.role = PickerRole::Target;
            multi.engine = LanguageEngine::HyMt2;
            multi.target_code = "English";
            PickerState state(multi);
            // 261116_0003 PR5-11: EARLY GUARD, not a continuing pin. TEST_CHECK
            // increments the counters and CONTINUES (run_tests.cpp ~83-90), so
            // guarding the precondition with it left the hits()[1] index below
            // reachable when the setup drifts — an OOB read instead of a clean
            // red check. The pin body now runs ONLY when the precondition holds;
            // the check itself still records the failure.
            const bool multi_hit_ready = state.SetQuery("chinese") && state.hits().size() >= 2;
            TEST_CHECK(multi_hit_ready,
                       "P5 (4): the multi-hit commit pin is non-vacuous (chinese has >=2 hits)");
            if (multi_hit_ready) {
                const LanguageInfo* first = state.hits()[0].language;
                const LanguageInfo* second = state.hits()[1].language;
                TEST_CHECK(first != second,
                           "P5 (4): the multi-hit pin's rows 0 and 1 differ (mutation-visible)");
                const PickerResolution multi_commit = state.Apply(PickerCommand::Commit);
                TEST_CHECK(multi_commit.committed && multi_commit.row == first,
                           "P5 (4): Enter on a multi-hit list commits the PRESELECTED row, not the "
                           "runner-up (mutation probe 261116_0003 P5)");
            }
        }
        // 261116_0003 PR5-01(b): Commit-after-MoveDown on a multi-hit list.
        // Enter must commit the MOVED-TO row, never a fixed row: every
        // pre-existing commit pin sat on row 0 or on a 1-row list, so a
        // Commit()->hits_[0] mutation passed all 83 checks. Two seats close
        // the class: (1) the filtered multi-hit list right after a MoveDown
        // (catches ->hits_[0]), (2) the whole-pool list from the role's start
        // seat KO — neither row 0 nor adjacent to the last row (catches
        // ->hits_[1] and ->back() as well).
        {
            PickerInit filtered;
            filtered.role = PickerRole::Target;
            filtered.engine = LanguageEngine::HyMt2;
            filtered.target_code = "English";
            PickerState moved(filtered);
            TEST_CHECK(moved.SetQuery("chinese") && moved.hits().size() >= 2,
                       "P5 (4): the Commit-after-MoveDown pin is non-vacuous (multi-hit query)");
            moved.Apply(PickerCommand::MoveDown);
            const int moved_to = moved.selection();
            // 261116_0003 PR5-08: validate the selection before indexing hits()
            // — a failed setup must be a RED CHECK here, never an OOB read.
            TEST_CHECK(moved_to >= 0 && moved_to < static_cast<int>(moved.hits().size()),
                       "P5 (4): the MoveDown selection is inside the hit list before indexing");
            const LanguageInfo* moved_row =
                (moved_to >= 0 && moved_to < static_cast<int>(moved.hits().size()))
                    ? moved.hits()[static_cast<std::size_t>(moved_to)].language
                    : nullptr;
            TEST_CHECK(moved_to == 1 && moved_row != nullptr &&
                           moved_row != moved.hits()[0].language,
                       "P5 (4): the MoveDown left the preselection (mutation-visible setup)");
            const PickerResolution moved_commit = moved.Apply(PickerCommand::Commit);
            TEST_CHECK(moved_commit.committed && moved_commit.row == moved_row,
                       "P5 (4): Enter after MoveDown on a multi-hit list commits the MOVED-TO "
                       "row, not hits_[0] (a Commit()->hits_[0] mutation fails here)");
        }
        {
            PickerInit seated_init;
            seated_init.role = PickerRole::Target;
            seated_init.engine = LanguageEngine::HyMt2;
            seated_init.target_code = "Korean";
            PickerState seated(seated_init);
            const int pool_rows = static_cast<int>(seated.hits().size());
            const int ko_seat = seated.selection(); // the role's current language, in-pool
            TEST_CHECK(pool_rows >= 4 && ko_seat > 0 && ko_seat < pool_rows - 2,
                       "P5 (4): the pool Commit-after-MoveDown pin is non-vacuous (KO's seat "
                       "is neither row 0 nor adjacent to the last row)");
            seated.Apply(PickerCommand::MoveDown);
            // 261116_0003 PR5-08: validate the selection before indexing hits()
            // — a failed setup must be a RED CHECK here, never an OOB read.
            const int below_index = seated.selection();
            TEST_CHECK(below_index >= 0 && below_index < pool_rows,
                       "P5 (4): the pool pin's post-MoveDown selection is inside the hit list "
                       "before indexing");
            const LanguageInfo* below_seat =
                (below_index >= 0 && below_index < pool_rows)
                    ? seated.hits()[static_cast<std::size_t>(below_index)].language
                    : nullptr;
            const PickerResolution seated_commit = seated.Apply(PickerCommand::Commit);
            TEST_CHECK(seated_commit.committed && below_seat != nullptr &&
                           seated_commit.row == below_seat,
                       "P5 (4): Enter after MoveDown from the start seat commits the row below "
                       "it (a Commit()->hits_[1] / last-hit mutation fails here)");
        }
        // Double-click commits the row it landed on, which may differ from the
        // preselection. 261116_0003 PR5-01(c): the old pin asserted
        // PayloadOf(row) == row->name_en — a tautology true for ANY row — so a
        // CommitRow(i)->hits_[0] mutation passed. Capture the row at the
        // clicked index BEFORE the apply and assert pointer identity after.
        // 261116_0003 PR5-07/PR5-08: the pin's OWN local guard — >=2 hits and
        // the clicked row differs from hits_[0] — so a future alias-table
        // drift yields a RED CHECK here instead of silently blinding the pin
        // (and the conditional indexing keeps a 0/1-hit drift from ever
        // touching hits_[-1], which would be UB, not a failure).
        state.SetQuery("chinese");
        const int hit_count = static_cast<int>(state.hits().size());
        TEST_CHECK(hit_count >= 2,
                   "P5 (4): the double-click pin is non-vacuous (a multi-hit query, >=2 hits)");
        const int clicked_index = hit_count - 1;
        const LanguageInfo* const first_hit =
            hit_count > 0 ? state.hits()[0].language : nullptr;
        const LanguageInfo* clicked_row =
            clicked_index >= 0 ? state.hits()[static_cast<std::size_t>(clicked_index)].language
                               : nullptr;
        TEST_CHECK(clicked_row != nullptr && clicked_row != first_hit,
                   "P5 (4): the clicked row differs from hits_[0] (mutation-visible)");
        const PickerResolution click = state.Apply(PickerCommand::ActivateRow, clicked_index);
        TEST_CHECK(click.committed && click.row == clicked_row,
                   "P5 (4): a double-click commits the row AT the clicked index (pointer "
                   "identity — a CommitRow(i)->hits_[0] mutation fails here)");
        TEST_CHECK(!state.Apply(PickerCommand::ActivateRow, -1).committed &&
                       !state.Apply(PickerCommand::ActivateRow, 9999).committed,
                   "P5 (4): an out-of-range double-click commits nothing");
        // Enter on an empty result list is a documented no-op (no commit, no
        // close): the popup stays open so the user can keep typing.
        state.SetQuery("zzzzz-no-such-language");
        const PickerResolution enter_empty = state.Apply(PickerCommand::Commit);
        TEST_CHECK(!enter_empty.committed && !enter_empty.cancelled &&
                       enter_empty.row == nullptr,
                   "P5 (4): Enter on an empty result list commits nothing and cancels nothing");
    }

    // ---- 5) Esc yields no commit ------------------------------------------
    {
        PickerInit init;
        init.role = PickerRole::Target;
        init.engine = LanguageEngine::HyMt2;
        init.target_code = "Korean";
        PickerState state(init);
        state.SetQuery("korean");
        const int selection_before = state.selection();
        const std::string query_before = state.query();
        const PickerResolution cancel = state.Apply(PickerCommand::Cancel);
        TEST_CHECK(cancel.cancelled && !cancel.committed && cancel.row == nullptr,
                   "P5 (5): Esc cancels with no committed row");
        TEST_CHECK(state.selection() == selection_before && state.query() == query_before,
                   "P5 (5): Esc mutates nothing (the caller discards, the state is inert)");
    }

    // ---- 6) Tab flips source↔target, including the view swap ---------------
    {
        PickerInit init;
        init.surface = PickerSurface::TrayTyping;
        init.role = PickerRole::Source;
        init.engine = LanguageEngine::HyMt2;
        init.source_code = "Korean";
        init.target_code = "English";
        PickerState state(init);
        const std::size_t source_pool = GetLanguagesForEngine(LanguageEngine::HyMt2, true).size();
        const std::size_t target_pool = GetLanguagesForEngine(LanguageEngine::HyMt2, false).size();
        TEST_CHECK(state.role() == PickerRole::Source && state.pool().size() == source_pool,
                   "P5 (6): the source role starts on the AUTO-inclusive view");
        TEST_CHECK(containsHit(state.hits(), "AUTO"),
                   "P5 (6): the source view offers the AUTO row");
        const int ko_src = indexOfHit(state.hits(), "KO");
        TEST_CHECK(state.selection() == ko_src,
                   "P5 (6): the source role preselects its current language (KO)");

        const PickerResolution flip = state.Apply(PickerCommand::FlipRole);
        TEST_CHECK(flip.changed && state.role() == PickerRole::Target,
                   "P5 (6): Tab flips the role to Target");
        TEST_CHECK(state.pool().size() == target_pool && target_pool + 1 == source_pool,
                   "P5 (6): the flip SWAPS the engine view (AUTO dropped for a target)");
        TEST_CHECK(!containsHit(state.hits(), "AUTO"),
                   "P5 (6): the target view never offers the AUTO row");
        TEST_CHECK(state.selection() == indexOfHit(state.hits(), "EN"),
                   "P5 (6): the flip preselects the TARGET role's current language (EN)");

        state.Apply(PickerCommand::FlipRole);
        TEST_CHECK(state.role() == PickerRole::Source && state.pool().size() == source_pool &&
                       state.selection() == ko_src,
                   "P5 (6): a second Tab flips back — view, role and selection restored");

        // The query survives the flip (type once, Tab, pick the other role).
        state.SetQuery("korean");
        const std::size_t filtered = state.hits().size();
        state.Apply(PickerCommand::FlipRole);
        TEST_CHECK(state.role() == PickerRole::Target && state.query() == "korean" &&
                       state.hits().size() == filtered && state.selection() == 0,
                   "P5 (6): the flip keeps the query and reseats row 0 when the role's "
                   "current language is not in the filtered list");
        const PickerResolution commit = state.Apply(PickerCommand::Commit);
        TEST_CHECK(commit.committed && PickerState::PayloadOf(commit.row) == "Korean",
                   "P5 (6): a commit after the flip carries the same name_en payload "
                   "(only the role — i.e. the callback P6 picks — changed)");

        // Engine identity survives the flip too (the view is re-derived from
        // the same engine, never from a cached pool).
        TEST_CHECK(state.engine() == LanguageEngine::HyMt2,
                   "P5 (6): the flip re-derives the view from the same engine");

        // 261116_0003 PR5-01(d): the flip's not-in-view -> row-0 reseat,
        // pinned on a MULTI-hit filtered list (the pin above runs on a 1-row
        // list, where any seat equals row 0 and the fallback is invisible).
        // The target role's current value is a cloud-only pin ("Hungarian"):
        // after the flip the kept "chinese" query offers the ZH rows only,
        // none of which is the current language, so the reseat must land on
        // row 0 — not -1, not another row.
        {
            PickerInit flip_absent;
            flip_absent.surface = PickerSurface::TrayTyping;
            flip_absent.role = PickerRole::Source;
            flip_absent.engine = LanguageEngine::HyMt2;
            flip_absent.source_code = "Korean";
            flip_absent.target_code = "Hungarian";
            PickerState flipped(flip_absent);
            TEST_CHECK(flipped.SetQuery("chinese") && flipped.hits().size() >= 2,
                       "P5 (6): the flip reseat pin is non-vacuous (multi-hit filtered list)");
            flipped.Apply(PickerCommand::FlipRole);
            TEST_CHECK(flipped.role() == PickerRole::Target && flipped.hits().size() >= 2 &&
                           flipped.selection() == 0,
                       "P5 (6): after a flip whose kept multi-hit query excludes the role's "
                       "current language, the selection seats at row 0 (not -1)");
        }
    }

    // ---- 7) the empty-query state: whole pool, registry order, tier None ---
    {
        PickerInit init;
        init.role = PickerRole::Target;
        init.engine = LanguageEngine::Google;
        init.target_code = "English";
        PickerState state(init);
        const auto& google = GetLanguagesForEngine(LanguageEngine::Google, false);
        bool order_ok = state.hits().size() == google.size();
        bool tier_ok = true;
        for (std::size_t i = 0; order_ok && i < state.hits().size(); ++i) {
            if (state.hits()[i].language != google[i]) {
                order_ok = false;
            }
            if (state.hits()[i].tier != AliasTier::None) {
                tier_ok = false;
            }
        }
        TEST_CHECK(order_ok,
                   "P5 (7): an empty query returns the whole pool in registry order");
        TEST_CHECK(tier_ok,
                   "P5 (7): every empty-query hit carries tier None (the unfiltered marker)");
        TEST_CHECK(!state.Filtering(),
                   "P5 (7): an empty query is not 'filtering'");

        // Whitespace-only queries are the unfiltered state (the matcher folds
        // ASCII whitespace, tab/CR/LF included, to nothing).
        const char* blanks[] = {"   ", "\t", "\r\n", " \t\r\n "};
        bool blanks_unfiltered = true;
        for (const char* blank : blanks) {
            state.SetQuery(blank);
            if (state.Filtering() || state.hits().size() != google.size() ||
                state.hits().empty() || state.hits()[0].language != google[0]) {
                blanks_unfiltered = false;
            }
        }
        TEST_CHECK(blanks_unfiltered,
                   "P5 (7): a whitespace-only query keeps the whole pool in registry order");
        // ClearQuery returns to exactly the construction state.
        state.SetQuery("korean");
        state.SelectRow(0);
        state.ClearQuery();
        TEST_CHECK(!state.Filtering() && state.hits().size() == google.size() &&
                       state.selection() == indexOfHit(state.hits(), "EN"),
                   "P5 (7): ClearQuery restores the pool and the role's current-language seat");
    }

    // ---- 8) the AUTO row: offered as a SOURCE, never as a TARGET -----------
    {
        PickerInit src;
        src.role = PickerRole::Source;
        src.engine = LanguageEngine::HyMt2;
        src.source_code = "Auto Detect";
        src.target_code = "English";
        PickerState source_state(src);
        TEST_CHECK(containsHit(source_state.hits(), "AUTO"),
                   "P5 (8): the source role's unfiltered list contains AUTO");
        TEST_CHECK(source_state.SetQuery("auto detect") &&
                       source_state.hits()[0].language->code == "AUTO",
                   "P5 (8): 'auto detect' ranks AUTO first in the source role");
        const PickerResolution auto_commit = source_state.Apply(PickerCommand::Commit);
        TEST_CHECK(auto_commit.committed &&
                       PickerState::PayloadOf(auto_commit.row) == "Auto Detect",
                   "P5 (8): committing AUTO yields the registry payload 'Auto Detect'");
        TEST_CHECK(source_state.PinnedAbsentLanguage() == nullptr,
                   "P5 (8): AUTO is never reported as a pinned-absent row");

        PickerInit tgt;
        tgt.role = PickerRole::Target;
        tgt.engine = LanguageEngine::HyMt2;
        tgt.target_code = "Auto Detect"; // a persisted AUTO target must not become a ghost
        PickerState target_state(tgt);
        TEST_CHECK(!containsHit(target_state.hits(), "AUTO"),
                   "P5 (8): the target role's view never contains AUTO");
        target_state.SetQuery("auto");
        TEST_CHECK(!containsHit(target_state.hits(), "AUTO"),
                   "P5 (8): no query can surface AUTO in the target role");
        TEST_CHECK(target_state.PinnedAbsentLanguage() == nullptr,
                   "P5 (8): an AUTO target value is not a pinned-absent ghost");
    }

    // ---- 9) pinned-absent ghost predicate (the ROW itself is P6 scope) -----
    {
        PickerInit local;
        local.role = PickerRole::Target;
        local.engine = LanguageEngine::HyMt2;
        local.target_code = "Hungarian";
        PickerState local_state(local);
        TEST_CHECK(local_state.PinnedAbsentLanguage() == FindLanguageByCode("HU"),
                   "P5 (9): a cloud-only pin under the Hy-MT2 engine is reported as pinned-absent");

        PickerInit google;
        google.role = PickerRole::Target;
        google.engine = LanguageEngine::Google;
        google.target_code = "Tibetan"; // BO carries no kLangGoogle bit
        PickerState google_state(google);
        TEST_CHECK(google_state.PinnedAbsentLanguage() == FindLanguageByCode("BO"),
                   "P5 (9): BO (no Google bit) is pinned-absent under the Google engine");
        google.target_code = "Hungarian"; // offered by Google -> not a ghost
        PickerState google_hu(google);
        TEST_CHECK(google_hu.PinnedAbsentLanguage() == nullptr,
                   "P5 (9): a language the engine offers is never reported as a ghost");
        PickerState local_hu(local);
        TEST_CHECK(local_hu.PinnedAbsentLanguage() != nullptr,
                   "P5 (9): the same pin IS a ghost under the local engine (engine-dependent)");

        PickerInit unknown = local;
        unknown.target_code = "Klingon"; // free-form value, not in the registry
        PickerState unknown_state(unknown);
        TEST_CHECK(unknown_state.PinnedAbsentLanguage() == nullptr,
                   "P5 (9): an unregistered free-form pin is not a ghost row");

        PickerInit unset = local;
        unset.target_code.clear();
        PickerState unset_state(unset);
        TEST_CHECK(unset_state.PinnedAbsentLanguage() == nullptr,
                   "P5 (9): an unset role value is not a ghost row");
    }

    // ---- 10) no window: structural + runtime proof -------------------------
    // (a) the pure section of the header contains no window/message-loop call;
    // (b) the .cpp is the only place a window can be created;
    // (c) this suite never names the window entry (the needles are assembled at
    //     runtime so this file cannot match itself);
    // (d) ZERO windows of the picker class exist while the suite runs.
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
        const std::string header_src = read_repo_text(L"src\\ui\\language_search_palette.hpp");
        const std::string cpp_src = read_repo_text(L"src\\ui\\language_search_palette.cpp");
        TEST_CHECK(!header_src.empty() && !cpp_src.empty(),
                   "P5 (10): the palette module sources are readable for the structural pins");

        // The unique SECTION B banner (the file-header comment also names
        // "SECTION B" in prose, so the marker must be the banner's own text —
        // anchoring on the bare token would truncate the scanned prefix).
        const std::string marker = "SECTION B — the Win32 window (never";
        const std::size_t split = header_src.find(marker);
        TEST_CHECK(split != std::string::npos,
                   "P5 (10): the header carries the SECTION A / SECTION B seam marker");
        const std::string pure_section = split == std::string::npos
                                             ? std::string()
                                             : header_src.substr(0, split);
        // ...and the marker really sits AFTER the logic layer, so the scanned
        // prefix covers the whole of it (not just the file header comment).
        const std::size_t logic_ns = header_src.find("namespace language_palette {");
        const std::size_t logic_class = header_src.find("class PickerState {");
        TEST_CHECK(logic_ns != std::string::npos && logic_class != std::string::npos &&
                       logic_ns < logic_class && split != std::string::npos &&
                       logic_class < split,
                   "P5 (10): the SECTION A prefix contains the whole logic layer "
                   "(namespace + PickerState) it is meant to prove Win32-free");
        // Scan CODE only: the logic layer's prose is allowed to NAME the Win32
        // APIs it deliberately does not call (PickerInit's "no pointer ever
        // crosses PostMessageW" comment), so comments are stripped first.
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
                   "P5 (10): SECTION A (the logic layer) contains no window, message-loop "
                   "or control-message call — it cannot create a window by construction");

        TEST_CHECK(cpp_src.find("CreateWindowExW") != std::string::npos &&
                       cpp_src.find("WS_POPUP") != std::string::npos,
                   "P5 (10): the window layer is the WS_POPUP CreateWindowExW path (plan D3)");
        TEST_CHECK(header_src.find("CreateWindow") == std::string::npos,
                   "P5 (10): the header itself never creates a window");
        TEST_CHECK(cpp_src.find("EM_SETCUEBANNER") != std::string::npos &&
                       cpp_src.find("LISTBOX") != std::string::npos &&
                       cpp_src.find("\"EDIT\"") != std::string::npos,
                   "P5 (10): the popup is the plan-D3 cue-banner EDIT + LISTBOX pair");
        TEST_CHECK(cpp_src.find("GetMessageW") != std::string::npos,
                   "P5 (10): the popup runs its OWN modal loop (no dialog box)");
        // The reported role/surface must be the state AT COMMIT time: a
        // Tab-then-Enter pick belongs to the flipped role, and echoing the
        // request's role instead would fire the wrong P6 callback.
        TEST_CHECK(cpp_src.find("result.surface = w.state.surface();") != std::string::npos &&
                       cpp_src.find("result.role = w.state.role();") != std::string::npos,
                   "P5 (10): the committed role/surface are read at commit time, not from "
                   "the open request");

        // (c) this suite never calls the window entry.
        const std::string self_src = read_repo_text(L"tests\\p5_language_picker_tests.inc");
        TEST_CHECK(!self_src.empty(),
                   "P5 (10): this suite's own source is readable for the self-call pin");
        const std::string show_entry = std::string("ShowLanguageSearch") + "Palette(";
        const std::string deferred_entry = std::string("PostDeferredLanguage") + "PaletteOpen(";
        TEST_CHECK(self_src.find(show_entry) == std::string::npos &&
                       self_src.find(deferred_entry) == std::string::npos,
                   "P5 (10): the suite never calls the window/deferred-open entries");

        // (d) runtime: no window of the picker class exists now. The class name
        // is duplicated here ON PURPOSE (the .cpp keeps it in an anonymous
        // namespace); the pin above fails loudly if the .cpp ever renames it.
        const wchar_t* kPaletteClass = L"Emebalachat_LanguageSearchPalette";
        TEST_CHECK(cpp_src.find("Emebalachat_LanguageSearchPalette") != std::string::npos,
                   "P5 (10): the pinned class name matches the .cpp's registered class");
        TEST_CHECK(::FindWindowW(kPaletteClass, nullptr) == nullptr,
                   "P5 (10): no picker window exists during the headless suite");
    }

    // ---- 11) the picker's own strings: 37 locales, real translations -------
    {
        const UiLocale initial = I18n::GetCurrentLocale();
        const auto& locales = GetSupportedUiLocales();
        TEST_CHECK(locales.size() == 37,
                   "P5 (11): the picker's two strings are authored for all 37 UI locales");
        I18n::SetLocale(UiLocale::English);
        const std::wstring en_cue = I18n::Get(StringId::LanguageSearchCue);
        const std::wstring en_no_match = I18n::Get(StringId::LanguageSearchNoMatch);
        TEST_CHECK(en_cue == L"Search languages…" && en_no_match == L"No matching language",
                   "P5 (11): the English literals are the struct defaults");

        int non_empty = 0;
        int english_copy = 0;
        for (const UiLocaleEntry& entry : locales) {
            I18n::SetLocale(entry.locale);
            const std::wstring cue = I18n::Get(StringId::LanguageSearchCue);
            const std::wstring no_match = I18n::Get(StringId::LanguageSearchNoMatch);
            if (!cue.empty() && !no_match.empty()) {
                ++non_empty;
            }
            if (entry.locale != UiLocale::English &&
                (cue == en_cue || no_match == en_no_match)) {
                ++english_copy;
            }
        }
        I18n::SetLocale(UiLocale::Korean);
        TEST_CHECK(I18n::Get(StringId::LanguageSearchCue) == L"언어 검색…" &&
                       I18n::Get(StringId::LanguageSearchNoMatch) == L"일치하는 언어가 없습니다",
                   "P5 (11): the Korean literals are authored (not the English defaults)");
        I18n::SetLocale(initial); // suite-state hygiene
        TEST_CHECK(non_empty == 37,
                   "P5 (11): both picker strings resolve non-empty in all 37 locales");
        TEST_CHECK(english_copy == 0,
                   "P5 (11): no non-English locale falls back to the English text");

        // Enum placement: appended after the user-model block, below the
        // sentinel — the completeness loop therefore covers them.
        volatile int theme_tail = static_cast<int>(StringId::ThemeMonolith);
        volatile int first_new = static_cast<int>(StringId::LanguageSearchCue);
        volatile int last_new = static_cast<int>(StringId::LanguageSearchNoMatch);
        volatile int sentinel = static_cast<int>(StringId::EnumCount);
        TEST_CHECK(first_new > theme_tail && last_new > first_new && last_new < sentinel &&
                       sentinel - 1 == last_new,
                   "P5 (11): the two ids are contiguous and sit at the enum tail");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P5 language picker logic completed." << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P5 language picker logic: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
