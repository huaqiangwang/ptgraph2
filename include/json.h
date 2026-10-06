#ifndef PTGRAPH2_JSON_H
#define PTGRAPH2_JSON_H

#include "log.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

struct FramePattern
{
    uint64_t                     timestamp{0};
    uint64_t                     pidtid{0};
    std::shared_ptr<std::string> dso{nullptr};
    std::shared_ptr<std::string> sym{nullptr};
    std::shared_ptr<std::string> caller{nullptr};
    uint64_t                     ip{0};
    uint64_t                     addr{0};
    uint64_t                     tstart{0};
    std::string                  type;

    void FromJson(const json &j)
    {
        if (j.contains("stamp")) {
            timestamp = j["stamp"].is_string() ? std::stoull(j["stamp"].get<std::string>(), nullptr, 0)
                                               : j["stamp"].get<uint64_t>();
        } else if (j.contains("timestamp")) {
            timestamp = j["timestamp"].is_string() ? std::stoull(j["timestamp"].get<std::string>(), nullptr, 0)
                                                   : j["timestamp"].get<uint64_t>();
        }
        if (j.contains("pidtid")) {
            pidtid = j["pidtid"].is_string() ? std::stoull(j["pidtid"].get<std::string>(), nullptr, 0)
                                             : j["pidtid"].get<uint64_t>();
        }
        if (j.contains("dso") && !j["dso"].is_null()) {
            dso = std::make_shared<std::string>(j["dso"].get<std::string>());
        }
        if (j.contains("sym") && !j["sym"].is_null()) {
            sym = std::make_shared<std::string>(j["sym"].get<std::string>());
        }
        if (j.contains("caller") && !j["caller"].is_null()) {
            caller = std::make_shared<std::string>(j["caller"].get<std::string>());
        }
        if (j.contains("ip")) {
            ip = j["ip"].is_string() ? std::stoull(j["ip"].get<std::string>(), nullptr, 0)
                                     : j["ip"].get<uint64_t>();
        }
        if (j.contains("addr")) {
            addr = j["addr"].is_string() ? std::stoull(j["addr"].get<std::string>(), nullptr, 0)
                                         : j["addr"].get<uint64_t>();
        }
        if (j.contains("type") && !j["type"].is_null()) {
            type = j["type"].get<std::string>();
        }
        if (j.contains("tstart")) {
            tstart = j["tstart"].is_string() ? std::stoull(j["tstart"].get<std::string>(), nullptr, 0)
                                             : j["tstart"].get<uint64_t>();
        }
    }

    bool Match(const std::string &curr_sym, const std::string &curr_dso = "", uint64_t curr_ip = 0, uint64_t curr_addr = 0) const
    {
        if (sym && !sym->empty() && *sym != curr_sym)
            return false;
        if (dso && !dso->empty() && !curr_dso.empty() && *dso != curr_dso)
            return false;
        if (ip != 0 && curr_ip != 0 && ip != curr_ip)
            return false;
        if (addr != 0 && curr_addr != 0 && addr != curr_addr)
            return false;
        return true;
    }
};

using FrontFrame = FramePattern;
using EndFrame   = FramePattern;
using FramePair  = std::pair<std::shared_ptr<FramePattern>, std::shared_ptr<FramePattern>>;

struct JsonPara
{
    struct Functrace
    {
        std::string currentSym;
        std::string targetIP;
        uint32_t    flags{0};
        uint32_t    tid{0};

        bool equal(const std::string &target, const std::string &current, uint32_t flag = 0, uint32_t thread = 0) const
        {
            if (!currentSym.empty() && currentSym != current)
                return false;
            if (!targetIP.empty() && targetIP != target)
                return false;
            if (flags != 0 && flags != flag)
                return false;
            if (tid != 0 && tid != thread)
                return false;
            return true;
        }

        bool equal(std::shared_ptr<std::string> target, std::shared_ptr<std::string> current, uint32_t flag = 0, uint32_t thread = 0) const
        {
            const std::string &t = target ? *target : "";
            const std::string &c = current ? *current : "";
            return equal(t, c, flag, thread);
        }

