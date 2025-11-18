#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

constexpr const char* HOST = "0.0.0.0";

std::atomic<bool> g_stop{false};

void signal_handler(int) {
    g_stop.store(true);
}

void safe_print(const std::string& msg) {
    try {
        std::cout << msg << std::endl;
    } catch (...) {
    }
}

std::vector<json> parse_json_objects_from_chunk(const std::string& chunk) {
    std::vector<json> result;

    std::string s;
    s.reserve(chunk.size());
    for (char c : chunk) {
        if (c != '\0') {
            s.push_back(c);
        }
    }

    const int n = static_cast<int>(s.size());
    int i = 0;

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (i >= n) break;

        if (s[i] != '{') {
            ++i;
            continue;
        }

        int depth = 0;
        bool in_string = false;
        bool escape = false;
        int start = i;
        int j = i;

        for (; j < n; ++j) {
            char c = s[j];

            if (in_string) {
                if (escape) {
                    escape = false;
                } else if (c == '\\') {
                    escape = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else {
                if (c == '"') {
                    in_string = true;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        std::string obj_str = s.substr(start, j - start + 1);
                        try {
                            result.push_back(json::parse(obj_str));
                        } catch (const std::exception& e) {
                            safe_print(std::string("[parse_json] erro ao parsear: ") + e.what());
                        }
                        i = j + 1;
                        break;
                    }
                }
            }
        }

        if (depth > 0 || j >= n) {
            break;
        }
    }

    return result;
}

class TelemetryServer {
public:
    using UpgradeFn = std::function<json(json)>;

    TelemetryServer(const std::string& name,
                    int port,
                    int broadcast_hz,
                    UpgradeFn upgrade_fn,
                    const std::string& log_file,
                    bool log_to_file = true,
                    double client_idle_timeout = 10.0,
                    double send_timeout = 0.5,
                    bool print_tx = false)
        : name_(name),
          host_(HOST),
          port_(port),
          broadcast_hz_(broadcast_hz),
          upgrade_json_(std::move(upgrade_fn)),
          log_file_(log_file),
          log_to_file_(log_to_file && !log_file.empty()),
          client_idle_timeout_(client_idle_timeout),
          send_timeout_(send_timeout),
          print_tx_(print_tx),
          server_fd_(-1),
          simulink_fd_(-1),
          running_(false),
          accept_thread_alive_(false),
          broadcaster_thread_alive_(false) {}

    TelemetryServer(const TelemetryServer&) = delete;
    TelemetryServer& operator=(const TelemetryServer&) = delete;

    ~TelemetryServer() {
        stop();
    }

    void start() {
        if (running_) return;
        running_ = true;

        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            safe_print("[" + name_ + "] erro ao criar socket");
            running_ = false;
            return;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            safe_print("[" + name_ + "] erro em bind");
            ::close(server_fd_);
            server_fd_ = -1;
            running_ = false;
            return;
        }

        if (::listen(server_fd_, 16) < 0) {
            safe_print("[" + name_ + "] erro em listen");
            ::close(server_fd_);
            server_fd_ = -1;
            running_ = false;
            return;
        }

        safe_print("[" + name_ + "] servidor rodando em " + host_ + ":" + std::to_string(port_));

        {
            std::lock_guard<std::mutex> lk(threads_mutex_);
            threads_.emplace_back(&TelemetryServer::broadcaster, this);
            threads_.emplace_back(&TelemetryServer::accept_connections, this);
        }
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }

        {
            std::lock_guard<std::mutex> lk(clients_mutex_);
            for (auto& kv : clients_by_ip_) {
                ::close(kv.second);
            }
            clients_by_ip_.clear();
        }

        {
            std::lock_guard<std::mutex> lk(simulink_mutex_);
            if (simulink_fd_ >= 0) {
                ::close(simulink_fd_);
                simulink_fd_ = -1;
            }
        }
 
        {
            std::lock_guard<std::mutex> lk(threads_mutex_);
            for (auto& t : threads_) {
                if (t.joinable()) t.join();
            }
            threads_.clear();
        }

        accept_thread_alive_ = false;
        broadcaster_thread_alive_ = false;

        safe_print("[" + name_ + "] servidor parado");
    }

    bool is_running() const {
        return running_.load();
    }

    bool is_healthy() const {
        return running_.load() &&
               accept_thread_alive_.load() &&
               broadcaster_thread_alive_.load();
    }

    const std::string& name() const {
        return name_;
    }

