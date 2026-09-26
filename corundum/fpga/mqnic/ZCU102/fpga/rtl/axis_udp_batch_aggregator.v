// SPDX-License-Identifier: BSD-2-Clause-Views
// V4: Ethernet/IPv4/UDP, 6-byte AG header, 1..16 records of
// {round_id:u32, reserved:u32, value[64]:u64}, all fields network endian.
// Four independent streaming writers and BRAM banks feed a pipelined sum tree.
// V5 adds self-recovery watchdogs: a stalled active transfer is aborted after
// STALL_TIMEOUT_CYCLES; a gap with no COMPLETED ingress frame for
// IDLE_TIMEOUT_CYCLES (tlast-based, so a wedged RX FIFO holding tvalid high
// with a truncated frame is still detected); and frames completing with no
// broadcast for NO_MATCH_TIMEOUT_CYCLES (one port's RX died). Each watchdog
// pulses recovery_pulse so fpga_core can reset the per-port CDC FIFOs, and
// flushes the banks - including the frame PARSER state (beat/good/accepting).
// The parser was the real cross-run wedge: it is only cleared by s_last or
// rst, so a truncated frame left `beat` parked at its maximum and that bank
// then rejected every later frame forever, surviving every host-side restart
// until the PL was reconfigured.
`timescale 1ns / 1ps
`default_nettype none

module axis_udp_batch_bank #(
    parameter SLOTS = 256,
    parameter USER_WIDTH = 1,
    parameter UDP_PORT = 16'h2345,
    parameter BATCHES = SLOTS/16,
    parameter BW = $clog2(BATCHES),
    parameter AW = $clog2(SLOTS*64)
)(
    input wire clk, rst,
    input wire [63:0] s_data,
    input wire [7:0] s_keep,
    input wire s_valid, s_last,
    input wire [USER_WIDTH-1:0] s_user,
    input wire release_valid,
    input wire [BW-1:0] release_slot, inspect_slot,
    output wire present,
    output wire [31:0] base_id,
    output wire [4:0] rounds,
    output wire [2:0] workers,
    input wire [2:0] header_index,
    output wire [63:0] header_data,
    input wire read_enable,
    input wire [AW-1:0] read_address,
    input wire flush,
    output reg [63:0] read_data,
    output reg [31:0] dropped = 0
);
function [31:0] be32;
    input [31:0] x;
    begin be32={x[7:0],x[15:8],x[23:16],x[31:24]}; end
endfunction
function [63:0] be64;
    input [63:0] x;
    begin be64={be32(x[31:0]),be32(x[63:32])}; end