        std::string toString() const
        {
            return "currentSym: " + currentSym + ", targetIP: " + targetIP +
                   ", flags: " + std::to_string(flags) + ", tid: " + std::to_string(tid);
        }
    };

    struct FuncZone
    {
#define FUNC_ZONE_CAPTURED 0x1

        struct threadSts
        {
            uint32_t status   = 0;
            uint32_t captured = 0;
        };

        bool IsInCapturing(uint32_t tid)
        {
            if (threadStatus.find(tid) == threadStatus.end())
                threadStatus[tid] = std::make_shared<threadSts>();

            return (threadStatus[tid]->status & FUNC_ZONE_CAPTURED) != 0;
        }

        bool CapturedEnoughFunctions(uint32_t tid)
        {
            if (threadStatus.find(tid) == threadStatus.end())
                threadStatus[tid] = std::make_shared<threadSts>();

            return (repeat > 0 && threadStatus[tid]->captured >= repeat);
        }

        void SetCapturing(uint32_t tid)
        {
            if (threadStatus.find(tid) == threadStatus.end())
                threadStatus[tid] = std::make_shared<threadSts>();

            threadStatus[tid]->status |= FUNC_ZONE_CAPTURED;
            if (Logger::IsDebug()) {
                std::cout << "Thread [" << tid << "] is now capturing functions: IP "
                          << (begin ? begin->targetIP : "") << ", sym: " << (begin ? begin->currentSym : "") << std::endl;
            }
        }

        void ClearCapturing(uint32_t tid)
        {
            if (threadStatus.find(tid) == threadStatus.end())
                threadStatus[tid] = std::make_shared<threadSts>();

            capturedaccumulated++;
            threadStatus[tid]->captured++;
            threadStatus[tid]->status &= ~FUNC_ZONE_CAPTURED;
            if (Logger::IsDebug()) {
                std::cout << "Thread [" << tid << "] has stopped capturing functions: IP "
                          << (end ? end->targetIP : "") << ", sym: " << (end ? end->currentSym : "") << std::endl;
            }
        }

        bool IsMatchBeginFunction(std::shared_ptr<std::string> target, std::shared_ptr<std::string> current, uint32_t flag = 0, uint32_t thread = 0)
        {
            if (!begin)
                return false;
            return begin->equal(target, current, flag, thread);
        }

        bool IsMatchBeginFunction(const std::string &target, const std::string &current, uint32_t flag = 0, uint32_t thread = 0)
        {
            if (!begin)
                return false;
            return begin->equal(target, current, flag, thread);
        }

        bool IsMatchEndFunction(std::shared_ptr<std::string> target, std::shared_ptr<std::string> current, uint32_t flag = 0, uint32_t thread = 0)
        {
            if (!end)
                return false;
            return end->equal(target, current, flag, thread);
        }

        bool IsMatchEndFunction(const std::string &target, const std::string &current, uint32_t flag = 0, uint32_t thread = 0)
        {
            if (!end)
                return false;
            return end->equal(target, current, flag, thread);
        }

        bool IsEnoughAccumulatedFunctions() const
        {
            return (repeataccumulated > 0 && capturedaccumulated >= repeataccumulated);
        }

        std::string toString() const
        {
            std::string result = "FuncZone: repeat: " + std::to_string(repeat) + "\n";
            if (begin)
                result += "Begin: " + begin->toString() + "\n";
            if (end)
                result += "End: " + end->toString() + "\n";
            return result;
        }

        std::shared_ptr<Functrace>                               begin;
        std::shared_ptr<Functrace>                               end;
        uint32_t                                                 repeat              = 0; // 0 for unlimited
        uint32_t                                                 repeataccumulated   = 0; // accumulated count across threads
        uint32_t                                                 capturedaccumulated = 0; // accumulated count of captured functions
        std::unordered_map<uint32_t, std::shared_ptr<threadSts>> threadStatus;
    };
    std::shared_ptr<FuncZone> funcZone;

