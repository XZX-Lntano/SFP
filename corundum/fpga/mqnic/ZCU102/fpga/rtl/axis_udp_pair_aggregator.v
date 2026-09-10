// SPDX-License-Identifier: BSD-2-Clause-Views
/*
 * Collect 2 to 4 worker packets, sum ENTRY_COUNT sequential values by round_id,
 * then broadcast one modified result packet back to every worker port.
 *
 * Fixed packet layout (no VLAN, IPv4 header without options).  Each entry
 * occupies 8 bytes starting at byte 44; entries are in slot order (0..63).
 *   byte 36-37       : UDP destination port
 *   byte 42-43       : 16-bit round_id
 *   byte 44+8*n..51+8*n : entry n value/result (no transmitted index)
 *
 * IPv4 TTL carries the number of workers participating in the round (2..4).
 * Only packets on the configured UDP port participate in aggregation.  Each
 * physical ingress port can contribute at most once to a round.  The final
 * worker frame is rewritten with the accumulated result and broadcast.
 */

`resetall
`timescale 1ns / 1ps
`default_nettype none

module axis_udp_pair_aggregator #
(
    parameter DATA_WIDTH = 64,
    parameter KEEP_WIDTH = DATA_WIDTH/8,
    parameter USER_WIDTH = 1,
    parameter DEST_WIDTH = 4,
    parameter FIFO_DEPTH = 4096,
    parameter META_FIFO_DEPTH = 4,
    parameter ENTRY_COUNT = 64,
    parameter [15:0] AGG_UDP_PORT = 16'h2345,
    parameter [DEST_WIDTH-1:0] RESULT_DEST = {DEST_WIDTH{1'b1}}
)
(
    input  wire                     clk,
    input  wire                     rst,

    input  wire [DATA_WIDTH-1:0]    s_axis_0_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_0_tkeep,
    input  wire                     s_axis_0_tvalid,
    output wire                     s_axis_0_tready,
    input  wire                     s_axis_0_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_0_tuser,

    input  wire [DATA_WIDTH-1:0]    s_axis_1_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_1_tkeep,
    input  wire                     s_axis_1_tvalid,
    output wire                     s_axis_1_tready,
    input  wire                     s_axis_1_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_1_tuser,

    input  wire [DATA_WIDTH-1:0]    s_axis_2_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_2_tkeep,
    input  wire                     s_axis_2_tvalid,
    output wire                     s_axis_2_tready,
    input  wire                     s_axis_2_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_2_tuser,

    input  wire [DATA_WIDTH-1:0]    s_axis_3_tdata,
    input  wire [KEEP_WIDTH-1:0]    s_axis_3_tkeep,
    input  wire                     s_axis_3_tvalid,
    output wire                     s_axis_3_tready,
    input  wire                     s_axis_3_tlast,
    input  wire [USER_WIDTH-1:0]    s_axis_3_tuser,

    output wire [DATA_WIDTH-1:0]    m_axis_tdata,
    output wire [KEEP_WIDTH-1:0]    m_axis_tkeep,
    output wire                     m_axis_tvalid,
    input  wire                     m_axis_tready,
    output wire                     m_axis_tlast,
    output wire [DEST_WIDTH-1:0]    m_axis_tdest,
    output wire [USER_WIDTH-1:0]    m_axis_tuser
);

localparam UDP_CHECKSUM_OFFSET = 40;
localparam ENTRY_BASE_OFFSET = 44;
localparam ENTRY_STRIDE_BYTES = 8;
localparam ENTRY_VALUE_OFFSET = 0;
localparam ENTRY_VALUE_BYTES = 8;
localparam ENTRY_INDEX_WIDTH = ENTRY_COUNT > 1 ? $clog2(ENTRY_COUNT) : 1;

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

function [2:0] worker_mask_count;
    input [3:0] mask;
    begin
        worker_mask_count = mask[0] + mask[1] + mask[2] + mask[3];
    end
endfunction

function [3:0] worker_mask_bit;
    input [1:0] worker;
    begin
        worker_mask_bit = 4'b0001 << worker;
    end
endfunction

reg active_frame_reg = 1'b0;
reg [1:0] active_worker_reg = 2'd0;
reg finalize_pending_reg = 1'b0;
reg finalize_after_write_reg = 1'b0;
reg [1:0] finalize_worker_reg = 2'd0;
reg copy_pending_reg = 1'b0;
reg calc_pending_reg = 1'b0;
reg frame_action_valid_reg = 1'b0;

wire [1:0] idle_worker = s_axis_0_tvalid ? 2'd0 :
                         s_axis_1_tvalid ? 2'd1 :
                         s_axis_2_tvalid ? 2'd2 :
                         s_axis_3_tvalid ? 2'd3 : 2'd0;
wire [1:0] select_worker = active_frame_reg ? active_worker_reg : idle_worker;
wire [DATA_WIDTH-1:0] selected_tdata = select_worker == 2'd0 ? s_axis_0_tdata :
                                         select_worker == 2'd1 ? s_axis_1_tdata :
                                         select_worker == 2'd2 ? s_axis_2_tdata : s_axis_3_tdata;
wire [KEEP_WIDTH-1:0] selected_tkeep = select_worker == 2'd0 ? s_axis_0_tkeep :
                                         select_worker == 2'd1 ? s_axis_1_tkeep :
                                         select_worker == 2'd2 ? s_axis_2_tkeep : s_axis_3_tkeep;
wire selected_tvalid = select_worker == 2'd0 ? s_axis_0_tvalid :
                       select_worker == 2'd1 ? s_axis_1_tvalid :
                       select_worker == 2'd2 ? s_axis_2_tvalid : s_axis_3_tvalid;
wire selected_tlast = select_worker == 2'd0 ? s_axis_0_tlast :
                      select_worker == 2'd1 ? s_axis_1_tlast :
                      select_worker == 2'd2 ? s_axis_2_tlast : s_axis_3_tlast;
wire [USER_WIDTH-1:0] selected_tuser = select_worker == 2'd0 ? s_axis_0_tuser :
                                          select_worker == 2'd1 ? s_axis_1_tuser :
                                          select_worker == 2'd2 ? s_axis_2_tuser : s_axis_3_tuser;

wire frame_fifo_in_tready;
wire input_paused = finalize_pending_reg || finalize_after_write_reg || copy_pending_reg || calc_pending_reg || frame_action_valid_reg;
wire frame_fifo_in_tvalid = selected_tvalid && !input_paused;

assign s_axis_0_tready = frame_fifo_in_tready && !input_paused && select_worker == 2'd0;
assign s_axis_1_tready = frame_fifo_in_tready && !input_paused && select_worker == 2'd1;
assign s_axis_2_tready = frame_fifo_in_tready && !input_paused && select_worker == 2'd2;
assign s_axis_3_tready = frame_fifo_in_tready && !input_paused && select_worker == 2'd3;

wire [DATA_WIDTH-1:0] frame_fifo_out_tdata;
wire [KEEP_WIDTH-1:0] frame_fifo_out_tkeep;
wire frame_fifo_out_tvalid;
wire frame_fifo_out_tready;
wire frame_fifo_out_tlast;
wire [USER_WIDTH-1:0] frame_fifo_out_tuser;

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
    .s_axis_tdata(selected_tdata),
    .s_axis_tkeep(selected_tkeep),
    .s_axis_tvalid(frame_fifo_in_tvalid),
    .s_axis_tready(frame_fifo_in_tready),
    .s_axis_tlast(selected_tlast),
    .s_axis_tid(0),
    .s_axis_tdest(0),
    .s_axis_tuser(selected_tuser),
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

reg [15:0] eth_type_reg = 16'd0;
reg [7:0] ip_version_ihl_reg = 8'd0;
reg [7:0] ip_protocol_reg = 8'd0;
reg [7:0] ip_ttl_reg = 8'd0;
reg [15:0] udp_dest_port_reg = 16'd0;
reg [15:0] round_id_reg = 16'd0;
reg [7:0] input_beat_index_reg = 8'd0;

(* ram_style = "distributed" *)
reg [63:0] packet_value_reg[0:ENTRY_COUNT-1];
(* ram_style = "distributed" *)
reg [63:0] pending_value_reg[0:ENTRY_COUNT-1];

reg pending_valid_reg = 1'b0;
reg [3:0] pending_worker_mask_reg = 4'd0;
reg [2:0] pending_worker_count_reg = 3'd0;
reg [15:0] pending_round_id_reg = 16'd0;

reg current_emit_reg = 1'b0;
(* ram_style = "distributed" *)
reg [63:0] current_result_reg[0:ENTRY_COUNT-1];
reg [ENTRY_INDEX_WIDTH-1:0] copy_index_reg = 0;
reg [ENTRY_INDEX_WIDTH-1:0] calc_index_reg = 0;
reg calc_emit_reg = 1'b0;
reg [3:0] calc_worker_mask_reg = 4'd0;
reg [7:0] output_beat_index_reg = 8'd0;
reg output_entry_active_reg = 1'b0;
reg [ENTRY_INDEX_WIDTH:0] output_entry_index_reg = 0;
reg [3:0] output_entry_byte_reg = 4'd0;
reg [63:0] output_value_reg = 64'd0;
reg [63:0] output_next_value_reg = 64'd0;

reg input_entry_active_reg = 1'b0;
reg [ENTRY_INDEX_WIDTH:0] input_entry_index_reg = 0;
reg [3:0] input_entry_byte_reg = 4'd0;
reg [63:0] input_value_shift_reg = 64'd0;

reg packet_value_wr_en_reg = 1'b0;
reg [ENTRY_INDEX_WIDTH-1:0] packet_value_wr_index_reg = 0;
reg [63:0] packet_value_wr_data_reg = 64'd0;

reg [DATA_WIDTH-1:0] m_axis_tdata_reg = {DATA_WIDTH{1'b0}};
reg [KEEP_WIDTH-1:0] m_axis_tkeep_reg = {KEEP_WIDTH{1'b0}};
reg m_axis_tvalid_reg = 1'b0;
reg m_axis_tlast_reg = 1'b0;
reg [USER_WIDTH-1:0] m_axis_tuser_reg = {USER_WIDTH{1'b0}};

wire output_ready = !m_axis_tvalid_reg || m_axis_tready;
reg [DATA_WIDTH-1:0] output_tdata_mux;
reg [KEEP_WIDTH-1:0] output_tkeep_mux;
integer output_lane;
integer output_byte_index;
integer output_slot;
integer output_value_byte;

assign frame_fifo_out_tready = frame_action_valid_reg && (current_emit_reg ? output_ready : 1'b1);

assign m_axis_tdata = m_axis_tdata_reg;
assign m_axis_tkeep = m_axis_tkeep_reg;
assign m_axis_tvalid = m_axis_tvalid_reg;
assign m_axis_tlast = m_axis_tlast_reg;
assign m_axis_tdest = RESULT_DEST;
assign m_axis_tuser = m_axis_tuser_reg;

always @* begin
    output_tdata_mux = frame_fifo_out_tdata;
    output_tkeep_mux = frame_fifo_out_tkeep;

    if (current_emit_reg) begin
        for (output_lane = 0; output_lane < KEEP_WIDTH; output_lane = output_lane + 1) begin
            output_byte_index = output_beat_index_reg * KEEP_WIDTH + output_lane;
            if (output_byte_index >= ENTRY_BASE_OFFSET &&
                    output_byte_index < ENTRY_BASE_OFFSET + ENTRY_COUNT * ENTRY_STRIDE_BYTES) begin
                output_slot = (output_byte_index - ENTRY_BASE_OFFSET) / ENTRY_STRIDE_BYTES;
                output_value_byte = (output_byte_index - ENTRY_BASE_OFFSET) % ENTRY_STRIDE_BYTES;
                output_tdata_mux[output_lane*8 +: 8] = result_byte(current_result_reg[output_slot], output_value_byte[3:0]);
            end
        end
        if (output_beat_index_reg == 8'd5) begin
            output_tdata_mux[0*8 +: 8] = 8'd0;
            output_tdata_mux[1*8 +: 8] = 8'd0;
        end
    end
end

reg input_entry_active_tmp;
reg [ENTRY_INDEX_WIDTH:0] input_entry_index_tmp;
reg [3:0] input_entry_byte_tmp;
reg [63:0] input_value_shift_tmp;
reg packet_value_wr_en_tmp;
reg [ENTRY_INDEX_WIDTH-1:0] packet_value_wr_index_tmp;
reg [63:0] packet_value_wr_data_tmp;
reg [63:0] calc_sum_tmp;
reg output_entry_active_tmp;
reg [ENTRY_INDEX_WIDTH:0] output_entry_index_tmp;
reg [3:0] output_entry_byte_tmp;
reg packet_valid_reg;
always @(posedge clk) begin
    if (rst) begin
        active_frame_reg <= 1'b0;
        active_worker_reg <= 1'b0;
        finalize_pending_reg <= 1'b0;
        finalize_after_write_reg <= 1'b0;
        finalize_worker_reg <= 1'b0;
        copy_pending_reg <= 1'b0;
        calc_pending_reg <= 1'b0;
        frame_action_valid_reg <= 1'b0;
        eth_type_reg <= 16'd0;
        ip_version_ihl_reg <= 8'd0;
        ip_protocol_reg <= 8'd0;
        ip_ttl_reg <= 8'd0;
        udp_dest_port_reg <= 16'd0;
        round_id_reg <= 16'd0;
        input_beat_index_reg <= 8'd0;
        pending_valid_reg <= 1'b0;
        pending_worker_mask_reg <= 4'd0;
        pending_worker_count_reg <= 3'd0;
        pending_round_id_reg <= 16'd0;
        current_emit_reg <= 1'b0;
        copy_index_reg <= 0;
        calc_index_reg <= 0;
        calc_emit_reg <= 1'b0;
        calc_worker_mask_reg <= 4'd0;
        output_beat_index_reg <= 8'd0;
        output_entry_active_reg <= 1'b0;
        output_entry_index_reg <= 0;
        output_entry_byte_reg <= 4'd0;
        output_value_reg <= 64'd0;
        output_next_value_reg <= 64'd0;
        input_entry_active_reg <= 1'b0;
        input_entry_index_reg <= 0;
        input_entry_byte_reg <= 4'd0;
        input_value_shift_reg <= 64'd0;
        packet_value_wr_en_reg <= 1'b0;
        packet_value_wr_index_reg <= 0;
        packet_value_wr_data_reg <= 64'd0;
        m_axis_tdata_reg <= {DATA_WIDTH{1'b0}};
        m_axis_tkeep_reg <= {KEEP_WIDTH{1'b0}};
        m_axis_tvalid_reg <= 1'b0;
        m_axis_tlast_reg <= 1'b0;
        m_axis_tuser_reg <= {USER_WIDTH{1'b0}};
    end else begin
        if (m_axis_tvalid_reg && m_axis_tready) begin
            m_axis_tvalid_reg <= 1'b0;
        end

        if (packet_value_wr_en_reg) begin
            packet_value_reg[packet_value_wr_index_reg] <= packet_value_wr_data_reg;
        end
        packet_value_wr_en_reg <= 1'b0;

        if (finalize_after_write_reg) begin
            finalize_pending_reg <= 1'b1;
            finalize_after_write_reg <= 1'b0;
        end

        if (frame_fifo_out_tvalid && frame_fifo_out_tready) begin
            if (current_emit_reg) begin
                m_axis_tdata_reg <= output_tdata_mux;
                m_axis_tkeep_reg <= output_tkeep_mux;
                m_axis_tvalid_reg <= 1'b1;
                m_axis_tlast_reg <= frame_fifo_out_tlast;
                m_axis_tuser_reg <= frame_fifo_out_tuser;
            end

            output_entry_active_tmp = output_entry_active_reg;
            output_entry_index_tmp = output_entry_index_reg;
            output_entry_byte_tmp = output_entry_byte_reg;

            if (output_beat_index_reg == 8'd5) begin
                output_entry_active_tmp = 1'b1;
                output_entry_index_tmp = 0;
                output_entry_byte_tmp = 4'd4;
            end else if (output_entry_active_reg && output_entry_index_reg < ENTRY_COUNT) begin
                case (output_entry_byte_reg)
                    4'd0: begin
                        output_entry_byte_tmp = 4'd8;
                    end
                    4'd2: begin
                        output_entry_index_tmp = output_entry_index_reg + 1'b1;
                        output_entry_byte_tmp = 4'd0;
                    end
                    4'd4: begin
                        output_entry_index_tmp = output_entry_index_reg + 1'b1;
                        output_entry_byte_tmp = 4'd2;
                    end
                    4'd6: begin
                        output_entry_index_tmp = output_entry_index_reg + 1'b1;
                        output_entry_byte_tmp = 4'd4;
                    end
                    4'd8: begin
                        output_entry_index_tmp = output_entry_index_reg + 1'b1;
                        output_entry_byte_tmp = 4'd6;
                    end
                    default: begin
                        output_entry_byte_tmp = 4'd0;
                    end
                endcase
            end

            if (frame_fifo_out_tlast) begin
                frame_action_valid_reg <= 1'b0;
                current_emit_reg <= 1'b0;
                output_beat_index_reg <= 8'd0;
                output_entry_active_reg <= 1'b0;
                output_entry_index_reg <= 0;
                output_entry_byte_reg <= 4'd0;
            end else begin
                output_beat_index_reg <= output_beat_index_reg + 1'b1;
                output_entry_active_reg <= output_entry_active_tmp;
                output_entry_index_reg <= output_entry_index_tmp;
                output_entry_byte_reg <= output_entry_byte_tmp;

                if (output_entry_index_tmp != output_entry_index_reg) begin
                    output_value_reg <= output_next_value_reg;
                    if (output_entry_index_tmp + 1 < ENTRY_COUNT) begin
                        output_next_value_reg <= current_result_reg[output_entry_index_tmp + 1];
                    end else begin
                        output_next_value_reg <= 64'd0;
                    end
                end
            end
        end

        if (finalize_pending_reg) begin
            packet_valid_reg =
                eth_type_reg == 16'h0800 &&
                ip_version_ihl_reg == 8'h45 &&
                ip_protocol_reg == 8'h11 &&
                ip_ttl_reg >= 8'd2 && ip_ttl_reg <= 8'd4 &&
                udp_dest_port_reg == AGG_UDP_PORT &&
                input_entry_index_reg >= ENTRY_COUNT;

            if (packet_valid_reg &&
                    pending_valid_reg &&
                    pending_round_id_reg == round_id_reg &&
                    pending_worker_count_reg == ip_ttl_reg[2:0] &&
                    !(pending_worker_mask_reg & worker_mask_bit(finalize_worker_reg))) begin
                calc_pending_reg <= 1'b1;
                calc_index_reg <= 0;
                calc_worker_mask_reg <= pending_worker_mask_reg | worker_mask_bit(finalize_worker_reg);
                calc_emit_reg <= worker_mask_count(pending_worker_mask_reg) + 1 >= ip_ttl_reg[2:0];
            end else if (packet_valid_reg && !pending_valid_reg) begin
                copy_pending_reg <= 1'b1;
                copy_index_reg <= 0;
                pending_worker_mask_reg <= worker_mask_bit(finalize_worker_reg);
                pending_worker_count_reg <= ip_ttl_reg[2:0];
                pending_round_id_reg <= round_id_reg;
            end else begin
                frame_action_valid_reg <= 1'b1;
                current_emit_reg <= 1'b0;
                output_beat_index_reg <= 8'd0;
                output_entry_active_reg <= 1'b0;
                output_entry_index_reg <= 0;
                output_entry_byte_reg <= 4'd0;
            end

            finalize_pending_reg <= 1'b0;

            eth_type_reg <= 16'd0;
            ip_version_ihl_reg <= 8'd0;
            ip_protocol_reg <= 8'd0;
            ip_ttl_reg <= 8'd0;
            udp_dest_port_reg <= 16'd0;
            round_id_reg <= 16'd0;
            input_beat_index_reg <= 8'd0;
            input_entry_active_reg <= 1'b0;
            input_entry_index_reg <= 0;
            input_entry_byte_reg <= 4'd0;
            input_value_shift_reg <= 64'd0;
        end

        if (copy_pending_reg) begin
            pending_value_reg[copy_index_reg] <= packet_value_reg[copy_index_reg];

            if (copy_index_reg == ENTRY_COUNT-1) begin
                pending_valid_reg <= 1'b1;
                copy_pending_reg <= 1'b0;
                frame_action_valid_reg <= 1'b1;
                current_emit_reg <= 1'b0;
                output_beat_index_reg <= 8'd0;
                output_entry_active_reg <= 1'b0;
                output_entry_index_reg <= 0;
                output_entry_byte_reg <= 4'd0;
            end else begin
                copy_index_reg <= copy_index_reg + 1'b1;
            end
        end

        if (calc_pending_reg) begin
            calc_sum_tmp = packet_value_reg[calc_index_reg] + pending_value_reg[calc_index_reg];
            if (calc_emit_reg) begin
                current_result_reg[calc_index_reg] <= calc_sum_tmp;
            end else begin
                pending_value_reg[calc_index_reg] <= calc_sum_tmp;
            end

            if (calc_index_reg == 0) begin
                output_value_reg <= calc_sum_tmp;
            end

            if (calc_index_reg == 1) begin
                output_next_value_reg <= calc_sum_tmp;
            end

            if (calc_index_reg == ENTRY_COUNT-1) begin
                calc_pending_reg <= 1'b0;
                if (calc_emit_reg) begin
                    pending_valid_reg <= 1'b0;
                    frame_action_valid_reg <= 1'b1;
                    current_emit_reg <= 1'b1;
                    output_beat_index_reg <= 8'd0;
                    output_entry_active_reg <= 1'b0;
                    output_entry_index_reg <= 0;
                    output_entry_byte_reg <= 4'd0;
                end else begin
                    pending_worker_mask_reg <= calc_worker_mask_reg;
                    frame_action_valid_reg <= 1'b1;
                    current_emit_reg <= 1'b0;
                    output_beat_index_reg <= 8'd0;
                    output_entry_active_reg <= 1'b0;
                    output_entry_index_reg <= 0;
                    output_entry_byte_reg <= 4'd0;
                end
            end else begin
                calc_index_reg <= calc_index_reg + 1'b1;
            end
        end

        if (frame_fifo_in_tvalid && frame_fifo_in_tready) begin
            input_entry_active_tmp = input_entry_active_reg;
            input_entry_index_tmp = input_entry_index_reg;
            input_entry_byte_tmp = input_entry_byte_reg;
            input_value_shift_tmp = input_value_shift_reg;
            packet_value_wr_en_tmp = 1'b0;
            packet_value_wr_index_tmp = 0;
            packet_value_wr_data_tmp = 64'd0;

            case (input_beat_index_reg)
                8'd1: begin
                    eth_type_reg[15:8] <= selected_tdata[4*8 +: 8];
                    eth_type_reg[7:0] <= selected_tdata[5*8 +: 8];
                    ip_version_ihl_reg <= selected_tdata[6*8 +: 8];
                end
                8'd2: begin
                    ip_ttl_reg <= selected_tdata[6*8 +: 8];
                    ip_protocol_reg <= selected_tdata[7*8 +: 8];
                end
                8'd4: begin
                    udp_dest_port_reg[15:8] <= selected_tdata[4*8 +: 8];
                    udp_dest_port_reg[7:0] <= selected_tdata[5*8 +: 8];
                end
                8'd5: begin
                    round_id_reg[15:8] <= selected_tdata[2*8 +: 8];
                    round_id_reg[7:0] <= selected_tdata[3*8 +: 8];
                    input_entry_active_tmp = 1'b1;
                    input_entry_index_tmp = 0;
                    input_entry_byte_tmp = 4'd4;
                    input_value_shift_tmp[63:56] = selected_tdata[4*8 +: 8];
                    input_value_shift_tmp[55:48] = selected_tdata[5*8 +: 8];
                    input_value_shift_tmp[47:40] = selected_tdata[6*8 +: 8];
                    input_value_shift_tmp[39:32] = selected_tdata[7*8 +: 8];
                end
                default: begin
                    if (input_entry_active_reg && input_entry_index_reg < ENTRY_COUNT) begin
                        if (input_entry_byte_reg == 4) begin
                            input_value_shift_tmp[31:24] = selected_tdata[0*8 +: 8];
                            input_value_shift_tmp[23:16] = selected_tdata[1*8 +: 8];
                            input_value_shift_tmp[15:8] = selected_tdata[2*8 +: 8];
                            input_value_shift_tmp[7:0] = selected_tdata[3*8 +: 8];
                            packet_value_wr_en_tmp = 1'b1;
                            packet_value_wr_index_tmp = input_entry_index_reg[ENTRY_INDEX_WIDTH-1:0];
                            packet_value_wr_data_tmp = input_value_shift_tmp;
                            input_entry_index_tmp = input_entry_index_reg + 1'b1;
                            input_entry_byte_tmp = 4'd4;
                            if (input_entry_index_reg + 1 < ENTRY_COUNT) begin
                                input_value_shift_tmp[63:56] = selected_tdata[4*8 +: 8];
                                input_value_shift_tmp[55:48] = selected_tdata[5*8 +: 8];
                                input_value_shift_tmp[47:40] = selected_tdata[6*8 +: 8];
                                input_value_shift_tmp[39:32] = selected_tdata[7*8 +: 8];
                            end else begin
                                input_entry_active_tmp = 1'b0;
                            end
                        end
                    end
                end
            endcase

            input_entry_active_reg <= input_entry_active_tmp;
            input_entry_index_reg <= input_entry_index_tmp;
            input_entry_byte_reg <= input_entry_byte_tmp;
            input_value_shift_reg <= input_value_shift_tmp;
            packet_value_wr_en_reg <= packet_value_wr_en_tmp;
            packet_value_wr_index_reg <= packet_value_wr_index_tmp;
            packet_value_wr_data_reg <= packet_value_wr_data_tmp;

            if (!active_frame_reg) begin
                active_worker_reg <= select_worker;
            end

            if (selected_tlast) begin
                active_frame_reg <= 1'b0;
                input_beat_index_reg <= 8'd0;
                finalize_after_write_reg <= 1'b1;
                finalize_worker_reg <= active_frame_reg ? active_worker_reg : select_worker;
            end else begin
                active_frame_reg <= 1'b1;
                input_beat_index_reg <= input_beat_index_reg + 1'b1;
            end
        end
    end
end

endmodule

`resetall
