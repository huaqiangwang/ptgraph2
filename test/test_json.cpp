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
    }
    PtGraphReader::UnlinkAll(prefix);

    // Clean up temporary file
    std::remove(test_conf_path);

    std::cout << "[TEST JSON] All JSON tests passed successfully!" << std::endl;
    return 0;
}
