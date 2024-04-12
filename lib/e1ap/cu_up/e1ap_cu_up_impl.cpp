/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "e1ap_cu_up_impl.h"
#include "../common/log_helpers.h"
#include "cu_up/procedures/e1ap_cu_up_event_manager.h"
#include "e1ap_cu_up_asn1_helpers.h"
#include "procedures/e1ap_cu_up_setup_procedure.h"
#include "srsran/e1ap/common/e1ap_message.h"
#include "srsran/e1ap/cu_up/e1ap_connection_client.h"
#include "srsran/ran/bcd_helpers.h"
#include "srsran/support/timers.h"
#include <memory>

using namespace srsran;
using namespace asn1::e1ap;
using namespace srs_cu_up;

namespace {

/// Adapter used to convert E1AP Rx PDUs coming from the CU-CP into E1AP messages.
class e1ap_rx_pdu_adapter final : public e1ap_message_notifier
{
public:
  e1ap_rx_pdu_adapter(e1ap_message_handler& msg_handler_) : msg_handler(msg_handler_) {}

  void on_new_message(const e1ap_message& msg) override { msg_handler.handle_message(msg); }

private:
  e1ap_message_handler& msg_handler;
};

} // namespace

e1ap_cu_up_impl::e1ap_cu_up_impl(e1ap_connection_client& e1ap_client_handler_,
                                 e1ap_cu_up_notifier&    cu_up_notifier_,
                                 timer_manager&          timers_,
                                 task_executor&          cu_up_exec_) :
  logger(srslog::fetch_basic_logger("CU-UP-E1")),
  cu_up_notifier(cu_up_notifier_),
  timers(timers_),
  cu_up_exec(cu_up_exec_),
  connection_handler(e1ap_client_handler_, *this),
  ue_ctxt_list(logger),
  ev_mng(std::make_unique<e1ap_event_manager>(timer_factory{timers, cu_up_exec}))
{
}

// Note: For fwd declaration of member types, dtor cannot be trivial.
e1ap_cu_up_impl::~e1ap_cu_up_impl() {}

bool e1ap_cu_up_impl::connect_to_cu_cp()
{
  e1ap_message_notifier* notif = connection_handler.connect_to_cu_cp();
  if (notif == nullptr) {
    logger.warning("Failed to connect to CU-CP");
    return false;
  }
  pdu_notifier = std::make_unique<e1ap_message_notifier_with_logging>(*this, *notif);
  return true;
}

async_task<cu_up_e1_setup_response>
e1ap_cu_up_impl::handle_cu_up_e1_setup_request(const cu_up_e1_setup_request& request)
{
  return launch_async<e1ap_cu_up_setup_procedure>(
      request, *pdu_notifier, *ev_mng, timer_factory{timers, cu_up_exec}, logger);
}

void e1ap_cu_up_impl::handle_bearer_context_inactivity_notification(
    const e1ap_bearer_context_inactivity_notification& msg)
{
  if (!ue_ctxt_list.contains(msg.ue_index)) {
    logger.error("ue={}: Dropping BearerContextInactivityNotification. UE does not exist", msg.ue_index);
    return;
  }

  // Get UE context
  e1ap_ue_context& ue_ctxt = ue_ctxt_list[msg.ue_index];

  e1ap_message e1ap_msg;
  e1ap_msg.pdu.set_init_msg();
  e1ap_msg.pdu.init_msg().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_INACTIVITY_NOTIF);
  auto& inactivity_notification                 = e1ap_msg.pdu.init_msg().value.bearer_context_inactivity_notif();
  inactivity_notification->gnb_cu_cp_ue_e1ap_id = gnb_cu_cp_ue_e1ap_id_to_uint(ue_ctxt.ue_ids.cu_cp_ue_e1ap_id);
  inactivity_notification->gnb_cu_up_ue_e1ap_id = gnb_cu_up_ue_e1ap_id_to_uint(ue_ctxt.ue_ids.cu_up_ue_e1ap_id);

  if (ue_ctxt.activity_notification_level == activity_notification_level_t::ue) {
    inactivity_notification->activity_info.set_ue_activity();
    inactivity_notification->activity_info.ue_activity() = asn1::e1ap::ue_activity_opts::options::not_active;
  } else if (ue_ctxt.activity_notification_level == activity_notification_level_t::pdu_session) {
    ue_ctxt.logger.log_warning("PDU session level activity notifications not supported");
  } else if (ue_ctxt.activity_notification_level == activity_notification_level_t::drb) {
    ue_ctxt.logger.log_warning("DRB level activity notifications not supported");
  } else {
    ue_ctxt.logger.log_error("Unsupported activity notification level");
  }

  // Send inactivity notification.
  ue_ctxt.logger.log_debug("Sending BearerContextInactivityNotification");
  pdu_notifier->on_new_message(e1ap_msg);
}

