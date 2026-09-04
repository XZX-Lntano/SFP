// SPDX-License-Identifier: BSD-2-Clause-Views
/*
 * Four-lane UDP aggregation engine for protocol v4.
 *
 * Each ingress lane is parsed independently.  Values are written one per
 * clock to a private BRAM bank; consequently four 10 Gb/s inputs do not share
 * a parser or an input arbiter.  A three-stage reducer reads the four banks,
 * performs two pairwise additions, then a final addition into result BRAM.
 * The completed jumbo frame is emitted once and replicated downstream.
 */

`resetall
`timescale 1ns / 1ps
`default_nettype none

module axis_udp_batch_parser #(
    parameter DATA_WIDTH = 64,
    parameter KEEP_WIDTH = DATA_WIDTH/8,
    parameter USER_WIDTH = 1,
    parameter [15:0] AGG_UDP_PORT = 16'h2345
)(
    input  wire                  clk,
    input  wire                  rst,
    input  wire                  enable,
    input  wire [DATA_WIDTH-1:0] s_axis_tdata,
    input  wire [KEEP_WIDTH-1:0] s_axis_tkeep,
    input  wire                  s_axis_tvalid,
    output wire                  s_axis_tready,
    input  wire                  s_axis_tlast,
    input  wire [USER_WIDTH-1:0] s_axis_tuser,
    output reg                   value_valid,
    output reg [3:0]             value_slot,
    output reg [5:0]             value_index,
    output reg [63:0]            value_data,
    output reg                   round_valid,
    output reg [3:0]             round_order,
    output reg [3:0]             round_slot,
    output reg [15:0]            round_id,
    output reg                   frame_done,
    output reg                   frame_good,
    output reg [4:0]             frame_round_count,
    output reg [2:0]             frame_worker_count
);

localparam [15:0] BATCH_MAGIC = 16'ha416;
localparam [7:0] BATCH_VERSION = 8'd4;

reg [11:0] beat_index_reg = 0;
reg [15:0] ethertype_reg = 0;
reg [7:0] ip_version_reg = 0;
reg [7:0] ip_protocol_reg = 0;
reg [7:0] ip_ttl_reg = 0;
reg [15:0] udp_dport_reg = 0;
reg [15:0] magic_reg = 0;
reg [7:0] version_reg = 0;
reg [4:0] count_reg = 0;
reg [15:0] flags_reg = 0;
reg [15:0] current_round_reg = 0;
reg bad_user_reg = 0;
reg [4:0] record_number_reg = 0;
reg [6:0] record_beat_reg = 0;
reg frame_hold_reg = 0;

wire transfer = s_axis_tvalid && s_axis_tready;

assign s_axis_tready = enable && !frame_hold_reg;

always @(posedge clk) begin
    if (rst) begin
        beat_index_reg <= 0;
        value_valid <= 0;
        round_valid <= 0;
        frame_done <= 0;
        frame_good <= 0;
        bad_user_reg <= 0;
        record_number_reg <= 0;
        record_beat_reg <= 0;
        frame_hold_reg <= 0;
    end else begin
        value_valid <= 0;
        round_valid <= 0;
        frame_done <= 0;

        if (!enable)
            frame_hold_reg <= 0;

        if (transfer) begin
            bad_user_reg <= bad_user_reg | |s_axis_tuser;
            case (beat_index_reg)
                12'd1: begin
                    ethertype_reg <= {s_axis_tdata[39:32], s_axis_tdata[47:40]};
                    ip_version_reg <= s_axis_tdata[55:48];
                end
                12'd2: begin
                    ip_ttl_reg <= s_axis_tdata[55:48];
                    ip_protocol_reg <= s_axis_tdata[63:56];
                end
                12'd4: udp_dport_reg <= {s_axis_tdata[39:32], s_axis_tdata[47:40]};
                12'd5: begin
                    magic_reg <= {s_axis_tdata[23:16], s_axis_tdata[31:24]};
                    version_reg <= s_axis_tdata[39:32];
                    count_reg <= s_axis_tdata[44:40];
                    flags_reg <= {s_axis_tdata[55:48], s_axis_tdata[63:56]};
                end
                default: begin end
            endcase

            if (beat_index_reg >= 12'd6 && record_number_reg < count_reg) begin
                if (record_beat_reg == 0) begin
                    current_round_reg <= {s_axis_tdata[7:0], s_axis_tdata[15:8]};
                    round_valid <= 1'b1;
                    round_order <= record_number_reg[3:0];
                    round_slot <= s_axis_tdata[11:8];
                    round_id <= {s_axis_tdata[7:0], s_axis_tdata[15:8]};
                end else if (record_beat_reg <= 64) begin
                    value_valid <= 1'b1;
                    value_slot <= current_round_reg[3:0];
                    value_index <= record_beat_reg-1'b1;
                    value_data <= {s_axis_tdata[7:0], s_axis_tdata[15:8],
                                   s_axis_tdata[23:16], s_axis_tdata[31:24],
                                   s_axis_tdata[39:32], s_axis_tdata[47:40],
                                   s_axis_tdata[55:48], s_axis_tdata[63:56]};
                end
            end

            if (s_axis_tlast) begin
                frame_hold_reg <= 1;
                frame_done <= 1'b1;
                frame_round_count <= count_reg;
                frame_worker_count <= ip_ttl_reg[2:0];
                frame_good <= ethertype_reg == 16'h0800 &&
                              ip_version_reg == 8'h45 &&
                              ip_protocol_reg == 8'h11 &&
                              ip_ttl_reg >= 2 && ip_ttl_reg <= 4 &&
                              udp_dport_reg == AGG_UDP_PORT &&
                              magic_reg == BATCH_MAGIC &&
                              version_reg == BATCH_VERSION &&
                              flags_reg == 0 && count_reg >= 1 && count_reg <= 16 &&
                              beat_index_reg == 12'd5 + count_reg*12'd65 &&
                              &s_axis_tkeep && !(bad_user_reg | |s_axis_tuser);
                beat_index_reg <= 0;
                bad_user_reg <= 0;
                record_number_reg <= 0;
                record_beat_reg <= 0;
            end else begin
                beat_index_reg <= beat_index_reg + 1'b1;
                if (beat_index_reg >= 12'd6) begin
                    if (record_beat_reg == 64) begin
                        record_beat_reg <= 0;
                        record_number_reg <= record_number_reg + 1'b1;
                    end else begin
                        record_beat_reg <= record_beat_reg + 1'b1;
                    end
                end
            end
        end
    end
end

endmodule

module axis_udp_batch_aggregator #(
    parameter DATA_WIDTH = 64,
    parameter KEEP_WIDTH = DATA_WIDTH/8,
    parameter USER_WIDTH = 1,
    parameter DEST_WIDTH = 4,
    parameter ENTRY_COUNT = 64,
    parameter ROUND_SLOTS = 16,
    parameter [15:0] AGG_UDP_PORT = 16'h2345,
    parameter [DEST_WIDTH-1:0] RESULT_DEST = {DEST_WIDTH{1'b1}}
)(
    input wire clk,
    input wire rst,
    input wire [DATA_WIDTH-1:0] s_axis_0_tdata,
    input wire [KEEP_WIDTH-1:0] s_axis_0_tkeep,
    input wire s_axis_0_tvalid,
    output wire s_axis_0_tready,
    input wire s_axis_0_tlast,
    input wire [USER_WIDTH-1:0] s_axis_0_tuser,
    input wire s_axis_0_overflow,
    input wire [DATA_WIDTH-1:0] s_axis_1_tdata,
    input wire [KEEP_WIDTH-1:0] s_axis_1_tkeep,
    input wire s_axis_1_tvalid,
    output wire s_axis_1_tready,
    input wire s_axis_1_tlast,
    input wire [USER_WIDTH-1:0] s_axis_1_tuser,
    input wire s_axis_1_overflow,
    input wire [DATA_WIDTH-1:0] s_axis_2_tdata,
    input wire [KEEP_WIDTH-1:0] s_axis_2_tkeep,
    input wire s_axis_2_tvalid,
    output wire s_axis_2_tready,
    input wire s_axis_2_tlast,
    input wire [USER_WIDTH-1:0] s_axis_2_tuser,
    input wire s_axis_2_overflow,
    input wire [DATA_WIDTH-1:0] s_axis_3_tdata,
    input wire [KEEP_WIDTH-1:0] s_axis_3_tkeep,
    input wire s_axis_3_tvalid,
    output wire s_axis_3_tready,
    input wire s_axis_3_tlast,
    input wire [USER_WIDTH-1:0] s_axis_3_tuser,
    input wire s_axis_3_overflow,
    output wire [DATA_WIDTH-1:0] m_axis_tdata,
    output wire [KEEP_WIDTH-1:0] m_axis_tkeep,
    output wire m_axis_tvalid,
    input wire m_axis_tready,
    output wire m_axis_tlast,
    output wire [DEST_WIDTH-1:0] m_axis_tdest,
    output wire [USER_WIDTH-1:0] m_axis_tuser
);

localparam STATE_INPUT = 3'd0;
localparam STATE_CHECK = 3'd1;
localparam STATE_REDUCE = 3'd2;
localparam STATE_DRAIN = 3'd3;
localparam STATE_OUTPUT = 3'd4;

wire [3:0] p_value_valid, p_round_valid, p_done, p_good;
wire [3:0] p_value_slot[0:3];
wire [5:0] p_value_index[0:3];
wire [63:0] p_value_data[0:3];
wire [3:0] p_round_order[0:3], p_round_slot[0:3];
wire [15:0] p_round_id[0:3];
wire [4:0] p_count[0:3];
wire [2:0] p_workers[0:3];

reg [2:0] state_reg = STATE_INPUT;
reg [3:0] done_mask_reg = 0;
reg [4:0] batch_count_reg = 0;
reg [2:0] worker_count_reg = 0;
reg batch_error_reg = 0;
reg [15:0] round_order_mem[0:ROUND_SLOTS-1];
reg [15:0] round_id_0[0:ROUND_SLOTS-1];
reg [15:0] round_id_1[0:ROUND_SLOTS-1];
reg [15:0] round_id_2[0:ROUND_SLOTS-1];
reg [15:0] round_id_3[0:ROUND_SLOTS-1];
reg [15:0] slot_valid_0 = 0, slot_valid_1 = 0, slot_valid_2 = 0, slot_valid_3 = 0;

(* ram_style = "block" *) reg [63:0] worker_mem_0[0:ROUND_SLOTS*ENTRY_COUNT-1];
(* ram_style = "block" *) reg [63:0] worker_mem_1[0:ROUND_SLOTS*ENTRY_COUNT-1];
(* ram_style = "block" *) reg [63:0] worker_mem_2[0:ROUND_SLOTS*ENTRY_COUNT-1];
(* ram_style = "block" *) reg [63:0] worker_mem_3[0:ROUND_SLOTS*ENTRY_COUNT-1];
(* ram_style = "block" *) reg [63:0] result_mem[0:ROUND_SLOTS*ENTRY_COUNT-1];

reg [DATA_WIDTH-1:0] header_mem[0:5];
reg [KEEP_WIDTH-1:0] header_keep[0:5];
reg [USER_WIDTH-1:0] header_user[0:5];
reg [2:0] header_write_index_reg = 0;

wire input_enable = state_reg == STATE_INPUT;
wire [3:0] parser_enable = {4{input_enable}} & ~done_mask_reg;

`define PARSER_INSTANCE(N) \
axis_udp_batch_parser #(.DATA_WIDTH(DATA_WIDTH), .KEEP_WIDTH(KEEP_WIDTH), \
    .USER_WIDTH(USER_WIDTH), .AGG_UDP_PORT(AGG_UDP_PORT)) parser_``N ( \
    .clk(clk), .rst(rst), .enable(parser_enable[N]), \
    .s_axis_tdata(s_axis_``N``_tdata), .s_axis_tkeep(s_axis_``N``_tkeep), \
    .s_axis_tvalid(s_axis_``N``_tvalid), .s_axis_tready(s_axis_``N``_tready), \
    .s_axis_tlast(s_axis_``N``_tlast), .s_axis_tuser(s_axis_``N``_tuser), \
    .value_valid(p_value_valid[N]), .value_slot(p_value_slot[N]), \
    .value_index(p_value_index[N]), .value_data(p_value_data[N]), \
    .round_valid(p_round_valid[N]), .round_order(p_round_order[N]), \
    .round_slot(p_round_slot[N]), .round_id(p_round_id[N]), \
    .frame_done(p_done[N]), .frame_good(p_good[N]), \
    .frame_round_count(p_count[N]), .frame_worker_count(p_workers[N]));

