`timescale 1ns/1ps
module fifo_tb;
parameter SLOTS=256, DEPTH=16384;
localparam N=64, WORDS=1046, PERIOD=2015, CYCLES=260000;
reg clk=0, macclk=0, rst=1;
always #1.666667 clk=~clk;
always #3.2 macclk=~macclk;
integer cycle=0, pause_us=0, skew_us=0, unused;
initial begin
 unused=$value$plusargs("PAUSE=%d",pause_us);
 unused=$value$plusargs("SKEW=%d",skew_us);
 repeat(12) @(negedge clk); rst=0;
end
always @(negedge clk) if(!rst) cycle<=cycle+1;
wire [255:0] data;wire [3:0] valid,last,ready;
wire [63:0] od;wire [7:0] ok;wire ov,ol,orr;wire [127:0] drops;wire [31:0] expired;
genvar p;
generate for(p=0;p<4;p=p+1) begin: src
 reg [63:0] mem[0:N*WORDS-1];
 initial $readmemh($sformatf("frames%0d.mem",p),mem);
 wire signed [31:0] t=cycle-100-(p==3?skew_us*300:0);
 wire signed [31:0] frame=t/PERIOD, beat=t%PERIOD;
 assign valid[p]=!rst && t>=0 && frame<N && beat<WORDS;
 assign last[p]=valid[p] && beat==WORDS-1;
 assign data[p*64+:64]=valid[p]?mem[frame*WORDS+beat]:64'd0;
end endgenerate
axis_udp_batch_aggregator #(.SLOTS(SLOTS)) dut(
 .clk(clk),.rst(rst),.s_axis_tdata(data),.s_axis_tkeep(32'hffffffff),.s_axis_tvalid(valid),.s_axis_tready(ready),.s_axis_tlast(last),.s_axis_tuser(4'd0),
 .m_axis_tdata(od),.m_axis_tkeep(ok),.m_axis_tvalid(ov),.m_axis_tready(orr),.m_axis_tlast(ol),.m_axis_tuser(),.expired_batches(expired),.dropped_frames(drops));
wire [255:0] bd;wire [31:0] bk;wire [3:0] bv,br,bl;
axis_broadcast #(.M_COUNT(4),.DATA_WIDTH(64),.KEEP_ENABLE(1),.KEEP_WIDTH(8),.ID_ENABLE(0),.ID_WIDTH(1),.DEST_ENABLE(0),.DEST_WIDTH(1),.USER_ENABLE(1),.USER_WIDTH(1)) bc(
 .clk(clk),.rst(rst),.s_axis_tdata(od),.s_axis_tkeep(ok),.s_axis_tvalid(ov),.s_axis_tready(orr),.s_axis_tlast(ol),.s_axis_tid(1'b0),.s_axis_tdest(1'b0),.s_axis_tuser(1'b0),
 .m_axis_tdata(bd),.m_axis_tkeep(bk),.m_axis_tvalid(bv),.m_axis_tready(br),.m_axis_tlast(bl),.m_axis_tid(),.m_axis_tdest(),.m_axis_tuser());
integer stalled=0, runstall=0, maxstall=0, occ=0, maxocc=0, k;
reg [SLOTS/16-1:0] occupied;
always @(posedge clk) if(!rst) begin
 if(ov&&!orr) begin stalled=stalled+1;runstall=runstall+1;if(runstall>maxstall)maxstall=runstall;end else runstall=0;
 occupied=dut.banks[0].bank.valid_mem | dut.banks[1].bank.valid_mem | dut.banks[2].bank.valid_mem | dut.banks[3].bank.valid_mem;
 occ=0;for(k=0;k<SLOTS/16;k=k+1)if(occupied[k])occ=occ+1;
 if(occ>maxocc)maxocc=occ;
 if(cycle==CYCLES) begin
 $display("AGG slots=%0d depth=%0d pause_us=%0d skew_us=%0d stalls=%0d max_stall_cycles=%0d max_occupied=%0d drops=%h expired=%0d",SLOTS,DEPTH,pause_us,skew_us,stalled,maxstall,maxocc,drops,expired);
 for(k=0;k<4;k=k+1)begin end
 $finish;
 end
end
generate for(p=0;p<4;p=p+1) begin: sink
 wire [63:0] md;wire [7:0] mk;wire mv,ml,mr;wire [$clog2(DEPTH):0] depth;wire overflow;
 integer high=0, frames=0,gap=0,overflows=0,fd; real first_ns=0,last_ns=0;
 initial fd=$fopen($sformatf("out%0d.txt",p),"w");
 assign mr=gap==0 && !(p==3 && cycle>=18000 && cycle<18000+pause_us*300);
 axis_async_fifo #(.DEPTH(DEPTH),.DATA_WIDTH(64),.KEEP_ENABLE(1),.KEEP_WIDTH(8),.ID_ENABLE(0),.ID_WIDTH(1),.DEST_ENABLE(0),.DEST_WIDTH(1),.USER_ENABLE(1),.USER_WIDTH(1),.RAM_PIPELINE(1),.FRAME_FIFO(1),.DROP_OVERSIZE_FRAME(1),.DROP_BAD_FRAME(0),.DROP_WHEN_FULL(0)) fifo(
 .s_clk(clk),.s_rst(rst),.s_axis_tdata(bd[p*64+:64]),.s_axis_tkeep(bk[p*8+:8]),.s_axis_tvalid(bv[p]),.s_axis_tready(br[p]),.s_axis_tlast(bl[p]),.s_axis_tid(1'b0),.s_axis_tdest(1'b0),.s_axis_tuser(1'b0),
 .m_clk(macclk),.m_rst(rst),.m_axis_tdata(md),.m_axis_tkeep(mk),.m_axis_tvalid(mv),.m_axis_tready(mr),.m_axis_tlast(ml),.m_axis_tid(),.m_axis_tdest(),.m_axis_tuser(),
 .s_pause_req(1'b0),.s_pause_ack(),.m_pause_req(1'b0),.m_pause_ack(),.s_status_depth(depth),.s_status_depth_commit(),.s_status_overflow(overflow),.s_status_bad_frame(),.s_status_good_frame(),.m_status_depth(),.m_status_depth_commit(),.m_status_overflow(),.m_status_bad_frame(),.m_status_good_frame());
 always @(posedge clk) if(!rst) begin
 if(depth>high)high=depth;
 if(overflow)overflows=overflows+1;
 if(cycle==CYCLES-1)$display("FIFO port=%0d high_bytes=%0d frames=%0d overflows=%0d first_ns=%.3f last_ns=%.3f",p,high,frames,overflows,first_ns,last_ns);
 end
 always @(posedge macclk) if(!rst) begin
 if(gap>0)gap<=gap-1;
 if(mv&&mr)begin
 if(first_ns==0)first_ns=$realtime;
 $fdisplay(fd,"%016h %0d",md,ml);
 if(ml)begin frames=frames+1;gap<=3;last_ns=$realtime;end
 end
 end
end endgenerate
endmodule
