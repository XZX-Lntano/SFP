/*

Copyright (c) 2020-2021 Alex Forencich

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

// Language: Verilog 2001

`resetall
`timescale 1ns / 1ps
`default_nettype none

/*
 * FPGA core logic - Two-port Ethernet Bridge
 */
module fpga_core
(
    /*
     * Clock: 156.25MHz
     * Synchronous reset
     */
    input  wire        clk,
    input  wire        rst,

    /*
     * GPIO
     */
    input  wire        btnu,
    input  wire        btnl,
    input  wire        btnd,
    input  wire        btnr,
    input  wire        btnc,
    input  wire [7:0]  sw,
    output wire [7:0]  led,

    /*
     * UART: 115200 bps, 8N1
     */
    input  wire        uart_rxd,
    output wire        uart_txd,
    input  wire        uart_rts,
    output wire        uart_cts,

    /*
     * Ethernet: SFP+
     */
    input  wire        sfp0_tx_clk,
    input  wire        sfp0_tx_rst,
    output wire [63:0] sfp0_txd,
    output wire [7:0]  sfp0_txc,
    input  wire        sfp0_rx_clk,
    input  wire        sfp0_rx_rst,
    input  wire [63:0] sfp0_rxd,
    input  wire [7:0]  sfp0_rxc,
    input  wire        sfp1_tx_clk,
    input  wire        sfp1_tx_rst,
    output wire [63:0] sfp1_txd,
    output wire [7:0]  sfp1_txc,
    input  wire        sfp1_rx_clk,
    input  wire        sfp1_rx_rst,
    input  wire [63:0] sfp1_rxd,
    input  wire [7:0]  sfp1_rxc,
    input  wire        sfp2_tx_clk,
    input  wire        sfp2_tx_rst,
    output wire [63:0] sfp2_txd,
    output wire [7:0]  sfp2_txc,
    input  wire        sfp2_rx_clk,
    input  wire        sfp2_rx_rst,
    input  wire [63:0] sfp2_rxd,
    input  wire [7:0]  sfp2_rxc,
    input  wire        sfp3_tx_clk,
    input  wire        sfp3_tx_rst,
    output wire [63:0] sfp3_txd,
    output wire [7:0]  sfp3_txc,
    input  wire        sfp3_rx_clk,
    input  wire        sfp3_rx_rst,
    input  wire [63:0] sfp3_rxd,
    input  wire [7:0]  sfp3_rxc
);

// Bridge SFP0 <-> SFP1
// SFP0 RX -> SFP1 TX
assign sfp1_txd = sfp0_rxd;
assign sfp1_txc = sfp0_rxc;

// SFP1 RX -> SFP0 TX
assign sfp0_txd = sfp1_rxd;
assign sfp0_txc = sfp1_rxc;

// Unused ports
assign sfp2_txd = 64'h0707070707070707;
assign sfp2_txc = 8'hff;
assign sfp3_txd = 64'h0707070707070707;
assign sfp3_txc = 8'hff;

// LEDs - indicate activity
reg [31:0] sfp0_activity_cnt = 0;
reg [31:0] sfp1_activity_cnt = 0;
reg [7:0] led_reg = 8'b00000000;

always @(posedge clk) begin
    if (rst) begin
        sfp0_activity_cnt <= 0;
        sfp1_activity_cnt <= 0;
        led_reg <= 0;
    end else begin
        // Detect activity on SFP0 (not idle)
        if (sfp0_rxc != 8'hff || sfp0_rxd != 64'h0707070707070707) begin
            sfp0_activity_cnt <= ~0;
        end else if (sfp0_activity_cnt != 0) begin
            sfp0_activity_cnt <= sfp0_activity_cnt - 1;
        end
        
        // Detect activity on SFP1 (not idle)
        if (sfp1_rxc != 8'hff || sfp1_rxd != 64'h0707070707070707) begin
            sfp1_activity_cnt <= ~0;
        end else if (sfp1_activity_cnt != 0) begin
            sfp1_activity_cnt <= sfp1_activity_cnt - 1;
        end
        
        // LED mapping
        led_reg[0] <= sfp0_activity_cnt != 0;  // SFP0 activity
        led_reg[1] <= sfp1_activity_cnt != 0;  // SFP1 activity
        led_reg[7:2] <= 6'b000000;
    end
end

assign led = led_reg;

assign uart_txd = uart_rxd;
assign uart_cts = uart_rts;

endmodule

`resetall
