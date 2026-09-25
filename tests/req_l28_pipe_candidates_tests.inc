// ============================================================================
// REQ-L28 / REQ-L32 부록 ② (session 260925): pipe-candidate priority + the
// REQ-L02 cancel-approve helper adoption — source-text pins (the approach the
// REQ-057 suite established: internals in an anonymous namespace are pinned by
// scanning src/engine_host_client.cpp / src/host_main.cpp, skipping silently
// when the sources are not resolvable from the test working directory).
//
// Pins:
//   (a) kPrimaryPipeName is the canonical \\.\pipe\emebala-engine and
//       kDefaultPipeName stays the frozen -v1 alias — the two-name contract;
//   (b) PipeCandidates() returns {primary, v1} in that order and the test
//       override replaces the whole list (REQ-L28 connect matrix);
//   (c) ConnectAndHandshake runs the retry matrix per candidate and the first
//       accepted candidate carries the handshake (ConnectPipeWithRetry);
//   (d) REQ-L32 부록 ②-4: WriteFrame/ReadFrame/WaitForClientClose all go
//       through ScopedOverlappedEvent + WaitOverlappedOrCancelApprove (the
//       cancel-approve discipline — no path closes the event before the
//       kernel acks the cancel).
// ============================================================================
static void TestReqL28PipeCandidates() {
    std::cout << "[RUN] Testing REQ-L28 pipe candidates + REQ-L02 cancel-approve pins..."
              << std::endl;

    const std::filesystem::path client_cpp = ResolveRepoFile(L"src\\engine_host_client.cpp");
    if (client_cpp.empty()) {
        std::cout << "[SKIP] src/engine_host_client.cpp not resolvable; REQ-L28 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(client_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

        // (a) the two-name frozen contract.
        TEST_CHECK(src.find("kPrimaryPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\emebala-engine\"") !=
                       std::string::npos,
                   "REQ-L28 (a): kPrimaryPipeName is the canonical shared-engine pipe");
        TEST_CHECK(src.find("kDefaultPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\emebala-engine-v1\"") !=
                       std::string::npos,
                   "REQ-L28 (a): kDefaultPipeName stays the frozen -v1 alias");

        // (b) candidate priority: primary first, v1 fallback; override wins.
        const size_t pc_pos = src.find("std::vector<std::wstring> PipeCandidates()");
        const size_t pc_end = pc_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("std::wstring TokenPath(", pc_pos);
        const std::string pc_scope =
            (pc_pos != std::string::npos && pc_end != std::string::npos)
                ? src.substr(pc_pos, pc_end - pc_pos)
                : "";
        TEST_CHECK(!pc_scope.empty() &&
                       pc_scope.find("return {g_pipe_override}") != std::string::npos,
                   "REQ-L28 (b): PipeCandidates honors the test override");
        TEST_CHECK(!pc_scope.empty() &&
                       pc_scope.find("return {std::wstring(kPrimaryPipeName), "
                                     "std::wstring(kDefaultPipeName)}") != std::string::npos,
                   "REQ-L28 (b): candidate priority is primary pipe first, -v1 fallback");

        // (c) per-candidate retry matrix; the first accepted candidate carries
        // the handshake.
        TEST_CHECK(src.find("HANDLE ConnectPipeWithRetry(") != std::string::npos,
                   "REQ-L28 (c): ConnectPipeWithRetry exists (retry matrix per candidate)");
        const size_t ch_pos = src.find("bool ConnectAndHandshake(");
        const size_t ch_end = ch_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("g_session.pipe = h;", ch_pos);
        const std::string ch_scope =
            (ch_pos != std::string::npos && ch_end != std::string::npos)
                ? src.substr(ch_pos, ch_end - ch_pos)
                : "";
        TEST_CHECK(!ch_scope.empty() &&
                       ch_scope.find("for (const std::wstring& pipe : candidates)") !=
                           std::string::npos,
                   "REQ-L28 (c): ConnectAndHandshake iterates the candidate list");

        // (d) REQ-L02 cancel-approve discipline on both frame paths.
        const size_t wf_pos = src.find("bool WriteFrame(");
        const size_t wf_end = wf_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("enum class ReadOutcome", wf_pos);
        const std::string wf_scope =
            (wf_pos != std::string::npos && wf_end != std::string::npos)
                ? src.substr(wf_pos, wf_end - wf_pos)
                : "";
        TEST_CHECK(!wf_scope.empty() &&
                       wf_scope.find("ScopedOverlappedEvent event") != std::string::npos &&
                       wf_scope.find("WaitOverlappedOrCancelApprove(") != std::string::npos,
                   "REQ-L02 (d): WriteFrame goes through the cancel-approve helper");
        const size_t rf_pos = src.find("ReadOutcome ReadFrame(");
        const size_t rf_end = rf_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("HANDLE ConnectPipeWithRetry(", rf_pos);
        const std::string rf_scope =
            (rf_pos != std::string::npos && rf_end != std::string::npos)
                ? src.substr(rf_pos, rf_end - rf_pos)
                : "";
        TEST_CHECK(!rf_scope.empty() &&
                       rf_scope.find("ScopedOverlappedEvent event") != std::string::npos &&
                       rf_scope.find("WaitOverlappedOrCancelApprove(") != std::string::npos,
                   "REQ-L02 (d): ReadFrame goes through the cancel-approve helper");
    }

    // (d, host side) WaitForClientClose adopted the same discipline (the
    // secondary candidate the REQ-L32 부록 ②-4 review identified).
    const std::filesystem::path host_cpp = ResolveRepoFile(L"src\\host_main.cpp");
    if (host_cpp.empty()) {
        std::cout << "[SKIP] src/host_main.cpp not resolvable; REQ-L02 host pin skipped."
                  << std::endl;
    } else {
        std::ifstream in(host_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const size_t wc_pos = src.find("void WaitForClientClose(");
        const size_t wc_end = wc_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("// P2-1 stabilization", wc_pos);
        const std::string wc_scope =
            (wc_pos != std::string::npos && wc_end != std::string::npos)
                ? src.substr(wc_pos, wc_end - wc_pos)
                : "";
        TEST_CHECK(!wc_scope.empty() &&
                       wc_scope.find("ScopedOverlappedEvent event") != std::string::npos &&
                       wc_scope.find("WaitOverlappedOrCancelApprove(") != std::string::npos,
                   "REQ-L02 (d, host): WaitForClientClose waits for the kernel cancel-ack");
    }

    // The shared helper itself must exist (verbatim Listener copy, a321916 +
    // 260925 완료경주 수정본).
    TEST_CHECK(!ResolveRepoFile(L"src\\overlapped_io_util.hpp").empty(),
               "REQ-L02: src/overlapped_io_util.hpp exists (shared cancel-approve helper)");

    std::cout << "[PASS] REQ-L28 pipe candidates + REQ-L02 cancel-approve pins passed."
              << std::endl;
}
