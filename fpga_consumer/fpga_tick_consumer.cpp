// fpga_tick_consumer.cpp
//
// Host-side C++ consumer for the Spartan 6 LFSR tick stream.
// Architecture:
//   [FPGA UART TX @ 9600 baud] -> serial port -> producer thread
//   -> Fifo5<TickPacket> lock-free ring buffer -> consumer thread
//
// Producer thread: reads byte pairs (LFSR, MISR) from serial port,
//                  constructs TickPacket, pushes into Fifo5
// Consumer thread: pops TickPackets, maps to synthetic price ticks,
//                  verifies MISR golden signature every 255 cycles,
//                  benchmarks throughput and p99 latency
//
// Build (macOS):
//   clang++ -std=c++17 -O2 -pthread -o fpga_consumer fpga_tick_consumer.cpp
//
// Build (Linux):
//   g++ -std=c++17 -O2 -pthread -o fpga_consumer fpga_tick_consumer.cpp
//
// Usage:
//   ./fpga_consumer /dev/cu.usbserial-XXXX      (macOS)
//   ./fpga_consumer /dev/ttyUSB0                 (Linux)
//   ./fpga_consumer COM4                         (Windows/MSYS2)

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <vector>
#include <string>

// POSIX serial port headers
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

// Your lock-free SPSC ring buffer
#include "../SPSC_lockfree_ring_buffer/fifo5.h"

// Tick packet -- what flows through the ring buffer
struct TickPacket {
    uint8_t  lfsr_byte;       // Raw LFSR output from FPGA
    uint8_t  misr_byte;       // Running MISR signature from FPGA
    uint16_t cycle_in_period; // Which cycle within current 255-cycle period
    uint64_t timestamp_ns;    // Host-side nanosecond timestamp at reception
};

// Serial port configuration
static constexpr speed_t BAUD = B9600;           // Must match FPGA
static constexpr uint8_t END_OF_PERIOD = 0xAA;   // Marker byte from FPGA
static constexpr uint16_t PERIOD_LENGTH = 255;    // LFSR maximal-length cycle

// Ring buffer sizing -- power of 2 as required by Fifo5
static constexpr std::size_t QUEUE_CAPACITY = 4096;

// Shared state
std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_total_ticks_produced{0};
std::atomic<uint64_t> g_total_ticks_consumed{0};
std::atomic<uint64_t> g_signature_checks{0};
std::atomic<uint64_t> g_signature_failures{0};
std::atomic<uint64_t> g_queue_full_spins{0};

// Utility: get current time in nanoseconds
inline uint64_t now_ns()
{
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}

// Open and configure serial port
int open_serial(const char* port_path)
{
    int fd = open(port_path, O_RDONLY | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        std::cerr << "[ERROR] Cannot open " << port_path
                  << ": " << strerror(errno) << "\n";
        return -1;
    }

    // Set blocking reads
    fcntl(fd, F_SETFL, 0);

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(fd, &tty) != 0) {
        std::cerr << "[ERROR] tcgetattr failed: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    // 8N1, no flow control, raw mode
    cfsetispeed(&tty, BAUD);
    cfsetospeed(&tty, BAUD);

    tty.c_cflag &= ~PARENB;         // No parity
    tty.c_cflag &= ~CSTOPB;         // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;             // 8 data bits
    tty.c_cflag &= ~CRTSCTS;        // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL;  // Enable receiver, ignore modem status

    // Raw input — no canonical processing, no echo, no signals
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // No software flow control
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                      INLCR | IGNCR | ICRNL);

    // Raw output
    tty.c_oflag &= ~OPOST;

    // Read returns after 1 byte, no timeout
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        std::cerr << "[ERROR] tcsetattr failed: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    // Flush any stale data in the serial buffer
    tcflush(fd, TCIOFLUSH);

    std::cout << "[SERIAL] Opened " << port_path << " @ 9600 8N1\n";
    return fd;
}