endfunction
(* ram_style = "block" *) reg [63:0] data_mem[0:SLOTS*64-1];
reg [63:0] ram_read_data;
reg [BATCHES-1:0] valid_mem = 0;
reg [31:0] tag_mem[0:BATCHES-1];
reg [4:0] count_mem[0:BATCHES-1];
reg [2:0] workers_mem[0:BATCHES-1];
reg [63:0] headers[0:BATCHES*8-1];
reg [63:0] header_hold[0:5];
reg [10:0] beat = 0;
reg good = 0, accepting = 0;
reg [15:0] ip_length = 0, udp_length = 0;
reg [4:0] frame_rounds = 0;
reg [2:0] frame_workers = 0;
reg [31:0] frame_base = 0;
reg [BW-1:0] frame_slot = 0;
reg [4:0] record_index = 0;
reg [6:0] record_beat = 0;
wire [31:0] incoming_id = be32(s_data[31:0]);
wire [BW-1:0] incoming_slot = incoming_id[4 +: BW];
assign present=valid_mem[inspect_slot];
assign base_id=tag_mem[inspect_slot];
assign rounds=count_mem[inspect_slot];
assign workers=workers_mem[inspect_slot];
assign header_data=headers[{inspect_slot,header_index}];
// Separate synchronous RAM read port. No reset on data storage.
always @(posedge clk) begin
    if (read_enable) begin
        ram_read_data <= data_mem[read_address];
        read_data <= ram_read_data;
    end
    if (s_valid && accepting && good && beat >= 7 && record_beat != 0)
        data_mem[{frame_slot,record_index[3:0],record_beat[5:0]-6'd1}] <= be64(s_data);
end
integer h;
always @(posedge clk) begin
    if (rst) begin
        valid_mem<=0; beat<=0; good<=0; accepting<=0; dropped<=0;
        record_index<=0; record_beat<=0;
    end else begin
        if (release_valid) valid_mem[release_slot]<=0;
        /* Watchdog flush: drop every stored batch AND reset the frame parser.
         * The parser is otherwise cleared only by s_last or rst, so a truncated
         * frame (e.g. an RX FIFO reset mid-frame or a link glitch) leaves
         * `beat` parked at its maximum and this bank then rejects every later
         * frame forever - the real root cause of the cross-run wedge. */
        if (flush) begin
            valid_mem<=0; beat<=0; good<=0; accepting<=0;
            record_index<=0; record_beat<=0;
        end else if (s_valid) begin
            if (beat < 6) header_hold[beat] <= s_data;
            if (beat == 0) begin
                good <= s_keep==8'hff && !s_user[0];
                accepting <= 0; record_index<=0; record_beat<=0;
            end else if (s_keep != 8'hff || s_user[0]) good<=0;
            case (beat)
                1: if (s_data[47:32]!=16'h0008 || s_data[55:48]!=8'h45) good<=0;
                2: begin
                    ip_length <= {s_data[7:0],s_data[15:8]};
                    if (s_data[63:56]!=17 || (s_data[47:32]&16'hff3f)!=0) good<=0;
                end
                4: begin
                    if ({s_data[39:32],s_data[47:40]}!=UDP_PORT) good<=0;
                    udp_length <= {s_data[55:48],s_data[63:56]};
                end
                5: begin
                    frame_workers<=s_data[42:40]; frame_rounds<=s_data[52:48];
                    if (s_data[31:16]!=16'h4741 || s_data[39:32]!=4 ||
                        s_data[47:40]<2 || s_data[47:40]>4 ||
                        s_data[55:48]<1 || s_data[55:48]>16 || s_data[63:56]!=1)
                        good<=0;
                end
                6: begin
                    frame_base<=incoming_id; frame_slot<=incoming_slot;
                    accepting<=good && !valid_mem[incoming_slot] && incoming_id[3:0]==0 && s_data[63:32]==0;
                    record_beat<=1;
                    if (incoming_id[3:0]!=0 || s_data[63:32]!=0 ||
                        ip_length!=34+frame_rounds*520 || udp_length!=14+frame_rounds*520) good<=0;
                end
                default: if (beat > 6) begin
                    if (record_beat==0) begin
                        if (incoming_id!=frame_base+record_index || s_data[63:32]!=0) good<=0;
                        record_beat<=1;
                    end else if (record_beat==64) begin
                        record_beat<=0; record_index<=record_index+1'b1;
                    end else record_beat<=record_beat+1'b1;
                end
            endcase
            if (s_last) begin
                if (good && accepting && s_keep==8'hff && !s_user[0] &&
                    record_beat==64 && record_index+1==frame_rounds &&
                    beat==5+frame_rounds*65) begin
                    valid_mem[frame_slot]<=1;
                    tag_mem[frame_slot]<=frame_base;
                    count_mem[frame_slot]<=frame_rounds;
                    workers_mem[frame_slot]<=frame_workers;
                    for (h=0; h<6; h=h+1) headers[frame_slot*8+h]<=header_hold[h];
                end else dropped<=dropped+1'b1;
                beat<=0; good<=0; accepting<=0;
            end else if (beat != 2047) beat<=beat+1'b1;
            else good<=0;
        end
    end
end
endmodule

module axis_udp_batch_aggregator #(
    parameter SLOTS=256,
    parameter USER_WIDTH=1,
    parameter UDP_PORT=16'h2345,
    parameter TIMEOUT_CYCLES=30000000,
    /* Self-recovery watchdogs (300 MHz clock):
     *   STALL_TIMEOUT_CYCLES : an active transfer that cannot advance for this
     *     long is aborted, so a stalled output port can never wedge the engine.
     *   IDLE_TIMEOUT_CYCLES  : no ingress frame for this long triggers a
     *     recovery pulse that flushes the engine and (via recovery_pulse) the
     *     port CDC FIFOs, so state left behind by one host run cannot poison
     *     the next run. 1 s is far longer than any intra-run ingress gap. */
    parameter STALL_TIMEOUT_CYCLES=300000,
    parameter IDLE_TIMEOUT_CYCLES=300000000,
    parameter NO_MATCH_TIMEOUT_CYCLES=600000000,
    parameter RECOVERY_PULSE_CYCLES=64,
    parameter BATCHES=SLOTS/16,
    parameter BW=$clog2(BATCHES),
    parameter AW=$clog2(SLOTS*64)
)(
    input wire clk, rst,
    input wire [255:0] s_axis_tdata,
    input wire [31:0] s_axis_tkeep,
    input wire [3:0] s_axis_tvalid,
    output wire [3:0] s_axis_tready,
    input wire [3:0] s_axis_tlast,
    input wire [4*USER_WIDTH-1:0] s_axis_tuser,
    output reg [63:0] m_axis_tdata=0,
    output wire [7:0] m_axis_tkeep,
    output reg m_axis_tvalid=0,
    input wire m_axis_tready,
    output reg m_axis_tlast=0,
    output wire [USER_WIDTH-1:0] m_axis_tuser,
    output reg [31:0] expired_batches=0,
    output reg recovery_pulse=0,
    output wire [127:0] dropped_frames
);
initial begin
    if (SLOTS<32 || (SLOTS&(SLOTS-1))!=0) $error("SLOTS must be power-of-two >=32");
end
function [63:0] wire64;
    input [63:0] v;
    begin wire64={v[7:0],v[15:8],v[23:16],v[31:24],v[39:32],v[47:40],v[55:48],v[63:56]}; end
endfunction
reg [BW-1:0] scan=0, active_slot=0, release_slot=0;
reg release_valid=0, active=0, issued_last=0;
reg [31:0] active_base=0;
reg [4:0] active_rounds=0;
reg [2:0] active_workers=0;
reg [10:0] issue_beat=0;
reg [3:0] issue_round=0;
reg [6:0] issue_record_beat=0;
wire [BW-1:0] inspect=active ? active_slot : scan;
wire [3:0] present;
wire [127:0] tags;
wire [19:0] counts;
wire [11:0] workers;
wire [255:0] bank_data, headers;
wire advance=!m_axis_tvalid || m_axis_tready;
wire issue=active && !issued_last;
wire value_beat=issue_beat>=6 && issue_record_beat!=0;
wire [AW-1:0] read_address={active_slot,issue_round,issue_record_beat[5:0]-6'd1};
assign s_axis_tready=4'hf;
assign m_axis_tkeep=8'hff;
assign m_axis_tuser=0;
/* Input pipeline register. The port RX FIFOs are BRAM based; without this
 * stage the path from their output registers to the bank header write-enables
 * (a wide combinational cone over s_keep/s_user/s_last and the beat counters)
 * dominated the 300 MHz timing. s_axis_tready is tied high, so this is a pure
 * latency stage: the banks simply parse the stream one cycle later. */
reg [255:0] in_tdata=0;
reg [31:0]  in_tkeep=0;
reg [3:0]   in_tvalid=0, in_tlast=0;
reg [4*USER_WIDTH-1:0] in_tuser=0;
always @(posedge clk) begin
    in_tdata<=s_axis_tdata; in_tkeep<=s_axis_tkeep;
    in_tvalid<=s_axis_tvalid; in_tlast<=s_axis_tlast; in_tuser<=s_axis_tuser;
end
genvar p;
generate for (p=0;p<4;p=p+1) begin: banks
    axis_udp_batch_bank #(.SLOTS(SLOTS),.USER_WIDTH(USER_WIDTH),.UDP_PORT(UDP_PORT)) bank (
        .clk(clk),.rst(rst),.s_data(in_tdata[p*64+:64]),
        .s_keep(in_tkeep[p*8+:8]),.s_valid(in_tvalid[p]),
        .s_last(in_tlast[p]),.s_user(in_tuser[p*USER_WIDTH+:USER_WIDTH]),
        .release_valid(release_valid),.release_slot(release_slot),.inspect_slot(inspect),
        .flush(flush_banks),
        .present(present[p]),.base_id(tags[p*32+:32]),.rounds(counts[p*5+:5]),
        .workers(workers[p*3+:3]),.header_index(issue_beat[2:0]),.header_data(headers[p*64+:64]),
        .read_enable(advance),.read_address(read_address),
        .read_data(bank_data[p*64+:64]),.dropped(dropped_frames[p*32+:32])
    );
end endgenerate
reg match;
integer k;
always @* begin
    match=present[0] && workers[2:0]>=2 && workers[2:0]<=4;
    for (k=1;k<4;k=k+1)
        if (k<workers[2:0] && (!present[k] || tags[k*32+:32]!=tags[31:0] ||
            counts[k*5+:5]!=counts[4:0] || workers[k*3+:3]!=workers[2:0])) match=0;
end
reg [31:0] ticks=0;
reg [BATCHES-1:0] age_valid=0;
reg [31:0] age[0:BATCHES-1];
/* Self-recovery watchdog state.
 * stall  : an active transfer cannot advance (output backpressure).
 * idle   : no ingress FRAME has completed for IDLE_TIMEOUT. Keyed off frame
 *          completion (tlast) rather than tvalid on purpose: a wedged RX FIFO
 *          can hold tvalid high with a partial frame forever, and a
 *          tvalid-based detector would then stay silent and never recover.
 * noprog : frames are completing but no batch has been broadcast for
 *          NO_MATCH_TIMEOUT (e.g. one port's RX died so the group can never
 *          match); recovery resets every port FIFO and typically restores it. */
wire frame_end = |(in_tvalid & in_tlast);
wire bcast_end = m_axis_tvalid && m_axis_tready && m_axis_tlast;
wire stall = active && !advance;
reg [31:0] stall_ticks=0, idle_ticks=0, noprog_ticks=0;
reg [31:0] stall_aborts=0, idle_resets=0, noprog_resets=0;
reg [6:0] recovery_cnt=0;
reg flush_banks=0;
reg vr=0,v0=0,v1=0,v2=0, valr=0,val0=0,val1=0,val2=0, lastr=0,last0=0,last1=0,last2=0;
reg [63:0] metar=0;
reg [63:0] meta0=0,meta1=0,meta2=0, sum01=0,sum23=0,total=0;
reg [63:0] metadata;
reg [31:0] record_id;
always @* begin
    record_id=active_base+issue_round;
    metadata=headers[63:0];
    if (issue_beat==5) begin
        metadata[15:0]=0; // UDP checksum disabled for rewritten IPv4 UDP data.
        metadata[63:56]=2;
    end
    if (issue_beat>=6)
        metadata={32'd0,record_id[7:0],record_id[15:8],record_id[23:16],record_id[31:24]};
end
always @(posedge clk) begin
    if (rst) begin
        scan<=0; active<=0; issued_last<=0; release_valid<=0;
        ticks<=0; age_valid<=0; expired_batches<=0;
        vr<=0;v0<=0;v1<=0;v2<=0;m_axis_tvalid<=0;m_axis_tlast<=0;
        stall_ticks<=0; idle_ticks<=0; noprog_ticks<=0; recovery_cnt<=0; flush_banks<=0;
        stall_aborts<=0; idle_resets<=0; noprog_resets<=0; recovery_pulse<=0;
    end else begin
        ticks<=ticks+1'b1; release_valid<=0;
        flush_banks<=0; recovery_pulse<=0;
        // Watchdog counters. idle is keyed off frame COMPLETION (tlast), not
        // tvalid: a wedged RX FIFO can hold tvalid high with a partial frame.
        if (frame_end) idle_ticks<=0;
        else if (idle_ticks<IDLE_TIMEOUT_CYCLES) idle_ticks<=idle_ticks+1'b1;
        if (bcast_end) noprog_ticks<=0;
        else if (noprog_ticks<NO_MATCH_TIMEOUT_CYCLES) noprog_ticks<=noprog_ticks+1'b1;
        if (stall) begin
            if (stall_ticks<STALL_TIMEOUT_CYCLES) stall_ticks<=stall_ticks+1'b1;
        end else stall_ticks<=0;
        // Reclaim incomplete batches; this prevents one lost worker blocking a slot forever.
        if (!active && !release_valid) begin
            if (match) begin
                active<=1;active_slot<=scan;active_base<=tags[31:0];
                active_rounds<=counts[4:0];active_workers<=workers[2:0];
                issue_beat<=0;issue_round<=0;issue_record_beat<=0;issued_last<=0;
                age_valid[scan]<=0;
            end else if (|present) begin
                if (!age_valid[scan]) begin age_valid[scan]<=1;age[scan]<=ticks; end
                else if (ticks-age[scan]>=TIMEOUT_CYCLES) begin
                    release_valid<=1;release_slot<=scan;age_valid[scan]<=0;
                    expired_batches<=expired_batches+1'b1;
                end
            end else age_valid[scan]<=0;
            scan<=scan+1'b1;
        end
        // One beat per cycle: synchronous BRAM -> pair sums -> final sum -> AXIS.
        // The whole read pipeline freezes on output backpressure; ingress banks remain independent.
        if (advance) begin
            vr<=issue;valr<=value_beat;metar<=metadata;
            lastr<=issue && issue_beat==5+active_rounds*65;
            v0<=vr;val0<=valr;meta0<=metar;last0<=lastr;
            v1<=v0;val1<=val0;meta1<=meta0;last1<=last0;
            sum01<=bank_data[63:0]+bank_data[127:64];
            sum23<=(active_workers>=3 ? bank_data[191:128] : 64'd0)+
                   (active_workers>=4 ? bank_data[255:192] : 64'd0);
            v2<=v1;val2<=val1;meta2<=meta1;last2<=last1;
            total<=sum01+sum23;
            m_axis_tvalid<=v2;m_axis_tlast<=last2;
            m_axis_tdata<=val2 ? wire64(total) : meta2;
            if (issue) begin
                if (issue_beat==5+active_rounds*65) issued_last<=1;
                issue_beat<=issue_beat+1'b1;
                if (issue_beat>=6) begin
                    if (issue_record_beat==64) begin
                        issue_record_beat<=0;issue_round<=issue_round+1'b1;
                    end else issue_record_beat<=issue_record_beat+1'b1;
                end
            end
        end
        if (m_axis_tvalid && m_axis_tready && m_axis_tlast) begin
            active<=0;release_valid<=1;release_slot<=active_slot;
        end
        /* ---- self-recovery, applied last so it overrides the normal state ----
         * The original engine could stall forever: once `active` was set it had
         * no timeout, so a stalled output port wedged it until the PL was
         * reconfigured. Both watchdogs now start the SAME recovery pulse, which
         * flushes the engine and every bank and (via recovery_pulse) resets the
         * per-port CDC FIFOs in both clock domains:
         *   - a stalled active transfer (dead/stuck output port), or
         *   - a long gap with no ingress at all (state left by a finished run).
         * Runs are seconds apart, so the idle case guarantees every host run
         * starts from a clean engine. */
        if (recovery_cnt!=0) begin
            recovery_cnt<=recovery_cnt-1'b1; recovery_pulse<=1; flush_banks<=1;
            active<=0; issued_last<=0; m_axis_tvalid<=0; m_axis_tlast<=0;
            scan<=0; age_valid<=0;
        end else if (idle_ticks>=IDLE_TIMEOUT_CYCLES) begin
            /* No frame has COMPLETED for IDLE_TIMEOUT: either the link is idle
             * (normal between runs) or an RX FIFO is wedged mid-frame with
             * tvalid stuck high. Reset the engine, the bank slots and (via
             * recovery_pulse) every port FIFO. */
            recovery_cnt<=RECOVERY_PULSE_CYCLES; recovery_pulse<=1; flush_banks<=1;
            active<=0; issued_last<=0; m_axis_tvalid<=0; m_axis_tlast<=0;
            scan<=0; age_valid<=0; idle_ticks<=0;
            idle_resets<=idle_resets+1'b1;
        end else if (noprog_ticks>=NO_MATCH_TIMEOUT_CYCLES) begin
            /* Frames are completing but nothing has been broadcast for
             * NO_MATCH_TIMEOUT: most likely one port's RX has died, so the
             * group can never match. Resetting the port FIFOs brings it back. */
            recovery_cnt<=RECOVERY_PULSE_CYCLES; recovery_pulse<=1; flush_banks<=1;
            active<=0; issued_last<=0; m_axis_tvalid<=0; m_axis_tlast<=0;
            scan<=0; age_valid<=0; noprog_ticks<=0;
            noprog_resets<=noprog_resets+1'b1;
        end else if (stall && stall_ticks>=STALL_TIMEOUT_CYCLES) begin
            recovery_cnt<=RECOVERY_PULSE_CYCLES; recovery_pulse<=1; flush_banks<=1;
            active<=0; issued_last<=0; m_axis_tvalid<=0; m_axis_tlast<=0;
            scan<=0; age_valid<=0; stall_ticks<=0;
            stall_aborts<=stall_aborts+1'b1;
        end
    end
end
endmodule
`resetall
