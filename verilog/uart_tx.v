module uart_tx #(
    parameter CLK_FREQ  = 100_000_000,
    parameter BAUD_RATE = 9600
)(
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] tx_data,
    input  wire       tx_start,
    output reg        tx_busy,
    output reg        uart_tx
);

    localparam integer BAUD_DIV = CLK_FREQ / BAUD_RATE;

    reg [31:0] baud_cnt;
    reg [3:0]  bit_idx;
    reg [9:0]  shift_reg;

    localparam S_IDLE = 1'b0;
    localparam S_SEND = 1'b1;
    reg state;

    always @(posedge clk) begin
        if (rst) begin
            state    <= S_IDLE;
            uart_tx  <= 1'b1;
            tx_busy  <= 1'b0;
            baud_cnt <= 32'd0;
            bit_idx  <= 4'd0;
            shift_reg<= 10'h3FF;
        end
        else begin
            case (state)
                S_IDLE: begin
                    uart_tx <= 1'b1;
                    tx_busy <= 1'b0;
                    if (tx_start) begin
                        shift_reg <= {1'b1, tx_data, 1'b0};
                        bit_idx   <= 4'd0;
                        baud_cnt  <= 32'd0;
                        tx_busy   <= 1'b1;
                        state     <= S_SEND;
                        uart_tx   <= 1'b0;
                    end
                end

                S_SEND: begin
                    if (baud_cnt < (BAUD_DIV - 1)) begin
                        baud_cnt <= baud_cnt + 32'd1;
                    end
                    else begin
                        baud_cnt <= 32'd0;
                        bit_idx  <= bit_idx + 4'd1;
                        if (bit_idx == 4'd9) begin
                            state   <= S_IDLE;
                            uart_tx <= 1'b1;
                            tx_busy <= 1'b0;
                        end
                        else begin
                            shift_reg <= {1'b1, shift_reg[9:1]};
                            uart_tx   <= shift_reg[1];
                        end
                    end
                end

                default: begin
                    state   <= S_IDLE;
                    uart_tx <= 1'b1;
                    tx_busy <= 1'b0;
                end
            endcase
        end
    end

endmodule