// MISR golden signature computation (software model)
//
// Runs the same Galois LFSR + MISR algorithm as the FPGA to compute
// the expected MISR signature after 255 LFSR cycles.
// This is called once at startup to get the golden value.
uint8_t compute_golden_signature()
{
    // Software model of LFSR: polynomial x^8 + x^6 + x^5 + x^4 + 1
    auto lfsr_step = [](uint8_t state) -> uint8_t {
        uint8_t feedback = state & 1; // bit 0
        uint8_t next = state >> 1;    // right shift
        if (feedback) {
            next ^= 0xE0; // XOR taps: bits 7,6,5 after shift = original 6,5,4
            // bit 7 ← feedback (from the x^8 term)
            // bit 6 ← old bit 7 XOR feedback
            // bit 5 ← old bit 6 XOR feedback
            // After right shift: bit 6 = old bit 7, we XOR feedback into bits 7,6,5
            // Actually let me re-derive:
            // After shift: next[7]=0, next[6]=state[7], next[5]=state[6], next[4]=state[5]...
            // Then XOR feedback into positions 7,6,5,4:
        }
        // Let me do it bit-by-bit to match the FPGA exactly:
        next = 0;
        next |= ((state >> 2) & 1) << 0;           // bit 0 ← old bit 1
        next |= ((state >> 3) & 1) << 1;           // bit 1 ← old bit 2
        next |= ((state >> 4) & 1) << 2;           // bit 2 ← old bit 3
        next |= ((state >> 5) & 1) << 3;           // bit 3 ← old bit 4
        next |= (((state >> 6) & 1) ^ feedback) << 4; // bit 4 ← old bit 5 XOR fb
        next |= (((state >> 7) & 1) ^ feedback) << 5; // bit 5 ← old bit 6 XOR fb
        // Wait, I need to match the Verilog exactly:
        // lfsr_out[7] <= feedback
        // lfsr_out[6] <= lfsr_out[7] ^ feedback
        // lfsr_out[5] <= lfsr_out[6] ^ feedback
        // lfsr_out[4] <= lfsr_out[5] ^ feedback
        // lfsr_out[3] <= lfsr_out[4]
        // lfsr_out[2] <= lfsr_out[3]
        // lfsr_out[1] <= lfsr_out[2]
        // lfsr_out[0] <= lfsr_out[1]
        next = 0;
        next |= (feedback) << 7;                        // bit 7 = feedback
        next |= (((state >> 7) & 1) ^ feedback) << 6;   // bit 6 = old[7] ^ fb
        next |= (((state >> 6) & 1) ^ feedback) << 5;   // bit 5 = old[6] ^ fb
        next |= (((state >> 5) & 1) ^ feedback) << 4;   // bit 4 = old[5] ^ fb
        next |= ((state >> 4) & 1) << 3;                // bit 3 = old[4]
        next |= ((state >> 3) & 1) << 2;                // bit 2 = old[3]
        next |= ((state >> 2) & 1) << 1;                // bit 1 = old[2]
        next |= ((state >> 1) & 1) << 0;                // bit 0 = old[1]
        return next;
    };

    // Software model of MISR: same polynomial, XOR data_in
    auto misr_step = [](uint8_t state, uint8_t data_in) -> uint8_t {
        uint8_t feedback = state & 1;
        uint8_t next = 0;
        next |= (feedback               ^ ((data_in >> 7) & 1)) << 7;
        next |= (((state >> 7) & 1) ^ feedback ^ ((data_in >> 6) & 1)) << 6;
        next |= (((state >> 6) & 1) ^ feedback ^ ((data_in >> 5) & 1)) << 5;
        next |= (((state >> 5) & 1) ^ feedback ^ ((data_in >> 4) & 1)) << 4;
        next |= (((state >> 4) & 1)             ^ ((data_in >> 3) & 1)) << 3;
        next |= (((state >> 3) & 1)             ^ ((data_in >> 2) & 1)) << 2;
        next |= (((state >> 2) & 1)             ^ ((data_in >> 1) & 1)) << 1;
        next |= (((state >> 1) & 1)             ^ ((data_in >> 0) & 1)) << 0;
        return next;
    };

    // Simulate 255 LFSR cycles with MISR accumulation
    uint8_t lfsr = 0xFF; // seed
    uint8_t misr = 0x00; // reset

    // The FPGA sends lfsr_data BEFORE advancing, then advances.
    // First packet: lfsr=0xFF (seed), misr gets 0xFF folded in, then LFSR steps.
    for (int i = 0; i < PERIOD_LENGTH; i++) {
        misr = misr_step(misr, lfsr);
        lfsr = lfsr_step(lfsr);
    }

    return misr;
}