`PARSER_INSTANCE(0)
`PARSER_INSTANCE(1)
`PARSER_INSTANCE(2)
`PARSER_INSTANCE(3)
`undef PARSER_INSTANCE

reg [4:0] check_index_reg = 0;
reg [9:0] reduce_index_reg = 0;
reg reduce_issue_valid_reg = 0;
reg reduce_sum_valid_reg = 0;
reg reduce_write_valid_reg = 0;
reg reduce_issue_last_reg = 0;
reg reduce_sum_last_reg = 0;
reg reduce_write_last_reg = 0;
reg [9:0] reduce_addr_pipe_1 = 0, reduce_addr_pipe_2 = 0;
reg [63:0] read_0_reg = 0, read_1_reg = 0, read_2_reg = 0, read_3_reg = 0;
reg [63:0] sum_01_reg = 0, sum_23_reg = 0;

reg [11:0] output_beat_reg = 0;
reg [4:0] output_record_reg = 0;
reg [6:0] output_record_beat_reg = 0;
reg [63:0] output_value_reg = 0;
wire [3:0] output_slot = round_order_mem[output_record_reg][3:0];
wire [9:0] output_value_addr = {output_slot, output_record_beat_reg[5:0]-1'b1};
wire output_prefetch = output_beat_reg >= 6 &&
                       (output_record_beat_reg == 0 ||
                        (output_record_beat_reg >= 1 && output_record_beat_reg < 64));
wire [9:0] output_prefetch_addr = output_record_beat_reg == 0 ?
                                  {output_slot, 6'd0} : output_value_addr+1'b1;
wire [11:0] output_last_beat = 12'd5 + batch_count_reg*12'd65;
reg [DATA_WIDTH-1:0] output_data_reg;

(* mark_debug = "true" *) reg [63:0] rx_packet_count_0 = 0;
(* mark_debug = "true" *) reg [63:0] rx_packet_count_1 = 0;
(* mark_debug = "true" *) reg [63:0] rx_packet_count_2 = 0;
(* mark_debug = "true" *) reg [63:0] rx_packet_count_3 = 0;
(* mark_debug = "true" *) reg [63:0] rx_byte_count_0 = 0;
(* mark_debug = "true" *) reg [63:0] rx_byte_count_1 = 0;
(* mark_debug = "true" *) reg [63:0] rx_byte_count_2 = 0;
(* mark_debug = "true" *) reg [63:0] rx_byte_count_3 = 0;
(* mark_debug = "true" *) reg [63:0] invalid_frame_count = 0;
(* mark_debug = "true" *) reg [63:0] slot_collision_count = 0;
(* mark_debug = "true" *) reg [63:0] fifo_overflow_count = 0;
(* mark_debug = "true" *) reg [63:0] output_backpressure_cycles = 0;
(* mark_debug = "true" *) reg [63:0] tx_packet_count = 0;
(* mark_debug = "true" *) reg [63:0] tx_byte_count = 0;

always @* begin
    output_data_reg = 0;
    if (output_beat_reg < 6) begin
        output_data_reg = header_mem[output_beat_reg];
        if (output_beat_reg == 5)
            output_data_reg[15:0] = 0; // UDP checksum
    end else if (output_record_beat_reg == 0) begin
        output_data_reg[7:0] = round_order_mem[output_record_reg][15:8];
        output_data_reg[15:8] = round_order_mem[output_record_reg][7:0];
    end else begin
        output_data_reg = {output_value_reg[7:0], output_value_reg[15:8],
                           output_value_reg[23:16], output_value_reg[31:24],
                           output_value_reg[39:32], output_value_reg[47:40],
                           output_value_reg[55:48], output_value_reg[63:56]};
    end
end

assign m_axis_tdata = output_data_reg;
assign m_axis_tkeep = output_beat_reg < 6 ? header_keep[output_beat_reg] : {KEEP_WIDTH{1'b1}};
assign m_axis_tvalid = state_reg == STATE_OUTPUT;
assign m_axis_tlast = state_reg == STATE_OUTPUT && output_beat_reg == output_last_beat;
assign m_axis_tdest = RESULT_DEST;
assign m_axis_tuser = output_beat_reg < 6 ? header_user[output_beat_reg] : {USER_WIDTH{1'b0}};

wire [3:0] done_now = done_mask_reg | p_done;
wire [3:0] required_now = p_done[0] ? ((4'b0001 << p_workers[0])-1'b1) :
                                    ((4'b0001 << worker_count_reg)-1'b1);
wire check_error_now =
    !slot_valid_0[round_order_mem[check_index_reg][3:0]] ||
    (worker_count_reg >= 2 &&
        (!slot_valid_1[round_order_mem[check_index_reg][3:0]] ||
         round_id_1[round_order_mem[check_index_reg][3:0]] != round_order_mem[check_index_reg])) ||
    (worker_count_reg >= 3 &&
        (!slot_valid_2[round_order_mem[check_index_reg][3:0]] ||
         round_id_2[round_order_mem[check_index_reg][3:0]] != round_order_mem[check_index_reg])) ||
    (worker_count_reg >= 4 &&
        (!slot_valid_3[round_order_mem[check_index_reg][3:0]] ||
         round_id_3[round_order_mem[check_index_reg][3:0]] != round_order_mem[check_index_reg]));
wire batch_metadata_error =
    (worker_count_reg >= 2 && (p_count[1] != batch_count_reg || p_workers[1] != worker_count_reg)) ||
    (worker_count_reg >= 3 && (p_count[2] != batch_count_reg || p_workers[2] != worker_count_reg)) ||
    (worker_count_reg >= 4 && (p_count[3] != batch_count_reg || p_workers[3] != worker_count_reg));
wire collision_0 = p_round_valid[0] && slot_valid_0[p_round_slot[0]] &&
                   round_id_0[p_round_slot[0]] != p_round_id[0];
wire collision_1 = p_round_valid[1] && slot_valid_1[p_round_slot[1]] &&
                   round_id_1[p_round_slot[1]] != p_round_id[1];
wire collision_2 = p_round_valid[2] && slot_valid_2[p_round_slot[2]] &&
                   round_id_2[p_round_slot[2]] != p_round_id[2];
wire collision_3 = p_round_valid[3] && slot_valid_3[p_round_slot[3]] &&
                   round_id_3[p_round_slot[3]] != p_round_id[3];
wire [2:0] overflow_increment = {2'b0, s_axis_0_overflow} +
                                {2'b0, s_axis_1_overflow} +
                                {2'b0, s_axis_2_overflow} +
                                {2'b0, s_axis_3_overflow};
wire [2:0] invalid_increment = {2'b0, p_done[0] && !p_good[0]} +
                               {2'b0, p_done[1] && !p_good[1]} +
                               {2'b0, p_done[2] && !p_good[2]} +
                               {2'b0, p_done[3] && !p_good[3]};
wire [2:0] collision_increment = {2'b0, collision_0} +
                                 {2'b0, collision_1} +
                                 {2'b0, collision_2} +
                                 {2'b0, collision_3};

integer k;
always @(posedge clk) begin
    if (rst) begin
        state_reg <= STATE_INPUT;
        done_mask_reg <= 0;
        batch_count_reg <= 0;
        worker_count_reg <= 0;
        batch_error_reg <= 0;
        slot_valid_0 <= 0; slot_valid_1 <= 0; slot_valid_2 <= 0; slot_valid_3 <= 0;
        header_write_index_reg <= 0;
        check_index_reg <= 0;
        reduce_index_reg <= 0;
        reduce_issue_valid_reg <= 0;
        reduce_sum_valid_reg <= 0;
        reduce_write_valid_reg <= 0;
        output_beat_reg <= 0;
        output_record_reg <= 0;
        output_record_beat_reg <= 0;
        rx_packet_count_0 <= 0; rx_packet_count_1 <= 0;
        rx_packet_count_2 <= 0; rx_packet_count_3 <= 0;
        rx_byte_count_0 <= 0; rx_byte_count_1 <= 0;
        rx_byte_count_2 <= 0; rx_byte_count_3 <= 0;
        invalid_frame_count <= 0;
        slot_collision_count <= 0;
        fifo_overflow_count <= 0;
        output_backpressure_cycles <= 0;
        tx_packet_count <= 0;
        tx_byte_count <= 0;
    end else begin
        fifo_overflow_count <= fifo_overflow_count + overflow_increment;
        invalid_frame_count <= invalid_frame_count + invalid_increment;
        slot_collision_count <= slot_collision_count + collision_increment;
        if (m_axis_tvalid && !m_axis_tready)
            output_backpressure_cycles <= output_backpressure_cycles + 1'b1;

        if (s_axis_0_tvalid && s_axis_0_tready) begin
            rx_byte_count_0 <= rx_byte_count_0 + 8;
            if (s_axis_0_tlast) rx_packet_count_0 <= rx_packet_count_0 + 1'b1;
            if (header_write_index_reg < 6) begin
                header_mem[header_write_index_reg] <= s_axis_0_tdata;
                header_keep[header_write_index_reg] <= s_axis_0_tkeep;
                header_user[header_write_index_reg] <= s_axis_0_tuser;
                header_write_index_reg <= header_write_index_reg + 1'b1;
            end
        end
        if (s_axis_1_tvalid && s_axis_1_tready) begin
            rx_byte_count_1 <= rx_byte_count_1 + 8;
            if (s_axis_1_tlast) rx_packet_count_1 <= rx_packet_count_1 + 1'b1;
        end
        if (s_axis_2_tvalid && s_axis_2_tready) begin
            rx_byte_count_2 <= rx_byte_count_2 + 8;
            if (s_axis_2_tlast) rx_packet_count_2 <= rx_packet_count_2 + 1'b1;
        end
        if (s_axis_3_tvalid && s_axis_3_tready) begin
            rx_byte_count_3 <= rx_byte_count_3 + 8;
            if (s_axis_3_tlast) rx_packet_count_3 <= rx_packet_count_3 + 1'b1;
        end

        if (p_value_valid[0]) worker_mem_0[{p_value_slot[0],p_value_index[0]}] <= p_value_data[0];
        if (p_value_valid[1]) worker_mem_1[{p_value_slot[1],p_value_index[1]}] <= p_value_data[1];
        if (p_value_valid[2]) worker_mem_2[{p_value_slot[2],p_value_index[2]}] <= p_value_data[2];
        if (p_value_valid[3]) worker_mem_3[{p_value_slot[3],p_value_index[3]}] <= p_value_data[3];

        if (p_round_valid[0]) begin
            if (collision_0) batch_error_reg <= 1;
            slot_valid_0[p_round_slot[0]] <= 1;
            round_id_0[p_round_slot[0]] <= p_round_id[0];
            round_order_mem[p_round_order[0]] <= p_round_id[0];
        end
        if (p_round_valid[1]) begin
            if (collision_1) batch_error_reg <= 1;
            slot_valid_1[p_round_slot[1]] <= 1; round_id_1[p_round_slot[1]] <= p_round_id[1];
        end
        if (p_round_valid[2]) begin
            if (collision_2) batch_error_reg <= 1;
            slot_valid_2[p_round_slot[2]] <= 1; round_id_2[p_round_slot[2]] <= p_round_id[2];
        end
        if (p_round_valid[3]) begin
            if (collision_3) batch_error_reg <= 1;
            slot_valid_3[p_round_slot[3]] <= 1; round_id_3[p_round_slot[3]] <= p_round_id[3];
        end

        for (k = 0; k < 4; k = k+1) begin
            if (p_done[k]) begin
                done_mask_reg[k] <= 1;
                if (!p_good[k]) batch_error_reg <= 1;
                if (k == 0) begin batch_count_reg <= p_count[0]; worker_count_reg <= p_workers[0]; end
            end
        end

        case (state_reg)
            STATE_INPUT: begin
                if (worker_count_reg >= 2 && (done_now & required_now) == required_now ||
                    p_done[0] && p_workers[0] >= 2 && (done_now & required_now) == required_now) begin
                    state_reg <= STATE_CHECK;
                    check_index_reg <= 0;
                end
            end
            STATE_CHECK: begin
                if (check_error_now)
                    batch_error_reg <= 1;
                if (check_index_reg+1 >= batch_count_reg) begin
                    if (batch_error_reg || batch_metadata_error || check_error_now) begin
                        state_reg <= STATE_INPUT;
                        done_mask_reg <= 0; header_write_index_reg <= 0;
                        slot_valid_0 <= 0; slot_valid_1 <= 0; slot_valid_2 <= 0; slot_valid_3 <= 0;
                        batch_error_reg <= 0;
                    end else begin
                        state_reg <= STATE_REDUCE;
                        reduce_index_reg <= 0;
                    end
                end else check_index_reg <= check_index_reg + 1'b1;
            end
            STATE_REDUCE: begin
                read_0_reg <= worker_mem_0[{round_order_mem[reduce_index_reg[9:6]][3:0],reduce_index_reg[5:0]}];
                read_1_reg <= worker_mem_1[{round_order_mem[reduce_index_reg[9:6]][3:0],reduce_index_reg[5:0]}];
                read_2_reg <= worker_mem_2[{round_order_mem[reduce_index_reg[9:6]][3:0],reduce_index_reg[5:0]}];
                read_3_reg <= worker_mem_3[{round_order_mem[reduce_index_reg[9:6]][3:0],reduce_index_reg[5:0]}];
                reduce_addr_pipe_1 <= {round_order_mem[reduce_index_reg[9:6]][3:0],reduce_index_reg[5:0]};
                reduce_issue_valid_reg <= 1;
                reduce_issue_last_reg <= {1'b0,reduce_index_reg}+11'd1 >= batch_count_reg*64;
                if ({1'b0,reduce_index_reg}+11'd1 >= batch_count_reg*64) state_reg <= STATE_DRAIN;
                else reduce_index_reg <= reduce_index_reg+1'b1;
            end
            STATE_DRAIN: begin
                reduce_issue_valid_reg <= 0;
                if (reduce_write_valid_reg && reduce_write_last_reg) begin
                    state_reg <= STATE_OUTPUT;
                    output_beat_reg <= 0;
                    output_record_reg <= 0;
                    output_record_beat_reg <= 0;
                    tx_byte_count <= tx_byte_count + 48 + batch_count_reg*520;
                end
            end
            STATE_OUTPUT: begin
                if (m_axis_tready) begin
                    if (output_prefetch)
                        output_value_reg <= result_mem[output_prefetch_addr];
                    if (output_beat_reg >= 6) begin
                        if (output_record_beat_reg == 64) begin
                            output_record_beat_reg <= 0;
                            output_record_reg <= output_record_reg+1'b1;
                        end else output_record_beat_reg <= output_record_beat_reg+1'b1;
                    end
                    if (output_beat_reg == output_last_beat) begin
                        state_reg <= STATE_INPUT;
                        output_beat_reg <= 0;
                        output_record_reg <= 0;
                        output_record_beat_reg <= 0;
                        done_mask_reg <= 0; header_write_index_reg <= 0;
                        slot_valid_0 <= 0; slot_valid_1 <= 0; slot_valid_2 <= 0; slot_valid_3 <= 0;
                        batch_error_reg <= 0;
                        tx_packet_count <= tx_packet_count+1'b1;
                    end else output_beat_reg <= output_beat_reg+1'b1;
                end
            end
            default: state_reg <= STATE_INPUT;
        endcase

        reduce_sum_valid_reg <= reduce_issue_valid_reg;
        reduce_sum_last_reg <= reduce_issue_last_reg;
        reduce_addr_pipe_2 <= reduce_addr_pipe_1;
        if (reduce_issue_valid_reg) begin
            sum_01_reg <= read_0_reg + (worker_count_reg >= 2 ? read_1_reg : 0);
            sum_23_reg <= (worker_count_reg >= 3 ? read_2_reg : 0) + (worker_count_reg >= 4 ? read_3_reg : 0);
        end
        reduce_write_valid_reg <= reduce_sum_valid_reg;
        reduce_write_last_reg <= reduce_sum_last_reg;
        if (reduce_sum_valid_reg) begin
            result_mem[reduce_addr_pipe_2] <= sum_01_reg + sum_23_reg;
        end
    end
end

endmodule

`resetall
