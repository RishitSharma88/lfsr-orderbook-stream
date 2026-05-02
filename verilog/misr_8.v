module misr_8 (
    input  wire       clk,
    input  wire       rst,
    input  wire       en,
    input  wire [7:0] data_in,
    output reg  [7:0] misr_out
);

    localparam [7:0] RESET_VAL = 8'h00;

    wire feedback = misr_out[0];

    always @(posedge clk) begin
        if (rst) begin
            misr_out <= RESET_VAL;
        end
        else if (en) begin
            misr_out[7] <= feedback                ^ data_in[7];
            misr_out[6] <= (misr_out[7] ^ feedback) ^ data_in[6];
            misr_out[5] <= (misr_out[6] ^ feedback) ^ data_in[5];
            misr_out[4] <= (misr_out[5] ^ feedback) ^ data_in[4];
            misr_out[3] <= misr_out[4]              ^ data_in[3];
            misr_out[2] <= misr_out[3]              ^ data_in[2];
            misr_out[1] <= misr_out[2]              ^ data_in[1];
            misr_out[0] <= misr_out[1]              ^ data_in[0];
        end
    end

endmodule
