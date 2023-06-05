/* Copyright (c) 2022 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include <cxxabi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <experimental/filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <hsa/hsa.h>
#include <mutex>
#include <unordered_map>

#include "rocprofiler.h"
#include "rocprofiler_plugin.h"
#include "../utils.h"

namespace fs = std::experimental::filesystem;

namespace {

static std::string output_file_name;
class file_plugin_t {
 private:
  enum class output_type_t { COUNTER, TRACER, PC_SAMPLING };

  class output_file_t {
   public:
    output_file_t(std::string name) : name_(std::move(name)) {}

    std::string name() const { return name_; }

    template <typename T> std::ostream& operator<<(T&& value) {
      if (!is_open()) open();
      return stream_ << std::forward<T>(value);
    }

    std::ostream& operator<<(std::ostream& (*func)(std::ostream&)) {
      if (!is_open()) open();
      return stream_ << func;
    }

    void open() {
      // If the stream is already in the failed state, there's no need to try
      // to open the file.
      if (fail()) return;

      const char* output_dir = getenv("OUTPUT_PATH");
      output_file_name = getenv("OUT_FILE_NAME") ? std::string(getenv("OUT_FILE_NAME")) + "_" : "";

      if (output_dir == nullptr && getenv("OUT_FILE_NAME") == nullptr) {
        stream_.copyfmt(std::cout);
        stream_.clear(std::cout.rdstate());
        stream_.basic_ios<char>::rdbuf(std::cout.rdbuf());
        return;
      }
      if (output_dir == nullptr)
        output_dir = "./";

      fs::path output_prefix(output_dir);
      if (!fs::is_directory(fs::status(output_prefix))) {
        if (!stream_.fail()) rocprofiler::warning("Cannot open output directory '%s'", output_dir);
        stream_.setstate(std::ios_base::failbit);
        return;
      }

      std::stringstream ss;
      output_file_name = replace_MPI_macros(output_file_name);

      ss << output_file_name << GetPid() << "_" << name_;
      stream_.open(output_prefix / ss.str());
    }

    bool is_open() const { return stream_.is_open(); }
    bool fail() const { return stream_.fail(); }

    // Returns a string with the MPI %macro replaced with the corresponding envvar
    std::string replace_MPI_macros(std::string output_file_name) {
      std::unordered_map<const char*, const char*> MPI_BUILTINS = {
        {"MPI_RANK", "%rank"},
        {"OMPI_COMM_WORLD_RANK", "%rank"},
        {"MV2_COMM_WORLD_RANK", "%rank"}
      };

      for (const auto& [envvar, key] : MPI_BUILTINS) {
        size_t key_find = output_file_name.rfind(key);
        if (key_find == std::string::npos) continue; // Does not contain a %?rank var

        const char* env_var_set = getenv(envvar);
        if (env_var_set == nullptr) continue; // MPI_COMM_WORLD_x var is does not exist

        int rank = atoi(env_var_set);
        output_file_name =  output_file_name.substr(0, key_find) + std::to_string(rank)
                          + output_file_name.substr(key_find + std::string(key).size());
      }

      return output_file_name;
    }

   private:
    const std::string name_;
    std::ofstream stream_;
  };

  output_file_t* get_output_file(output_type_t output_type, uint32_t domain = 0) {
    switch (output_type) {
      case output_type_t::COUNTER:
        return &output_file_;
      case output_type_t::TRACER:
        switch (domain) {
          case ACTIVITY_DOMAIN_ROCTX:
            return &roctx_file_;
          case ACTIVITY_DOMAIN_HSA_API:
            return &hsa_api_file_;
          case ACTIVITY_DOMAIN_HIP_API:
            return &hip_api_file_;
          case ACTIVITY_DOMAIN_HIP_OPS:
            return &hip_activity_file_;
          case ACTIVITY_DOMAIN_HSA_OPS:
            return &hsa_async_copy_file_;
          default:
            assert(!"domain/op not supported!");
            break;
        }
        break;
      case output_type_t::PC_SAMPLING:
        return &pc_sample_file_;
    }
    return nullptr;
  }

 public:
  file_plugin_t() {
    output_file_t hsa_handles("hsa_handles.txt");

    [[maybe_unused]] hsa_status_t status = hsa_iterate_agents(
        [](hsa_agent_t agent, void* user_data) {
          auto* file = static_cast<decltype(hsa_handles)*>(user_data);
          hsa_device_type_t type;

          if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS)
            return HSA_STATUS_ERROR;

          *file << std::hex << std::showbase << agent.handle << " agent "
                << ((type == HSA_DEVICE_TYPE_CPU) ? "cpu" : "gpu") << std::endl;
          return HSA_STATUS_SUCCESS;
        },
        &hsa_handles);
    assert(status == HSA_STATUS_SUCCESS && "failed to iterate HSA agents");
    if (hsa_handles.fail()) {
      rocprofiler::warning("Cannot write to '%s'", hsa_handles.name().c_str());
      return;
    }

    // App begin timestamp begin_ts_file.txt
    output_file_t begin_ts("begin_ts_file.txt");

    [[maybe_unused]] rocprofiler_timestamp_t app_begin_timestamp = {};
    CHECK_ROCPROFILER(rocprofiler_get_timestamp(&app_begin_timestamp));

    begin_ts << std::dec << app_begin_timestamp.value << std::endl;
    if (begin_ts.fail()) {
      rocprofiler::warning("Cannot write to '%s'", begin_ts.name().c_str());
      return;
    }

    valid_ = true;
  }

  std::mutex writing_lock;

  const char* GetDomainName(rocprofiler_tracer_activity_domain_t domain) {
    switch (domain) {
      case ACTIVITY_DOMAIN_ROCTX:
        return "ROCTX_DOMAIN";
        break;
      case ACTIVITY_DOMAIN_HIP_API:
        return "HIP_API_DOMAIN";
        break;
      case ACTIVITY_DOMAIN_HIP_OPS:
        return "HIP_OPS_DOMAIN";
        break;
      case ACTIVITY_DOMAIN_HSA_API:
        return "HSA_API_DOMAIN";
        break;
      case ACTIVITY_DOMAIN_HSA_OPS:
        return "HSA_OPS_DOMAIN";
        break;
      case ACTIVITY_DOMAIN_HSA_EVT:
        return "HSA_EVT_DOMAIN";
        break;
      default:
        return "";
    }
  }

  void FlushTracerRecord(rocprofiler_record_tracer_t tracer_record,
                         rocprofiler_session_id_t session_id,
                         rocprofiler_buffer_id_t buffer_id = rocprofiler_buffer_id_t{0}) {
    std::lock_guard<std::mutex> lock(writing_lock);
    if (tracer_record.timestamps.end.value <= 0 && tracer_record.domain != ACTIVITY_DOMAIN_ROCTX)
      return;
    std::string kernel_name;
    std::string roctx_message;
    uint64_t roctx_id;
    if (tracer_record.name) {
      if (tracer_record.domain == ACTIVITY_DOMAIN_HIP_API)
        kernel_name = rocprofiler::cxx_demangle(tracer_record.name);
      if (tracer_record.domain == ACTIVITY_DOMAIN_ROCTX) roctx_message = tracer_record.name;
    }
    size_t function_name_size = 0;
    char* function_name_c = nullptr;
    if (tracer_record.domain == ACTIVITY_DOMAIN_HSA_API) {
      CHECK_ROCPROFILER(rocprofiler_query_hsa_tracer_api_data_info_size(
          rocprofiler_session_id_t{0}, ROCPROFILER_HSA_FUNCTION_NAME, tracer_record.api_data_handle,
          tracer_record.operation_id, &function_name_size));
      function_name_c = new char[function_name_size];
      if (function_name_size > 1) {
        CHECK_ROCPROFILER(rocprofiler_query_hsa_tracer_api_data_info(
            rocprofiler_session_id_t{0}, ROCPROFILER_HSA_FUNCTION_NAME,
            tracer_record.api_data_handle, tracer_record.operation_id, &function_name_c));
      }
    }
    if (tracer_record.domain == ACTIVITY_DOMAIN_HIP_API) {
      CHECK_ROCPROFILER(rocprofiler_query_hip_tracer_api_data_info_size(
          rocprofiler_session_id_t{0}, ROCPROFILER_HIP_FUNCTION_NAME, tracer_record.api_data_handle,
          tracer_record.operation_id, &function_name_size));
      if (function_name_size > 1) {
        CHECK_ROCPROFILER(rocprofiler_query_hip_tracer_api_data_info(
            rocprofiler_session_id_t{0}, ROCPROFILER_HIP_FUNCTION_NAME,
            tracer_record.api_data_handle, tracer_record.operation_id, &function_name_c));
      }
    }
    output_file_t* output_file = get_output_file(output_type_t::TRACER, tracer_record.domain);
    *output_file << "Record(" << tracer_record.header.id.handle << "), Domain("
                 << GetDomainName(tracer_record.domain) << "),";
    if (tracer_record.domain == ACTIVITY_DOMAIN_ROCTX && roctx_id >= 0)
      *output_file << " ROCTX_ID(" << tracer_record.operation_id.id << "),";
    if (tracer_record.domain == ACTIVITY_DOMAIN_ROCTX && tracer_record.name)
      *output_file << " ROCTX_Message(" << reinterpret_cast<const char*>(tracer_record.name)
                   << "),";
    if (function_name_c) *output_file << " Function(" << function_name_c << "),";
    if (kernel_name.size() > 1) *output_file << " Kernel_Name(" << kernel_name.c_str() << "),";
    if (tracer_record.domain == ACTIVITY_DOMAIN_HSA_OPS ||
        tracer_record.domain == ACTIVITY_DOMAIN_HIP_OPS) {
      switch (tracer_record.operation_id.id) {
        case 0:
          *output_file << " Operation(DISPATCH_OP),";
          break;
        case 1:
          *output_file << " Operation(COPY_OP),";
          break;
        case 2:
          *output_file << " Operation(BARRIER_OP),";
          break;
        default:
          break;
      }
    }
    if (tracer_record.domain == ACTIVITY_DOMAIN_ROCTX) {
      *output_file << " timestamp(" << tracer_record.timestamps.begin.value << ")";
    } else if (tracer_record.phase == ROCPROFILER_PHASE_EXIT ||
               tracer_record.phase == ROCPROFILER_PHASE_NONE) {
      *output_file << " Begin(" << tracer_record.timestamps.begin.value << "), End("
                   << tracer_record.timestamps.end.value << ")";
    }
    if (tracer_record.domain != ACTIVITY_DOMAIN_ROCTX)
      *output_file << ", Correlation_ID(" << tracer_record.correlation_id.value << ")";
    *output_file << '\n';
  }

  void FlushProfilerRecord(const rocprofiler_record_profiler_t* profiler_record,
                           rocprofiler_session_id_t session_id, rocprofiler_buffer_id_t buffer_id) {
    std::lock_guard<std::mutex> lock(writing_lock);
    size_t name_length = 0;
    output_file_t* output_file{nullptr};
    output_file = get_output_file(output_type_t::COUNTER);
    CHECK_ROCPROFILER(rocprofiler_query_kernel_info_size(ROCPROFILER_KERNEL_NAME,
                                                         profiler_record->kernel_id, &name_length));
    // Taken from rocprofiler: The size hasn't changed in  recent past
    static const uint32_t lds_block_size = 128 * 4;
    const char* kernel_name_c = nullptr;
    if (name_length > 1) {
      CHECK_ROCPROFILER(rocprofiler_query_kernel_info(ROCPROFILER_KERNEL_NAME,
                                                      profiler_record->kernel_id, &kernel_name_c));
    }
    *output_file << std::string("dispatch[") << std::to_string(profiler_record->header.id.handle)
                 << "], " << std::string("gpu_id(")
                 << std::to_string(profiler_record->gpu_id.handle) << "), "
                 << std::string("queue_id(") << std::to_string(profiler_record->queue_id.handle)
                 << "), " << std::string("queue_index(")
                 << std::to_string(profiler_record->queue_idx.value) << "), " << std::string("pid(")
                 << std::to_string(GetPid()) << "), " << std::string("tid(")
                 << std::to_string(profiler_record->thread_id.value) << ")";
    *output_file << ", " << std::string("grd(")
                 << std::to_string(profiler_record->kernel_properties.grid_size) << "), "
                 << std::string("wgr(")
                 << std::to_string(profiler_record->kernel_properties.workgroup_size) << "), "
                 << std::string("lds(")
                 << std::to_string(
                        ((profiler_record->kernel_properties.lds_size + (lds_block_size - 1)) &
                         ~(lds_block_size - 1)))
                 << "), " << std::string("scr(")
                 << std::to_string(profiler_record->kernel_properties.scratch_size) << "), "
                 << std::string("arch_vgpr(")
                 << std::to_string(profiler_record->kernel_properties.arch_vgpr_count) << "), "
                 << std::string("accum_vgpr(")
                 << std::to_string(profiler_record->kernel_properties.accum_vgpr_count) << "), "
                 << std::string("sgpr(")
                 << std::to_string(profiler_record->kernel_properties.sgpr_count) << "), "
                 << std::string("wave_size(")
                 << std::to_string(profiler_record->kernel_properties.wave_size) << "), "
                 << std::string("sig(")
                 << std::to_string(profiler_record->kernel_properties.signal_handle);
    std::string kernel_name = "";
    if (name_length > 1) {
      kernel_name = rocprofiler::truncate_name(rocprofiler::cxx_demangle(kernel_name_c));
    }
    *output_file << "), " << std::string("obj(")
                 << std::to_string(profiler_record->kernel_id.handle) << "), "
                 << std::string("kernel-name(\"") << kernel_name << "\")"
                 << std::string(", start_time(")
                 << std::to_string(profiler_record->timestamps.begin.value) << ")"
                 << std::string(", end_time(")
                 << std::to_string(profiler_record->timestamps.end.value) << ")";

    // For Counters
    if (profiler_record->counters) {
      for (uint64_t i = 0; i < profiler_record->counters_count.value; i++) {
        if (profiler_record->counters[i].counter_handler.handle > 0) {
          size_t counter_name_length = 0;
          CHECK_ROCPROFILER(rocprofiler_query_counter_info_size(
              session_id, ROCPROFILER_COUNTER_NAME, profiler_record->counters[i].counter_handler,
              &counter_name_length));
          if (counter_name_length > 1) {
            const char* name_c = nullptr;
            CHECK_ROCPROFILER(rocprofiler_query_counter_info(
                session_id, ROCPROFILER_COUNTER_NAME, profiler_record->counters[i].counter_handler,
                &name_c));
            *output_file << ", " << name_c << " ("
                         << std::to_string(profiler_record->counters[i].value.value) << ')';
          }
        }
      }
    }
    *output_file << '\n';
    if (kernel_name_c) {
      free(const_cast<char*>(kernel_name_c));
    }
  }

  void FlushPCSamplingRecord(const rocprofiler_record_pc_sample_t* pc_sampling_record) {
    output_file_t* output_file{nullptr};
    output_file = get_output_file(output_type_t::PC_SAMPLING);
    const auto& sample = pc_sampling_record->pc_sample;
    *output_file << "dispatch[" << sample.dispatch_id.value << "], "
                 << "timestamp(" << sample.timestamp.value << "), "
                 << "gpu_id(" << sample.gpu_id.handle << "), "
                 << "pc-sample(" << std::hex << std::showbase << sample.pc << "), "
                 << "se(" << sample.se << ')' << std::endl;
  }
  int WriteBufferRecords(const rocprofiler_record_header_t* begin,
                         const rocprofiler_record_header_t* end,
                         rocprofiler_session_id_t session_id, rocprofiler_buffer_id_t buffer_id) {
    while (begin < end) {
      if (!begin) return 0;
      switch (begin->kind) {
        case ROCPROFILER_PROFILER_RECORD: {
          const rocprofiler_record_profiler_t* profiler_record =
              reinterpret_cast<const rocprofiler_record_profiler_t*>(begin);
          FlushProfilerRecord(profiler_record, session_id, buffer_id);
          break;
        }
        case ROCPROFILER_TRACER_RECORD: {
          rocprofiler_record_tracer_t* tracer_record = const_cast<rocprofiler_record_tracer_t*>(
              reinterpret_cast<const rocprofiler_record_tracer_t*>(begin));
          FlushTracerRecord(*tracer_record, session_id, buffer_id);
          break;
        }
        case ROCPROFILER_ATT_TRACER_RECORD: {
          break;
        }
        case ROCPROFILER_PC_SAMPLING_RECORD: {
          const rocprofiler_record_pc_sample_t* pc_sampling_record =
              reinterpret_cast<const rocprofiler_record_pc_sample_t*>(begin);
          FlushPCSamplingRecord(pc_sampling_record);
          break;
        }
        default:
          break;
      }
      rocprofiler_next_record(begin, &begin, session_id, buffer_id);
    }
    return 0;
  }

  bool is_valid() const { return valid_; }

 private:
  bool valid_{false};

  output_file_t roctx_file_{"roctx_trace.txt"}, hsa_api_file_{"hsa_api_trace.txt"},
      hip_api_file_{"hip_api_trace.txt"}, hip_activity_file_{"hcc_ops_trace.txt"},
      hsa_async_copy_file_{"async_copy_trace.txt"}, pc_sample_file_{"pcs_trace.txt"},
      output_file_{"results.txt"};
};

file_plugin_t* file_plugin = nullptr;

}  // namespace

ROCPROFILER_EXPORT int rocprofiler_plugin_initialize(uint32_t rocprofiler_major_version,
                                                     uint32_t rocprofiler_minor_version) {
  if (rocprofiler_major_version != ROCPROFILER_VERSION_MAJOR ||
      rocprofiler_minor_version < ROCPROFILER_VERSION_MINOR)
    return -1;

  if (file_plugin != nullptr) return -1;

  file_plugin = new file_plugin_t();
  if (file_plugin->is_valid()) return 0;

  // The plugin failed to initialized, destroy it and return an error.
  delete file_plugin;
  file_plugin = nullptr;
  return -1;
}

ROCPROFILER_EXPORT void rocprofiler_plugin_finalize() {
  if (!file_plugin) return;
  delete file_plugin;
  file_plugin = nullptr;
}

ROCPROFILER_EXPORT int rocprofiler_plugin_write_buffer_records(
    const rocprofiler_record_header_t* begin, const rocprofiler_record_header_t* end,
    rocprofiler_session_id_t session_id, rocprofiler_buffer_id_t buffer_id) {
  if (!file_plugin || !file_plugin->is_valid()) return -1;
  return file_plugin->WriteBufferRecords(begin, end, session_id, buffer_id);
}

ROCPROFILER_EXPORT int rocprofiler_plugin_write_record(rocprofiler_record_tracer_t record) {
  if (!file_plugin || !file_plugin->is_valid()) return -1;
  if (record.header.id.handle == 0) return 0;
  file_plugin->FlushTracerRecord(record, rocprofiler_session_id_t{0}, rocprofiler_buffer_id_t{0});
  return 0;
}
