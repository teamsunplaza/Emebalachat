// ---------------------------------------------------------------------------
// v0.10.10 phased fixes (261122_0002 session, unit P2-C: tooltip surface).
// Five of the six fixes are user-visible GUI behavior inside the layered
// window (pin state, live preview card, RTL swap, cached layout, anchor DPI)
// and are NOT unit-testable without an interactive desktop; this suite covers
// the PURE factors the unit introduced and pins the structural seams over the
// sources, the ws3_tooltip_tests.inc idiom.
//
//   (1) TooltipWindow::CardWidthDipForContentChars - the L1-B width tiers
//       (<=400 -> 360, <=1200 -> 480, else 620 DIP).
//   (2) TooltipWindow::MaxCardHeightDipForWorkArea - the L1-B "70% of the
//       anchoring monitor's work area" cap.
//   (3) TooltipWindow::PlanHeaderActionCluster - the right-anchored
//       [pin][edit][open] cluster (labelled vs icon-only, never over the ✕).
//   (4) TryLcidOverride + GetLcidForLanguageWithResolver - the FUP-06
//       bcp47 -> LCID rewrite, driven through an INJECTED LocaleNameToLCID so
//       the rule is provable without NLS dependencies.
//   (5) TooltipWindow::PlanBackTransViewPanels - the FUP-05 split/solo toggle
//       (additive over the frozen PlanBackTransPanels contract).
//   (6) structural pins: the L3-B preview seams + value queue, the FUP-08
//       cached-layout lifecycle, the FUP-05 per-pane offsets and RTL paths,
//       and the L1-B pin gate on Dismiss().
//
// Zero windows, zero network, zero DirectWrite: pure math + source reads.
// ---------------------------------------------------------------------------
namespace p2c_test {

// Injected LocaleNameToLCID stand-in. Records the tag it was handed (so the
// test can prove the BCP-47 column is what reaches Windows) and answers a
// sentinel LCID so the caller's fallthrough is unambiguous.
std::wstring g_last_tag;
int g_call_count = 0;
DWORD FakeLocaleNameToLcid(const wchar_t* name) {
    ++g_call_count;
    g_last_tag = (name != nullptr) ? std::wstring(name) : std::wstring();
    return 0x0999;
}
void ResetFake() {
    g_last_tag.clear();
    g_call_count = 0;
}

bool ReadTooltipSrc(const wchar_t* rel, std::string& out) {
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    std::string norm;
    norm.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
    return true;
}

std::size_t CountOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// Function body = [marker, first column-0 '}' after it) - the same helper shape
// the ws3 suite uses.
std::string FuncBody(const std::string& src, const std::string& marker) {
    const std::size_t i = src.find(marker);
    if (i == std::string::npos) return std::string();
    const std::size_t j = src.find("\n}", i);
    return src.substr(i, (j == std::string::npos ? src.size() : j) - i);
}

} // namespace p2c_test