    struct TimeZone
    {
        uint64_t begin{0};
        uint64_t end{0};

        bool InRange(uint64_t timestamp) const
        {
            if (begin != 0 && timestamp < begin)
                return false;
            if (end != 0 && timestamp > end)
                return false;
            return true;
        }

        std::string toString() const
        {
            return "TimeZone: begin: " + std::to_string(begin) + ", end: " + std::to_string(end);
        }
    };
    std::shared_ptr<TimeZone> timeZone;

    struct FrameZone
    {
        uint64_t begin{0};
        uint64_t count{0};

        bool InRange(uint64_t index) const
        {
            if (count == 0)
                return true;
            if (index < begin)
                return false;
            if (index > (begin + count))
                return false;
            return true;
        }

        std::string toString() const
        {
            return "FrameZone: begin: " + std::to_string(begin) + ", count: " + std::to_string(count);
        }
    };
    std::shared_ptr<FrameZone> frameZone;

    struct EndFramePair
    {
        std::shared_ptr<std::string> got;
        std::shared_ptr<std::string> wanted;
        std::shared_ptr<std::string> replace;
    };
    std::shared_ptr<std::vector<std::shared_ptr<EndFramePair>>> endFramePair;

    std::shared_ptr<std::vector<std::shared_ptr<FramePair>>>  frameFrontModification;
    std::shared_ptr<std::vector<std::shared_ptr<FramePair>>>  frameEndModification;
    std::shared_ptr<std::vector<std::shared_ptr<FrontFrame>>> frameFrontSkipping;
    std::shared_ptr<std::vector<std::shared_ptr<EndFrame>>>   frameEndSkipping;

    bool perTidOutput{false};

    LogLevel debugLevel = LogLevel::INFO;

    bool HasFuncZone() const { return funcZone != nullptr; }
    bool HasTimeZone() const { return timeZone != nullptr; }
    bool HasFrameZone() const { return frameZone != nullptr; }

    bool IsBetweenValidSampleTimeZone(uint64_t timestamp) const
    {
        if (timeZone) {
            return timeZone->InRange(timestamp);
        }
        return true;
    }

