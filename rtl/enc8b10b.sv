// enc8b10b.sv - 8b/10b encoder, one symbol per clock, 1-cycle latency.
// Requests for an illegal control symbol are sent as data and flagged.
module enc8b10b
  import link_pkg::*;
(
  input  logic       clk,
  input  logic       rst_n,
  input  logic       in_valid,
  input  logic [7:0] in_data,
  input  logic       in_k,
  output logic       out_valid,
  output logic [9:0] out_code,
  output logic       out_k_err
);
  logic        rd;  // running disparity, starts RD-
  logic        k_ok;
  logic [10:0] enc;

  always_comb begin
    k_ok = k_valid(in_data);
    enc  = encode(in_data, in_k && k_ok, rd);
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rd        <= 1'b0;
      out_valid <= 1'b0;
      out_code  <= '0;
      out_k_err <= 1'b0;
    end else begin
      out_valid <= in_valid;
      if (in_valid) begin
        out_code  <= enc[9:0];
        rd        <= enc[10];
        out_k_err <= in_k && !k_ok;
      end
    end
  end
endmodule
