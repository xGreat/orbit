// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "PerfEventReaders.h"

#include <vector>

#include "PerfEventRecords.h"
#include "PerfEventRingBuffer.h"

namespace orbit_linux_tracing {

pid_t ReadMmapRecordPid(PerfEventRingBuffer* ring_buffer) {
  // Mmap records have the following layout:
  // struct {
  //   struct perf_event_header header;
  //   u32    pid, tid;
  //   u64    addr;
  //   u64    len;
  //   u64    pgoff;
  //   char   filename[];
  //   struct sample_id sample_id; /* if sample_id_all */
  // };
  // Because of filename, the layout is not fixed.

  pid_t pid;
  ring_buffer->ReadValueAtOffset(&pid, sizeof(perf_event_header));
  return pid;
}

uint64_t ReadSampleRecordTime(PerfEventRingBuffer* ring_buffer) {
  uint64_t time;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &time,
      sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, time));
  return time;
}

uint64_t ReadSampleRecordStreamId(PerfEventRingBuffer* ring_buffer) {
  uint64_t stream_id;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &stream_id,
      sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, stream_id));
  return stream_id;
}

pid_t ReadSampleRecordPid(PerfEventRingBuffer* ring_buffer) {
  pid_t pid;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &pid, sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, pid));
  return pid;
}

std::unique_ptr<StackSamplePerfEvent> ConsumeStackSamplePerfEvent(PerfEventRingBuffer* ring_buffer,
                                                                  const perf_event_header& header) {
  // Data in the ring buffer has the layout of perf_event_stack_sample, but we
  // copy it into dynamically_sized_perf_event_stack_sample.
  uint64_t dyn_size;
  ring_buffer->ReadValueAtOffset(&dyn_size, offsetof(perf_event_stack_sample, stack.dyn_size));
  auto event = std::make_unique<StackSamplePerfEvent>(dyn_size);
  event->ring_buffer_record->header = header;
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record->sample_id,
                                 offsetof(perf_event_stack_sample, sample_id));
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record->regs,
                                 offsetof(perf_event_stack_sample, regs));
  ring_buffer->ReadRawAtOffset(event->ring_buffer_record->stack.data.get(),
                               offsetof(perf_event_stack_sample, stack.data), dyn_size);
  ring_buffer->SkipRecord(header);
  return event;
}

std::unique_ptr<CallchainSamplePerfEvent> ConsumeCallchainSamplePerfEvent(
    PerfEventRingBuffer* ring_buffer, const perf_event_header& header) {
  uint64_t nr = 0;
  ring_buffer->ReadValueAtOffset(&nr, offsetof(perf_event_callchain_sample_fixed, nr));
  auto event = std::make_unique<CallchainSamplePerfEvent>(nr);
  event->ring_buffer_record.header = header;
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record.sample_id,
                                 offsetof(perf_event_callchain_sample_fixed, sample_id));

  // TODO(kuebler): we should have templated read methods
  uint64_t size_in_bytes = nr * sizeof(uint64_t) / sizeof(char);
  ring_buffer->ReadRawAtOffset(event->ips.data(),
                               offsetof(perf_event_callchain_sample_fixed, nr) +
                                   sizeof(perf_event_callchain_sample_fixed::nr),
                               size_in_bytes);
  ring_buffer->SkipRecord(header);
  return event;
}

std::unique_ptr<GenericTracepointPerfEvent> ConsumeGenericTracepointPerfEvent(
    PerfEventRingBuffer* ring_buffer, const perf_event_header& header) {
  auto event = std::make_unique<GenericTracepointPerfEvent>();
  ring_buffer->ReadRawAtOffset(&event->ring_buffer_record, 0, sizeof(perf_event_raw_sample_fixed));
  ring_buffer->SkipRecord(header);
  return event;
}

}  // namespace orbit_linux_tracing
