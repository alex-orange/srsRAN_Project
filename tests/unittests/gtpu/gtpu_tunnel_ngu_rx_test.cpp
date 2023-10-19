/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "lib/gtpu/gtpu_pdu.h"
#include "lib/gtpu/gtpu_tunnel_ngu_rx.h"
#include "lib/gtpu/gtpu_tunnel_ngu_tx.h"
#include "srsran/support/bit_encoding.h"
#include "srsran/support/executors/manual_task_worker.h"
#include <gtest/gtest.h>
#include <queue>
#include <sys/socket.h>

using namespace srsran;

class gtpu_pdu_generator
{
  class gtpu_tunnel_tx_upper_dummy : public gtpu_tunnel_tx_upper_layer_notifier
  {
    void on_new_pdu(byte_buffer buf, const ::sockaddr_storage& dest_addr) final { parent.gen_pdu = std::move(buf); }
    gtpu_pdu_generator& parent;

  public:
    gtpu_tunnel_tx_upper_dummy(gtpu_pdu_generator& parent_) : parent(parent_) {}
  };

public:
  gtpu_pdu_generator(gtpu_teid_t teid) : tx_upper_dummy(*this)
  {
    gtpu_config::gtpu_tx_config cfg = {};
    cfg.peer_teid                   = teid;
    cfg.peer_addr                   = "127.0.0.1";

    tx = std::make_unique<gtpu_tunnel_ngu_tx>(srs_cu_up::ue_index_t::MIN_UE_INDEX, cfg, dummy_pcap, tx_upper_dummy);
  }

  byte_buffer create_gtpu_pdu(byte_buffer sdu, gtpu_teid_t teid, qos_flow_id_t flow_id)
  {
    tx->handle_sdu(std::move(sdu), flow_id);
    return std::move(gen_pdu);
  }

  //  hard-coded alternative
  //
  //  byte_buffer create_gtpu_pdu(byte_buffer buf, gtpu_teid_t teid, qos_flow_id_t flow_id, optional<uint16_t> sn)
  //  {
  //    gtpu_header hdr         = {};
  //    hdr.flags.version       = GTPU_FLAGS_VERSION_V1;
  //    hdr.flags.protocol_type = GTPU_FLAGS_GTP_PROTOCOL;
  //    hdr.flags.ext_hdr       = true;
  //    hdr.message_type        = GTPU_MSG_DATA_PDU;
  //    hdr.length              = buf.length() + 4 + 4;
  //    hdr.teid                = teid;
  //    hdr.next_ext_hdr_type   = gtpu_extension_header_type::pdu_session_container;

  //    // Put PDU session container
  //    byte_buffer ext_buf;
  //    bit_encoder encoder{ext_buf};
  //    encoder.pack(1, 4);                            // PDU type
  //    encoder.pack(0, 4);                            // unused options
  //    encoder.pack(0, 1);                            // spare
  //    encoder.pack(qos_flow_id_to_uint(flow_id), 7); // QFI

  //    gtpu_extension_header ext;
  //    ext.extension_header_type = gtpu_extension_header_type::pdu_session_container;
  //    ext.container             = ext_buf;

  //    hdr.ext_list.push_back(ext);

  //    if (sn.has_value()) {
  //      hdr.flags.seq_number = true;
  //      hdr.seq_number       = sn.value();
  //    }

  //    bool write_ok = gtpu_write_header(buf, hdr, logger);

  //    if (!write_ok) {
  //      logger.log_error("Dropped SDU, error writing GTP-U header. teid={}", hdr.teid);
  //      return {};
  //    }

  //    return buf;
  //  }

private:
  dummy_dlt_pcap                      dummy_pcap = {};
  gtpu_tunnel_tx_upper_dummy          tx_upper_dummy;
  std::unique_ptr<gtpu_tunnel_ngu_tx> tx;
  byte_buffer                         gen_pdu;

public:
};

