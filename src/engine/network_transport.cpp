#include "engine/network_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace noemancer {
namespace {

using Json=nlohmann::json;

#ifdef _WIN32
using Socket=SOCKET;
constexpr Socket invalid_socket=INVALID_SOCKET;
void close_socket(const Socket socket) { if(socket!=invalid_socket) closesocket(socket); }
int socket_error() { return WSAGetLastError(); }
struct SocketRuntime final {
    bool ready{};
    SocketRuntime() { WSADATA data{}; ready=WSAStartup(MAKEWORD(2,2),&data)==0; }
    ~SocketRuntime() { if(ready) WSACleanup(); }
};
#else
using Socket=int;
constexpr Socket invalid_socket=-1;
void close_socket(const Socket socket) { if(socket!=invalid_socket) close(socket); }
int socket_error() { return errno; }
struct SocketRuntime final { bool ready{true}; };
#endif

struct SocketGuard final {
    Socket value{invalid_socket};
    ~SocketGuard() { close_socket(value); }
};

Json failure(const std::string& code,const std::string& detail,const int native_error=0) {
    return {{"schemaVersion","noemancer.network-transport-verification/0.1"},{"valid",false},
        {"code",code},{"detail",detail},{"nativeError",native_error},{"transport","udp-loopback"}};
}

bool wait_readable(const Socket socket_value,const std::uint32_t timeout_milliseconds) {
    fd_set reads; FD_ZERO(&reads); FD_SET(socket_value,&reads);
    timeval timeout{static_cast<long>(timeout_milliseconds/1000U),static_cast<long>((timeout_milliseconds%1000U)*1000U)};
#ifdef _WIN32
    return select(0,&reads,nullptr,nullptr,&timeout)>0;
#else
    return select(socket_value+1,&reads,nullptr,nullptr,&timeout)>0;
#endif
}

bool send_all(const Socket socket_value,const char* data,std::size_t bytes) {
    while(bytes>0) {
        const auto sent=send(socket_value,data,static_cast<int>(std::min<std::size_t>(bytes,16384U)),0);
        if(sent<=0) return false;
        data+=sent; bytes-=static_cast<std::size_t>(sent);
    }
    return true;
}

bool receive_all(const Socket socket_value,char* data,std::size_t bytes,const std::uint32_t timeout_milliseconds) {
    while(bytes>0) {
        if(!wait_readable(socket_value,timeout_milliseconds)) return false;
        const auto received=recv(socket_value,data,static_cast<int>(std::min<std::size_t>(bytes,16384U)),0);
        if(received<=0) return false;
        data+=received; bytes-=static_cast<std::size_t>(received);
    }
    return true;
}

bool send_document(const Socket socket_value,const std::string_view document) {
    if(document.empty()||document.size()>65536U) return false;
    const auto network_size=htonl(static_cast<std::uint32_t>(document.size()));
    return send_all(socket_value,reinterpret_cast<const char*>(&network_size),sizeof(network_size))&&
        send_all(socket_value,document.data(),document.size());
}

Json receive_document(const Socket socket_value,const std::uint32_t timeout_milliseconds) {
    std::uint32_t network_size{};
    if(!receive_all(socket_value,reinterpret_cast<char*>(&network_size),sizeof(network_size),timeout_milliseconds)) return Json();
    const auto size=ntohl(network_size);
    if(size==0||size>65536U) return Json();
    std::string document(size,'\0');
    if(!receive_all(socket_value,document.data(),document.size(),timeout_milliseconds)) return Json();
    return Json::parse(document,nullptr,false);
}

void enable_address_reuse(const Socket socket_value) {
    constexpr int enabled=1;
#ifdef _WIN32
    setsockopt(socket_value,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&enabled),sizeof(enabled));
#else
    setsockopt(socket_value,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled));
#endif
}

Json session_failure(const std::string_view role,const std::string& code,const std::string& detail,const int native_error=0) {
    return {{"schemaVersion","noemancer.network-session/0.1"},{"success",false},{"role",role},
        {"code",code},{"detail",detail},{"nativeError",native_error},
        {"reliableControl",false},{"unreliableState",false}};
}

} // namespace

std::string verify_udp_loopback_transport_json(const std::size_t requested_payload_bytes) {
    SocketRuntime runtime;
    if(!runtime.ready) return failure("network.transport.initialize-failed","socket runtime initialization failed",socket_error()).dump();
    SocketGuard server{socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)};
    SocketGuard client{socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)};
    if(server.value==invalid_socket||client.value==invalid_socket)
        return failure("network.transport.socket-failed","unable to create UDP endpoints",socket_error()).dump();