void TestP2CTooltipSurface() {
    std::cout << "[RUN] Testing V10F P2-C tooltip surface (width tiers / LCID / "
                 "preview / view toggle)..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;

    // ---- (1) L1-B width tiers --------------------------------------------
    TEST_CHECK(TT::CardWidthDipForContentChars(0) == 360,
               "P2-C (1): an empty body keeps the shipped 360 DIP card");
    TEST_CHECK(TT::CardWidthDipForContentChars(400) == 360,
               "P2-C (1): the 400-character boundary stays on the narrow tier");
    TEST_CHECK(TT::CardWidthDipForContentChars(401) == 480,
               "P2-C (1): one character past 400 earns the 480 DIP tier");
    TEST_CHECK(TT::CardWidthDipForContentChars(1200) == 480,
               "P2-C (1): the 1200-character boundary stays on the medium tier");
    TEST_CHECK(TT::CardWidthDipForContentChars(1201) == 620,
               "P2-C (1): one character past 1200 earns the 620 DIP tier");
    TEST_CHECK(TT::CardWidthDipForContentChars(1000000) == 620,
               "P2-C (1): a pathological body saturates at the wide tier");
    // The two constants are equal by construction; read one through a volatile
    // so the comparison is a RUNTIME check (/W4 C4127 otherwise flags it, the
    // same idiom ws3 uses for kBackTransCardWidthDip).
    {
        volatile int narrow_tier = TT::kCardWidthNarrowDip;
        volatile int backtrans_width = TT::kBackTransCardWidthDip;
        TEST_CHECK(narrow_tier == backtrans_width,
                   "P2-C (1): the narrow tier IS the pre-V10F card width (zero visual "
                   "change for short translations)");
    }

    // ---- (2) L1-B work-area height cap -----------------------------------
    TEST_CHECK(TT::MaxCardHeightDipForWorkArea(1080, 96) == 756,
               "P2-C (2): a 1080 px work area at 100% allows 756 DIP (70%)");
    TEST_CHECK(TT::MaxCardHeightDipForWorkArea(728, 120) == 407,
               "P2-C (2): a 125% laptop work area converts to DIP before taking 70%");
    TEST_CHECK(TT::MaxCardHeightDipForWorkArea(150, 96) == TT::kCardHeightMinDip,
               "P2-C (2): a work area below the floor keeps a usable 140 DIP card");
    TEST_CHECK(TT::MaxCardHeightDipForWorkArea(0, 96) == TT::kCardHeightMinDip &&
                   TT::MaxCardHeightDipForWorkArea(-500, 96) == TT::kCardHeightMinDip,
               "P2-C (2): a failed work-area query degrades to the floor, never to 0");
    TEST_CHECK(TT::MaxCardHeightDipForWorkArea(2160, 0) == 1512,
               "P2-C (2): a zero DPI clamps to the 96 DPI baseline (no divide-by-zero)");

    // ---- (3) header action cluster ---------------------------------------
    {
        // 360 DIP card, 60 DIP source tag: the close button's left edge is 328.
        const float close_left = 328.0f;
        const float min_left = 44.0f + 60.0f + 28.0f + TT::kHeaderTargetMinWidthDip;
        // Korean labels (the widest shipped forms): "고정"/"수정 · 재번역"/"창으로 열기".
        const TT::HeaderActionClusterPlan icons =
            TT::PlanHeaderActionCluster(close_left, min_left, 26.0f, 90.0f, 66.0f);
        TEST_CHECK(!icons.labels,
                   "P2-C (3): the 360 DIP tier falls back to icon-only buttons");
        TEST_CHECK(icons.pin_w == TT::kHeaderActionIconDip &&
                       icons.edit_w == TT::kHeaderActionIconDip &&
                       icons.open_w == TT::kHeaderActionIconDip,
                   "P2-C (3): the icon fallback boxes are 20 DIP each");
        TEST_CHECK(icons.open_w > 0.0f &&
                       (icons.left + icons.pin_w + icons.edit_w + icons.open_w +
                        2.0f * TT::kHeaderActionGapDip) ==
                           close_left - TT::kHeaderCloseGapDip,
                   "P2-C (3): the cluster's right edge keeps the 8 DIP gap to the close button");
        TEST_CHECK(icons.left >= min_left,
                   "P2-C (3): the cluster never runs into the target pill's minimum position");

        // 620 DIP tier: the labels fit with room to spare.
        const TT::HeaderActionClusterPlan labelled =
            TT::PlanHeaderActionCluster(588.0f, min_left, 26.0f, 90.0f, 66.0f);
        TEST_CHECK(labelled.labels,
                   "P2-C (3): the wide tier draws the localized labels");
        TEST_CHECK(labelled.edit_w == 90.0f + TT::kHeaderActionLabelPadDip &&
                       labelled.open_w == 66.0f + TT::kHeaderActionLabelPadDip,
                   "P2-C (3): a labelled button is its measured label + padding");
        TEST_CHECK(labelled.left + labelled.pin_w + labelled.edit_w + labelled.open_w +
                           2.0f * TT::kHeaderActionGapDip ==
                       588.0f - TT::kHeaderCloseGapDip,
                   "P2-C (3): a labelled cluster keeps the same 8 DIP gap to the close button");

        // Degenerate: no room at all still yields clickable boxes.
        const TT::HeaderActionClusterPlan squeezed =
            TT::PlanHeaderActionCluster(120.0f, 200.0f, 26.0f, 90.0f, 66.0f);
        TEST_CHECK(!squeezed.labels &&
                       squeezed.pin_w == TT::kHeaderActionIconDip &&
                       squeezed.edit_w == TT::kHeaderActionIconDip,
                   "P2-C (3): even a squeezed card keeps 20 DIP icon boxes (never "
                   "negative/zero-width buttons)");
    }

    // ---- (4) FUP-06 LCID resolver ----------------------------------------
    {
        using namespace emebalachat;
        DWORD lcid = 0;
        TEST_CHECK(TryLcidOverride("YUE", lcid) && lcid == kLcidCantoneseYue,
                   "P2-C (4): Cantonese overrides to 0x0C04 (zh-HK is not a Cantonese LCID)");
        TEST_CHECK(TryLcidOverride("FIL", lcid) && lcid == kLcidFilipinoFil,
                   "P2-C (4): Filipino overrides to 0x0464");
        TEST_CHECK(TryLcidOverride("ZH-CN", lcid) && lcid == kLcidChineseSimplified,
                   "P2-C (4): Simplified Chinese overrides to 0x0804");
        TEST_CHECK(TryLcidOverride("ZH", lcid) && lcid == kLcidChineseSimplified,
                   "P2-C (4): the bare ZH spelling maps to the same LCID");
        TEST_CHECK(TryLcidOverride("ZH-TW", lcid) && lcid == kLcidChineseTraditional,
                   "P2-C (4): Traditional Chinese overrides to 0x0404");
        TEST_CHECK(!TryLcidOverride("KO", lcid),
                   "P2-C (4): a row without an override falls through to bcp47");

        // Injected resolver: the BCP-47 column reaches Windows, and its answer
        // is returned verbatim.
        p2c_test::ResetFake();
        const DWORD ko = GetLcidForLanguageWithResolver("KO", &p2c_test::FakeLocaleNameToLcid);
        TEST_CHECK(ko == 0x0999 && p2c_test::g_last_tag == L"ko",
                   "P2-C (4): KO resolves through the registry's \"ko\" BCP-47 tag");
        TEST_CHECK(p2c_test::g_call_count == 1,
                   "P2-C (4): exactly one LocaleNameToLCID call per resolution");

        // Overrides short-circuit the resolver.
        p2c_test::ResetFake();
        TEST_CHECK(GetLcidForLanguageWithResolver("YUE", &p2c_test::FakeLocaleNameToLcid) ==
                           0x0C04 &&
                       p2c_test::g_call_count == 0,
                   "P2-C (4): YUE answers from the override table without calling Windows");

        // Unknown / empty -> 0, and no spurious call.
        p2c_test::ResetFake();
        TEST_CHECK(GetLcidForLanguageWithResolver("", &p2c_test::FakeLocaleNameToLcid) == 0 &&
                       p2c_test::g_call_count == 0,
                   "P2-C (4): an empty token answers 0 without calling the resolver");
        p2c_test::ResetFake();
        TEST_CHECK(GetLcidForLanguageWithResolver("not-a-language",
                                                  &p2c_test::FakeLocaleNameToLcid) == 0,
                   "P2-C (4): an unknown token answers 0 (the pre-V10F contract)");

        // The two hex spellings keep their pre-V10F meaning.
        TEST_CHECK(GetLcidForLanguageWithResolver("0x0412",
                                                  &p2c_test::FakeLocaleNameToLcid) == 0x0412,
                   "P2-C (4): the 0x hex prefix is parsed directly");
        TEST_CHECK(GetLcidForLanguageWithResolver("412",
                                                  &p2c_test::FakeLocaleNameToLcid) == 0x0412,
                   "P2-C (4): the bare-hex fallback still resolves \"412\"");
        // English names resolve through the same registry row.
        p2c_test::ResetFake();
        TEST_CHECK(GetLcidForLanguageWithResolver("Korean",
                                                  &p2c_test::FakeLocaleNameToLcid) == 0x0999 &&
                       p2c_test::g_last_tag == L"ko",
                   "P2-C (4): the display name \"Korean\" reaches the same bcp47 tag");
        // The production resolver is wired to the real Windows call.
        TEST_CHECK(emebalachat::GetLcidForLanguage("KO") != 0,
                   "P2-C (4): the production seam resolves Korean on a real Windows NLS");
    }

    // ---- (5) FUP-05 view toggle planner ----------------------------------
    {
        using TT2 = emebalachat::TooltipWindow;
        const TT2::BackTransPanelPlan split =
            TT2::PlanBackTransViewPanels(TT2::BackTransView::Split, 400, 48);
        const TT2::BackTransPanelPlan frozen = TT2::PlanBackTransPanels(400, 48);
        TEST_CHECK(split.card_height_dip == frozen.card_height_dip &&
                       split.back_view_h == frozen.back_view_h &&
                       split.ref_view_h == frozen.ref_view_h &&
                       split.max_scroll_offset == frozen.max_scroll_offset,
                   "P2-C (5): Split is byte-identical to the frozen two-panel plan");
        const TT2::BackTransPanelPlan back_only =
            TT2::PlanBackTransViewPanels(TT2::BackTransView::BackOnly, 400, 48);
        TEST_CHECK(back_only.ref_view_h == 0 && back_only.back_view_h == back_only.body_area_dip,
                   "P2-C (5): the [역번역] solo view gives that pane the whole body area");
        const TT2::BackTransPanelPlan ref_only =
            TT2::PlanBackTransViewPanels(TT2::BackTransView::RefOnly, 400, 48);
        TEST_CHECK(ref_only.back_view_h == 0 && ref_only.ref_view_h == ref_only.body_area_dip,
                   "P2-C (5): the [원문] solo view mirrors it");
        TEST_CHECK(ref_only.ref_view_top + ref_only.ref_view_h +
                           TT2::kBackTransPanelBottomPadDip +
                           TT2::kBackTransCardBottomPadDip ==
                       ref_only.card_height_dip,
                   "P2-C (5): the solo view still closes the card exactly");
    }

    // ---- (6) structural pins over the sources ----------------------------
    {
        std::string cpp;
        std::string hpp;
        const bool read = p2c_test::ReadTooltipSrc(L"src\\ui\\tooltip.cpp", cpp) &&
                          p2c_test::ReadTooltipSrc(L"src\\ui\\tooltip.hpp", hpp);
        TEST_CHECK(read, "P2-C (6): tooltip.{cpp,hpp} resolve for the structural pins");
        if (read) {
            // L3-B preview API + SEC-ADJ value queue.
            TEST_CHECK(hpp.find("void BeginChunkPreview(int total_chunks);") != std::string::npos &&
                           hpp.find("void AppendChunkPreviewText(const std::wstring& cumulative);") !=
                               std::string::npos &&
                           hpp.find("void EndChunkPreview(bool success);") != std::string::npos,
                       "P2-C (6): the three progressive-preview seams are declared");
            TEST_CHECK(hpp.find("static constexpr UINT kChunkPreviewMessage = WM_APP + 0x207;") !=
                               std::string::npos,
                       "P2-C (6): the preview wake-up owns the free +0x207 id");
            TEST_CHECK(cpp.find("std::mutex g_tt_preview_mu;") != std::string::npos &&
                           cpp.find("std::deque<TooltipWindow::ChunkPreviewPayload> "
                                    "g_tt_preview_queue;") != std::string::npos,
                       "P2-C (6): the preview queue is the by-value SEC-ADJ transport");
            TEST_CHECK(cpp.find("case kChunkPreviewMessage: {") != std::string::npos &&
                           cpp.find("g_tt_preview_queue.pop_front();") != std::string::npos,
                       "P2-C (6): the WndProc drains the preview queue");
            TEST_CHECK(cpp.find("void TooltipWindow::RenderChunkPreview()") != std::string::npos &&
                           cpp.find("StringId::BadgeTranslatingProgress") != std::string::npos,
                       "P2-C (6): the live card renders the existing progress string (no "
                       "new i18n id)");
            TEST_CHECK(cpp.find("void TooltipWindow::LayoutChunkPreviewCard()") != std::string::npos &&
                           cpp.find("MeasureBackTransTextHeight(") != std::string::npos,
                       "P2-C (6): the card layouts (preview included) measure with DirectWrite");

            // L2 hooks (the P2-G editor contract).
            TEST_CHECK(hpp.find("void SetEditRequestCallback(std::function<void()> cb)") !=
                               std::string::npos &&
                           hpp.find("void SetOpenInWindowCallback(std::function<void()> cb)") !=
                               std::string::npos,
                       "P2-C (6): the two editor hand-off seams are declared");
            TEST_CHECK(cpp.find("StringId::EditRetranslate") != std::string::npos &&
                           cpp.find("StringId::OpenInWindow") != std::string::npos,
                       "P2-C (6): both header buttons use the shipped i18n ids");
            TEST_CHECK(cpp.find("if (pThis->edit_request_cb_) {") != std::string::npos &&
                           cpp.find("if (pThis->open_in_window_cb_) {") != std::string::npos,
                       "P2-C (6): both callbacks are invoked behind a null guard");

            // L1-B pin: Dismiss honours the pin, the card's ✕ does not.
            {
                const std::string dismiss =
                    p2c_test::FuncBody(cpp, "void TooltipWindow::Dismiss()");
                const std::size_t gate = dismiss.find("pinned_.load(std::memory_order_relaxed)");
                const std::size_t begin = dismiss.find("card_visibility_.TryBeginDismiss(");
                TEST_CHECK(!dismiss.empty() && gate != std::string::npos &&
                               (begin == std::string::npos || gate < begin),
                           "P2-C (6): a pinned card returns BEFORE the dismissal transition");
                const std::string explicit_body =
                    p2c_test::FuncBody(cpp, "void TooltipWindow::DismissExplicit()");
                TEST_CHECK(!explicit_body.empty() &&
                               explicit_body.find("TeardownCard();") != std::string::npos &&
                               explicit_body.find("pinned_.load") == std::string::npos,
                           "P2-C (6): DismissExplicit tears down without consulting the pin");
            }
            TEST_CHECK(cpp.find("tooltip_backtrans_autohide ignored reason=pinned") !=
                               std::string::npos,
                       "P2-C (6): the autohide timer is gated on the pin too");
            TEST_CHECK(hpp.find("std::atomic<bool> pinned_{false};") != std::string::npos,
                       "P2-C (6): the pin is atomic (the hook threads read it)");

            // FUP-05: per-pane offsets + RTL application.
            TEST_CHECK(hpp.find("float backtrans_ref_offset_dip_ = 0.0f;") != std::string::npos,
                       "P2-C (6): the original panel owns its own scroll offset");
            {
                const std::string render =
                    p2c_test::FuncBody(cpp, "void TooltipWindow::RenderBackTranslation() {");
                TEST_CHECK(render.find("ClampScrollOffset(backtrans_ref_offset_dip_") !=
                               std::string::npos,
                           "P2-C (6): the renderer clamps the original's offset separately");
                TEST_CHECK(render.find("draw_pane_thumb(") != std::string::npos,
                           "P2-C (6): each pane draws its own scrollbar thumb");
            }
            {
                const std::string show =
                    p2c_test::FuncBody(cpp, "void TooltipWindow::ShowBackTranslation(");
                TEST_CHECK(show.find("ApplyBackTransBodyFormat(backtrans_body_format_") !=
                                   std::string::npos &&
                               show.find("ApplyBackTransBodyFormat(backtrans_ref_format_") !=
                                   std::string::npos,
                           "P2-C (6): both comparison bodies get the direction/locale pass");
                TEST_CHECK(show.find("AnchorMonitorDpi(target_hwnd, badge_hwnd)") !=
                               std::string::npos,
                           "P2-C (6): the card's DPI comes from the ANCHOR window, not the cursor");
            }
            {
                const std::string apply =
                    p2c_test::FuncBody(cpp, "void TooltipWindow::ApplyBackTransBodyFormat(");
                TEST_CHECK(apply.find("DWRITE_READING_DIRECTION_RIGHT_TO_LEFT") !=
                                   std::string::npos &&
                               apply.find("CloneFormatWithLocale(") != std::string::npos,
                           "P2-C (6): the RTL pass sets the direction and clone-swaps the locale");
            }

            // FUP-08: cached layout lifecycle.
            TEST_CHECK(cpp.find("void TooltipWindow::EnsureBodyTextLayout(") != std::string::npos &&
                           cpp.find("dc_render_target_->DrawTextLayout(") != std::string::npos,
                       "P2-C (6): Render paints the cached IDWriteTextLayout");
            TEST_CHECK(cpp.find("void TooltipWindow::ReleaseBodyTextLayout()") != std::string::npos &&
                           p2c_test::CountOf(cpp, "ReleaseBodyTextLayout();") >= 3,
                       "P2-C (6): the cache is released on teardown, destroy and rebuild");
            TEST_CHECK(p2c_test::CountOf(cpp, "EnsureBodyTextLayout(") >= 3,
                       "P2-C (6): the cache is (re)built by the show, preview and measure paths");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F P2-C tooltip surface checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F P2-C tooltip surface: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
