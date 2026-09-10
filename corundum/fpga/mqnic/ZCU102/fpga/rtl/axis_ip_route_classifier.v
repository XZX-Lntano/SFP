// SPDX-License-Identifier: BSD-2-Clause-Views
/*
 * AXI stream frame classifier for static IPv4/ARP routing
 */

`resetall
`timescale 1ns / 1ps
`default_nettype none

module axis_ip_route_classifier #
(
    parameter DATA_WIDTH = 64,
    parameter KEEP_WIDTH = DATA_WIDTH/8,
    parameter USER_WIDTH = 1,
    parameter DEST_WIDTH = 4,
    parameter FIFO_DEPTH = 4096,
    parameter ROUTE_FIFO_DEPTH = 32,
    parameter [DEST_WIDTH-1:0] DEFAULT_DEST = 0,
    parameter [31:0] ROUTE_IP_0 = 32'hC0A80A01,
    parameter [31:0] ROUTE_IP_1 = 32'hC0A80A02,
    parameter [31:0] ROUTE_IP_2 = 32'hC0A80A03,
    parameter [31:0] ROUTE_IP_3 = 32'hC0A80A04,
    parameter [DEST_WIDTH-1:0] ROUTE_DEST_0 = 4'b0001,
    parameter [DEST_WIDTH-1:0] ROUTE_DEST_1 = 4'b0010,
    parameter [DEST_WIDTH-1:0] ROUTE_DEST_2 = 4'b0100,
    parameter [DEST_WIDTH-1:0] ROUTE_DEST_3 = 4'b1000
)
(
    input  wire                     clk,
    input  wire                     rst,

    /*
     * AXI-Stream input
     */
    input  wire [DATA_WIDTH-1:0]    s_axis_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_tkeep,
    input  wire                     s_axis_tvalid,
    output wire                     s_axis_tready,
    input  wire                     s_axis_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_tuser,

    /*
     * AXI-Stream output
     */
    output wire [DATA_WIDTH-1:0]    m_axis_tdata,
    output wire [KEEP_WIDTH-1:0]    m_axis_tkeep,
    output wire                     m_axis_tvalid,
    input  wire                     m_axis_tready,
    output wire                     m_axis_tlast,
    output wire [DEST_WIDTH-1:0]    m_axis_tdest,
    output wire [USER_WIDTH-1:0]    m_axis_tuser
);

localparam ROUTE_FIFO_ADDR_WIDTH = ROUTE_FIFO_DEPTH > 1 ? $clog2(ROUTE_FIFO_DEPTH) : 1;

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

function [DEST_WIDTH-1:0] select_route;
    input [15:0] eth_type;
    input [31:0] ip_dst;
    begin
        select_route = DEFAULT_DEST;

        if (eth_type == 16'h0800 || eth_type == 16'h0806) begin
            if (ip_dst == ROUTE_IP_0) begin
                select_route = ROUTE_DEST_0;
            end else if (ip_dst == ROUTE_IP_1) begin
                select_route = ROUTE_DEST_1;
            end else if (ip_dst == ROUTE_IP_2) begin
                select_route = ROUTE_DEST_2;
            end else if (ip_dst == ROUTE_IP_3) begin
                select_route = ROUTE_DEST_3;
            end
        end
    end
endfunction

reg [15:0] eth_type_reg = 16'd0;
reg [31:0] ipv4_dst_ip_reg = 32'd0;
reg [31:0] arp_target_ip_reg = 32'd0;
reg [15:0] frame_byte_index_reg = 16'd0;

reg [DEST_WIDTH-1:0] route_fifo_mem[0:ROUTE_FIFO_DEPTH-1];
reg [ROUTE_FIFO_ADDR_WIDTH-1:0] route_fifo_wr_ptr_reg = 0;
reg [ROUTE_FIFO_ADDR_WIDTH-1:0] route_fifo_rd_ptr_reg = 0;
reg [ROUTE_FIFO_ADDR_WIDTH:0] route_fifo_count_reg = 0;

reg [DEST_WIDTH-1:0] current_route_reg = DEFAULT_DEST;
reg current_route_valid_reg = 1'b0;

wire route_fifo_full = route_fifo_count_reg >= ROUTE_FIFO_DEPTH;
wire route_fifo_empty = route_fifo_count_reg == 0;
wire route_fifo_pop = !current_route_valid_reg && !route_fifo_empty;
wire route_fifo_push = frame_fifo_in_tvalid && frame_fifo_in_tready && s_axis_tlast;
wire [DEST_WIDTH-1:0] selected_route = select_route(
    eth_type_reg,
    eth_type_reg == 16'h0806 ? arp_target_ip_reg : ipv4_dst_ip_reg
);

wire frame_fifo_in_tready;
wire frame_fifo_in_tvalid;

wire [DATA_WIDTH-1:0] frame_fifo_out_tdata;
wire [KEEP_WIDTH-1:0] frame_fifo_out_tkeep;
wire frame_fifo_out_tvalid;
wire frame_fifo_out_tready;
wire frame_fifo_out_tlast;
wire [USER_WIDTH-1:0] frame_fifo_out_tuser;

assign frame_fifo_in_tvalid = s_axis_tvalid && !(s_axis_tlast && route_fifo_full);
assign s_axis_tready = frame_fifo_in_tready && !(s_axis_tlast && route_fifo_full);

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

assign frame_fifo_out_tready = m_axis_tready && current_route_valid_reg;

assign m_axis_tdata = frame_fifo_out_tdata;
assign m_axis_tkeep = frame_fifo_out_tkeep;
assign m_axis_tvalid = frame_fifo_out_tvalid && current_route_valid_reg;
assign m_axis_tlast = frame_fifo_out_tlast;
assign m_axis_tdest = current_route_reg;
assign m_axis_tuser = frame_fifo_out_tuser;

integer lane;
integer byte_index;
always @(posedge clk) begin
    if (rst) begin
        eth_type_reg <= 16'd0;
        ipv4_dst_ip_reg <= 32'd0;
        arp_target_ip_reg <= 32'd0;
        frame_byte_index_reg <= 16'd0;
        route_fifo_wr_ptr_reg <= 0;
        route_fifo_rd_ptr_reg <= 0;
        route_fifo_count_reg <= 0;
        current_route_reg <= DEFAULT_DEST;
        current_route_valid_reg <= 1'b0;
    end else begin
        if (route_fifo_pop) begin
            current_route_reg <= route_fifo_mem[route_fifo_rd_ptr_reg];
            current_route_valid_reg <= 1'b1;
        end

        if (frame_fifo_out_tvalid && frame_fifo_out_tready && frame_fifo_out_tlast) begin
            current_route_valid_reg <= 1'b0;
        end

        case ({route_fifo_push, route_fifo_pop})
            2'b01: begin
                route_fifo_rd_ptr_reg <= route_fifo_rd_ptr_reg + 1;
                route_fifo_count_reg <= route_fifo_count_reg - 1;
            end
            2'b10: begin
                route_fifo_wr_ptr_reg <= route_fifo_wr_ptr_reg + 1;
                route_fifo_count_reg <= route_fifo_count_reg + 1;
            end
            2'b11: begin
                route_fifo_wr_ptr_reg <= route_fifo_wr_ptr_reg + 1;
                route_fifo_rd_ptr_reg <= route_fifo_rd_ptr_reg + 1;
            end
        endcase

        if (frame_fifo_in_tvalid && frame_fifo_in_tready) begin
            for (lane = 0; lane < KEEP_WIDTH; lane = lane + 1) begin
                if (s_axis_tkeep[lane]) begin
                    byte_index = frame_byte_index_reg + lane;
                    case (byte_index)
                        12: eth_type_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        13: eth_type_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                        30: ipv4_dst_ip_reg[31:24] <= s_axis_tdata[lane*8 +: 8];
                        31: ipv4_dst_ip_reg[23:16] <= s_axis_tdata[lane*8 +: 8];
                        32: ipv4_dst_ip_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        33: ipv4_dst_ip_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                        38: arp_target_ip_reg[31:24] <= s_axis_tdata[lane*8 +: 8];
                        39: arp_target_ip_reg[23:16] <= s_axis_tdata[lane*8 +: 8];
                        40: arp_target_ip_reg[15:8] <= s_axis_tdata[lane*8 +: 8];
                        41: arp_target_ip_reg[7:0] <= s_axis_tdata[lane*8 +: 8];
                    endcase
                end
            end

            if (s_axis_tlast) begin
                route_fifo_mem[route_fifo_wr_ptr_reg] <= selected_route;
                eth_type_reg <= 16'd0;
                ipv4_dst_ip_reg <= 32'd0;
                arp_target_ip_reg <= 32'd0;
                frame_byte_index_reg <= 16'd0;
            end else begin
                frame_byte_index_reg <= frame_byte_index_reg + count_keep(s_axis_tkeep);
            end
        end
    end
end

endmodule

`resetall