void e1ap_cu_up_impl::handle_message(const e1ap_message& msg)
{
  // Run E1AP protocols in CU-UP executor.
  if (not cu_up_exec.execute([this, msg]() {
        // Log PDU.
        log_pdu(true, msg);

        switch (msg.pdu.type().value) {
          case asn1::e1ap::e1ap_pdu_c::types_opts::init_msg:
            handle_initiating_message(msg.pdu.init_msg());
            break;
          case asn1::e1ap::e1ap_pdu_c::types_opts::successful_outcome:
            handle_successful_outcome(msg.pdu.successful_outcome());
            break;
          case asn1::e1ap::e1ap_pdu_c::types_opts::unsuccessful_outcome:
            handle_unsuccessful_outcome(msg.pdu.unsuccessful_outcome());
            break;
          default:
            logger.error("Invalid PDU type");
            break;
        }
      })) {
    logger.warning("Discarding E1AP PDU. Cause: CU-UP task queue is full");
  }
}

void e1ap_cu_up_impl::handle_initiating_message(const asn1::e1ap::init_msg_s& msg)
{
  switch (msg.value.type().value) {
    case asn1::e1ap::e1ap_elem_procs_o::init_msg_c::types_opts::options::bearer_context_setup_request: {
      handle_bearer_context_setup_request(msg.value.bearer_context_setup_request());
    } break;
    case asn1::e1ap::e1ap_elem_procs_o::init_msg_c::types_opts::options::bearer_context_mod_request: {
      handle_bearer_context_modification_request(msg.value.bearer_context_mod_request());
    } break;
    case asn1::e1ap::e1ap_elem_procs_o::init_msg_c::types_opts::options::bearer_context_release_cmd: {
      handle_bearer_context_release_command(msg.value.bearer_context_release_cmd());
    } break;
    default:
      logger.error("Initiating message of type {} is not supported", msg.value.type().to_string());
  }
}