private:
    std::string name_;
    std::string host_;
    int port_;
    int broadcast_hz_;
    UpgradeFn upgrade_json_;
    std::string log_file_;
    bool log_to_file_;
    double client_idle_timeout_;
    double send_timeout_;
    bool print_tx_;

    int server_fd_;
    std::atomic<bool> running_;

    int simulink_fd_;
    std::mutex simulink_mutex_;

    std::unordered_map<std::string, int> clients_by_ip_;
    std::mutex clients_mutex_;

    std::string latest_payload_;
    std::mutex payload_mutex_;

    std::vector<std::thread> threads_;
    std::mutex threads_mutex_;

    std::atomic<bool> accept_thread_alive_;
    std::atomic<bool> broadcaster_thread_alive_;

    void append_log(const std::string& line) {
        if (!log_to_file_) return;
        try {
            std::ofstream f(log_file_, std::ios::app);
            if (f) {
                f << line << "\n";
            }
        } catch (...) {
        }
    }

    void set_latest_payload_from_obj(const json& obj) {
        try {
            json upgraded = upgrade_json_(obj);
            std::string s = upgraded.dump();
            {
                std::lock_guard<std::mutex> lk(payload_mutex_);
                latest_payload_ = s;
            }
            append_log(s);
        } catch (const std::exception& e) {
            safe_print("[" + name_ + "] falha ao serializar/gravar JSON: " + std::string(e.what()));
        }
    }

    void handle_simulink_client(int conn_fd, const std::string& addr_str) {
        {
            std::lock_guard<std::mutex> lk(simulink_mutex_);
            simulink_fd_ = conn_fd;
        }

        safe_print("[" + name_ + "] Simulink conectado de " + addr_str);

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buffer[8192];

        while (running_) {
            ssize_t n = ::recv(conn_fd, buffer, sizeof(buffer), 0);
            if (n == 0) {
                safe_print("[" + name_ + "] Simulink desconectou (EOF)");
                break;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                safe_print("[" + name_ + "] erro de recv do Simulink");
                break;
            }

            std::string chunk(buffer, buffer + n);
            auto objs = parse_json_objects_from_chunk(chunk);
            if (!objs.empty()) {
                json last = objs.back();
                set_latest_payload_from_obj(last);
            }
        }

        {
            std::lock_guard<std::mutex> lk(simulink_mutex_);
            if (simulink_fd_ >= 0) {
                ::close(simulink_fd_);
                simulink_fd_ = -1;
            }
        }

        safe_print("[" + name_ + "] handler do Simulink finalizado");
    }

    void handle_client(int conn_fd, const std::string& addr_str, const std::string& ip) {
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[1024];

        while (running_) {
            ssize_t n = ::recv(conn_fd, buf, sizeof(buf), 0);
            if (n == 0) {
                break;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_by_ip_.find(ip);
            if (it != clients_by_ip_.end() && it->second == conn_fd) {
                clients_by_ip_.erase(it);
            }
        }

        ::close(conn_fd);
        safe_print("[" + name_ + "] cliente desconectado " + addr_str);
    }

    void broadcaster() {
        broadcaster_thread_alive_ = true;
        const double interval = 1.0 / std::max(1, broadcast_hz_);

        while (running_) {
            auto next_t = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(interval);

            std::string lp;
            {
                std::lock_guard<std::mutex> lk(payload_mutex_);
                lp = latest_payload_;
            }

            if (!lp.empty()) {
                std::string wire = lp + "\n";

                if (print_tx_) {
                    safe_print("[" + name_ + "] TX: " + lp);
                }

                std::vector<std::string> to_drop;

                {
                    std::lock_guard<std::mutex> lk(clients_mutex_);
                    for (auto& kv : clients_by_ip_) {
                        int fd = kv.second;
                        ssize_t sent = ::send(fd, wire.data(), wire.size(), 0);
                        if (sent < 0) {
                            safe_print("[" + name_ + "] envio para " + kv.first + " falhou; removendo cliente");
                            to_drop.push_back(kv.first);
                        }
                    }

                    for (const auto& ip : to_drop) {
                        auto it = clients_by_ip_.find(ip);
                        if (it != clients_by_ip_.end()) {
                            ::close(it->second);
                            clients_by_ip_.erase(it);
                        }
                    }
                }
            }

            std::this_thread::sleep_until(next_t);
        }

        broadcaster_thread_alive_ = false;
    }

    void accept_connections() {
        accept_thread_alive_ = true;

        while (running_) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(server_fd_, &rfds);
            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int ret = ::select(server_fd_ + 1, &rfds, nullptr, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                if (running_) {
                    safe_print("[" + name_ + "] erro em select/accept");
                }
                break;
            } else if (ret == 0) {
                continue;
            }

            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int conn_fd = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (conn_fd < 0) {
                continue;
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            std::string ip(ip_str);
            std::string addr_str = ip + ":" + std::to_string(ntohs(client_addr.sin_port));

            bool has_simulink = false;
            {
                std::lock_guard<std::mutex> lk(simulink_mutex_);
                has_simulink = (simulink_fd_ >= 0);
            }

            if (!has_simulink) {
                {
                    std::lock_guard<std::mutex> lk(threads_mutex_);
                    threads_.emplace_back(&TelemetryServer::handle_simulink_client, this, conn_fd, addr_str);
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                if (clients_by_ip_.count(ip)) {
                    const char* msg = "Only one client per IP is allowed. Closing.\n";
                    ::send(conn_fd, msg, std::strlen(msg), 0);
                    ::close(conn_fd);
                    safe_print("[" + name_ + "] cliente duplicado de " + ip + " rejeitado");
                    continue;
                }

                clients_by_ip_[ip] = conn_fd;
                safe_print("[" + name_ + "] cliente conectado de " + addr_str +
                           " (total " + std::to_string(clients_by_ip_.size()) + ")");
            }

            {
                std::lock_guard<std::mutex> lk(threads_mutex_);
                threads_.emplace_back(&TelemetryServer::handle_client, this, conn_fd, addr_str, ip);
            }
        }

        accept_thread_alive_ = false;
    }
};

json upgrade_json_pfd(json obj) {
    obj["isWaypoint"] = true;
    obj["id"] = "Marker1";
    return obj;
}

json upgrade_json_mfd(json obj) {
    obj["batteryValueView"] = 22.0;
    obj["internalTempValueView"] = 23;
    obj["externalTempValueView"] = 35;
    obj["suggestion"] = "Don''t crash (optional)";
    return obj;
}

void server_supervisor(TelemetryServer& server) {
    while (!g_stop.load()) {
        if (!server.is_healthy()) {
            safe_print("[" + server.name() + "] supervisor: servidor não saudável, reiniciando...");
            server.stop();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            server.start();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.stop();
}

int main() {
    std::signal(SIGINT, signal_handler);

    TelemetryServer pfd_server(
        "PFD",
        12345,
        120,
        upgrade_json_pfd,
        "pfd_received_data.jsonl",
        true,
        10.0,
        0.5,
        false
    );

    TelemetryServer mfd_server(
        "MFD",
        12346,
        50,
        upgrade_json_mfd,
        "mfd_received_data.jsonl",
        true,
        10.0,
        0.5,
        false
    );

    std::thread pfd_thread(server_supervisor, std::ref(pfd_server));
    std::thread mfd_thread(server_supervisor, std::ref(mfd_server));

    safe_print("Pressione Ctrl+C para encerrar ambos os servidores.");

    pfd_thread.join();
    mfd_thread.join();

    safe_print("Servidores encerrados.");
    return 0;
}
