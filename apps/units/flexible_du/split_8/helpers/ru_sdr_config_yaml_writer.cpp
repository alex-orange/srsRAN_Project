/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "ru_sdr_config_yaml_writer.h"
#include "ru_sdr_config.h"

using namespace srsran;

static void fill_ru_sdr_log_section(YAML::Node node, const ru_sdr_unit_logger_config& config)
{
  node["radio_level"] = srslog::basic_level_to_string(config.radio_level);
  node["phy_level"]   = srslog::basic_level_to_string(config.phy_level);
}

static std::string to_string(lower_phy_thread_profile profile)
{
  switch (profile) {
    case lower_phy_thread_profile::blocking:
      // Blocking is an internal profile for ZMQ. Output 'single' for the configuration.
      return "single";
    case lower_phy_thread_profile::dual:
      return "dual";
    case lower_phy_thread_profile::quad:
      return "quad";
    case lower_phy_thread_profile::single:
      return "single";
    default:
      srsran_assert(0, "Invalid low PHY profile");
      break;
  }
  return {};
}

static void fill_ru_sdr_expert_execution_section(YAML::Node node, const ru_sdr_unit_expert_execution_config& config)
{
  {
    YAML::Node threads_node         = node["threads"];
    YAML::Node lower_node           = threads_node["lower_phy"];
    lower_node["execution_profile"] = to_string(config.threads.execution_profile);
  }

  auto cell_affinities_node = node["cell_affinities"];
  while (config.cell_affinities.size() > cell_affinities_node.size()) {
    cell_affinities_node.push_back(YAML::Node());
  }

  unsigned index = 0;
  for (auto cell : cell_affinities_node) {
    const auto& expert = config.cell_affinities[index];

    if (expert.l1_dl_cpu_cfg.mask.any()) {
      cell["l1_dl_cpus"] = fmt::format("{:,}", span<const size_t>(expert.l1_dl_cpu_cfg.mask.get_cpu_ids()));
    }
    cell["l1_dl_pinning"] = to_string(expert.l1_dl_cpu_cfg.pinning_policy);

    if (expert.l1_ul_cpu_cfg.mask.any()) {
      cell["l1_ul_cpus"] = fmt::format("{:,}", span<const size_t>(expert.l1_ul_cpu_cfg.mask.get_cpu_ids()));
    }
    cell["l1_ul_pinning"] = to_string(expert.l1_ul_cpu_cfg.pinning_policy);

    if (expert.ru_cpu_cfg.mask.any()) {
      cell["l1_dl_cpus"] = fmt::format("{:,}", span<const size_t>(expert.ru_cpu_cfg.mask.get_cpu_ids()));
    }
    cell["l1_dl_pinning"] = to_string(expert.ru_cpu_cfg.pinning_policy);

    ++index;
  }
}

static void fill_ru_sdr_section(YAML::Node node, const ru_sdr_unit_config& config)
{
  node["srate"]         = config.srate_MHz;
  node["device_driver"] = config.device_driver;
  node["device_args"]   = config.device_arguments;
  node["tx_gain"]       = config.tx_gain_dB;
  node["rx_gain"]       = config.rx_gain_dB;
  node["freq_offset"]   = config.center_freq_offset_Hz;
  node["clock_ppm"]     = config.calibrate_clock_ppm;
  node["lo_offset"]     = config.lo_offset_MHz;
  node["clock"]         = config.clock_source;
  node["sync"]          = config.synch_source;
  node["otw_format"]    = config.otw_format;
  if (config.time_alignment_calibration.has_value()) {
    node["time_alignment_calibration"] = config.time_alignment_calibration.value();
  }

  {
    YAML::Node amp_crtl_node         = node["amplitude_control"];
    amp_crtl_node["tx_gain_backoff"] = config.amplitude_cfg.gain_backoff_dB;
    amp_crtl_node["enable_clipping"] = config.amplitude_cfg.enable_clipping;
    amp_crtl_node["ceiling"]         = config.amplitude_cfg.power_ceiling_dBFS;
  }

  {
    YAML::Node expert_node               = node["expert_cfg"];
    expert_node["low_phy_dl_throttling"] = config.expert_cfg.lphy_dl_throttling;
    expert_node["tx_mode"]               = config.expert_cfg.transmission_mode;
    expert_node["power_ramping_time_us"] = config.expert_cfg.power_ramping_time_us;
    expert_node["pps_time_offset_us"]    = config.expert_cfg.pps_time_offset_us;
    expert_node["sample_offset"]         = config.expert_cfg.sample_offset;
    auto gpio_tx_cells = expert_node["gpio_tx_cells"];
    while (config.expert_cfg.gpio_tx_cells.size() > gpio_tx_cells.size()) {
      gpio_tx_cells.push_back(YAML::Node());
    }
    for (unsigned i = 0; i != config.expert_cfg.gpio_tx_cells.size(); ++i) {
      auto gpio_tx_sectors = gpio_tx_cells[i]["sectors"];
      auto config_gpio_tx_sectors =
          config.expert_cfg.gpio_tx_cells[i].sectors;

      while (config_gpio_tx_sectors.size() > gpio_tx_sectors.size()) {
        gpio_tx_cells.push_back(YAML::Node());
      }

      for (unsigned j = 0; j != config_gpio_tx_sectors.size(); ++j) {
        if (config_gpio_tx_sectors[j].gpio_index.has_value()) {
          gpio_tx_sectors[j]["gpio_index"] =
              config_gpio_tx_sectors[j].gpio_index.value();
          gpio_tx_sectors[j]["sense"] = config_gpio_tx_sectors[j].sense;
          gpio_tx_sectors[j]["source"] = config_gpio_tx_sectors[j].source;
          gpio_tx_sectors[j]["prelude"] = config_gpio_tx_sectors[j].prelude;
        }
      }
    }
    expert_node["dl_buffer_size_policy"] = config.expert_cfg.dl_buffer_size_policy;
  }
}

void srsran::fill_ru_sdr_config_in_yaml_schema(YAML::Node& node, const ru_sdr_unit_config& config)
{
  fill_ru_sdr_log_section(node["log"], config.loggers);
  fill_ru_sdr_expert_execution_section(node["expert_execution"], config.expert_execution_cfg);
  fill_ru_sdr_section(node["ru_sdr"], config);
}
