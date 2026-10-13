// link_pkg.sv - constants and pure functions shared by the link blocks:
// 8b/10b code tables, running-disparity rules, CRC-32 byte update.
//
// Code word layout: code[9:0] = {a,b,c,d,e,i, f,g,h,j}; bit 9 ('a') is sent first.
// Running disparity rd: 0 = RD-, 1 = RD+.
package link_pkg;

  // Special symbols, byte = {y, x} for K.x.y
  localparam logic [7:0] COM = 8'hBC;  // K28.5 comma: alignment, scrambler reset
  localparam logic [7:0] SKP = 8'h1C;  // K28.0 skip: scrambler holds
  localparam logic [7:0] STP = 8'hFB;  // K27.7 start of packet
  localparam logic [7:0] END = 8'hFD;  // K29.7 end of packet

  localparam logic [31:0] CRC_INIT    = 32'hFFFF_FFFF;
  localparam logic [31:0] CRC_RESIDUE = 32'hDEBB_20E3;  // register after data + its CRC

  // ---------------------------------------------------------------------------
  // Running disparity after a sub-block. Unbalanced blocks set RD to their sign;
  // balanced blocks keep it, except 000111/111000 and 0011/1100, which are only
  // ever sent in one RD and so define it.
  function automatic logic rd_after6(input logic [5:0] c, input logic rd);
    if ($countones(c) > 3) return 1'b1;
    if ($countones(c) < 3) return 1'b0;
    if (c == 6'b000111) return 1'b1;
    if (c == 6'b111000) return 1'b0;
    return rd;
  endfunction

  function automatic logic rd_after4(input logic [3:0] c, input logic rd);
    if ($countones(c) > 2) return 1'b1;
    if ($countones(c) < 2) return 1'b0;
    if (c == 4'b0011) return 1'b1;
    if (c == 4'b1100) return 1'b0;
    return rd;
  endfunction

  // The 12 legal control symbols: K28.0-K28.7, K23.7, K27.7, K29.7, K30.7.
  function automatic logic k_valid(input logic [7:0] d);
    return d[4:0] == 5'd28 ||
           (d[7:5] == 3'd7 && (d[4:0] == 5'd23 || d[4:0] == 5'd27 || d[4:0] == 5'd29 || d[4:0] == 5'd30));
  endfunction

  // ---------------------------------------------------------------------------
  // Encoder tables. Only the RD- column is stored; the RD+ code is the bitwise
  // complement whenever the RD- code is unbalanced (or is one of the
  // RD-selected balanced codes). The C++ model uses the full two-column tables
  // instead, so the two implementations are independent.

  // 5b/6b data codes, RD- column.
  function automatic logic [5:0] enc6_rdm(input logic [4:0] x);
    case (x)
      5'd0 : return 6'b100111;
      5'd1 : return 6'b011101;
      5'd2 : return 6'b101101;
      5'd3 : return 6'b110001;
      5'd4 : return 6'b110101;
      5'd5 : return 6'b101001;
      5'd6 : return 6'b011001;
      5'd7 : return 6'b111000;
      5'd8 : return 6'b111001;
      5'd9 : return 6'b100101;
      5'd10: return 6'b010101;
      5'd11: return 6'b110100;
      5'd12: return 6'b001101;
      5'd13: return 6'b101100;
      5'd14: return 6'b011100;
      5'd15: return 6'b010111;
      5'd16: return 6'b011011;
      5'd17: return 6'b100011;
      5'd18: return 6'b010011;
      5'd19: return 6'b110010;
      5'd20: return 6'b001011;
      5'd21: return 6'b101010;
      5'd22: return 6'b011010;
      5'd23: return 6'b111010;
      5'd24: return 6'b110011;
      5'd25: return 6'b100110;
      5'd26: return 6'b010110;
      5'd27: return 6'b110110;
      5'd28: return 6'b001110;
      5'd29: return 6'b101110;
      5'd30: return 6'b011110;
      5'd31: return 6'b101011;
      default: return 6'b000000;
    endcase
  endfunction

  function automatic logic [5:0] enc6(input logic [4:0] x, input logic k28, input logic rd);
    logic [5:0] c;
    c = k28 ? 6'b001111 : enc6_rdm(x);
    if (rd && ($countones(c) != 3 || c == 6'b111000)) c = ~c;
    return c;
  endfunction

  // 3b/4b data codes, RD- column. alt7 selects A7 (0111) instead of P7 (1110).
  function automatic logic [3:0] enc4d_rdm(input logic [2:0] y, input logic alt7);
    case (y)
      3'd0: return 4'b1011;
      3'd1: return 4'b1001;
      3'd2: return 4'b0101;
      3'd3: return 4'b1100;
      3'd4: return 4'b1101;
      3'd5: return 4'b1010;
      3'd6: return 4'b0110;
      default: return alt7 ? 4'b0111 : 4'b1110;
    endcase
  endfunction

  // 3b/4b control codes, RD- column (RD+ is always the complement).
  function automatic logic [3:0] enc4k_rdm(input logic [2:0] y);
    case (y)
      3'd0: return 4'b1011;
      3'd1: return 4'b0110;
      3'd2: return 4'b1010;
      3'd3: return 4'b1100;
      3'd4: return 4'b1101;
      3'd5: return 4'b0101;
      3'd6: return 4'b1001;
      default: return 4'b0111;
    endcase
  endfunction

  function automatic logic [3:0] enc4(input logic [2:0] y, input logic k, input logic alt7, input logic rd);
    logic [3:0] c;
    c = k ? enc4k_rdm(y) : enc4d_rdm(y, alt7);
    if (rd && (k || $countones(c) != 2 || c == 4'b1100)) c = ~c;
    return c;
  endfunction

  // Full 8b/10b encode. Returns {rd_out, code[9:0]}. k must be a legal K symbol.
  function automatic logic [10:0] encode(input logic [7:0] d, input logic k, input logic rd);
    logic [4:0] x;
    logic [2:0] y;
    logic [5:0] c6;
    logic [3:0] c4;
    logic rd1, alt7;
    x = d[4:0];
    y = d[7:5];
    c6 = enc6(x, k && x == 5'd28, rd);
    rd1 = rd_after6(c6, rd);
    // A7 avoids five equal bits in a row across the e,i / f,g,h boundary.
    alt7 = (!rd1 && (x == 5'd17 || x == 5'd18 || x == 5'd20)) ||
           ( rd1 && (x == 5'd11 || x == 5'd13 || x == 5'd14));
    c4 = enc4(y, k, alt7, rd1);
    return {rd_after4(c4, rd1), c6, c4};
  endfunction

  // ---------------------------------------------------------------------------
  // Decoder tables: map sub-blocks back to bits, ignoring RD. The decoder then
  // re-encodes the candidate to check it was legal under the current RD.

  // 6b -> {valid, is_k28, x}
  function automatic logic [6:0] dec6(input logic [5:0] c);
    case (c)
      6'b100111, 6'b011000: return {1'b1, 1'b0, 5'd0};
      6'b011101, 6'b100010: return {1'b1, 1'b0, 5'd1};
      6'b101101, 6'b010010: return {1'b1, 1'b0, 5'd2};
      6'b110001: return {1'b1, 1'b0, 5'd3};
      6'b110101, 6'b001010: return {1'b1, 1'b0, 5'd4};
      6'b101001: return {1'b1, 1'b0, 5'd5};
      6'b011001: return {1'b1, 1'b0, 5'd6};
      6'b111000, 6'b000111: return {1'b1, 1'b0, 5'd7};
      6'b111001, 6'b000110: return {1'b1, 1'b0, 5'd8};
      6'b100101: return {1'b1, 1'b0, 5'd9};
      6'b010101: return {1'b1, 1'b0, 5'd10};
      6'b110100: return {1'b1, 1'b0, 5'd11};
      6'b001101: return {1'b1, 1'b0, 5'd12};
      6'b101100: return {1'b1, 1'b0, 5'd13};
      6'b011100: return {1'b1, 1'b0, 5'd14};
      6'b010111, 6'b101000: return {1'b1, 1'b0, 5'd15};
      6'b011011, 6'b100100: return {1'b1, 1'b0, 5'd16};
      6'b100011: return {1'b1, 1'b0, 5'd17};
      6'b010011: return {1'b1, 1'b0, 5'd18};
      6'b110010: return {1'b1, 1'b0, 5'd19};
      6'b001011: return {1'b1, 1'b0, 5'd20};
      6'b101010: return {1'b1, 1'b0, 5'd21};
      6'b011010: return {1'b1, 1'b0, 5'd22};
      6'b111010, 6'b000101: return {1'b1, 1'b0, 5'd23};
      6'b110011, 6'b001100: return {1'b1, 1'b0, 5'd24};
      6'b100110: return {1'b1, 1'b0, 5'd25};
      6'b010110: return {1'b1, 1'b0, 5'd26};
      6'b110110, 6'b001001: return {1'b1, 1'b0, 5'd27};
      6'b001110: return {1'b1, 1'b0, 5'd28};
      6'b101110, 6'b010001: return {1'b1, 1'b0, 5'd29};
      6'b011110, 6'b100001: return {1'b1, 1'b0, 5'd30};
      6'b101011, 6'b010100: return {1'b1, 1'b0, 5'd31};
      6'b001111, 6'b110000: return {1'b1, 1'b1, 5'd28};  // K28
      default: return 7'b0;
    endcase
  endfunction

  // 4b -> {valid, y}. For K28 the two RD columns overlap (0101 is K28.2 in
  // RD+ but K28.5 in RD-), so the column is chosen by the 6b block's polarity:
  // 001111 leaves RD+, 110000 leaves RD-.
  function automatic logic [3:0] dec4(input logic [3:0] c, input logic k28, input logic k28_rd_plus);
    if (k28) begin
      for (int y = 0; y < 8; y++)
        if (c == (k28_rd_plus ? ~enc4k_rdm(3'(y)) : enc4k_rdm(3'(y)))) return {1'b1, 3'(y)};
      return 4'b0;
    end
    case (c)
      4'b1011, 4'b0100:                     return {1'b1, 3'd0};
      4'b1001:                              return {1'b1, 3'd1};
      4'b0101:                              return {1'b1, 3'd2};
      4'b1100, 4'b0011:                     return {1'b1, 3'd3};
      4'b1101, 4'b0010:                     return {1'b1, 3'd4};
      4'b1010:                              return {1'b1, 3'd5};
      4'b0110:                              return {1'b1, 3'd6};
      4'b1110, 4'b0001, 4'b0111, 4'b1000:   return {1'b1, 3'd7};
      default:                              return 4'b0;  // 0000, 1111
    endcase
  endfunction

  // ---------------------------------------------------------------------------
  // CRC-32 (polynomial 0x04C11DB7, reflected form 0xEDB88320), one byte per call.
  function automatic logic [31:0] crc32_byte(input logic [31:0] crc, input logic [7:0] d);
    logic [31:0] r;
    r = crc ^ {24'h0, d};
    for (int i = 0; i < 8; i++) r = r[0] ? ((r >> 1) ^ 32'hEDB8_8320) : (r >> 1);
    return r;
  endfunction

endpackage
