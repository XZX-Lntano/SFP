// SPDX-License-Identifier: BSD-2-Clause-Views
/*
 * AXI stream UDP accumulator
 *
 * Fixed packet layout (no VLAN, IPv4 header without options):
 *   byte 36-37 : UDP destination port
 *   byte 42    : opcode (0 = add, 1 = clear)
 *   byte 43    : reserved
 *   byte 44-45 : 16-bit register index
 *   byte 46-53 : 64-bit operand / result
 *
 * If the UDP destination port matches ACCUM_UDP_PORT, then
 *   opcode 0: accum[index] <= accum[index] + operand
 *   opcode 1: accum[index] <= 0
 * and bytes 46-53 are replaced with the updated accumulated result.
 * The UDP checksum field is cleared on modified packets.
 */

`resetall
`timescale 1ns / 1ps
`default_nettype none

module axis_udp_accumulator #
(
    parameter DATA_WIDTH = 64,
    parameter KEEP_WIDTH = DATA_WIDTH/8,
    parameter USER_WIDTH = 1,
    parameter FIFO_DEPTH = 4096,
    parameter META_FIFO_DEPTH = 32,
    parameter ACCUM_REG_COUNT = 256,
    parameter [15:0] ACCUM_UDP_PORT = 16'h1234
)
(
    input  wire                     clk,
    input  wire                     rst,

    input  wire [DATA_WIDTH-1:0]    s_axis_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_tkeep,
    input  wire                     s_axis_tvalid,
    output wire                     s_axis_tready,
    input  wire                     s_axis_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_tuser,

    output wire [DATA_WIDTH-1:0]    m_axis_tdata,
    output wire [KEEP_WIDTH-1:0]    m_axis_tkeep,
    output wire                     m_axis_tvalid,
    input  wire                     m_axis_tready,
    output wire                     m_axis_tlast,
    output wire [USER_WIDTH-1:0]    m_axis_tuser
);

localparam META_FIFO_ADDR_WIDTH = META_FIFO_DEPTH > 1 ? $clog2(META_FIFO_DEPTH) : 1;
localparam ACCUM_REG_ADDR_WIDTH = ACCUM_REG_COUNT > 1 ? $clog2(ACCUM_REG_COUNT) : 1;

function integer count_keep;
    input [KEEP_WIDTH-1:0] keep;
    integer i;
    begin
        count_keep = 0;
        for (i = 0; i < KEEP_WIDTH; i = i + 1) begin
            if (keep[i]) begin
                count_keep = count_keep + 1;
            end
        end
    end
endfunction

function [7:0] result_byte;
    input [63:0] value;
    input [3:0] byte_offset;
    begin
        case (byte_offset)
            4'd0: result_byte = value[63:56];
            4'd1: result_byte = value[55:48];
            4'd2: result_byte = value[47:40];
            4'd3: result_byte = value[39:32];
            4'd4: result_byte = value[31:24];
            4'd5: result_byte = value[23:16];
            4'd6: result_byte = value[15:8];
            4'd7: result_byte = value[7:0];
            default: result_byte = 8'd0;
        endcase
    end
endfunction

reg [63:0] accum_mem[0:ACCUM_REG_COUNT-1];

reg [15:0] eth_type_reg = 16'd0;
reg [7:0] ip_version_ihl_reg = 8'd0;
reg [7:0] ip_protocol_reg = 8'd0;
reg [15:0] udp_dest_port_reg = 16'd0;
reg [7:0] opcode_reg = 8'd0;
reg [15:0] operand_index_reg = 16'd0;
reg [63:0] operand_value_reg = 64'd0;
reg [15:0] frame_byte_index_reg = 16'd0;

reg        meta_fifo_process_mem[0:META_FIFO_DEPTH-1];
reg [63:0] meta_fifo_result_mem[0:META_FIFO_DEPTH-1];
reg [META_FIFO_ADDR_WIDTH-1:0] meta_fifo_wr_ptr_reg = 0;
reg [META_FIFO_ADDR_WIDTH-1:0] meta_fifo_rd_ptr_reg = 0;
reg [META_FIFO_ADDR_WIDTH:0] meta_fifo_count_reg = 0;

reg current_process_reg = 1'b0;
reg [63:0] current_result_reg = 64'd0;
reg current_meta_valid_reg = 1'b0;
reg [15:0] output_byte_index_reg = 16'd0;

wire meta_fifo_full = meta_fifo_count_reg >= META_FIFO_DEPTH;
wire meta_fifo_empty = meta_fifo_count_reg == 0;
wire meta_fifo_pop = !current_meta_valid_reg && !meta_fifo_empty;
wire meta_fifo_push = frame_fifo_in_tvalid && frame_fifo_in_tready && s_axis_tlast;

wire frame_fifo_in_tready;
wire frame_fifo_in_tvalid;

wire [DATA_WIDTH-1:0] frame_fifo_out_tdata;
wire [KEEP_WIDTH-1:0] frame_fifo_out_tkeep;
wire frame_fifo_out_tvalid;
wire frame_fifo_out_tready;
wire frame_fifo_out_tlast;
wire [USER_WIDTH-1:0] frame_fifo_out_tuser;

assign frame_fifo_in_tvalid = s_axis_tvalid && !(s_axis_tlast && meta_fifo_full);
assign s_axis_tready = frame_fifo_in_tready && !(s_axis_tlast && meta_fifo_full);

axis_fifo #(
    .DEPTH(FIFO_DEPTH),
    .DATA_WIDTH(DATA_WIDTH),
    .KEEP_ENABLE(1),
    .KEEP_WIDTH(KEEP_WIDTH),
    .LAST_ENABLE(1),
    .ID_ENABLE(0),
    .DEST_ENABLE(0),
    .USER_ENABLE(1),
    .USER_WIDTH(USER_WIDTH),
    .RAM_PIPELINE(1),
    .OUTPUT_FIFO_ENABLE(0),
    .FRAME_FIFO(1),
    .USER_BAD_FRAME_VALUE(1'b1),
    .USER_BAD_FRAME_MASK(1'b1),
    .DROP_OVERSIZE_FRAME(1),
    .DROP_BAD_FRAME(1),
    .DROP_WHEN_FULL(1)
)
frame_fifo_inst (
    .clk(clk),
    .rst(rst),
    .s_axis_tdata(s_axis_tdata),
    .s_axis_tkeep(s_axis_tkeep),
    .s_axis_tvalid(frame_fifo_in_tvalid),
    .s_axis_tready(frame_fifo_in_tready),
    .s_axis_tlast(s_axis_tlast),
    .s_axis_tid(0),
    .s_axis_tdest(0),
    .s_axis_tuser(s_axis_tuser),
    .m_axis_tdata(frame_fifo_out_tdata),
    .m_axis_tkeep(frame_fifo_out_tkeep),
    .m_axis_tvalid(frame_fifo_out_tvalid),
    .m_axis_tready(frame_fifo_out_tready),
    .m_axis_tlast(frame_fifo_out_tlast),
    .m_axis_tid(),
    .m_axis_tdest(),
    .m_axis_tuser(frame_fifo_out_tuser),
    .status_overflow(),
    .status_bad_frame(),
    .status_good_frame()
);

assign frame_fifo_out_tready = m_axis_tready && current_meta_valid_reg;
assign m_axis_tvalid = frame_fifo_out_tvalid && current_meta_valid_reg;
assign m_axis_tlast = frame_fifo_out_tlast;
assign m_axis_tuser = frame_fifo_out_tuser;

genvar g;
generate
    for (g = 0; g < KEEP_WIDTH; g = g + 1) begin : output_override
        wire [15:0] out_byte_index = output_byte_index_reg + g;
        wire checksum_byte = current_process_reg && (out_byte_index == 16'd40 || out_byte_index == 16'd41);
        wire result_window = current_process_reg && out_byte_index >= 16'd46 && out_byte_index <= 16'd53;
        wire [7:0] raw_byte = frame_fifo_out_tdata[g*8 +: 8];
        wire [7:0] replaced_byte = result_window ? result_byte(current_result_reg, out_byte_index-16'd46) : 8'd0;

        assign m_axis_tdata[g*8 +: 8] = checksum_byte ? 8'd0 : (result_window ? replaced_byte : raw_byte);
        assign m_axis_tkeep[g] = frame_fifo_out_tkeep[g];
    end
endgenerate

integer lane;
integer byte_index;
reg [63:0] accum_result_reg;
reg process_frame_reg;
always @(posedge clk) begin
    if (rst) begin
        eth_type_reg <= 16'd0;
        ip_version_ihl_reg <= 8'd0;
        ip_protocol_reg <= 8'd0;
        udp_dest_port_reg <= 16'd0;
        opcode_reg <= 8'd0;
        operand_index_reg <= 16'd0;
        operand_value_reg <= 64'd0;
        frame_byte_index_reg <= 16'd0;
        meta_fifo_wr_ptr_reg <= 0;
        meta_fifo_rd_ptr_reg <= 0;
        meta_fifo_count_reg <= 0;
        current_process_reg <= 1'b0;
        current_result_reg <= 64'd0;
        current_meta_valid_reg <= 1'b0;
        output_byte_index_reg <= 16'd0;
    end else begin
        if (meta_fifo_pop) begin
            current_process_reg <= meta_fifo_process_mem[meta_fifo_rd_ptr_reg];
            current_result_reg <= meta_fifo_result_mem[meta_fifo_rd_ptr_reg];
            current_meta_valid_reg <= 1'b1;
            output_byte_index_reg <= 16'd0;
        end

        if (frame_fifo_out_tvalid && frame_fifo_out_tready) begin
            if (frame_fifo_out_tlast) begin
                current_meta_valid_reg <= 1'b0;
                output_byte_index_reg <= 16'd0;
            end else begin
                output_byte_index_reg <= output_byte_index_reg + count_keep(frame_fifo_out_tkeep);
            end
        end

        case ({meta_fifo_push, meta_fifo_pop})
            2'b01: begin
                meta_fifo_rd_ptr_reg <= meta_fifo_rd_ptr_reg + 1;
                meta_fifo_count_reg <= meta_fifo_count_reg - 1;
            end
            2'b10: begin
                meta_fifo_wr_ptr_reg <= meta_fifo_wr_ptr_reg + 1;
                meta_fifo_count_reg <= meta_fifo_count_reg + 1;
            end
            2'b11: begin
                meta_fifo_wr_ptr_reg <= meta_fifo_wr_ptr_reg + 1;
                meta_fifo_rd_ptr_reg <= meta_fifo_rd_ptr_reg + 1;
            end
        endcase

        if (frame_fifo_in_tvalid && frame_fifo_in_tready) begin
            for (lane = 0; lane < KEEP_WIDTH; lane = lane + 1) begin
                if (s_axis_tkeep[lane]) begin
                    byte_index = frame_byte_index_reg + lane;
                    case (byte_index)
                        12: eth_type_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        13: eth_type_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                        14: ip_version_ihl_reg <= s_axis_tdata[lane*8 +: 8];
                        23: ip_protocol_reg <= s_axis_tdata[lane*8 +: 8];
                        36: udp_dest_port_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        37: udp_dest_port_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                        42: opcode_reg <= s_axis_tdata[lane*8 +: 8];
                        44: operand_index_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        45: operand_index_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                        46: operand_value_reg[63:56] <= s_axis_tdata[lane*8 +: 8];
                        47: operand_value_reg[55:48] <= s_axis_tdata[lane*8 +: 8];
                        48: operand_value_reg[47:40] <= s_axis_tdata[lane*8 +: 8];
                        49: operand_value_reg[39:32] <= s_axis_tdata[lane*8 +: 8];
                        50: operand_value_reg[31:24] <= s_axis_tdata[lane*8 +: 8];
                        51: operand_value_reg[23:16] <= s_axis_tdata[lane*8 +: 8];
                        52: operand_value_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        53: operand_value_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                    endcase
                end
            end

            if (s_axis_tlast) begin
                process_frame_reg =
                    eth_type_reg == 16'h0800 &&
                    ip_version_ihl_reg == 8'h45 &&
                    ip_protocol_reg == 8'h11 &&
                    udp_dest_port_reg == ACCUM_UDP_PORT &&
                    (opcode_reg == 8'd0 || opcode_reg == 8'd1) &&
                    operand_index_reg < ACCUM_REG_COUNT;

                accum_result_reg = operand_value_reg;
                if (process_frame_reg) begin
                    if (opcode_reg == 8'd1) begin
                        accum_result_reg = 64'd0;
                        accum_mem[operand_index_reg[ACCUM_REG_ADDR_WIDTH-1:0]] <= 64'd0;
                    end else begin
                        accum_result_reg = accum_mem[operand_index_reg[ACCUM_REG_ADDR_WIDTH-1:0]] + operand_value_reg;
                        accum_mem[operand_index_reg[ACCUM_REG_ADDR_WIDTH-1:0]] <= accum_result_reg;
                    end
                end

                meta_fifo_process_mem[meta_fifo_wr_ptr_reg] <= process_frame_reg;
                meta_fifo_result_mem[meta_fifo_wr_ptr_reg] <= accum_result_reg;

                eth_type_reg <= 16'd0;
                ip_version_ihl_reg <= 8'd0;
                ip_protocol_reg <= 8'd0;
                udp_dest_port_reg <= 16'd0;
                opcode_reg <= 8'd0;
                operand_index_reg <= 16'd0;
                operand_value_reg <= 64'd0;
                frame_byte_index_reg <= 16'd0;
            end else begin
                frame_byte_index_reg <= frame_byte_index_reg + count_keep(s_axis_tkeep);
            end
        end
    end
end

endmodule

`resetall
