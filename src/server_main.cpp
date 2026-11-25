#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "zephyr/c_abi.h"
#include "zephyr/net/pgwire.hpp"
#include "zephyr/net/raft.hpp"
#include "zephyr/net/studio.hpp"

namespace {

std::atomic<bool> g_shutdown{false};

void handle_signal(int) {
    g_shutdown.store(true, std::memory_order_release);
}

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --data-dir <path>         Database directory (default: ./zephyr_data)\n"
        << "  --encryption-key <secret> Enable ChaCha20-Poly1305 AEAD encryption at rest\n"
        << "  --pgwire-port <port>      PostgreSQL wire protocol TCP port (default: 5432, 0=disabled)\n"
        << "  --studio-port <port>      Zephyr Studio HTTP Web UI port (default: 0=disabled)\n"
        << "  --raft-id <id>            Raft node ID (default: 0 = standalone)\n"
        << "  --raft-port <port>        Raft UDP port (default: 7000)\n"
        << "  --peers <csv>             Raft peer list (e.g. 2@127.0.0.1:7002,3@127.0.0.1:7003)\n"
        << "  --help                    Show this help message\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string data_dir = "./zephyr_data";
    std::string encryption_key;
    uint16_t pgwire_port = 5432;
    uint16_t studio_port = 0;
    uint32_t raft_id = 0;
    uint16_t raft_port = 7000;
    std::string peers_csv;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--encryption-key" && i + 1 < argc) {
            encryption_key = argv[++i];
        } else if (arg == "--pgwire-port" && i + 1 < argc) {
            pgwire_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--studio-port" && i + 1 < argc) {
            studio_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--raft-id" && i + 1 < argc) {
            raft_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--raft-port" && i + 1 < argc) {
            raft_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--peers" && i + 1 < argc) {
            peers_csv = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    ZephyrConfig cfg = zephyr_default_config();
    cfg.data_dir = data_dir.c_str();
    ZephyrDB* db = nullptr;
    if (!encryption_key.empty()) {
        db = zephyr_db_open_encrypted(
            &cfg,
            reinterpret_cast<const uint8_t*>(encryption_key.data()),
            static_cast<uint32_t>(encryption_key.size()));
    } else {
        db = zephyr_db_open(&cfg);
    }
    if (!db) {
        std::cerr << "Failed to open ZephyrDB at " << data_dir
                  << (encryption_key.empty() ? "" : " (encryption authentication failed)")
                  << "\n";
        return 1;
    }
    if (!encryption_key.empty()) {
        std::cout << "[Security] ChaCha20-Poly1305 AEAD encryption at rest enabled\n";
    }

    zephyr::net::RaftNode raft;
    if (raft_id > 0) {
        bool ok = raft.start(raft_id, raft_port, peers_csv, [db](uint64_t lsn, std::string_view sql) {
            (void)lsn;
            std::string sql_str(sql);
            zephyr_exec(db, sql_str.c_str(), nullptr);
        });
        if (!ok) {
            std::cerr << "Failed to start Raft Diplomat on port " << raft_port << "\n";
            zephyr_db_close(db);
            return 1;
        }
        std::cout << "[Raft Diplomat] node=" << raft_id
                  << " listening on UDP :" << raft.bound_port() << "\n";
    }

    zephyr::net::PgWireServer pgwire;
    if (pgwire_port > 0) {
        if (!pgwire.start(db, pgwire_port)) {
            std::cerr << "Failed to start PGWire server on TCP port " << pgwire_port << "\n";
            raft.stop();
            zephyr_db_close(db);
            return 1;
        }
        std::cout << "[PGWire v3.0] listening on TCP :" << pgwire.bound_port() << "\n";
    }

    zephyr::net::StudioServer studio;
    if (studio_port > 0) {
        if (!studio.start(db, studio_port)) {
            std::cerr << "Failed to start Zephyr Studio on TCP port " << studio_port << "\n";
            pgwire.stop();
            raft.stop();
            zephyr_db_close(db);
            return 1;
        }
        std::cout << "[Zephyr Studio] Web UI at http://127.0.0.1:" << studio.bound_port() << std::endl;
    }

    std::cout << "ZephyrDB server ready (data-dir=" << data_dir << "). Press Ctrl+C to stop." << std::endl;
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down ZephyrDB server...\n";
    studio.stop();
    pgwire.stop();
    raft.stop();
    zephyr_db_close(db);
    return 0;
}
