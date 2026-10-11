#include "graph.h"
#include "json.h"
#include <cassert>
#include <iostream>

struct perf_dlfilter_fns perf_dlfilter_fns{};

int main()
{
    std::cout << "[TEST JSON] Running JSON config unit tests..." << std::endl;

    // 1. Test basic JSON configuration parsing
    const char *test_conf_path = "/tmp/test_ptgraph_conf.json";
    {
        std::ofstream out(test_conf_path);
        out << R"({
  "zone": {
    "function": {
      "begin": {
        "flags": 1,
        "currentip": "main",
        "targetsym": "callee",
        "tid": 100
      },
      "end": {
        "flags": 2,
        "currentip": "callee",
        "targetsym": "main",
        "tid": 100
      },
      "repeat": 2,
      "repeataccumulated": 5
    },
    "time": {
      "begin": 1000000,
      "end": 5000000
    },
    "frame": {
      "begin": 10,
      "count": 50
    }
  },
  "skip": {
    "frontframe": [
      {
        "sym": "skip_func",
        "dso": "skip_lib.so"
      }
    ],
    "endframe": [
      {
        "sym": "skip_ret_func"
      }
    ]
  },
  "modify": {
    "frontframe": [
      {
        "match": {
          "sym": "old_func",
          "dso": "libc.so"
        },
        "replace": {
          "sym": "new_func"
        }
      }
    ],
    "endframepair": [
      {
        "got": "got_sym",
        "wanted": "wanted_sym",
        "replace": "replaced_sym"
      }
    ]
  },
  "debug": {
    "level": "debug"
  }
})";
    }

    JsonPara para;
    para.Parse(test_conf_path);

    // Verify zone parsing
    assert(para.HasFuncZone());
    assert(para.funcZone->repeat == 2);
    assert(para.funcZone->repeataccumulated == 5);
    assert(para.funcZone->begin != nullptr);
    assert(para.funcZone->begin->currentSym == "main");
    assert(para.funcZone->begin->targetIP == "callee");
    assert(para.funcZone->begin->flags == 1);
    assert(para.funcZone->begin->tid == 100);

    assert(para.funcZone->end != nullptr);
    assert(para.funcZone->end->currentSym == "callee");
    assert(para.funcZone->end->targetIP == "main");
    assert(para.funcZone->end->flags == 2);
    assert(para.funcZone->end->tid == 100);

    // Verify time zone
    assert(para.HasTimeZone());
    assert(para.timeZone->begin == 1000000);
    assert(para.timeZone->end == 5000000);
    assert(!para.timeZone->InRange(999999));
    assert(para.timeZone->InRange(1000000));
    assert(para.timeZone->InRange(3000000));
    assert(para.timeZone->InRange(5000000));
    assert(!para.timeZone->InRange(5000001));

    // Verify frame zone
    assert(para.HasFrameZone());
    assert(para.frameZone->begin == 10);
    assert(para.frameZone->count == 50);
    assert(!para.frameZone->InRange(9));
    assert(para.frameZone->InRange(10));
    assert(para.frameZone->InRange(35));
    assert(para.frameZone->InRange(60));
    assert(!para.frameZone->InRange(61));

    // Verify skipping rules
    assert(para.frameFrontSkipping != nullptr);
    assert(para.frameFrontSkipping->size() == 1);
    assert((*para.frameFrontSkipping)[0]->Match("skip_func", "skip_lib.so"));
    assert(!(*para.frameFrontSkipping)[0]->Match("other_func", "skip_lib.so"));

    assert(para.frameEndSkipping != nullptr);
    assert(para.frameEndSkipping->size() == 1);
    assert((*para.frameEndSkipping)[0]->Match("skip_ret_func"));

    // Verify modification rules
    assert(para.frameFrontModification != nullptr);
    assert(para.frameFrontModification->size() == 1);
    assert((*para.frameFrontModification)[0]->first->Match("old_func", "libc.so"));
    assert(*(*para.frameFrontModification)[0]->second->sym == "new_func");

    assert(para.endFramePair != nullptr);
    assert(para.endFramePair->size() == 1);
    assert(*(*para.endFramePair)[0]->got == "got_sym");
    assert(*(*para.endFramePair)[0]->replace == "replaced_sym");

    // Verify debug level
    assert(para.debugLevel == LogLevel::DEBUG);
    assert(Logger::GetLevel() == LogLevel::DEBUG);

    // Verify FuncZone state machine
    uint32_t tid = 100;
    assert(!para.funcZone->IsInCapturing(tid));
    assert(para.funcZone->IsMatchBeginFunction("callee", "main", 1, tid));
    assert(!para.funcZone->IsMatchBeginFunction("callee", "wrong", 1, tid));

    para.funcZone->SetCapturing(tid);
    assert(para.funcZone->IsInCapturing(tid));

    assert(para.funcZone->IsMatchEndFunction("main", "callee", 2, tid));
    para.funcZone->ClearCapturing(tid);
    assert(!para.funcZone->IsInCapturing(tid));
    assert(!para.funcZone->CapturedEnoughFunctions(tid));

    // Second capture
    para.funcZone->SetCapturing(tid);
    para.funcZone->ClearCapturing(tid);
    assert(para.funcZone->CapturedEnoughFunctions(tid)); // repeat reached (2)

    // Test Integration with GraphDrawer
    const std::string prefix = "ptgraph_test_json_" + std::to_string(getpid());
    PtGraphReader::UnlinkAll(prefix);
    {
        GraphDrawer drawer(nullptr, prefix);
        drawer.LoadConfig(test_conf_path);

        assert(drawer.GetJsonPara().HasFuncZone());
        assert(drawer.GetJsonPara().HasTimeZone());
        assert(!drawer.GetCloseStackOnTraceBegin());
    }
    PtGraphReader::UnlinkAll(prefix);

    // Clean up temporary file
    std::remove(test_conf_path);

    // 2. Test closeStackOnTraceBegin configuration options
    {
        // Default is false
        JsonPara default_para;
        assert(!default_para.closeStackOnTraceBegin);

        const char *conf_true = "/tmp/test_trace_begin_true.json";
        {
            std::ofstream out(conf_true);
            out << R"({ "closeStackOnTraceBegin": true })";
        }
        JsonPara para_true;
        para_true.Parse(conf_true);
        assert(para_true.closeStackOnTraceBegin == true);
        std::remove(conf_true);

        const char *conf_false = "/tmp/test_trace_begin_false.json";
        {
            std::ofstream out(conf_false);
            out << R"({ "closeStackOnTraceBegin": false })";
        }
        JsonPara para_false;
        para_false.Parse(conf_false);
        assert(para_false.closeStackOnTraceBegin == false);
        std::remove(conf_false);

        // Alias: closeFrameOnTraceBegin
        const char *conf_alias1 = "/tmp/test_trace_begin_alias1.json";
        {
            std::ofstream out(conf_alias1);
            out << R"({ "closeFrameOnTraceBegin": true })";
        }
        JsonPara para_alias1;
        para_alias1.Parse(conf_alias1);
        assert(para_alias1.closeStackOnTraceBegin == true);
        std::remove(conf_alias1);

        // Alias: closeFramesOnTraceBegin
        const char *conf_alias2 = "/tmp/test_trace_begin_alias2.json";
        {
            std::ofstream out(conf_alias2);
            out << R"({ "closeFramesOnTraceBegin": true })";
        }
        JsonPara para_alias2;
        para_alias2.Parse(conf_alias2);
        assert(para_alias2.closeStackOnTraceBegin == true);
        std::remove(conf_alias2);

        // Nested: traceBegin.closeStack
        const char *conf_nested = "/tmp/test_trace_begin_nested.json";
        {
            std::ofstream out(conf_nested);
            out << R"({ "traceBegin": { "closeStack": true } })";
        }
        JsonPara para_nested;
        para_nested.Parse(conf_nested);
        assert(para_nested.closeStackOnTraceBegin == true);
        std::remove(conf_nested);
    }

    // 3. Test GraphDrawer with closeStackOnTraceBegin
    {
        const std::string tb_prefix = "ptgraph_test_tb_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(tb_prefix);
        {
            GraphDrawer drawer(nullptr, tb_prefix);
            assert(!drawer.GetCloseStackOnTraceBegin());

            drawer.SetCloseStackOnTraceBegin(true);
            assert(drawer.GetCloseStackOnTraceBegin());

            drawer.SetCloseStackOnTraceBegin(false);
            assert(!drawer.GetCloseStackOnTraceBegin());
        }
        PtGraphReader::UnlinkAll(tb_prefix);
    }

    // 4. Test TRACE_BEGIN followed by CALL sample pushes caller and callee
    // Sequence:
    //   2258 [0x101] Addr _dl_start (TRACE_BEGIN)
    //   2259 [0x63]  IP _dl_start, Addr asm_exc_page_fault (CALL / INTERRUPT) -> pushes _dl_start first, then asm_exc_page_fault
    //   2260 [0x3]   IP asm_exc_page_fault, Addr error_entry (CALL) -> pushes error_entry on top of asm_exc_page_fault
    //   2261 [0x5]   IP error_entry, Addr asm_exc_page_fault (RETURN) -> pops error_entry
    //   2262 [0x5]   IP asm_exc_page_fault, Addr _dl_start (RETURN) -> pops asm_exc_page_fault
    //   2263 [0x5]   IP _dl_start, Addr _start (RETURN) -> pops _dl_start
    {
        const std::string tb_prefix = "ptgraph_test_tb_call_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(tb_prefix);
        {
            GraphDrawer drawer(nullptr, tb_prefix, 32, 1024, 4);

            // Frame 0 (2258): TRACE_BEGIN at timestamp 1000, Addr = _dl_start
            FrameEle fe_tb{};
            fe_tb.timestamp = 1000;
            fe_tb.pid       = 100;
            fe_tb.tid       = 57955;
            fe_tb.type      = FrameType::TRACE_BEGIN;
            fe_tb.flags     = PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH; // 0x101
            drawer.AppendFrameWithSymbols(fe_tb, "_dl_start", "", "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2");

            // Frame 1 (2259): First sample after TRACE_BEGIN with CALL flag (0x63)
            FrameEle fe_call1{};
            fe_call1.timestamp = 2000;
            fe_call1.pid       = 100;
            fe_call1.tid       = 57955;
            fe_call1.type      = FrameType::CALL;
            fe_call1.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL | PERF_DLFILTER_FLAG_ASYNC | PERF_DLFILTER_FLAG_INTERRUPT; // 0x63
            fe_call1.ip        = 0x7fff0001;
            fe_call1.addr      = 0xffff0002;
            drawer.AppendFrameWithSymbols(fe_call1, "asm_exc_page_fault", "_dl_start", "[kernel.kallsyms]");

            // Frame 2 (2260): CALL error_entry from asm_exc_page_fault (flags 0x3)
            FrameEle fe_call2{};
            fe_call2.timestamp = 2500;
            fe_call2.pid       = 100;
            fe_call2.tid       = 57955;
            fe_call2.type      = FrameType::CALL;
            fe_call2.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL; // 0x3
            fe_call2.ip        = 0xffff0002;
            fe_call2.addr      = 0xffff0010;
            drawer.AppendFrameWithSymbols(fe_call2, "error_entry", "asm_exc_page_fault", "[kernel.kallsyms]");

            // Frame 3 (2261): RETURN for error_entry at timestamp 3000
            FrameEle fe_ret1{};
            fe_ret1.timestamp = 3000;
            fe_ret1.pid       = 100;
            fe_ret1.tid       = 57955;
            fe_ret1.type      = FrameType::RETURN;
            fe_ret1.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret1.ip        = 0xffff0010;
            fe_ret1.addr      = 0xffff0002;
            drawer.AppendFrameWithSymbols(fe_ret1, "error_entry", "asm_exc_page_fault", "[kernel.kallsyms]");

            // Frame 4 (2262): RETURN for asm_exc_page_fault at timestamp 3500
            FrameEle fe_ret2{};
            fe_ret2.timestamp = 3500;
            fe_ret2.pid       = 100;
            fe_ret2.tid       = 57955;
            fe_ret2.type      = FrameType::RETURN;
            fe_ret2.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret2.ip        = 0xffff0002;
            fe_ret2.addr      = 0x7fff0001;
            drawer.AppendFrameWithSymbols(fe_ret2, "asm_exc_page_fault", "_dl_start", "[kernel.kallsyms]");

            // Frame 5 (2263): RETURN for _dl_start at timestamp 4000
            FrameEle fe_ret3{};
            fe_ret3.timestamp = 4000;
            fe_ret3.pid       = 100;
            fe_ret3.tid       = 57955;
            fe_ret3.type      = FrameType::RETURN;
            fe_ret3.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret3.ip        = 0x7fff0001;
            fe_ret3.addr      = 0x7fff0000;
            drawer.AppendFrameWithSymbols(fe_ret3, "_dl_start", "_start", "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_tb_call.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(tb_prefix);
    }

    // 5. Test Interrupt 0x63 entry and 0x45 exit with intermediate frame closure
    // Sequence:
    //   1) CALL _start (0x3) at ts 1000
    //   2) Interrupt entry 0x63: IP _start, Addr asm_exc_page_fault at ts 2000
    //   3) CALL error_entry (0x3) from asm_exc_page_fault at ts 2100
    //   4) RETURN error_entry (0x5) at ts 2200
    //   5) CALL exc_page_fault (0x3) from asm_exc_page_fault at ts 2300 (not explicitly returned)
    //   6) Interrupt exit 0x45: IP native_irq_return_iret, Addr _start at ts 3000
    //      -> must auto-close intermediate exc_page_fault, then close asm_exc_page_fault at ts 3000
    //   7) RETURN _start (0x5) at ts 4000
    {
        const std::string irq_prefix = "ptgraph_test_irq_45_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(irq_prefix);
        {
            GraphDrawer drawer(nullptr, irq_prefix, 32, 1024, 4);

            // Frame 0: CALL _start
            FrameEle fe_start{};
            fe_start.timestamp = 1000;
            fe_start.pid       = 100;
            fe_start.tid       = 57955;
            fe_start.type      = FrameType::CALL;
            fe_start.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_start.ip        = 0x7fff0000;
            fe_start.addr      = 0x7fff0010;
            drawer.AppendFrameWithSymbols(fe_start, "_start", "[entry]", "/bin/app");

            // Frame 1: 0x63 interrupt entry to asm_exc_page_fault
            FrameEle fe_irq_entry{};
            fe_irq_entry.timestamp = 2000;
            fe_irq_entry.pid       = 100;
            fe_irq_entry.tid       = 57955;
            fe_irq_entry.type      = FrameType::CALL;
            fe_irq_entry.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL |
                                     PERF_DLFILTER_FLAG_ASYNC | PERF_DLFILTER_FLAG_INTERRUPT; // 0x63
            fe_irq_entry.ip        = 0x7fff0010;
            fe_irq_entry.addr      = 0xffff0002;
            drawer.AppendFrameWithSymbols(fe_irq_entry, "asm_exc_page_fault", "_start", "[kernel.kallsyms]");

            // Frame 2: CALL error_entry
            FrameEle fe_call1{};
            fe_call1.timestamp = 2100;
            fe_call1.pid       = 100;
            fe_call1.tid       = 57955;
            fe_call1.type      = FrameType::CALL;
            fe_call1.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call1.ip        = 0xffff0002;
            fe_call1.addr      = 0xffff0010;
            drawer.AppendFrameWithSymbols(fe_call1, "error_entry", "asm_exc_page_fault", "[kernel.kallsyms]");

            // Frame 3: RETURN error_entry
            FrameEle fe_ret1{};
            fe_ret1.timestamp = 2200;
            fe_ret1.pid       = 100;
            fe_ret1.tid       = 57955;
            fe_ret1.type      = FrameType::RETURN;
            fe_ret1.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret1.ip        = 0xffff0010;
            fe_ret1.addr      = 0xffff0002;
            drawer.AppendFrameWithSymbols(fe_ret1, "error_entry", "asm_exc_page_fault", "[kernel.kallsyms]");

            // Frame 4: CALL exc_page_fault (unreturned intermediate frame)
            FrameEle fe_call2{};
            fe_call2.timestamp = 2300;
            fe_call2.pid       = 100;
            fe_call2.tid       = 57955;
            fe_call2.type      = FrameType::CALL;
            fe_call2.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call2.ip        = 0xffff0002;
            fe_call2.addr      = 0xffff0020;
            drawer.AppendFrameWithSymbols(fe_call2, "exc_page_fault", "asm_exc_page_fault", "[kernel.kallsyms]");

            // Frame 5: 0x45 interrupt exit via native_irq_return_iret returning to _start
            FrameEle fe_irq_exit{};
            fe_irq_exit.timestamp = 3000;
            fe_irq_exit.pid       = 100;
            fe_irq_exit.tid       = 57955;
            fe_irq_exit.type      = FrameType::RETURN;
            fe_irq_exit.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_RETURN |
                                    PERF_DLFILTER_FLAG_INTERRUPT; // 0x45
            fe_irq_exit.ip        = 0xffff0050;
            fe_irq_exit.addr      = 0x7fff0010;
            drawer.AppendFrameWithSymbols(fe_irq_exit, "native_irq_return_iret", "_start", "[kernel.kallsyms]");

            // Frame 6: RETURN _start
            FrameEle fe_ret_start{};
            fe_ret_start.timestamp = 4000;
            fe_ret_start.pid       = 100;
            fe_ret_start.tid       = 57955;
            fe_ret_start.type      = FrameType::RETURN;
            fe_ret_start.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret_start.ip        = 0x7fff0010;
            fe_ret_start.addr      = 0x7fff0000;
            drawer.AppendFrameWithSymbols(fe_ret_start, "_start", "[entry]", "/bin/app");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_irq_45.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(irq_prefix);
    }

    // 6. Test Syscall 0x13 entry and 0x15 exit:
    //    - Symbol "entry_SYSCALL_64_after_hwframe" converted to "entry_SYSCALL_64"
    //    - Full closure across intermediate child kernel calls (e.g. unreturned do_syscall_64, sys_read)
    //    - entry_SYSCALL_64 bounded by start ts (from 0x13) and end ts (from 0x15)
    {
        const std::string sc_prefix = "ptgraph_test_syscall_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(sc_prefix);
        {
            GraphDrawer drawer(nullptr, sc_prefix, 32, 1024, 4);

            // Frame 0: User space CALL main
            FrameEle fe_main{};
            fe_main.timestamp = 1000;
            fe_main.pid       = 100;
            fe_main.tid       = 57955;
            fe_main.type      = FrameType::CALL;
            fe_main.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_main.ip        = 0x400000;
            fe_main.addr      = 0x400100;
            drawer.AppendFrameWithSymbols(fe_main, "main", "_start", "/bin/app");

            // Frame 1: 0x13 syscall entry to entry_SYSCALL_64_after_hwframe
            FrameEle fe_sc_entry{};
            fe_sc_entry.timestamp = 2000;
            fe_sc_entry.pid       = 100;
            fe_sc_entry.tid       = 57955;
            fe_sc_entry.type      = FrameType::CALL;
            fe_sc_entry.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL | PERF_DLFILTER_FLAG_SYSCALLRET; // 0x13
            fe_sc_entry.ip        = 0x400150;
            fe_sc_entry.addr      = 0xffffffff96800080ULL;
            drawer.AppendFrameWithSymbols(fe_sc_entry, "entry_SYSCALL_64_after_hwframe", "main", "[kernel.kallsyms]");

            // Frame 2: CALL do_syscall_64 inside syscall
            FrameEle fe_call1{};
            fe_call1.timestamp = 2200;
            fe_call1.pid       = 100;
            fe_call1.tid       = 57955;
            fe_call1.type      = FrameType::CALL;
            fe_call1.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call1.ip        = 0xffffffff96800080ULL;
            fe_call1.addr      = 0xffffffff96800200ULL;
            drawer.AppendFrameWithSymbols(fe_call1, "do_syscall_64", "entry_SYSCALL_64", "[kernel.kallsyms]");

            // Frame 3: CALL sys_read inside do_syscall_64 (unreturned child)
            FrameEle fe_call2{};
            fe_call2.timestamp = 2400;
            fe_call2.pid       = 100;
            fe_call2.tid       = 57955;
            fe_call2.type      = FrameType::CALL;
            fe_call2.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call2.ip        = 0xffffffff96800200ULL;
            fe_call2.addr      = 0xffffffff96800300ULL;
            drawer.AppendFrameWithSymbols(fe_call2, "sys_read", "do_syscall_64", "[kernel.kallsyms]");

            // Frame 4: 0x15 syscall exit returning to main
            FrameEle fe_sc_exit{};
            fe_sc_exit.timestamp = 3000;
            fe_sc_exit.pid       = 100;
            fe_sc_exit.tid       = 57955;
            fe_sc_exit.type      = FrameType::SYSCALL;
            fe_sc_exit.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_SYSCALLRET; // 0x15
            fe_sc_exit.ip        = 0xffffffff96800080ULL;
            fe_sc_exit.addr      = 0x400155;
            drawer.AppendFrameWithSymbols(fe_sc_exit, "entry_SYSCALL_64_after_hwframe", "main", "[kernel.kallsyms]");

            // Frame 5: RETURN main
            FrameEle fe_ret_main{};
            fe_ret_main.timestamp = 4000;
            fe_ret_main.pid       = 100;
            fe_ret_main.tid       = 57955;
            fe_ret_main.type      = FrameType::RETURN;
            fe_ret_main.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret_main.ip        = 0x400100;
            fe_ret_main.addr      = 0x400000;
            drawer.AppendFrameWithSymbols(fe_ret_main, "main", "_start", "/bin/app");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_syscall.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(sc_prefix);
    }

    // 7. Test Syscall 0x15 exit without preceding 0x13:
    //    - TRACE_BEGIN 0x101 in kernel space at ts 1000
    //    - CALL sys_read in kernel space at ts 1500
    //    - Syscall exit 0x15 at ts 2500
    //    - Must backtrack to flags 0x101 and close all kernel space functions
    {
        const std::string sc_bt_prefix = "ptgraph_test_syscall_bt_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(sc_bt_prefix);
        {
            GraphDrawer drawer(nullptr, sc_bt_prefix, 32, 1024, 4);

            // Frame 0: TRACE_BEGIN 0x101 in kernel space
            FrameEle fe_tb{};
            fe_tb.timestamp = 1000;
            fe_tb.pid       = 100;
            fe_tb.tid       = 57955;
            fe_tb.type      = FrameType::TRACE_BEGIN;
            fe_tb.flags     = PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH; // 0x101
            fe_tb.ip        = 0xffffffff96800000ULL;
            fe_tb.addr      = 0xffffffff96800080ULL;
            drawer.AppendFrameWithSymbols(fe_tb, "entry_SYSCALL_64", "main", "[kernel.kallsyms]");

            // Frame 1: CALL sys_read in kernel space
            FrameEle fe_call{};
            fe_call.timestamp = 1500;
            fe_call.pid       = 100;
            fe_call.tid       = 57955;
            fe_call.type      = FrameType::CALL;
            fe_call.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call.ip        = 0xffffffff96800080ULL;
            fe_call.addr      = 0xffffffff96800300ULL;
            drawer.AppendFrameWithSymbols(fe_call, "sys_read", "entry_SYSCALL_64", "[kernel.kallsyms]");

            // Frame 2: 0x15 syscall exit returning to main (no 0x13 preceding)
            FrameEle fe_sc_exit{};
            fe_sc_exit.timestamp = 2500;
            fe_sc_exit.pid       = 100;
            fe_sc_exit.tid       = 57955;
            fe_sc_exit.type      = FrameType::SYSCALL;
            fe_sc_exit.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_SYSCALLRET; // 0x15
            fe_sc_exit.ip        = 0xffffffff96800080ULL;
            fe_sc_exit.addr      = 0x400155;
            drawer.AppendFrameWithSymbols(fe_sc_exit, "entry_SYSCALL_64_after_hwframe", "main", "[kernel.kallsyms]");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_syscall_bt.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(sc_bt_prefix);
    }

    // 8. Test Syscall 0x15 exit without preceding 0x13 and entry_SYSCALL_64 not on stack:
    //    - TRACE_BEGIN 0x101 in kernel space at ts 1000
    //    - CALL sys_read in kernel space at ts 1500 (caller is do_syscall_64, not entry_SYSCALL_64)
    //    - Syscall exit 0x15 at ts 2500
    //    - Must pop and close kernel functions on stack, and create full closure for entry_SYSCALL_64 from 1000 to 2500
    {
        const std::string sc_bt2_prefix = "ptgraph_test_syscall_bt2_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(sc_bt2_prefix);
        {
            GraphDrawer drawer(nullptr, sc_bt2_prefix, 32, 1024, 4);

            // Frame 0: TRACE_BEGIN 0x101 in kernel space
            FrameEle fe_tb{};
            fe_tb.timestamp = 1000;
            fe_tb.pid       = 100;
            fe_tb.tid       = 57955;
            fe_tb.type      = FrameType::TRACE_BEGIN;
            fe_tb.flags     = PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH; // 0x101
            fe_tb.ip        = 0xffffffff96800000ULL;
            fe_tb.addr      = 0xffffffff96800080ULL;
            drawer.AppendFrameWithSymbols(fe_tb, "ffffffff96800080", "main", "[kernel.kallsyms]");

            // Frame 1: CALL sys_read in kernel space (caller do_syscall_64)
            FrameEle fe_call{};
            fe_call.timestamp = 1500;
            fe_call.pid       = 100;
            fe_call.tid       = 57955;
            fe_call.type      = FrameType::CALL;
            fe_call.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_call.ip        = 0xffffffff96800200ULL;
            fe_call.addr      = 0xffffffff96800300ULL;
            drawer.AppendFrameWithSymbols(fe_call, "sys_read", "do_syscall_64", "[kernel.kallsyms]");

            // Frame 2: 0x15 syscall exit returning to main
            FrameEle fe_sc_exit{};
            fe_sc_exit.timestamp = 2500;
            fe_sc_exit.pid       = 100;
            fe_sc_exit.tid       = 57955;
            fe_sc_exit.type      = FrameType::SYSCALL;
            fe_sc_exit.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_SYSCALLRET; // 0x15
            fe_sc_exit.ip        = 0xffffffff96800080ULL;
            fe_sc_exit.addr      = 0x400155;
            drawer.AppendFrameWithSymbols(fe_sc_exit, "entry_SYSCALL_64_after_hwframe", "main", "[kernel.kallsyms]");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_syscall_bt2.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(sc_bt2_prefix);
    }

    // 8. Test 0x101 TRACE_BEGIN popping stack down to target sym (inclusive)
    // Sequence:
    //   - Frame 0: CALL _start at ts 1000
    //   - Frame 1: CALL _dl_start at ts 2000
    //   - Frame 2: CALL child_func at ts 2500
    //   - Frame 3: 0x101 TRACE_BEGIN with IP=0, caller="", callee="_dl_start" at ts 3000
    //              Must pop child_func AND _dl_start, closing both at ts 3000
    //   - Frame 4: RETURN _start at ts 4000
    //              Matches _start on stack, closing it at ts 4000
    {
        const std::string tb_sym_pop_prefix = "ptgraph_test_tb_sym_pop_" + std::to_string(getpid());
        PtGraphReader::UnlinkAll(tb_sym_pop_prefix);
        {
            GraphDrawer drawer(nullptr, tb_sym_pop_prefix, 32, 1024, 4);

            // Frame 0: CALL _start
            FrameEle fe_start{};
            fe_start.timestamp = 1000;
            fe_start.pid       = 100;
            fe_start.tid       = 57955;
            fe_start.type      = FrameType::CALL;
            fe_start.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_start.ip        = 0x400000;
            fe_start.addr      = 0x400100;
            drawer.AppendFrameWithSymbols(fe_start, "_start", "[entry]", "/bin/app");

            // Frame 1: CALL _dl_start
            FrameEle fe_dl{};
            fe_dl.timestamp = 2000;
            fe_dl.pid       = 100;
            fe_dl.tid       = 57955;
            fe_dl.type      = FrameType::CALL;
            fe_dl.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_dl.ip        = 0x400100;
            fe_dl.addr      = 0x7fff0000;
            drawer.AppendFrameWithSymbols(fe_dl, "_dl_start", "_start", "/usr/lib/x86_64-linux-gnu/ld.so");

            // Frame 2: CALL child_func
            FrameEle fe_child{};
            fe_child.timestamp = 2500;
            fe_child.pid       = 100;
            fe_child.tid       = 57955;
            fe_child.type      = FrameType::CALL;
            fe_child.flags     = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL;
            fe_child.ip        = 0x7fff0000;
            fe_child.addr      = 0x7fff0500;
            drawer.AppendFrameWithSymbols(fe_child, "child_func", "_dl_start", "/usr/lib/x86_64-linux-gnu/ld.so");

            // Frame 3: 0x101 TRACE_BEGIN with IP=0, caller="", callee="_dl_start"
            FrameEle fe_tb{};
            fe_tb.timestamp = 3000;
            fe_tb.pid       = 100;
            fe_tb.tid       = 57955;
            fe_tb.type      = FrameType::TRACE_BEGIN;
            fe_tb.flags     = PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH; // 0x101
            fe_tb.ip        = 0;
            fe_tb.addr      = 0x7fff0000;
            drawer.AppendFrameWithSymbols(fe_tb, "_dl_start", "", "/usr/lib/x86_64-linux-gnu/ld.so");

            // Frame 4: RETURN _start
            FrameEle fe_ret{};
            fe_ret.timestamp = 4000;
            fe_ret.pid       = 100;
            fe_ret.tid       = 57955;
            fe_ret.type      = FrameType::RETURN;
            fe_ret.flags     = PERF_DLFILTER_FLAG_RETURN | PERF_DLFILTER_FLAG_BRANCH;
            fe_ret.ip        = 0x400100;
            fe_ret.addr      = 0x400000;
            drawer.AppendFrameWithSymbols(fe_ret, "_start", "[entry]", "/bin/app");

            drawer.Finish();

            std::string out_ftf = "/tmp/test_tb_sym_pop.ftf";
            drawer.GenerateGraph(out_ftf);

            // Verify the generated trace file exists
            std::ifstream ftf(out_ftf, std::ios::binary);
            assert(ftf.is_open());
            ftf.close();
            std::remove(out_ftf.c_str());
            std::string gz_ftf = out_ftf + ".gz";
            std::remove(gz_ftf.c_str());
        }
        PtGraphReader::UnlinkAll(tb_sym_pop_prefix);
    }

    std::cout << "[TEST JSON] All JSON tests passed successfully!" << std::endl;
    return 0;
}
