// ============================================================================
// orderbook_builder.cpp
//
// This section is being implemented.
//
// This file will contain the order book consumer thread and main entry point.
// Responsibilities:
//   - Pop TickPackets from the Fifo5 SPSC ring buffer
//     (the producer side lives in fpga_consumer/fpga_tick_consumer.cpp)
//   - Interpret LFSR-derived tick data into order book events
//     (price level, side, quantity, etc.)
//   - Feed events into the OrderBook (defined in orderbook.h)
//   - MISR signature verification at end of each 255-cycle period
//   - Latency measurement and throughput reporting
//
// The ring buffer is #include'd from:
//   ../SPSC_lockfree_ring_buffer/fifo5.h
//
// Build:
//   clang++ -std=c++17 -O2 -pthread -o orderbook_builder orderbook_builder.cpp
// ============================================================================
