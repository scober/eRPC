/**
 * @file dpdk_daemon.cc
 * @brief A DPDK management daemon that initializes DPDK queues and mempools
 * for processes that use eRPC.
 */

#include <gflags/gflags.h>

#include <chrono>

#include "dpdk_externs.h"
#include "dpdk_transport.h"

DEFINE_uint64(phy_port, 0, "DPDK port ID to manage with this daemon");
DEFINE_uint64(numa_node, 0,
              "NUMA node for the DPDK port managed by this daemon");

namespace {
std::string build_socket_mem_arg(size_t numa_node) {
  std::string arg;

  for (size_t i = 0; i < numa_node; ++i) {
    arg += "0,";
  }
  arg += "1024";

  return arg;
}
}  // namespace

int main(int argc, char **argv) {
  erpc::rt_assert(!getuid(), "You need to be root to use eRPC");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  ERPC_WARN("eRPC DPDK daemon: Managing DPDK port %zu on NUMA node %zu\n",
            FLAGS_phy_port, FLAGS_numa_node);

  // this vector owns the memory so that it outlives the call to
  //   rte_eal_init without being statically allocated
  std::vector<std::string> args{
      "-c",
      "0x0",
      "-n",
      "6",  // Memory channels
      "--socket-mem",
      build_socket_mem_arg(numa_node),
      "--proc-type",
      "auto",
      "--log-level",
      (ERPC_LOG_LEVEL >= ERPC_LOG_LEVEL_INFO) ? "8" : "1",
  };

  std::vector<char *> argv;
  for (std::string &arg : args) {
    argv.push_back(&arg[0]);
  }
  argv.push_back(nullptr);

  int ret = rte_eal_init(static_cast<int>(args.size()), argv.data());
  if (ret < 0) {
    fprintf(stderr,
            "eRPC DPDK daemon: Failed to initialize DPDK. Is another "
            "eRPC DPDK daemon or primary DPDK process already running? DPDK "
            "error code = %s.\n",
            rte_strerror(rte_errno));
    exit(-1);
  }

  erpc::rt_assert(
      ret >= 0,
      "eRPC DPDK daemon: Error: Failed to initialize DPDK. Is another "
      "eRPC DPDK daemon or primary DPDK process already running?");

  ERPC_WARN("eRPC DPDK daemon: Successfully initialized DPDK EAL\n");

  const std::string memzone_name = erpc::DpdkTransport::get_memzone_name();
  const rte_memzone *memzone = rte_memzone_reserve(
      memzone_name.c_str(), sizeof(erpc::DpdkTransport::ownership_memzone_t),
      FLAGS_numa_node, RTE_MEMZONE_2MB);
  erpc::rt_assert(memzone != nullptr,
                  "eRPC DPDK daemon: Failed to create memzone");
  ERPC_WARN("eRPC DPDK daemon: Successfully initialized shared memzone %s\n",
            memzone_name.c_str());

  auto *ownership_memzone =
      reinterpret_cast<erpc::DpdkTransport::ownership_memzone_t *>(
          memzone->addr);
  ownership_memzone->init();

  erpc::DpdkTransport::setup_phy_port(
      FLAGS_phy_port, FLAGS_numa_node,
      erpc::DpdkTransport::DpdkProcType::kPrimary);

  ERPC_WARN("eRPC DPDK daemon: Successfully initialized DPDK port %zu\n",
            FLAGS_phy_port);

  // Check if the link is up
  struct rte_eth_link link;
  rte_eth_link_get(static_cast<uint8_t>(FLAGS_phy_port), &link);
  if (link.link_status != ETH_LINK_UP) {
    fprintf(stderr, "eRPC DPDK daemon: Error: Port %zu link is down\n",
            FLAGS_phy_port);
    exit(-1);
  }
  ownership_memzone->link_[FLAGS_phy_port] = link;

  size_t prev_epoch = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    size_t cur_epoch = ownership_memzone->get_epoch();
    if (cur_epoch != prev_epoch) {
      ERPC_WARN("eRPC DPDK daemon: %s\n",
                ownership_memzone->get_summary(FLAGS_phy_port).c_str());
      prev_epoch = cur_epoch;
    }
    ownership_memzone->daemon_reclaim_qps_from_crashed(FLAGS_phy_port);
  }
  return 0;
}
