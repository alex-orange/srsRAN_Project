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

#include "amf_connection_manager.h"
#include "du_connection_manager.h"
#include "node_connection_notifier.h"
#include "srsran/cu_cp/cu_cp_configuration.h"
#include "srsran/cu_cp/cu_cp_e1_handler.h"

namespace srsran {
namespace srs_cu_cp {

class cu_up_processor_repository;
class ue_manager;

/// \brief Entity responsible for managing the CU-CP connections to remote nodes and determining whether the CU-CP
/// is in a state to accept new connections.
///
/// In particular, this class is responsible for:
/// - triggering an AMF connection and tracking the status of the connection;
/// - determining whether a new DU setup request should be accepted based on the status of other remote node
/// connections;
/// - determining whether new UEs should be accepted depending on the status of the CU-CP remote connections.
class cu_cp_controller
{
public:
  cu_cp_controller(cu_cp_routine_manager&            routine_manager_,
                   ue_manager&                       ue_mng_,
                   const ngap_configuration&         ngap_cfg_,
                   ngap_connection_manager&          ngap_conn_mng_,
                   const cu_up_processor_repository& cu_ups_,
                   du_processor_repository&          dus_,
                   task_executor&                    ctrl_exec);

  amf_connection_manager& amf_connection_handler() { return amf_mng; }

  bool handle_du_setup_request(const du_setup_request& req);

  /// \brief Determines whether the CU-CP should accept a new UE connection.
  bool request_ue_setup() const;

  cu_cp_f1c_handler& get_f1c_handler() { return du_mng; }

private:
  ue_manager&                       ue_mng;
  const cu_up_processor_repository& cu_ups;

  amf_connection_manager amf_mng;

  du_connection_manager du_mng;
};

} // namespace srs_cu_cp
} // namespace srsran