#ifdef _WIN32
    constexpr DWORD timeout_ms=1000;
    setsockopt(server.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout_ms),sizeof(timeout_ms));
    setsockopt(client.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout_ms),sizeof(timeout_ms));
#else
    constexpr timeval timeout{1,0};
    setsockopt(server.value,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(client.value,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#endif
    sockaddr_in server_address{};
    server_address.sin_family=AF_INET; server_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); server_address.sin_port=0;
    if(bind(server.value,reinterpret_cast<const sockaddr*>(&server_address),sizeof(server_address))!=0)
        return failure("network.transport.bind-failed","unable to bind loopback endpoint",socket_error()).dump();
#ifdef _WIN32
    int address_size=sizeof(server_address);
#else
    socklen_t address_size=sizeof(server_address);
#endif
    if(getsockname(server.value,reinterpret_cast<sockaddr*>(&server_address),&address_size)!=0)
        return failure("network.transport.endpoint-failed","unable to resolve bound endpoint",socket_error()).dump();

    const auto payload_bytes=std::clamp<std::size_t>(requested_payload_bytes,1U,1200U);
    const std::string payload(payload_bytes,'N');
    const auto request=Json{{"schemaVersion","noemancer.network-datagram/0.1"},{"channel","state"},
        {"sequence",1},{"payload",payload}}.dump();
    const auto start=std::chrono::steady_clock::now();
    const auto sent=sendto(client.value,request.data(),static_cast<int>(request.size()),0,
        reinterpret_cast<const sockaddr*>(&server_address),sizeof(server_address));
    if(sent!=static_cast<int>(request.size()))
        return failure("network.transport.send-failed","state datagram was not fully sent",socket_error()).dump();
    std::array<char,2048> buffer{};
    sockaddr_in client_address{};
#ifdef _WIN32
    int client_size=sizeof(client_address);
#else
    socklen_t client_size=sizeof(client_address);
#endif
    const auto received=recvfrom(server.value,buffer.data(),static_cast<int>(buffer.size()),0,
        reinterpret_cast<sockaddr*>(&client_address),&client_size);
    if(received<=0) return failure("network.transport.receive-failed","server did not receive state datagram",socket_error()).dump();
    const auto decoded=Json::parse(std::string_view(buffer.data(),static_cast<std::size_t>(received)),nullptr,false);
    if(decoded.is_discarded()||decoded.value("sequence",0)!=1||decoded.value("payload",std::string{}).size()!=payload_bytes)
        return failure("network.transport.payload-invalid","server rejected the bounded datagram envelope").dump();
    const auto acknowledgement=Json{{"schemaVersion","noemancer.network-ack/0.1"},{"sequence",1},{"acceptedBytes",payload_bytes}}.dump();
    const auto acknowledged=sendto(server.value,acknowledgement.data(),static_cast<int>(acknowledgement.size()),0,
        reinterpret_cast<const sockaddr*>(&client_address),client_size);
    if(acknowledged!=static_cast<int>(acknowledgement.size()))
        return failure("network.transport.ack-send-failed","acknowledgement was not fully sent",socket_error()).dump();
    const auto ack_bytes=recvfrom(client.value,buffer.data(),static_cast<int>(buffer.size()),0,nullptr,nullptr);
    if(ack_bytes<=0) return failure("network.transport.ack-timeout","client did not receive acknowledgement",socket_error()).dump();
    const auto ack=Json::parse(std::string_view(buffer.data(),static_cast<std::size_t>(ack_bytes)),nullptr,false);
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    const bool valid=!ack.is_discarded()&&ack.value("sequence",0)==1&&ack.value("acceptedBytes",0U)==payload_bytes;
    return Json{{"schemaVersion","noemancer.network-transport-verification/0.1"},{"valid",valid},
        {"code",valid?"ok":"network.transport.ack-invalid"},{"transport","udp-loopback"},
        {"channel","unreliable-state"},{"address","127.0.0.1"},{"port",ntohs(server_address.sin_port)},
        {"payloadBytes",payload_bytes},{"requestBytes",request.size()},{"ackBytes",acknowledgement.size()},
        {"roundTripMicroseconds",elapsed},{"kernelSocket",true},{"boundedDatagramBytes",1200}}.dump();
}

