/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsran/hal/phy/upper/channel_processors/hw_accelerator_factories.h"
#include "../../../dpdk/bbdev/bbdev.h"
#include "../../../dpdk/dpdk.h"
#include "hw_accelerator_pdsch_enc_acc100_impl.h"
#include "hw_accelerator_pdsch_enc_impl.h"

using namespace srsran;
using namespace hal;

#ifdef ENABLE_PDSCH_HWACC

namespace {

class hw_accelerator_pdsch_enc_factory_spec : public hw_accelerator_pdsch_enc_factory
{
private:
  /// Accelerator type.
  std::string acc_type;
  /// Interfacing to a bbdev-based hardware-accelerator.
  std::shared_ptr<srsran::dpdk::bbdev_acc> bbdev_accelerator;
  /// Operation mode of the PDSCH encoder (CB = true, TB = false [default]).
  bool cb_mode = false;
  /// Maximum supported TB size in bytes (used to size the mbufs).
  unsigned max_tb_size;
  /// Indicates if the accelerated function uses a dedicated hardware queue or needs to reserve one for each operation.
  bool dedicated_queue = true;

public:
  // Default constructor.
  explicit hw_accelerator_pdsch_enc_factory_spec(const hw_accelerator_pdsch_enc_configuration& accelerator_config) :
    acc_type(accelerator_config.acc_type),
    bbdev_accelerator(std::move(accelerator_config.bbdev_accelerator)),
    cb_mode(accelerator_config.cb_mode),
    max_tb_size(accelerator_config.max_tb_size),
    dedicated_queue(accelerator_config.dedicated_queue)
  {
  }

  std::unique_ptr<hw_accelerator_pdsch_enc> create() override
  {
    if (acc_type == "acc100") {
      return std::make_unique<hw_accelerator_pdsch_enc_acc100_impl>(
          bbdev_accelerator, cb_mode, max_tb_size, dedicated_queue);
    }
    // Handle other accelerator types here.
    return {};
  }
};

} // namespace

#endif // ENABLE_PDSCH_HWACC

std::shared_ptr<hw_accelerator_pdsch_enc_factory>
srsran::hal::create_hw_accelerator_pdsch_enc_factory(const hw_accelerator_pdsch_enc_configuration& accelerator_config)
{
#ifdef ENABLE_PDSCH_HWACC
  return std::make_shared<hw_accelerator_pdsch_enc_factory_spec>(accelerator_config);
#else  // ENABLE_PDSCH_HWACC
  return nullptr;
#endif // ENABLE_PDSCH_HWACC
}
