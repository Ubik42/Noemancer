#include "engine/managed_debug_protocol.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

int main() {
    const auto initialize=noemancer::dap_request_frame(1,"initialize",R"({"clientID":"noemancer"})");
    const auto attach=noemancer::dap_request_frame(2,"attach",R"({"processId":42})");
    noemancer::DapStreamDecoder decoder;
    const auto split=initialize.size()/2;
    if(!decoder.feed(std::string_view(initialize).substr(0,split)).messages.empty())return 1;
    auto completed=decoder.feed(std::string_view(initialize).substr(split));
    if(completed.error||completed.messages.size()!=1)return 2;
    const auto initialize_json=nlohmann::json::parse(completed.messages.front());
    if(initialize_json.at("seq")!=1||initialize_json.at("command")!="initialize"||
       initialize_json.at("arguments").at("clientID")!="noemancer")return 3;

    const auto combined=attach+noemancer::dap_request_frame(3,"configurationDone");
    auto multiple=decoder.feed(combined);
    if(multiple.error||multiple.messages.size()!=2||nlohmann::json::parse(multiple.messages[1]).at("seq")!=3)return 4;

    auto malformed=decoder.feed("Content-Length: nope\r\n\r\n{}");
    if(!malformed.error||*malformed.error!="dap.invalid-content-length"||!decoder.failed())return 5;
    if(decoder.feed(initialize).error!="dap.decoder-failed")return 6;
    decoder.reset();
    if(decoder.failed()||decoder.feed("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\nContent-Length: 2\r\n\r\n{}").messages.size()!=1)return 7;

    decoder.reset();
    const auto oversized="Content-Length: "+std::to_string(noemancer::DapStreamDecoder::maximum_message_bytes+1)+"\r\n\r\n";
    const auto rejected=decoder.feed(oversized);
    if(!rejected.error||*rejected.error!="dap.invalid-content-length")return 8;
    try {static_cast<void>(noemancer::dap_request_frame(0,"attach"));return 9;}
    catch(const std::invalid_argument&) {}
    noemancer::DapObservationReducer observation;
    if(!observation.ingest(R"({"seq":10,"type":"event","event":"initialized"})")||
       !observation.ingest(R"({"seq":11,"type":"event","event":"stopped","body":{"reason":"breakpoint","threadId":7,"allThreadsStopped":true}})")||
       !observation.ingest(R"({"seq":12,"type":"response","request_seq":3,"success":true,"command":"threads","body":{"threads":[{"id":7,"name":"Main Thread"}]}})")||
       !observation.ingest(R"({"seq":13,"type":"response","request_seq":4,"success":true,"command":"stackTrace","body":{"stackFrames":[{"id":21,"name":"PlayerGameplay.OnTriggerEnter","source":{"path":"PlayerGameplay.cs"},"line":24,"column":9}]}})")||
       !observation.ingest(R"({"seq":14,"type":"response","request_seq":5,"success":true,"command":"scopes","body":{"scopes":[{"name":"Locals","variablesReference":88,"expensive":false}]}})")||
       !observation.ingest(R"({"seq":15,"type":"response","request_seq":6,"success":true,"command":"variables","body":{"variables":[{"name":"other","value":"entity.demo-sphere","type":"EntityId","evaluateName":"other","variablesReference":0}]}})"))return 10;
    const auto paused=nlohmann::json::parse(observation.snapshot_json());
    if(paused.at("state")!="paused"||paused.at("stop").at("reason")!="breakpoint"||paused.at("frames").at(0).at("line")!=24||
       paused.at("variables").at(0).at("value")!="entity.demo-sphere")return 11;
    if(!observation.ingest(R"({"seq":16,"type":"event","event":"continued","body":{"threadId":7}})")||
       nlohmann::json::parse(observation.snapshot_json()).at("state")!="running")return 12;

    // Observation is intentionally bounded before it crosses into an editor
    // or Agent context. Exercise each collection limit and terminal state so
    // a verbose adapter cannot grow an unbounded semantic snapshot.
    noemancer::DapObservationReducer bounded;
    if(!bounded.ingest(R"({"seq":1,"type":"event","event":"initialized"})"))return 13;
    nlohmann::json threads=nlohmann::json::array();
    for(std::uint64_t index=0;index<65U;++index)
        threads.push_back({{"id",index+1U},{"name","thread-"+std::to_string(index)}});
    if(!bounded.ingest(nlohmann::json{{"seq",2},{"type","response"},{"request_seq",1},
        {"success",true},{"command","threads"},{"body",{{"threads",std::move(threads)}}}}.dump()))return 14;
    nlohmann::json frames=nlohmann::json::array();
    for(std::uint64_t index=0;index<65U;++index)
        frames.push_back({{"id",index+1U},{"name","frame-"+std::to_string(index)},
            {"source",{{"path","script-"+std::to_string(index)+".cs"}}},{"line",index+1U},{"column",1U}});
    if(!bounded.ingest(nlohmann::json{{"seq",3},{"type","response"},{"request_seq",2},
        {"success",true},{"command","stackTrace"},{"body",{{"stackFrames",std::move(frames)}}}}.dump()))return 15;
    nlohmann::json scopes=nlohmann::json::array();
    for(std::uint64_t index=0;index<40U;++index)
        scopes.push_back({{"name","scope-"+std::to_string(index)},{"variablesReference",index+1U},{"expensive",false}});
    if(!bounded.ingest(nlohmann::json{{"seq",4},{"type","response"},{"request_seq",3},
        {"success",true},{"command","scopes"},{"body",{{"scopes",std::move(scopes)}}}}.dump()))return 16;
    nlohmann::json variables=nlohmann::json::array();
    for(std::uint64_t index=0;index<257U;++index)
        variables.push_back({{"name","variable-"+std::to_string(index)},{"value",std::to_string(index)},
            {"type","int"},{"evaluateName","value"},{"variablesReference",0U}});
    if(!bounded.ingest(nlohmann::json{{"seq",5},{"type","response"},{"request_seq",4},
        {"success",true},{"command","variables"},{"body",{{"variables",std::move(variables)}}}}.dump()))return 17;
    const auto bounded_snapshot=nlohmann::json::parse(bounded.snapshot_json());
    if(bounded_snapshot.at("threads").size()!=64U||bounded_snapshot.at("frames").size()!=64U||
       !bounded_snapshot.at("framesTruncated").get<bool>()||bounded_snapshot.at("scopes").size()!=32U||
       bounded_snapshot.at("variables").size()!=256U||!bounded_snapshot.at("variablesTruncated").get<bool>())return 18;
    if(!bounded.ingest(R"({"seq":6,"type":"request","command":"runInTerminal","arguments":{}})"))return 19;
    if(!bounded.ingest(R"({"seq":7,"type":"event","event":"terminated","body":{}})"))return 20;
    const auto terminated=nlohmann::json::parse(bounded.snapshot_json());
    if(terminated.at("state")!="terminated"||terminated.at("messageCount")!=7U)return 21;
    std::cout<<"DAP framing and bounded stream decoding passed\n";
    return 0;
}
