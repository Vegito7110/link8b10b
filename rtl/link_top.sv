// link_top.sv - transmit and receive paths of a toy PCIe Gen1/2-style link.
//
//   TX: symbols -> scrambler -> 8b/10b encoder -> 10-bit code words
//   RX: code words -> 8b/10b decoder -> descrambler -> symbols -> packet checker
//
// The two paths are separate ports so the testbench can sit in between as the
// "channel" and corrupt bits. Packet format on the wire:
//   COM  STP  payload...  CRC[0] CRC[1] CRC[2] CRC[3]  END
// The packet checker runs CRC-32 over everything between STP and END (payload
// plus CRC); a correct packet leaves the register at CRC_RESIDUE.
module link_top
  import link_pkg::*;
(
  input  logic        clk,
  input  logic        rst_n,
  // transmit side
  input  logic        tx_valid,
  input  logic [7:0]  tx_data,
  input  logic        tx_k,
  output logic        tx_code_valid,
  output logic [9:0]  tx_code,
  output logic        tx_k_err,
  // receive side
  input  logic        rx_code_valid,
  input  logic [9:0]  rx_code,
  output logic        rx_valid,
  output logic [7:0]  rx_data,
  output logic        rx_k,
  output logic        rx_code_err,
  output logic        rx_disp_err,
  // receive packet checker (one-cycle pulses)
  output logic        pkt_done,     // END seen
  output logic        pkt_crc_ok,   // with pkt_done: CRC matched
  output logic        pkt_abort,    // packet cut short by an unexpected control symbol
  output logic [31:0] pkt_crc       // with pkt_done: CRC of the received STP..END bytes
);
  // ---- TX -------------------------------------------------------------------
  logic       s_valid, s_k;
  logic [7:0] s_data;

  scrambler u_tx_scr (
    .clk, .rst_n,
    .in_valid(tx_valid), .in_data(tx_data), .in_k(tx_k),
    .out_valid(s_valid), .out_data(s_data), .out_k(s_k)
  );

  enc8b10b u_enc (
    .clk, .rst_n,
    .in_valid(s_valid), .in_data(s_data), .in_k(s_k),
    .out_valid(tx_code_valid), .out_code(tx_code), .out_k_err(tx_k_err)
  );

  // ---- RX -------------------------------------------------------------------
  logic       d_valid, d_k, d_code_err, d_disp_err;
  logic [7:0] d_data;

  dec8b10b u_dec (
    .clk, .rst_n,
    .in_valid(rx_code_valid), .in_code(rx_code),
    .out_valid(d_valid), .out_data(d_data), .out_k(d_k),
    .out_code_err(d_code_err), .out_disp_err(d_disp_err)
  );

  scrambler u_rx_scr (
    .clk, .rst_n,
    .in_valid(d_valid), .in_data(d_data), .in_k(d_k),
    .out_valid(rx_valid), .out_data(rx_data), .out_k(rx_k)
  );

  // Delay the decoder's error flags one cycle so they line up with the
  // descrambled symbol they belong to.
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rx_code_err <= 1'b0;
      rx_disp_err <= 1'b0;
    end else begin
      rx_code_err <= d_valid && d_code_err;
      rx_disp_err <= d_valid && d_disp_err;
    end
  end

  // ---- Packet framing and CRC check -----------------------------------------
  logic        in_pkt;
  logic [31:0] crc;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      in_pkt     <= 1'b0;
      crc        <= CRC_INIT;
      pkt_done   <= 1'b0;
      pkt_crc_ok <= 1'b0;
      pkt_abort  <= 1'b0;
      pkt_crc    <= '0;
    end else begin
      pkt_done   <= 1'b0;
      pkt_crc_ok <= 1'b0;
      pkt_abort  <= 1'b0;
      if (rx_valid) begin
        if (rx_k && rx_data == STP) begin
          pkt_abort <= in_pkt;  // STP inside a packet: drop the old one
          in_pkt    <= 1'b1;
          crc       <= CRC_INIT;
        end else if (in_pkt) begin
          if (!rx_k) begin
            crc <= crc32_byte(crc, rx_data);
          end else begin
            in_pkt <= 1'b0;
            if (rx_data == END) begin
              pkt_done   <= 1'b1;
              pkt_crc_ok <= (crc == CRC_RESIDUE);
              pkt_crc    <= ~crc;
            end else begin
              pkt_abort <= 1'b1;
            end
          end
        end
      end
    end
  end
endmodule
