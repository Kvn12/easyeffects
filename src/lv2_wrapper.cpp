/*
 *  Copyright © 2017-2023 Wellington Wallace
 *
 *  This file is part of Easy Effects.
 *
 *  Easy Effects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Easy Effects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Easy Effects. If not, see <https://www.gnu.org/licenses/>.
 */

#include "lv2_wrapper.hpp"

namespace lv2 {

constexpr auto min_quantum = 32;
constexpr auto max_quantum = 8192;

auto lv2_printf(LV2_Log_Handle handle, LV2_URID type, const char* format, ...) -> int {
  va_list args;

  va_start(args, format);

  int r = std::vprintf(format, args);

  va_end(args);

  return r;
}

Lv2Wrapper::Lv2Wrapper(const std::string& plugin_uri) : plugin_uri(plugin_uri), world(lilv_world_new()) {
  if (world == nullptr) {
    util::warning("failed to initialized the world");

    return;
  }

  auto* const uri = lilv_new_uri(world, plugin_uri.c_str());

  if (uri == nullptr) {
    util::warning("Invalid plugin URI: " + plugin_uri);

    return;
  }

  lilv_world_load_all(world);

  const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

  plugin = lilv_plugins_get_by_uri(plugins, uri);

  lilv_node_free(uri);

  if (plugin == nullptr) {
    util::warning("Could not find the plugin: " + plugin_uri);

    return;
  }

  found_plugin = true;

  check_required_features();

  create_ports();
}

Lv2Wrapper::~Lv2Wrapper() {
  if (instance != nullptr) {
    lilv_instance_deactivate(instance);
    lilv_instance_free(instance);

    instance = nullptr;
  }

  if (world != nullptr) {
    lilv_world_free(world);
  }
}

void Lv2Wrapper::check_required_features() {
  LilvNodes* required_features = lilv_plugin_get_required_features(plugin);

  if (required_features != nullptr) {
    for (auto* i = lilv_nodes_begin(required_features); !lilv_nodes_is_end(required_features, i);
         i = lilv_nodes_next(required_features, i)) {
      const LilvNode* required_feature = lilv_nodes_get(required_features, i);

      const char* required_feature_uri = lilv_node_as_uri(required_feature);

      util::debug(plugin_uri + " requires feature: " + required_feature_uri);
    }

    lilv_nodes_free(required_features);
  }
}

void Lv2Wrapper::create_ports() {
  n_ports = lilv_plugin_get_num_ports(plugin);

  ports.resize(n_ports);

  // Get default values for all ports

  std::vector<float> values(n_ports);

  lilv_plugin_get_port_ranges_float(plugin, nullptr, nullptr, values.data());

  LilvNode* lv2_InputPort = lilv_new_uri(world, LV2_CORE__InputPort);
  LilvNode* lv2_OutputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
  LilvNode* lv2_AudioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
  LilvNode* lv2_ControlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
  LilvNode* lv2_AtomPort = lilv_new_uri(world, LV2_ATOM__AtomPort);
  LilvNode* lv2_connectionOptional = lilv_new_uri(world, LV2_CORE__connectionOptional);

  for (uint n = 0U; n < n_ports; n++) {
    auto* port = &ports[n];

    const auto* lilv_port = lilv_plugin_get_port_by_index(plugin, n);

    auto* port_name = lilv_port_get_name(plugin, lilv_port);

    port->index = n;
    port->name = lilv_node_as_string(port_name);
    port->symbol = lilv_node_as_string(lilv_port_get_symbol(plugin, lilv_port));
    port->value = std::isnan(values[n]) ? 0.0F : values[n];
    port->optional = lilv_port_has_property(plugin, lilv_port, lv2_connectionOptional);

    // util::warning("port name: " + port->name);
    // util::warning("port symbol: " + port->symbol);

    if (lilv_port_is_a(plugin, lilv_port, lv2_InputPort)) {
      port->is_input = true;
    } else if (!lilv_port_is_a(plugin, lilv_port, lv2_OutputPort) && !port->optional) {
      util::warning("Port " + port->name + " is neither input nor output!");
    }

    if (lilv_port_is_a(plugin, lilv_port, lv2_ControlPort)) {
      port->type = TYPE_CONTROL;
    } else if (lilv_port_is_a(plugin, lilv_port, lv2_AtomPort)) {
      port->type = TYPE_ATOM;

      // util::warning("port name: " + port->name);
    } else if (lilv_port_is_a(plugin, lilv_port, lv2_AudioPort)) {
      port->type = TYPE_AUDIO;

      n_audio_in = (port->is_input) ? n_audio_in + 1 : n_audio_in;
      n_audio_out = (!port->is_input) ? n_audio_out + 1 : n_audio_out;
    } else if (!port->optional) {
      util::warning("Port " + port->name + " has un unsupported type!");
    }

    lilv_node_free(port_name);
  }

  // util::warning("n audio_in ports: " + util::to_string(n_audio_in));
  // util::warning("n audio_out ports: " + util::to_string(n_audio_out));

  lilv_node_free(lv2_connectionOptional);
  lilv_node_free(lv2_ControlPort);
  lilv_node_free(lv2_AtomPort);
  lilv_node_free(lv2_AudioPort);
  lilv_node_free(lv2_OutputPort);
  lilv_node_free(lv2_InputPort);
}

auto Lv2Wrapper::create_instance(const uint& rate) -> bool {
  this->rate = rate;

  if (instance != nullptr) {
    deactivate();

    lilv_instance_free(instance);

    instance = nullptr;
  }

  LV2_Log_Log lv2_log = {this, &lv2_printf, [](LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap) {
                           return std::vprintf(fmt, ap);
                         }};

  LV2_URID_Map lv2_map = {this, [](LV2_URID_Map_Handle handle, const char* uri) {
                            auto* lw = static_cast<Lv2Wrapper*>(handle);

                            return lw->map_urid(uri);
                          }};

  LV2_URID_Unmap lv2_unmap = {this, [](LV2_URID_Unmap_Handle handle, LV2_URID urid) {
                                auto* lw = static_cast<Lv2Wrapper*>(handle);

                                return lw->map_urid_to_uri[urid].c_str();
                              }};

  const LV2_Feature lv2_log_feature = {LV2_LOG__log, &lv2_log};

  const LV2_Feature lv2_map_feature = {LV2_URID__map, &lv2_map};

  const LV2_Feature lv2_unmap_feature = {LV2_URID__unmap, &lv2_unmap};

  auto options = std::to_array<LV2_Options_Option>(
      {{LV2_OPTIONS_INSTANCE, 0, map_urid(LV2_PARAMETERS__sampleRate), sizeof(float), map_urid(LV2_ATOM__Float), &rate},
       {LV2_OPTIONS_INSTANCE, 0, map_urid(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), map_urid(LV2_ATOM__Int),
        &min_quantum},
       {LV2_OPTIONS_INSTANCE, 0, map_urid(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), map_urid(LV2_ATOM__Int),
        &max_quantum},
       {LV2_OPTIONS_INSTANCE, 0, map_urid(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), map_urid(LV2_ATOM__Int),
        &n_samples},
       {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr}});

  LV2_Feature feature_options = {.URI = LV2_OPTIONS__options, .data = options.data()};

  const auto features = std::to_array<const LV2_Feature*>(
      {&lv2_log_feature, &lv2_map_feature, &lv2_unmap_feature, &feature_options, static_features.data(), nullptr});

  instance = lilv_plugin_instantiate(plugin, rate, features.data());

  if (instance == nullptr) {
    util::warning("failed to instantiate " + plugin_uri);

    return false;
  }

  connect_control_ports();

  activate();

  return true;
}

void Lv2Wrapper::connect_control_ports() {
  for (auto& p : ports) {
    if (p.type == PortType::TYPE_CONTROL) {
      lilv_instance_connect_port(instance, p.index, &p.value);
    }
  }
}

void Lv2Wrapper::connect_data_ports(std::span<float>& left_in,
                                    std::span<float>& right_in,
                                    std::span<float>& left_out,
                                    std::span<float>& right_out) {
  int count_input = 0;
  int count_output = 0;

  for (const auto& p : ports) {
    if (p.type == PortType::TYPE_AUDIO) {
      if (p.is_input) {
        if (count_input == 0) {
          lilv_instance_connect_port(instance, p.index, left_in.data());
        } else if (count_input == 1) {
          lilv_instance_connect_port(instance, p.index, right_in.data());
        }

        count_input++;
      } else {
        if (count_output == 0) {
          lilv_instance_connect_port(instance, p.index, left_out.data());
        } else if (count_output == 1) {
          lilv_instance_connect_port(instance, p.index, right_out.data());
        }

        count_output++;
      }
    }
  }
}

void Lv2Wrapper::connect_data_ports(std::span<float>& left_in,
                                    std::span<float>& right_in,
                                    std::span<float>& left_out,
                                    std::span<float>& right_out,
                                    std::span<float>& probe_left,
                                    std::span<float>& probe_right) {
  int count_input = 0;
  int count_output = 0;

  for (const auto& p : ports) {
    if (p.type == PortType::TYPE_AUDIO) {
      if (p.is_input) {
        if (count_input == 0) {
          lilv_instance_connect_port(instance, p.index, left_in.data());
        } else if (count_input == 1) {
          lilv_instance_connect_port(instance, p.index, right_in.data());
        } else if (count_input == 2) {
          lilv_instance_connect_port(instance, p.index, probe_left.data());
        } else if (count_input == 3) {
          lilv_instance_connect_port(instance, p.index, probe_right.data());
        }

        count_input++;
      } else {
        if (count_output == 0) {
          lilv_instance_connect_port(instance, p.index, left_out.data());
        } else if (count_output == 1) {
          lilv_instance_connect_port(instance, p.index, right_out.data());
        }

        count_output++;
      }
    }
  }
}

void Lv2Wrapper::set_n_samples(const uint& value) {
  this->n_samples = value;
}

auto Lv2Wrapper::get_n_samples() const -> uint {
  return this->n_samples;
}

auto Lv2Wrapper::get_rate() const -> uint {
  return this->rate;
}

void Lv2Wrapper::activate() {
  lilv_instance_activate(instance);
}

void Lv2Wrapper::run() const {
  if (instance != nullptr) {
    lilv_instance_run(instance, n_samples);
  }
}

void Lv2Wrapper::deactivate() {
  lilv_instance_deactivate(instance);
}

void Lv2Wrapper::set_control_port_value(const std::string& symbol, const float& value) {
  auto found = false;

  for (auto& p : ports) {
    if (p.type == PortType::TYPE_CONTROL && p.symbol == symbol) {
      if (!p.is_input) {
        util::warning(plugin_uri + " port " + symbol + " is not an input!");

        return;
      }

      p.value = value;

      found = true;

      break;
    }
  }

  if (!found) {
    util::warning(plugin_uri + " port symbol not found: " + symbol);
  }
}
auto Lv2Wrapper::get_control_port_value(const std::string& symbol) -> float {
  for (const auto& p : ports) {
    if (p.type == PortType::TYPE_CONTROL && p.symbol == symbol) {
      return p.value;
    }
  }

  util::warning(plugin_uri + " port symbol not found: " + symbol);

  return 0.0F;
}

auto Lv2Wrapper::has_instance() -> bool {
  return instance != nullptr;
}

auto Lv2Wrapper::map_urid(const std::string& uri) -> LV2_URID {
  if (map_uri_to_urid.contains(uri)) {
    return map_uri_to_urid[uri];
  }

  const auto hash = std::hash<std::string>{}(uri);

  map_uri_to_urid[uri] = hash;

  map_urid_to_uri[hash] = uri;

  return static_cast<LV2_URID>(hash);
}

}  // namespace lv2
