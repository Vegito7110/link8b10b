// dec8b10b.sv - 8b/10b decoder with error detection, 1-cycle latency.
//
// Decode-then-re-encode: the 6b and 4b sub-blocks are mapped back to bits
// without looking at the running disparity, which gives the only possible
// candidate symbol. Re-encoding that candidate under the current RD (and under
// the opposite RD) then classifies the input exactly:
//   matches with current RD  -> valid
//   matches with opposite RD -> disparity error (legal word, wrong polarity)
//   matches neither          -> code violation (output D0.0)
// RD is updated from the received bits either way, so the decoder resyncs on
// the next unbalanced symbol after an error.
module dec8b10b
  import link_pkg::*;
(
  input  logic       clk,
  input  logic       rst_n,
  input  logic       in_valid,
  input  logic [9:0] in_code,
  output logic       out_valid,
  output logic [7:0] out_data,
  output logic       out_k,
  output logic       out_code_err,
  output logic       out_disp_err
);
  logic        rd;
  logic [5:0]  c6;
  logic [3:0]  c4;
  logic [6:0]  d6;      // {valid, is_k28, x}
  logic [3:0]  d4;      // {valid, y}
  logic        is_k, tables_ok, ok_same, ok_other, rd_next;
  logic [7:0]  cand;
  logic [9:0]  enc_same, enc_other;  // re-encoded candidate (RD bit dropped)

  always_comb begin
    c6   = in_code[9:4];
    c4   = in_code[3:0];
    d6   = dec6(c6);
    d4   = dec4(c4, d6[5], c6 == 6'b001111);
    cand = {d4[2:0], d6[4:0]};
    // Control symbols: any K28.y, or K23/27/29/30 followed by the A7-shaped
    // 4b block (data x=23/27/29/30 always uses P7, so this is unambiguous).
    is_k = d6[5] ||
           (d4[2:0] == 3'd7 && (c4 == 4'b0111 || c4 == 4'b1000) &&
            (d6[4:0] == 5'd23 || d6[4:0] == 5'd27 || d6[4:0] == 5'd29 || d6[4:0] == 5'd30));
    tables_ok = d6[6] && d4[3];
    enc_same  = 10'(encode(cand, is_k, rd));
    enc_other = 10'(encode(cand, is_k, !rd));
    ok_same   = tables_ok && enc_same  == in_code;
    ok_other  = tables_ok && enc_other == in_code;
    rd_next   = rd_after4(c4, rd_after6(c6, rd));
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rd           <= 1'b0;
      out_valid    <= 1'b0;
      out_data     <= '0;
      out_k        <= 1'b0;
      out_code_err <= 1'b0;
      out_disp_err <= 1'b0;
    end else begin
      out_valid <= in_valid;
      if (in_valid) begin
        rd           <= rd_next;
        out_code_err <= !ok_same && !ok_other;
        out_disp_err <= !ok_same && ok_other;
        out_data     <= (ok_same || ok_other) ? cand : 8'h00;
        out_k        <= (ok_same || ok_other) && is_k;
      end
    end
  end
endmodule
