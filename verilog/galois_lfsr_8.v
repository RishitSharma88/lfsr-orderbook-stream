module galois_lfsr_8 (
    input  wire       clk,
    input  wire       rst,
    input  wire       en,
    output reg  [7:0] lfsr_out
);

    localparam [7:0] SEED = 8'hFF;

    wire feedback = lfsr_out[0];

    always @(posedge clk) begin
        if (rst) begin
            lfsr_out <= SEED;
        end
        else if (en) begin
            lfsr_out[7] <= feedback;
            lfsr_out[6] <= lfsr_out[7] ^ feedback;
            lfsr_out[5] <= lfsr_out[6] ^ feedback;
            lfsr_out[4] <= lfsr_out[5] ^ feedback;
            lfsr_out[3] <= lfsr_out[4];
            lfsr_out[2] <= lfsr_out[3];
            lfsr_out[1] <= lfsr_out[2];
            lfsr_out[0] <= lfsr_out[1];
        end
    end

endmodule