void e1ap_cu_up_impl::handle_bearer_context_setup_request(const asn1::e1ap::bearer_context_setup_request_s& msg)
{
  // create failure message for early returns
  e1ap_message e1ap_msg;
  e1ap_msg.pdu.set_unsuccessful_outcome();
  e1ap_msg.pdu.unsuccessful_outcome().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_SETUP);
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_setup_fail()->gnb_cu_cp_ue_e1ap_id =
      msg->gnb_cu_cp_ue_e1ap_id;
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_setup_fail()->cause.set_protocol();

  // Do basic syntax/semantic checks on the validity of the received message.
  if (not check_e1ap_bearer_context_setup_request_valid(msg, logger)) {
    logger.debug("Sending BearerContextSetupFailure. Cause: Received invalid BearerContextSetupRequest");
    pdu_notifier->on_new_message(e1ap_msg);
    return;
  }

  logger.debug("Received BearerContextSetupRequest (plmn={})", plmn_bcd_to_string(msg->serving_plmn.to_number()));

  gnb_cu_up_ue_e1ap_id_t cu_up_ue_e1ap_id = ue_ctxt_list.next_gnb_cu_up_ue_e1ap_id();
  if (cu_up_ue_e1ap_id == gnb_cu_up_ue_e1ap_id_t::invalid) {
    logger.error("Sending BearerContextSetupFailure. Cause: No CU-UP-UE-E1AP-ID available");

    // send response
    pdu_notifier->on_new_message(e1ap_msg);
    return;
  }

  // Add gnb_cu_up_ue_e1ap_id to failure message
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_setup_fail()->gnb_cu_up_ue_e1ap_id =
      gnb_cu_up_ue_e1ap_id_to_uint(cu_up_ue_e1ap_id);

  // Forward message to CU-UP
  e1ap_bearer_context_setup_request bearer_context_setup = {};
  fill_e1ap_bearer_context_setup_request(bearer_context_setup, msg);

  e1ap_bearer_context_setup_response bearer_context_setup_response_msg =
      cu_up_notifier.on_bearer_context_setup_request_received(bearer_context_setup);

  if (bearer_context_setup_response_msg.ue_index == INVALID_UE_INDEX) {
    logger.error("Sending BearerContextSetupFailure. Cause: Invalid UE index");

    // send response
    pdu_notifier->on_new_message(e1ap_msg);
    return;
  }

  // Create UE context and store it
  ue_ctxt_list.add_ue(bearer_context_setup_response_msg.ue_index,
                      cu_up_ue_e1ap_id,
                      int_to_gnb_cu_cp_ue_e1ap_id(msg->gnb_cu_cp_ue_e1ap_id),
                      asn1_to_activity_notification_level(msg->activity_notif_level.value));
  e1ap_ue_context& ue_ctxt = ue_ctxt_list[cu_up_ue_e1ap_id];

  ue_ctxt.logger.log_debug("Added UE context");

  if (bearer_context_setup_response_msg.success) {
    e1ap_msg.pdu.set_successful_outcome();
    e1ap_msg.pdu.successful_outcome().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_SETUP);
    e1ap_msg.pdu.successful_outcome().value.bearer_context_setup_resp()->gnb_cu_cp_ue_e1ap_id =
        msg->gnb_cu_cp_ue_e1ap_id;
    e1ap_msg.pdu.successful_outcome().value.bearer_context_setup_resp()->gnb_cu_up_ue_e1ap_id =
        gnb_cu_up_ue_e1ap_id_to_uint(cu_up_ue_e1ap_id);

    fill_asn1_bearer_context_setup_response(
        e1ap_msg.pdu.successful_outcome().value.bearer_context_setup_resp()->sys_bearer_context_setup_resp,
        bearer_context_setup_response_msg);

    // send response
    pdu_notifier->on_new_message(e1ap_msg);
  } else {
    e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_setup_fail()->cause =
        cause_to_asn1(bearer_context_setup_response_msg.cause.value());

    // send response
    pdu_notifier->on_new_message(e1ap_msg);
  }
}