// Producer thread: reads serial -> pushes into Fifo5
void producer_thread(int serial_fd, Fifo5<TickPacket>& queue)
{
    uint16_t cycle = 0;
    uint8_t buf[2];

    std::cout << "[PRODUCER] Started — reading from FPGA serial stream\n";

    while (g_running.load(std::memory_order_relaxed)) {
        // Read 1 byte — the LFSR byte
        ssize_t n = read(serial_fd, &buf[0], 1);
        if (n <= 0) {
            if (n == 0) continue;
            if (errno == EAGAIN || errno == EINTR) continue;
            std::cerr << "[PRODUCER] Read error: " << strerror(errno) << "\n";
            break;
        }

        // Check for end-of-period marker
        // The marker 0xAA appears as a single byte after 255 LFSR+MISR pairs
        // We detect it by checking if we've reached cycle 255
        // But the marker can also be a valid LFSR value, so we use the
        // cycle counter for framing instead of byte value detection
        if (cycle >= PERIOD_LENGTH) {
            // This byte should be the 0xAA marker
            if (buf[0] != END_OF_PERIOD) {
                std::cerr << "[PRODUCER] WARNING: Expected 0xAA marker at cycle "
                          << cycle << ", got 0x" << std::hex
                          << (int)buf[0] << std::dec << "\n";
            }
            cycle = 0;
            continue;
        }

        // Read the MISR byte (second byte of pair)
        n = read(serial_fd, &buf[1], 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            std::cerr << "[PRODUCER] Read error on MISR byte: "
                      << strerror(errno) << "\n";
            break;
        }

        uint64_t ts = now_ns();

        // Push into lock-free ring buffer using Fifo5 zero-copy API
        while (g_running.load(std::memory_order_relaxed)) {
            auto pusher = queue.push();
            if (pusher) {
                // Construct TickPacket directly in the queue slot (zero-copy)
                new (pusher.get()) TickPacket{
                    buf[0],           // lfsr_byte
                    buf[1],           // misr_byte
                    cycle,            // cycle_in_period
                    ts                // timestamp_ns
                };
                break;
                // pusher destructor fires → atomically publishes cursor
            }
            // Queue full — spin (this should be rare since UART is slow)
            g_queue_full_spins.fetch_add(1, std::memory_order_relaxed);
        }

        cycle++;
        g_total_ticks_produced.fetch_add(1, std::memory_order_relaxed);
    }

    std::cout << "[PRODUCER] Stopped\n";
}