class gtpu_tunnel_rx_lower_dummy : public gtpu_tunnel_ngu_rx_lower_layer_notifier
{
  void on_new_sdu(byte_buffer sdu, qos_flow_id_t qos_flow_id) final
  {
    last_rx             = std::move(sdu);
    last_rx_qos_flow_id = qos_flow_id;
  }

public:
  byte_buffer   last_rx;
  qos_flow_id_t last_rx_qos_flow_id;
};

class gtpu_tunnel_rx_upper_dummy : public gtpu_tunnel_rx_upper_layer_interface
{
public:
  void handle_pdu(byte_buffer pdu, const sockaddr_storage& src_addr) final
  {
    last_rx   = std::move(pdu);
    last_addr = src_addr;
  }

  byte_buffer      last_rx;
  sockaddr_storage last_addr = {};
};

/// Fixture class for GTP-U tunnel NG-U Rx tests
class gtpu_tunnel_ngu_rx_test : public ::testing::Test
{
public:
  gtpu_tunnel_ngu_rx_test() :
    logger(srslog::fetch_basic_logger("TEST", false)), gtpu_logger(srslog::fetch_basic_logger("GTPU", false))
  {
  }

protected:
  void SetUp() override
  {
    // init test's logger
    srslog::init();
    logger.set_level(srslog::basic_levels::debug);

    // init GTP-U logger
    gtpu_logger.set_level(srslog::basic_levels::debug);
    gtpu_logger.set_hex_dump_max_size(100);
  }

  void TearDown() override
  {
    // flush logger after each test
    srslog::flush();
  }

  gtpu_pdu_generator pdu_generator{gtpu_teid_t{0x1}};

  // Test logger
  srslog::basic_logger& logger;

  // GTP-U logger
  srslog::basic_logger& gtpu_logger;
  gtpu_tunnel_logger    gtpu_rx_logger{"GTPU", {srs_cu_up::ue_index_t{}, gtpu_teid_t{1}, "DL"}};

  // Timers
  manual_task_worker worker{64};
  timer_manager      timers_manager;
  timer_factory      timers{timers_manager, worker};

  // GTP-U tunnel Rx entity
  std::unique_ptr<gtpu_tunnel_ngu_rx> rx;

  // Surrounding tester
  gtpu_tunnel_rx_lower_dummy rx_lower = {};
};

/// \brief Test correct creation of Rx entity
TEST_F(gtpu_tunnel_ngu_rx_test, entity_creation)
{
  // create Rx entity
  gtpu_config::gtpu_rx_config rx_cfg = {};
  rx_cfg.local_teid                  = gtpu_teid_t{0x1};
  rx_cfg.t_reordering_ms             = 10;

  rx = std::make_unique<gtpu_tunnel_ngu_rx>(srs_cu_up::ue_index_t::MIN_UE_INDEX, rx_cfg, rx_lower, timers);

  ASSERT_NE(rx, nullptr);
};

/// \brief Test in order reception of PDUs
TEST_F(gtpu_tunnel_ngu_rx_test, rx_in_order)
{
  // create Rx entity
  gtpu_config::gtpu_rx_config rx_cfg = {};
  rx_cfg.local_teid                  = gtpu_teid_t{0x1};
  rx_cfg.t_reordering_ms             = 10;

  rx = std::make_unique<gtpu_tunnel_ngu_rx>(srs_cu_up::ue_index_t::MIN_UE_INDEX, rx_cfg, rx_lower, timers);
  ASSERT_NE(rx, nullptr);

  sockaddr_storage src_addr;

  for (unsigned i = 0; i < 3; i++) {
    byte_buffer sdu;
    sdu.append(0x11);
    // FIXME: this generator creates PDUs with PDU session containers of type 1 (UL), but we need type 0 (DL).
    byte_buffer          pdu = pdu_generator.create_gtpu_pdu(sdu.deep_copy(), rx_cfg.local_teid, qos_flow_id_t::min);
    gtpu_tunnel_base_rx* rx_base = rx.get();
    rx_base->handle_pdu(std::move(pdu), src_addr);

    EXPECT_EQ(rx_lower.last_rx_qos_flow_id, qos_flow_id_t::min);
    EXPECT_EQ(rx_lower.last_rx, sdu);
  }
};

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
