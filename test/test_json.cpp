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

    std::cout << "[TEST JSON] All JSON tests passed successfully!" << std::endl;
    return 0;
}