// Consumer thread: pops from Fifo5 -> processes tick data
void consumer_thread(Fifo5<TickPacket>& queue, uint8_t golden_signature)
{
    std::cout << "[CONSUMER] Started — golden MISR signature: 0x"
              << std::hex << (int)golden_signature << std::dec << "\n";

    // Simulated order book state
    double price = 100.0;
    uint64_t total_volume = 0;

    // Latency tracking for p99
    std::vector<uint64_t> latencies;
    latencies.reserve(100000);

    while (g_running.load(std::memory_order_relaxed)) {
        auto popper = queue.pop();
        if (!popper) {
            // Queue empty — yield briefly since UART is slow
            std::this_thread::yield();
            continue;
        }

        TickPacket pkt = *popper.get();
        popper.get()->~TickPacket();
        // popper destructor fires → atomically frees slot

        // Measure end-to-end latency (reception timestamp → now)
        uint64_t consume_time = now_ns();
        uint64_t latency = consume_time - pkt.timestamp_ns;
        if (latencies.size() < 10000000) {
            latencies.push_back(latency);
        }

        // Synthetic order book simulation:
        // Map LFSR byte to price tick: direction from bit 7, magnitude from bits 0-6
        int direction = (pkt.lfsr_byte & 0x80) ? 1 : -1;
        double magnitude = (pkt.lfsr_byte & 0x7F) * 0.01;
        price += direction * magnitude;
        total_volume++;

        // MISR signature verification:
        // At the last cycle of each period (cycle 254), the MISR should
        // match the golden signature
        if (pkt.cycle_in_period == PERIOD_LENGTH - 1) {
            g_signature_checks.fetch_add(1, std::memory_order_relaxed);
            if (pkt.misr_byte != golden_signature) {
                g_signature_failures.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[CONSUMER] ⚠ MISR MISMATCH at period end! "
                          << "Expected: 0x" << std::hex << (int)golden_signature
                          << " Got: 0x" << (int)pkt.misr_byte << std::dec << "\n";
            }
        }

        g_total_ticks_consumed.fetch_add(1, std::memory_order_relaxed);
    }

    // Final statistics
    std::cout << "\nFPGA Tick Consumer Results\n\n";

    std::cout << "  Total ticks consumed:  " << g_total_ticks_consumed.load() << "\n";
    std::cout << "  Final simulated price: " << std::fixed << std::setprecision(2)
              << price << "\n";
    std::cout << "  Total volume:          " << total_volume << "\n\n";

    // Signature verification summary
    uint64_t checks = g_signature_checks.load();
    uint64_t failures = g_signature_failures.load();
    std::cout << "  MISR checks:           " << checks << "\n";
    std::cout << "  MISR failures:         " << failures << "\n";
    std::cout << "  MISR status:           "
              << (failures == 0 ? "✓ ALL PASS" : "✗ HARDWARE FAULT DETECTED")
              << "\n\n";

    // Latency stats
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        size_t count = latencies.size();
        uint64_t median = latencies[count / 2];
        uint64_t p99    = latencies[(size_t)(count * 0.99)];
        uint64_t p999   = latencies[(size_t)(count * 0.999)];
        uint64_t max_l  = latencies.back();

        std::cout << "  Latency (queue transit):\n";
        std::cout << "    Median:  " << median << " ns\n";
        std::cout << "    p99:     " << p99 << " ns\n";
        std::cout << "    p99.9:   " << p999 << " ns\n";
        std::cout << "    Max:     " << max_l << " ns\n\n";
    }

    std::cout << "  Queue full spins:      " << g_queue_full_spins.load() << "\n";
}

// Main
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <serial_port>\n";
        std::cerr << "  macOS:  ./fpga_consumer /dev/cu.usbserial-XXXX\n";
        std::cerr << "  Linux:  ./fpga_consumer /dev/ttyUSB0\n";
        return 1;
    }

    std::cout << "FPGA LFSR Tick Stream Consumer\n";
    std::cout << "Fifo5 Lock-Free Ring Buffer\n\n";

    // Compute golden MISR signature
    uint8_t golden = compute_golden_signature();
    std::cout << "[INIT] Golden MISR signature: 0x"
              << std::hex << (int)golden << std::dec << "\n";

    // Open serial port
    int serial_fd = open_serial(argv[1]);
    if (serial_fd < 0) return 1;

    // Create the lock-free ring buffer (power of 2 capacity)
    Fifo5<TickPacket> queue(QUEUE_CAPACITY);
    std::cout << "[INIT] Fifo5<TickPacket> capacity: " << QUEUE_CAPACITY
              << " slots (" << QUEUE_CAPACITY * sizeof(TickPacket) << " bytes)\n\n";

    // Launch producer and consumer threads
    std::thread prod(producer_thread, serial_fd, std::ref(queue));
    std::thread cons(consumer_thread, std::ref(queue), golden);

    // Run until user presses Enter
    std::cout << "[MAIN] Streaming... Press ENTER to stop.\n\n";
    std::cin.get();
    g_running.store(false, std::memory_order_relaxed);

    // Cleanup
    prod.join();
    cons.join();
    close(serial_fd);

    return 0;
}