    void Parse(const char *conf)
    {
        if (!conf || strlen(conf) == 0) {
            return;
        }

        try {
            std::ifstream confJson(conf);
            if (!confJson.is_open()) {
                std::cerr << "Failed to open JSON configuration file: " << conf << std::endl;
                return;
            }

            auto json_conf = json::parse(confJson);
            confJson.close();

            // Zone parsing
            if (json_conf.contains("zone")) {
                auto zone = json_conf["zone"];

                // Function zone
                if (zone.contains("function")) {
                    auto function = zone["function"];
                    funcZone      = std::make_shared<FuncZone>();

                    if (function.contains("begin")) {
                        funcZone->begin = std::make_shared<Functrace>();
                        if (function["begin"].contains("currentip") && !function["begin"]["currentip"].is_null())
                            funcZone->begin->currentSym = function["begin"]["currentip"].get<std::string>();
                        if (function["begin"].contains("targetsym") && !function["begin"]["targetsym"].is_null())
                            funcZone->begin->targetIP = function["begin"]["targetsym"].get<std::string>();
                        if (function["begin"].contains("flags"))
                            funcZone->begin->flags = function["begin"]["flags"].is_string()
                                                         ? std::stoul(function["begin"]["flags"].get<std::string>(), nullptr, 0)
                                                         : function["begin"]["flags"].get<uint32_t>();
                        if (function["begin"].contains("tid"))
                            funcZone->begin->tid = function["begin"]["tid"].is_string()
                                                       ? std::stoul(function["begin"]["tid"].get<std::string>(), nullptr, 0)
                                                       : function["begin"]["tid"].get<uint32_t>();
                    }

                    if (function.contains("end")) {
                        funcZone->end = std::make_shared<Functrace>();
                        if (function["end"].contains("currentip") && !function["end"]["currentip"].is_null())
                            funcZone->end->currentSym = function["end"]["currentip"].get<std::string>();
                        if (function["end"].contains("targetsym") && !function["end"]["targetsym"].is_null())
                            funcZone->end->targetIP = function["end"]["targetsym"].get<std::string>();
                        if (function["end"].contains("flags"))
                            funcZone->end->flags = function["end"]["flags"].is_string()
                                                       ? std::stoul(function["end"]["flags"].get<std::string>(), nullptr, 0)
                                                       : function["end"]["flags"].get<uint32_t>();
                        if (function["end"].contains("tid"))
                            funcZone->end->tid = function["end"]["tid"].is_string()
                                                     ? std::stoul(function["end"]["tid"].get<std::string>(), nullptr, 0)
                                                     : function["end"]["tid"].get<uint32_t>();
                    }

                    if (function.contains("repeat")) {
                        funcZone->repeat = function["repeat"].is_string()
                                               ? std::stoul(function["repeat"].get<std::string>(), nullptr, 0)
                                               : function["repeat"].get<uint32_t>();
                    }
                    if (function.contains("repeataccumulated")) {
                        funcZone->repeataccumulated = function["repeataccumulated"].is_string()
                                                          ? std::stoul(function["repeataccumulated"].get<std::string>(), nullptr, 0)
                                                          : function["repeataccumulated"].get<uint32_t>();
                    }
                }

                // Time zone
                if (zone.contains("time")) {
                    auto time = std::make_shared<TimeZone>();
                    if (zone["time"].contains("begin")) {
                        time->begin = zone["time"]["begin"].is_string()
                                          ? std::stoull(zone["time"]["begin"].get<std::string>(), nullptr, 0)
                                          : zone["time"]["begin"].get<uint64_t>();
                    }
                    if (zone["time"].contains("end")) {
                        time->end = zone["time"]["end"].is_string()
                                        ? std::stoull(zone["time"]["end"].get<std::string>(), nullptr, 0)
                                        : zone["time"]["end"].get<uint64_t>();
                    }
                    if (time->begin != 0 || time->end != 0)
                        timeZone = time;
                }

                // Frame zone
                if (zone.contains("frame")) {
                    auto frame = std::make_shared<FrameZone>();
                    if (zone["frame"].contains("begin")) {
                        frame->begin = zone["frame"]["begin"].is_string()
                                           ? std::stoull(zone["frame"]["begin"].get<std::string>(), nullptr, 0)
                                           : zone["frame"]["begin"].get<uint64_t>();
                    }
                    if (zone["frame"].contains("count")) {
                        frame->count = zone["frame"]["count"].is_string()
                                           ? std::stoull(zone["frame"]["count"].get<std::string>(), nullptr, 0)
                                           : zone["frame"]["count"].get<uint64_t>();
                    }
                    if (frame->begin != 0 || frame->count != 0)
                        frameZone = frame;
                }
            }

            // Frame Modification
            if (json_conf.contains("modify")) {
                if (json_conf["modify"].contains("frontframe")) {
                    frameFrontModification = std::make_shared<std::vector<std::shared_ptr<FramePair>>>();
                    auto modifications     = json_conf["modify"]["frontframe"];
                    for (const auto &item : modifications) {
                        auto frameMatch   = std::make_shared<FrontFrame>();
                        auto frameReplace = std::make_shared<FrontFrame>();
                        if (item.contains("match")) {
                            frameMatch->FromJson(item["match"]);
                        }
                        if (item.contains("replace")) {
                            frameReplace->FromJson(item["replace"]);
                        }
                        auto matchReplacePair = std::make_shared<FramePair>(frameMatch, frameReplace);
                        frameFrontModification->push_back(matchReplacePair);
                    }
                }

                if (json_conf["modify"].contains("endframe")) {
                    frameEndModification = std::make_shared<std::vector<std::shared_ptr<FramePair>>>();
                    auto modifications   = json_conf["modify"]["endframe"];
                    for (const auto &item : modifications) {
                        auto frameMatch   = std::make_shared<EndFrame>();
                        auto frameReplace = std::make_shared<EndFrame>();
                        if (item.contains("match")) {
                            frameMatch->FromJson(item["match"]);
                        }
                        if (item.contains("replace")) {
                            frameReplace->FromJson(item["replace"]);
                        }
                        auto matchReplacePair = std::make_shared<FramePair>(frameMatch, frameReplace);
                        frameEndModification->push_back(matchReplacePair);
                    }
                }

                // Endframe pair
                if (json_conf["modify"].contains("endframepair")) {
                    auto pairs   = json_conf["modify"]["endframepair"];
                    endFramePair = std::make_shared<std::vector<std::shared_ptr<EndFramePair>>>();
                    for (auto &p : pairs) {
                        auto framePair = std::make_shared<EndFramePair>();
                        if (p.contains("got") && !p["got"].is_null())
                            framePair->got = std::make_shared<std::string>(p["got"].get<std::string>());
                        if (p.contains("wanted") && !p["wanted"].is_null())
                            framePair->wanted = std::make_shared<std::string>(p["wanted"].get<std::string>());
                        if (p.contains("replace") && !p["replace"].is_null())
                            framePair->replace = std::make_shared<std::string>(p["replace"].get<std::string>());
                        endFramePair->push_back(framePair);
                    }
                }
            }

            // Frame skipping
            if (json_conf.contains("skip")) {
                if (json_conf["skip"].contains("frontframe")) {
                    auto skippings     = json_conf["skip"]["frontframe"];
                    frameFrontSkipping = std::make_shared<std::vector<std::shared_ptr<FrontFrame>>>();
                    for (auto &item : skippings) {
                        auto frame = std::make_shared<FrontFrame>();
                        frame->FromJson(item);
                        frameFrontSkipping->push_back(frame);
                    }
                }
                if (json_conf["skip"].contains("endframe")) {
                    auto skippings   = json_conf["skip"]["endframe"];
                    frameEndSkipping = std::make_shared<std::vector<std::shared_ptr<EndFrame>>>();
                    for (auto &item : skippings) {
                        auto frame = std::make_shared<EndFrame>();
                        frame->FromJson(item);
                        frameEndSkipping->push_back(frame);
                    }
                }
            }

            // Debug level
            if (json_conf.contains("debug")) {
                std::string levelStr;
                if (json_conf["debug"].is_string()) {
                    levelStr = json_conf["debug"].get<std::string>();
                } else if (json_conf["debug"].is_object() && json_conf["debug"].contains("level")) {
                    levelStr = json_conf["debug"]["level"].get<std::string>();
                }
                if (!levelStr.empty()) {
                    Logger::SetLevel(levelStr);
                    debugLevel = Logger::GetLevel();
                    if (Logger::IsInfo()) {
                        std::cout << "Debug level set to: " << Logger::LevelToString(Logger::GetLevel()) << std::endl;
                    }
                }
            }

            // Per-TID Perfetto output option
            if (json_conf.contains("perTidOutput")) {
                perTidOutput = json_conf["perTidOutput"].get<bool>();
            }

        } catch (const nlohmann::json::parse_error &e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
            clear();
        } catch (const std::exception &e) {
            std::cerr << "JSON configuration exception: " << e.what() << std::endl;
            clear();
        }
    }

    void Parse(const std::string &conf)
    {
        Parse(conf.c_str());
    }

    void clear()
    {
        funcZone               = nullptr;
        timeZone               = nullptr;
        frameZone              = nullptr;
        frameFrontModification = nullptr;
        frameEndModification   = nullptr;
        frameFrontSkipping     = nullptr;
        frameEndSkipping       = nullptr;
        endFramePair           = nullptr;
        perTidOutput           = false;
        debugLevel             = LogLevel::INFO;
        Logger::SetLevel(LogLevel::INFO);
    }
};

#endif // PTGRAPH2_JSON_H
