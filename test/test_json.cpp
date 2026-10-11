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

    std::cout << "[TEST JSON] All JSON tests passed successfully!" << std::endl;
    return 0;
}
