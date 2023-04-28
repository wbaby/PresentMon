// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#include "WmiCpu.h"

namespace pwr::cpu::wmi {

std::wstring kProcessorFrequency =
    L"\\Processor Information(_Total)\\Processor Frequency";
std::wstring kProcessorPerformance =
    L"\\Processor Information(_Total)\\% Processor Performance";
std::wstring kProcessorTime = L"\\Processor(_Total)\\% Processor Time";

WmiCpu::WmiCpu() {
  if (const auto result = PdhOpenQuery(NULL, NULL, &query_);
      result != ERROR_SUCCESS) {
    throw std::runtime_error{"PdhOpenQuery failed"};
  }

  if (const auto result = PdhAddCounter(query_, kProcessorFrequency.c_str(), 0,
                                        &processor_frequency_counter_);
      result != ERROR_SUCCESS) {
    throw std::runtime_error{
        "PdhAddCounter failed when adding processor frequency counter"};
  }

  if (const auto result = PdhAddCounter(query_, kProcessorPerformance.c_str(),
                                        0, &processor_performance_counter_);
      result != ERROR_SUCCESS) {
    throw std::runtime_error{
        "PdhAddCounter failed when adding processor performance counter"};
  }

  if (const auto result = PdhAddCounter(query_, kProcessorTime.c_str(), 0,
                                        &processor_time_counter_);
      result != ERROR_SUCCESS) {
    throw std::runtime_error{
        "PdhAddCounter failed when adding processor time counter"};
  }

  // Most counters require two sample values to display a formatted value.
  // PDH stores the current sample value and the previously collected
  // sample value. This call retrieves the first value that will be used
  // by PdhGetFormattedCounterValue in the Sample() call.
  if (const auto result = PdhCollectQueryData(query_);
      result != ERROR_SUCCESS) {
    throw std::runtime_error{
        "PdhAddCounter failed when adding processor time counter"};
  }

  // WMI specifies that it should not be sampled faster than once every
  // second. We however allow the user to specify the sample rate for
  // telemetry. Through testing it was observed that allowing a
  // sample rate of faster than one second will cause the CPU utilization
  // number to become inaccurate. Because of this we will impose
  // a one second wait for WMI sampling.

  // Grab the current QPC frequency which returns the current performance-
  // counter frequency in counts per SECOND.
  QueryPerformanceFrequency(&frequency_);
  // Now grab the current value of the performance counter
  QueryPerformanceCounter(&next_sample_qpc_);
  // To calculate the next time we should sample take the just sampled
  // performance counter and the frequency.
  next_sample_qpc_.QuadPart += frequency_.QuadPart;
}

WmiCpu::~WmiCpu() {
  if (query_) {
    PdhCloseQuery(query_);
  }
}

bool WmiCpu::Sample() noexcept {
  DWORD counter_type;

  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  if (qpc.QuadPart < next_sample_qpc_.QuadPart) {
    return true;
  }

  CpuTelemetryInfo info{
      .qpc = (uint64_t)qpc.QuadPart,
  };

  if (const auto result = PdhCollectQueryData(query_);
      result != ERROR_SUCCESS) {
    return false;
  }

  // Sample cpu clock. This is an approximation using the frequency and then
  // the current percentage.
  PDH_FMT_COUNTERVALUE counter_value;
  {
    if (const auto result = PdhGetFormattedCounterValue(
            processor_frequency_counter_, PDH_FMT_DOUBLE, &counter_type,
            &counter_value);
        result == ERROR_SUCCESS) {
      info.cpu_frequency = counter_value.doubleValue;

      if (const auto result = PdhGetFormattedCounterValue(
              processor_performance_counter_, PDH_FMT_DOUBLE, &counter_type,
              &counter_value);
          result == ERROR_SUCCESS) {
        info.cpu_frequency =
            info.cpu_frequency * (counter_value.doubleValue / 100.);
      }
    }
  }

  // Sample cpu utilization
  {
    if (const auto result =
            PdhGetFormattedCounterValue(processor_time_counter_, PDH_FMT_DOUBLE,
                                        &counter_type, &counter_value);
        result == ERROR_SUCCESS) {
      info.cpu_utilization = counter_value.doubleValue;
    }
  }

  // insert telemetry into history
  std::lock_guard lock{history_mutex_};
  history_.Push(info);

  // Update the next sample qpc based on the current sample qpc
  // and adding in the frequency
  next_sample_qpc_.QuadPart = qpc.QuadPart + frequency_.QuadPart;

  return true;
}

std::optional<CpuTelemetryInfo> WmiCpu::GetClosest(uint64_t qpc)
      const noexcept {
  std::lock_guard lock{history_mutex_};
  return history_.GetNearest(qpc);
}

}