void e1ap_cu_up_impl::handle_bearer_context_modification_request(const asn1::e1ap::bearer_context_mod_request_s& msg)
{
  // create failure message for early returns
  e1ap_message e1ap_msg;
  e1ap_msg.pdu.set_unsuccessful_outcome();
  e1ap_msg.pdu.unsuccessful_outcome().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_MOD);
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_mod_fail()->gnb_cu_cp_ue_e1ap_id = msg->gnb_cu_cp_ue_e1ap_id;
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_mod_fail()->gnb_cu_up_ue_e1ap_id = msg->gnb_cu_up_ue_e1ap_id;
  e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_mod_fail()->cause.set_protocol();

  e1ap_bearer_context_modification_request bearer_context_mod = {};

  if (!ue_ctxt_list.contains(int_to_gnb_cu_up_ue_e1ap_id(msg->gnb_cu_up_ue_e1ap_id))) {
    logger.warning("Sending BearerContextModificationFailure. UE context not available");
    pdu_notifier->on_new_message(e1ap_msg);
    return;
  }

  e1ap_ue_context& ue_ctxt = ue_ctxt_list[int_to_gnb_cu_up_ue_e1ap_id(msg->gnb_cu_up_ue_e1ap_id)];

  bearer_context_mod.ue_index = ue_ctxt.ue_ids.ue_index;

  // sys bearer context mod request
  if (msg->sys_bearer_context_mod_request_present) {
    // We only support NG-RAN Bearer
    if (msg->sys_bearer_context_mod_request.type() !=
        asn1::e1ap::sys_bearer_context_mod_request_c::types::ng_ran_bearer_context_mod_request) {
      ue_ctxt.logger.log_error("Sending BearerContextModificationFailure. Cause: Not handling E-UTRAN Bearers");

      // send response
      pdu_notifier->on_new_message(e1ap_msg);
      return;
    }

    fill_e1ap_bearer_context_modification_request(bearer_context_mod, msg);
  }

  // Forward message to CU-UP
  e1ap_bearer_context_modification_response bearer_context_mod_response_msg =
      cu_up_notifier.on_bearer_context_modification_request_received(bearer_context_mod);

  if (bearer_context_mod_response_msg.ue_index == INVALID_UE_INDEX) {
    ue_ctxt.logger.log_debug("Sending BearerContextModificationFailure: Cause: Invalid UE index");

    // send response
    pdu_notifier->on_new_message(e1ap_msg);
    return;
  }

  if (bearer_context_mod_response_msg.success) {
    e1ap_msg.pdu.set_successful_outcome();
    e1ap_msg.pdu.successful_outcome().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_MOD);
    e1ap_msg.pdu.successful_outcome().value.bearer_context_mod_resp()->gnb_cu_cp_ue_e1ap_id = msg->gnb_cu_cp_ue_e1ap_id;
    e1ap_msg.pdu.successful_outcome().value.bearer_context_mod_resp()->gnb_cu_up_ue_e1ap_id = msg->gnb_cu_up_ue_e1ap_id;
    e1ap_msg.pdu.successful_outcome().value.bearer_context_mod_resp()->sys_bearer_context_mod_resp_present = true;
    fill_asn1_bearer_context_modification_response(
        e1ap_msg.pdu.successful_outcome().value.bearer_context_mod_resp()->sys_bearer_context_mod_resp,
        bearer_context_mod_response_msg);
    ue_ctxt.logger.log_debug("Sending BearerContextModificationResponse");
    // send response
    pdu_notifier->on_new_message(e1ap_msg);
  } else {
    e1ap_msg.pdu.unsuccessful_outcome().value.bearer_context_mod_fail()->cause =
        cause_to_asn1(bearer_context_mod_response_msg.cause.value());

    // send response
    ue_ctxt.logger.log_debug("Sending BearerContextModificationFailure");
    pdu_notifier->on_new_message(e1ap_msg);
  }
}