std::string run_network_server_json(const std::uint16_t port,const std::uint32_t requested_session_budget,
                                    const std::uint32_t requested_timeout_milliseconds) {
    const auto session_budget=std::clamp(requested_session_budget,1U,64U);
    const auto timeout_milliseconds=std::clamp(requested_timeout_milliseconds,100U,60000U);
    if(port==0) return session_failure("server","network.server.invalid-port","port must be in [1, 65535]").dump();
    SocketRuntime runtime;
    if(!runtime.ready) return session_failure("server","network.transport.initialize-failed","socket runtime initialization failed",socket_error()).dump();
    SocketGuard control_listener{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    SocketGuard state_socket{socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)};
    if(control_listener.value==invalid_socket||state_socket.value==invalid_socket)
        return session_failure("server","network.server.socket-failed","unable to create server endpoints",socket_error()).dump();
    enable_address_reuse(control_listener.value); enable_address_reuse(state_socket.value);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(port);
    if(bind(control_listener.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=0||
       bind(state_socket.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=0)
        return session_failure("server","network.server.bind-failed","unable to bind TCP and UDP endpoints",socket_error()).dump();
    if(listen(control_listener.value,8)!=0)
        return session_failure("server","network.server.listen-failed","unable to listen for control sessions",socket_error()).dump();

    Json sessions=Json::array();
    for(std::uint32_t session_index=0;session_index<session_budget;++session_index) {
        if(!wait_readable(control_listener.value,timeout_milliseconds))
            return session_failure("server","network.server.control-timeout","no client opened a control session",socket_error()).dump();
        sockaddr_in peer_address{};
#ifdef _WIN32
        int peer_size=sizeof(peer_address);
#else
        socklen_t peer_size=sizeof(peer_address);
#endif
        SocketGuard control{accept(control_listener.value,reinterpret_cast<sockaddr*>(&peer_address),&peer_size)};
        if(control.value==invalid_socket)
            return session_failure("server","network.server.accept-failed","unable to accept control session",socket_error()).dump();
        const auto hello=receive_document(control.value,timeout_milliseconds);
        if(hello.is_discarded()||hello.is_null()||hello.value("schemaVersion",std::string{})!="noemancer.network-control-hello/0.1")
            return session_failure("server","network.server.hello-invalid","client control hello is invalid").dump();
        const auto peer_id=hello.value("peerId",std::string{});
        if(peer_id.empty()||peer_id.size()>128U)
            return session_failure("server","network.server.peer-invalid","peer ID is empty or too large").dump();
        const auto control_ack=Json{{"schemaVersion","noemancer.network-control-ack/0.1"},{"accepted",true},
            {"peerId",peer_id},{"session",session_index+1U},{"statePort",port}}.dump();
        if(!send_document(control.value,control_ack))
            return session_failure("server","network.server.control-send-failed","unable to send reliable control acknowledgement",socket_error()).dump();
        if(!wait_readable(state_socket.value,timeout_milliseconds))
            return session_failure("server","network.server.state-timeout","no state datagram arrived",socket_error()).dump();
        std::array<char,2048> buffer{}; sockaddr_in state_peer{};
#ifdef _WIN32
        int state_peer_size=sizeof(state_peer);
#else
        socklen_t state_peer_size=sizeof(state_peer);
#endif
        const auto state_bytes=recvfrom(state_socket.value,buffer.data(),static_cast<int>(buffer.size()),0,
            reinterpret_cast<sockaddr*>(&state_peer),&state_peer_size);
        const auto state=state_bytes>0?Json::parse(std::string_view(buffer.data(),static_cast<std::size_t>(state_bytes)),nullptr,false):Json();
        if(state_bytes<=0||state.is_discarded()||state.is_null()||state.value("schemaVersion",std::string{})!="noemancer.network-datagram/0.1"||
           state.value("peerId",std::string{})!=peer_id)
            return session_failure("server","network.server.state-invalid","state datagram did not match the control session",socket_error()).dump();
        const auto sequence=state.value("sequence",0ULL);
        const auto payload_bytes=state.value("payload",std::string{}).size();
        const auto state_ack=Json{{"schemaVersion","noemancer.network-ack/0.1"},{"sequence",sequence},
            {"acceptedBytes",payload_bytes},{"peerId",peer_id}}.dump();
        if(sendto(state_socket.value,state_ack.data(),static_cast<int>(state_ack.size()),0,
            reinterpret_cast<const sockaddr*>(&state_peer),state_peer_size)!=static_cast<int>(state_ack.size()))
            return session_failure("server","network.server.state-ack-failed","unable to acknowledge state datagram",socket_error()).dump();
        sessions.push_back({{"session",session_index+1U},{"peerId",peer_id},{"sequence",sequence},
            {"statePayloadBytes",payload_bytes},{"controlBytes",control_ack.size()}});
    }
    return Json{{"schemaVersion","noemancer.network-session/0.1"},{"success",true},{"role","server"},{"code","ok"},
        {"address","127.0.0.1"},{"port",port},{"sessionBudget",session_budget},{"completedSessions",sessions.size()},
        {"lifecycle","stopped-after-session-budget"},{"reliableControl",true},{"unreliableState",true},
        {"boundedControlBytes",65536},{"boundedDatagramBytes",1200},{"sessions",std::move(sessions)}}.dump();
}

std::string run_network_client_json(const std::string_view host,const std::uint16_t port,const std::string_view requested_peer_id,
                                    const std::size_t requested_state_payload_bytes,const std::uint32_t requested_timeout_milliseconds) {
    const auto timeout_milliseconds=std::clamp(requested_timeout_milliseconds,100U,60000U);
    const auto payload_bytes=std::clamp<std::size_t>(requested_state_payload_bytes,1U,1200U);
    const std::string peer_id=requested_peer_id.empty()?"client.local":std::string(requested_peer_id.substr(0,128));
    if(port==0) return session_failure("client","network.client.invalid-port","port must be in [1, 65535]").dump();
    SocketRuntime runtime;
    if(!runtime.ready) return session_failure("client","network.transport.initialize-failed","socket runtime initialization failed",socket_error()).dump();
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port);
    if(inet_pton(AF_INET,std::string(host).c_str(),&address.sin_addr)!=1)
        return session_failure("client","network.client.invalid-address","only explicit IPv4 addresses are accepted").dump();
    SocketGuard control{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    SocketGuard state_socket{socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)};
    if(control.value==invalid_socket||state_socket.value==invalid_socket)
        return session_failure("client","network.client.socket-failed","unable to create client endpoints",socket_error()).dump();
    const auto started=std::chrono::steady_clock::now();
    if(connect(control.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=0)
        return session_failure("client","network.client.connect-failed","unable to connect reliable control channel",socket_error()).dump();
    const auto hello=Json{{"schemaVersion","noemancer.network-control-hello/0.1"},{"peerId",peer_id},
        {"capabilities",{"reliable-control","unreliable-state"}}}.dump();
    if(!send_document(control.value,hello))
        return session_failure("client","network.client.hello-send-failed","unable to send reliable control hello",socket_error()).dump();
    const auto control_ack=receive_document(control.value,timeout_milliseconds);
    if(control_ack.is_discarded()||control_ack.is_null()||!control_ack.value("accepted",false)||control_ack.value("peerId",std::string{})!=peer_id)
        return session_failure("client","network.client.control-rejected","server rejected the reliable control session").dump();
    const std::string payload(payload_bytes,'S');
    const auto state=Json{{"schemaVersion","noemancer.network-datagram/0.1"},{"channel","state"},{"peerId",peer_id},
        {"sequence",1},{"payload",payload}}.dump();
    if(sendto(state_socket.value,state.data(),static_cast<int>(state.size()),0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=static_cast<int>(state.size()))
        return session_failure("client","network.client.state-send-failed","unable to send bounded state datagram",socket_error()).dump();
    if(!wait_readable(state_socket.value,timeout_milliseconds))
        return session_failure("client","network.client.state-ack-timeout","server did not acknowledge state datagram",socket_error()).dump();
    std::array<char,2048> buffer{};
    const auto ack_bytes=recvfrom(state_socket.value,buffer.data(),static_cast<int>(buffer.size()),0,nullptr,nullptr);
    const auto state_ack=ack_bytes>0?Json::parse(std::string_view(buffer.data(),static_cast<std::size_t>(ack_bytes)),nullptr,false):Json();
    const bool accepted=ack_bytes>0&&!state_ack.is_discarded()&&!state_ack.is_null()&&state_ack.value("sequence",0ULL)==1ULL&&
        state_ack.value("acceptedBytes",0U)==payload_bytes&&state_ack.value("peerId",std::string{})==peer_id;
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
    if(!accepted) return session_failure("client","network.client.state-ack-invalid","state acknowledgement is invalid",socket_error()).dump();
    return Json{{"schemaVersion","noemancer.network-session/0.1"},{"success",true},{"role","client"},{"code","ok"},
        {"peerId",peer_id},{"serverAddress",host},{"serverPort",port},{"session",control_ack.value("session",0U)},
        {"reliableControl",true},{"unreliableState",true},{"statePayloadBytes",payload_bytes},
        {"controlRequestBytes",hello.size()},{"stateRequestBytes",state.size()},{"stateAckBytes",ack_bytes},
        {"roundTripMicroseconds",elapsed},{"boundedControlBytes",65536},{"boundedDatagramBytes",1200}}.dump();
}

} // namespace noemancer
