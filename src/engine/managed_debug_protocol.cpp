#include "engine/managed_debug_protocol.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <stdexcept>

namespace noemancer {
namespace {
using Json=nlohmann::json;

std::optional<std::size_t> content_length(const std::string_view header) {
    constexpr std::string_view name="Content-Length:";
    std::optional<std::size_t> result;
    std::size_t line_begin{};
    while(line_begin<header.size()) {
        const auto line_end=header.find("\r\n",line_begin);
        const auto line=header.substr(line_begin,line_end==std::string_view::npos?header.size()-line_begin:line_end-line_begin);
        if(line.starts_with(name)) {
            auto value=line.substr(name.size());
            while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.remove_prefix(1);
            std::size_t parsed{};
            const auto converted=std::from_chars(value.data(),value.data()+value.size(),parsed);
            if(converted.ec!=std::errc{}||converted.ptr!=value.data()+value.size()||result)return std::nullopt;
            result=parsed;
        }
        if(line_end==std::string_view::npos)break;
        line_begin=line_end+2;
    }
    return result;
}
}

DapDecodeBatch DapStreamDecoder::feed(const std::string_view bytes) {
    DapDecodeBatch batch;
    if(failed_) {batch.error="dap.decoder-failed";return batch;}
    if(buffer_.size()+bytes.size()>maximum_header_bytes+maximum_message_bytes) {
        failed_=true;buffer_.clear();batch.error="dap.buffer-limit-exceeded";return batch;
    }
    buffer_.append(bytes);
    while(true) {
        if(!expected_body_bytes_) {
            const auto header_end=buffer_.find("\r\n\r\n");
            if(header_end==std::string::npos) {
                if(buffer_.size()>maximum_header_bytes) {failed_=true;buffer_.clear();batch.error="dap.header-limit-exceeded";}
                return batch;
            }
            if(header_end>maximum_header_bytes) {failed_=true;buffer_.clear();batch.error="dap.header-limit-exceeded";return batch;}
            const auto length=content_length(std::string_view(buffer_).substr(0,header_end));
            if(!length||*length==0||*length>maximum_message_bytes) {
                failed_=true;buffer_.clear();batch.error="dap.invalid-content-length";return batch;
            }
            expected_body_bytes_=*length;
            buffer_.erase(0,header_end+4);
        }
        if(buffer_.size()<*expected_body_bytes_)return batch;
        auto message=buffer_.substr(0,*expected_body_bytes_);
        buffer_.erase(0,*expected_body_bytes_);
        expected_body_bytes_.reset();
        const auto parsed=Json::parse(message,nullptr,false);
        if(parsed.is_discarded()||!parsed.is_object()) {
            failed_=true;buffer_.clear();batch.messages.clear();batch.error="dap.invalid-json-message";return batch;
        }
        batch.messages.push_back(std::move(message));
    }
}

void DapStreamDecoder::reset() noexcept {
    buffer_.clear();expected_body_bytes_.reset();failed_=false;
}

bool DapObservationReducer::ingest(const std::string_view message_json) {
    const auto message=Json::parse(message_json,nullptr,false);
    if(message.is_discarded()||!message.is_object()||!message.contains("type")||!message.at("type").is_string())return false;
    ++message_count_;
    const auto type=message.at("type").get<std::string>();
    if(type=="event") {
        const auto event=message.value("event",std::string{});const auto body=message.value("body",Json::object());
        if(event=="initialized")state_="initialized";
        else if(event=="stopped") {
            state_="paused";stop_reason_=body.value("reason",std::string("unknown"));thread_id_=body.value("threadId",0ULL);
            all_threads_stopped_=body.value("allThreadsStopped",false);frames_.clear();scopes_.clear();variables_.clear();
        } else if(event=="continued") {
            state_="running";stop_reason_.clear();frames_.clear();scopes_.clear();variables_.clear();
        } else if(event=="terminated"||event=="exited")state_="terminated";
        return true;
    }
    if(type!="response")return type=="request";
    if(!message.value("success",false)) {last_error_=message.value("message",std::string("DAP request failed."));return true;}
    const auto command=message.value("command",std::string{});const auto body=message.value("body",Json::object());
    if(command=="threads") {
        threads_.clear();
        for(const auto& thread:body.value("threads",Json::array()))if(threads_.size()<64)
            threads_.push_back({thread.value("id",0ULL),thread.value("name",std::string{})});
    } else if(command=="stackTrace") {
        frames_.clear();const auto source_frames=body.value("stackFrames",Json::array());frames_truncated_=source_frames.size()>64;
        for(const auto& frame:source_frames)if(frames_.size()<64) {
            const auto source=frame.value("source",Json::object());
            frames_.push_back({frame.value("id",0ULL),frame.value("name",std::string{}),source.value("path",std::string{}),
                frame.value("line",0U),frame.value("column",0U)});
        }
    } else if(command=="scopes") {
        scopes_.clear();for(const auto& scope:body.value("scopes",Json::array()))if(scopes_.size()<32)
            scopes_.push_back({scope.value("name",std::string{}),scope.value("variablesReference",0ULL),scope.value("expensive",false)});
    } else if(command=="variables") {
        variables_.clear();const auto source_variables=body.value("variables",Json::array());variables_truncated_=source_variables.size()>256;
        for(const auto& variable:source_variables)if(variables_.size()<256)variables_.push_back({variable.value("name",std::string{}),
            variable.value("value",std::string{}),variable.value("type",std::string{}),variable.value("evaluateName",std::string{}),
            variable.value("variablesReference",0ULL)});
    }
    return true;
}

std::string DapObservationReducer::snapshot_json() const {
    Json threads=Json::array();for(const auto& value:threads_)threads.push_back({{"id",value.id},{"name",value.name}});
    Json frames=Json::array();for(const auto& value:frames_)frames.push_back({{"id",value.id},{"name",value.name},
        {"source",value.source},{"line",value.line},{"column",value.column}});
    Json scopes=Json::array();for(const auto& value:scopes_)scopes.push_back({{"name",value.name},
        {"variablesReference",value.variables_reference},{"expensive",value.expensive}});
    Json variables=Json::array();for(const auto& value:variables_)variables.push_back({{"name",value.name},{"value",value.value},
        {"type",value.type},{"evaluateName",value.evaluate_name},{"variablesReference",value.variables_reference}});
    return Json{{"schemaVersion","noemancer.managed-debug-observation/0.1"},{"state",state_},{"messageCount",message_count_},
        {"stop",{{"reason",stop_reason_},{"threadId",thread_id_},{"allThreadsStopped",all_threads_stopped_}}},
        {"threads",std::move(threads)},{"frames",std::move(frames)},{"framesTruncated",frames_truncated_},
        {"scopes",std::move(scopes)},{"variables",std::move(variables)},{"variablesTruncated",variables_truncated_},
        {"lastError",last_error_}}.dump();
}

std::string dap_request_frame(const std::uint64_t sequence,const std::string_view command,const std::string_view arguments_json) {
    const auto arguments=Json::parse(arguments_json,nullptr,false);
    if(sequence==0||command.empty()||arguments.is_discarded()||!arguments.is_object())
        throw std::invalid_argument("DAP request requires a positive sequence, command and object arguments.");
    return dap_message_frame(Json{{"seq",sequence},{"type","request"},{"command",command},{"arguments",arguments}}.dump());
}

std::string dap_message_frame(const std::string_view message_json) {
    const auto message=Json::parse(message_json,nullptr,false);
    if(message.is_discarded()||!message.is_object())throw std::invalid_argument("DAP message must be a JSON object.");
    const auto body=message.dump();
    return "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}

} // namespace noemancer
