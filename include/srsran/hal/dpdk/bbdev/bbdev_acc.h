/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "srsran/adt/bounded_bitset.h"
#include "srsran/adt/mpmc_queue.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/units.h"
#include <rte_bbdev.h>
#include <rte_bbdev_op.h>

namespace srsran {
namespace dpdk {

/// Maximum number of queues supported by a bbdev-based hardware-accelerator.
static constexpr unsigned MAX_NOF_BBDEV_QUEUES = 128;
/// Maximum number of operations that can be stored in a hardware queue at a given time.
static constexpr unsigned MAX_NOF_OP_IN_QUEUE = 16;
/// Maximum number of VF instances supported by a bbdev-based hardware-accelerator.
static constexpr unsigned MAX_NOF_BBDEV_VF_INSTANCES = 64;

/// Configuration parameters and objects tied to a bbdev-based hardware-accelerator.
struct bbdev_acc_configuration {
  /// ID of the bbdev-based hardware-accelerator.
  unsigned id;
  /// Number of lcores available to the hardware-accelerated LDPC encoder (disabled if 0).
  unsigned nof_ldpc_enc_lcores = 0;
  /// Number of lcores available to the hardware-accelerated LDPC decoder (disabled if 0).
  unsigned nof_ldpc_dec_lcores = 0;
  /// Number of lcores available to the hardware-accelerated FFT (disabled if 0).
  unsigned nof_fft_lcores = 0;
  /// Size of each mbuf used to exchange unencoded and unrate-matched messages with the accelerator in bytes.
  /// Note that by default it is initialized to the maximum size supported by an mbuf.
  unsigned msg_mbuf_size = RTE_BBDEV_LDPC_E_MAX_MBUF;
  /// Size of each mbuf used to exchange encoded and rate-matched messages with the accelerator in bytes.
  /// Note that by default it is initialized to the maximum size supported by an mbuf.
  unsigned rm_mbuf_size = RTE_BBDEV_LDPC_E_MAX_MBUF;
  /// Number of mbufs in each memory pool.
  unsigned nof_mbuf = 256;
};

/// Abstracted interfacing to bbdev-based hardware-accelerators.
class bbdev_acc
{
public:
  /// Constructor.
  /// \param[in] cfg    Configuration parameters of the bbdev-based hardware-accelerator.
  /// \param[in] info   bbdev Device information.
  /// \param[in] logger SRS logger.
  explicit bbdev_acc(const bbdev_acc_configuration& cfg, const ::rte_bbdev_info& info_, srslog::basic_logger& logger);

  /// Destructor.
  ~bbdev_acc();

  /// Returns the ID of the bbdev-based hardware-accelerator device.
  /// \return Device ID.
  unsigned get_device_id() const { return id; }

  /// Returns the ID of the socket used by the bbdev-based hardware-accelerator.
  /// \return Socket ID.
  int get_socket_id() const { return info.socket_id; }

  /// Returns the number of LDPC encoder cores provided by the bbdev-based hardware-accelerator.
  /// \return Number of LDPC encoder cores.
  unsigned get_nof_ldpc_enc_cores() const { return nof_ldpc_enc_lcores; }

  /// Returns the number of LDPC decoder cores provided by the bbdev-based hardware-accelerator.
  /// \return Number of LDPC decoder cores.
  unsigned get_nof_ldpc_dec_cores() const { return nof_ldpc_dec_lcores; }

  /// Returns the number of FFT cores provided by the bbdev-based hardware-accelerator.
  /// \return Number of FFT cores.
  unsigned get_nof_fft_cores() const { return nof_fft_lcores; }

  /// Returns the size of the (external) HARQ buffer size embedded in the hardware-accelerator.
  /// \return HARQ buffer size in bytes. Note that 64 bits are used to enable sizes >= 4GB.
  uint64_t get_harq_buff_size_bytes() const { return static_cast<uint64_t>(info.drv.harq_buffer_size) * 1024; }

  /// Returns the size of each mbuf used to exchange unencoded and unrate-matched messages with the accelerator.
  /// \return Unencoded and unrate-matched mbuf size (in bytes).
  units::bytes get_msg_mbuf_size() const { return units::bytes(msg_mbuf_size); }

  /// Returns the size of each mbuf used to exchange encoded and rate-matched messages with the accelerator.
  /// \return Encoded and rate-matched mbuf size (in bytes).
  units::bytes get_rm_mbuf_size() const { return units::bytes(rm_mbuf_size); }

  /// Returns the number of mbufs in each memory pool used to exchange data with the accelerator.
  /// \return Number of mbufs.
  unsigned get_nof_mbuf() const { return nof_mbuf; }

  /// Returns the internal SRS logger.
  /// \return SRS logger.
  srslog::basic_logger& get_logger() { return logger; }

  /// Reserves a free queue to be used by a specific hardware-accelerated channel processor function.
  /// \param[in] op_type Type of bbdev op.
  /// \return ID of the reserved queue.
  int reserve_queue(::rte_bbdev_op_type op_type);

  /// Frees a queue used by a specific hardware-accelerated channel processor function.
  /// \param[in] queue_id ID of the queue to be freed.
  void free_queue(::rte_bbdev_op_type op_type, unsigned queue_id);

  /// Returns a unique ID for an instance of an LDPC encoder using the bbdev-based accelerator.
  /// \return Encoder ID.
  unsigned reserve_encoder() { return nof_ldpc_enc_instances++; }

  /// Returns a unique ID for an instance of an LDPC decoder using the bbdev-based accelerator.
  /// \return Decoder ID.
  unsigned reserve_decoder() { return nof_ldpc_dec_instances++; }

private:
  /// Codeblock identifier list type.
  using queue_id_list =
      concurrent_queue<unsigned, concurrent_queue_policy::lockfree_mpmc, concurrent_queue_wait_policy::non_blocking>;

  /// ID of the bbdev-based hardware-accelerator.
  unsigned id;
  /// Structure providing device information.
  ::rte_bbdev_info info;
  /// Number of lcores available to the hardware-accelerated LDPC encoder (disabled if 0).
  unsigned nof_ldpc_enc_lcores;
  /// Number of lcores available to the hardware-accelerated LDPC decoder (disabled if 0).
  unsigned nof_ldpc_dec_lcores;
  /// Number of lcores available to the hardware-accelerated FFT (disabled if 0).
  unsigned nof_fft_lcores;
  /// List containing the free queue ids for hardware-acclerated LDPC encoder functions.
  queue_id_list available_ldpc_enc_queue;
  /// List containing the free queue ids for hardware-acclerated LDPC decoder functions.
  queue_id_list available_ldpc_dec_queue;
  /// List containing the free queue ids for hardware-acclerated FFT functions.
  queue_id_list available_fft_queue;
  /// Size of each mbuf used to exchange unencoded and unrate-matched messages with the accelerator in bytes.
  unsigned msg_mbuf_size;
  /// Size of each mbuf used to exchange encoded and rate-matched messages with the accelerator in bytes.
  unsigned rm_mbuf_size;
  /// Number of mbufs in each memory pool.
  unsigned nof_mbuf;
  /// SRS logger.
  srslog::basic_logger& logger;
  /// Number of LDPC encoder instances using this bbdev accelerator.
  unsigned nof_ldpc_enc_instances;
  /// Number of LDPC decoder instances using this bbdev accelerator.
  unsigned nof_ldpc_dec_instances;
};

} // namespace dpdk
} // namespace srsran