void e1ap_cu_up_impl::handle_bearer_context_release_command(const asn1::e1ap::bearer_context_release_cmd_s& msg)
{
  e1ap_bearer_context_release_command bearer_context_release_cmd = {};

  if (!ue_ctxt_list.contains(int_to_gnb_cu_up_ue_e1ap_id(msg->gnb_cu_up_ue_e1ap_id))) {
    logger.error("No UE context for the received gnb_cu_up_ue_e1ap_id={} available", msg->gnb_cu_up_ue_e1ap_id);
    return;
  }

  e1ap_ue_context& ue_ctxt = ue_ctxt_list[int_to_gnb_cu_up_ue_e1ap_id(msg->gnb_cu_up_ue_e1ap_id)];

  bearer_context_release_cmd.ue_index = ue_ctxt.ue_ids.ue_index;
  bearer_context_release_cmd.cause    = asn1_to_cause(msg->cause);

  // Forward message to CU-UP
  cu_up_notifier.on_bearer_context_release_command_received(bearer_context_release_cmd);

  // Remove UE context
  ue_ctxt_list.remove_ue(ue_ctxt.ue_ids.ue_index);

  e1ap_message e1ap_msg;
  e1ap_msg.pdu.set_successful_outcome();
  e1ap_msg.pdu.successful_outcome().load_info_obj(ASN1_E1AP_ID_BEARER_CONTEXT_RELEASE);
  e1ap_msg.pdu.successful_outcome().value.bearer_context_release_complete()->gnb_cu_cp_ue_e1ap_id =
      msg->gnb_cu_cp_ue_e1ap_id;
  e1ap_msg.pdu.successful_outcome().value.bearer_context_release_complete()->gnb_cu_up_ue_e1ap_id =
      msg->gnb_cu_up_ue_e1ap_id;

  // send response
  logger.debug("ue={} cu_up_ue_e1ap_id={} cu_cp_ue_e1ap_id={}: Sending BearerContextReleaseComplete",
               bearer_context_release_cmd.ue_index,
               msg->gnb_cu_up_ue_e1ap_id,
               msg->gnb_cu_cp_ue_e1ap_id);
  pdu_notifier->on_new_message(e1ap_msg);
}

void e1ap_cu_up_impl::handle_successful_outcome(const asn1::e1ap::successful_outcome_s& outcome)
{
  optional<uint8_t> transaction_id = get_transaction_id(outcome);
  if (not transaction_id.has_value()) {
    logger.error("Successful outcome of type {} is not supported", outcome.value.type().to_string());
    return;
  }

  // Set transaction result and resume suspended procedure.
  if (not ev_mng->transactions.set_response(transaction_id.value(), outcome)) {
    logger.warning("Unexpected transaction id={}", transaction_id.value());
  }
}

void e1ap_cu_up_impl::handle_unsuccessful_outcome(const asn1::e1ap::unsuccessful_outcome_s& outcome)
{
  optional<uint8_t> transaction_id = get_transaction_id(outcome);
  if (not transaction_id.has_value()) {
    logger.error("Unsuccessful outcome of type {} is not supported", outcome.value.type().to_string());
    return;
  }

  // Set transaction result and resume suspended procedure.
  if (not ev_mng->transactions.set_response(transaction_id.value(), outcome)) {
    logger.warning("Unexpected transaction id={}", transaction_id.value());
  }
}

void e1ap_cu_up_impl::log_pdu(bool is_rx, const e1ap_message& e1ap_pdu)
{
  if (not logger.info.enabled()) {
    return;
  }

  // Fetch UE index.
  auto                 up_ue_id = get_gnb_cu_up_ue_e1ap_id(e1ap_pdu.pdu);
  optional<ue_index_t> ue_idx;
  if (up_ue_id.has_value()) {
    auto* ue_ptr = ue_ctxt_list.find_ue(up_ue_id.value());
    if (ue_ptr != nullptr and ue_ptr->ue_ids.ue_index != INVALID_UE_INDEX) {
      ue_idx = ue_ptr->ue_ids.ue_index;
    }
  }

  // Log PDU.
  log_e1ap_pdu(logger, is_rx, ue_idx, e1ap_pdu, logger.debug.enabled());
}

// ---- e1ap_message_notifier_with_logging

e1ap_cu_up_impl::e1ap_message_notifier_with_logging::e1ap_message_notifier_with_logging(
    e1ap_cu_up_impl&       parent_,
    e1ap_message_notifier& notifier_) :
  parent(parent_), notifier(notifier_)
{
}

void e1ap_cu_up_impl::e1ap_message_notifier_with_logging::on_new_message(const e1ap_message& msg)
{
  // Log message.
  parent.log_pdu(false, msg);

  // Forward message to notifier.
  notifier.on_new_message(msg);
}
