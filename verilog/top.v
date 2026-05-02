module top #(
    parameter CLK_FREQ  = 100_000_000,
    parameter BAUD_RATE = 9600
)(
    input  wire       clk,
    input  wire       rst_raw,
    output wire       uart_tx_pin,
    output wire [7:0] led
);

    wire rst = ~rst_raw;

    wire [7:0] lfsr_data;
    wire [7:0] misr_data;

    reg        lfsr_en;
    reg        misr_en;
    reg        misr_rst;

    reg [7:0]  tx_byte;
    reg        tx_start;
    wire       tx_busy;

    reg [7:0]  cycle_count;

    localparam [2:0] S_IDLE        = 3'd0;
    localparam [2:0] S_SEND_LFSR   = 3'd1;
    localparam [2:0] S_WAIT_LFSR   = 3'd2;
    localparam [2:0] S_SEND_MISR   = 3'd3;
    localparam [2:0] S_WAIT_MISR   = 3'd4;
    localparam [2:0] S_ADVANCE     = 3'd5;
    localparam [2:0] S_SEND_MARKER = 3'd6;
    localparam [2:0] S_WAIT_MARKER = 3'd7;

    reg [2:0] state;

    galois_lfsr_8 u_lfsr (
        .clk      (clk),
        .rst      (rst),
        .en       (lfsr_en),
        .lfsr_out (lfsr_data)
    );

    misr_8 u_misr (
        .clk      (clk),
        .rst      (rst | misr_rst),
        .en       (misr_en),
        .data_in  (lfsr_data),
        .misr_out (misr_data)
    );

    uart_tx #(
        .CLK_FREQ  (CLK_FREQ),
        .BAUD_RATE (BAUD_RATE)
    ) u_uart (
        .clk      (clk),
        .rst      (rst),
        .tx_data  (tx_byte),
        .tx_start (tx_start),
        .tx_busy  (tx_busy),
        .uart_tx  (uart_tx_pin)
    );

    assign led = ~lfsr_data;

    always @(posedge clk) begin
        if (rst) begin
            state       <= S_IDLE;
            cycle_count <= 8'd0;
            tx_byte     <= 8'd0;
            tx_start    <= 1'b0;
            lfsr_en     <= 1'b0;
            misr_en     <= 1'b0;
            misr_rst    <= 1'b0;
        end
        else begin
            tx_start <= 1'b0;
            lfsr_en  <= 1'b0;
            misr_en  <= 1'b0;
            misr_rst <= 1'b0;

            case (state)
                S_IDLE: begin
                    state <= S_SEND_LFSR;
                end

                S_SEND_LFSR: begin
                    if (!tx_busy) begin
                        tx_byte  <= lfsr_data;
                        tx_start <= 1'b1;
                        state    <= S_WAIT_LFSR;
                    end
                end

                S_WAIT_LFSR: begin
                    if (!tx_busy) begin
                        state <= S_SEND_MISR;
                    end
                end

                S_SEND_MISR: begin
                    if (!tx_busy) begin
                        tx_byte  <= misr_data;
                        tx_start <= 1'b1;
                        state    <= S_WAIT_MISR;
                    end
                end

                S_WAIT_MISR: begin
                    if (!tx_busy) begin
                        state <= S_ADVANCE;
                    end
                end

                S_ADVANCE: begin
                    lfsr_en <= 1'b1;
                    misr_en <= 1'b1;

                    if (cycle_count == 8'd254) begin
                        cycle_count <= 8'd0;
                        state       <= S_SEND_MARKER;
                    end
                    else begin
                        cycle_count <= cycle_count + 8'd1;
                        state       <= S_SEND_LFSR;
                    end
                end

                S_SEND_MARKER: begin
                    if (!tx_busy) begin
                        tx_byte  <= 8'hAA;
                        tx_start <= 1'b1;
                        misr_rst <= 1'b1;
                        state    <= S_WAIT_MARKER;
                    end
                end

                S_WAIT_MARKER: begin
                    if (!tx_busy) begin
                        state <= S_SEND_LFSR;
                    end
                end

                default: begin
                    state <= S_IDLE;
                end
            endcase
        end
    end

endmodule
