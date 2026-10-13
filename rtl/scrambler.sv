// scrambler.sv - PCIe Gen1/2-style data scrambler, 1-cycle latency.
//
// 16-bit LFSR, G(x) = x^16 + x^5 + x^4 + x^3 + 1, seeded with 0xFFFF.
// Each data byte is XORed with 8 LFSR output bits (bit 0 first) and the LFSR
// advances 8 steps. Control symbols are not scrambled but still advance the
// LFSR, except SKP (no advance) and COM (re-seeds to 0xFFFF). XOR is its own
// inverse, so the same module descrambles on the receive side.
//
// Why scramble: long runs of the same byte (e.g. idle zeros) would put all
// the signal energy at a few frequencies (EMI); scrambling spreads it out.
module scrambler
  import link_pkg::*;
(
  input  logic       clk,
  input  logic       rst_n,
  input  logic       in_valid,
  input  logic [7:0] in_data,
  input  logic       in_k,
  output logic       out_valid,
  output logic [7:0] out_data,
  output logic       out_k
);
  logic [15:0] lfsr, lfsr_next;
  logic [7:0]  data_next;

  always_comb begin
    lfsr_next = lfsr;
    data_next = in_data;
    if (in_k && in_data == COM) begin
      lfsr_next = 16'hFFFF;
    end else if (!(in_k && in_data == SKP)) begin
      // Eight serial LFSR steps, unrolled into one clock cycle.
      for (int i = 0; i < 8; i++) begin
        if (!in_k) data_next[i] = in_data[i] ^ lfsr_next[15];
        lfsr_next = {lfsr_next[14:0], 1'b0} ^ (lfsr_next[15] ? 16'h0039 : 16'h0000);
      end
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      lfsr      <= 16'hFFFF;
      out_valid <= 1'b0;
      out_data  <= '0;
      out_k     <= 1'b0;
    end else begin
      out_valid <= in_valid;
      if (in_valid) begin
        lfsr     <= lfsr_next;
        out_data <= data_next;
        out_k    <= in_k;
      end
    end
  end
endmodule
