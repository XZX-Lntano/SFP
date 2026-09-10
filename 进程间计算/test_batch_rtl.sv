`timescale 1ns/1ps
module test_batch_rtl;
reg clk=0,rst=1;
always #1.6665 clk=~clk;
localparam CYCLES=180000;
reg [31:0] cycle=0;
wire [255:0] data;
wire [31:0] keep;
wire [3:0] valid,last,user,ready;
wire [63:0] out_data;
wire [7:0] out_keep;
wire out_valid,out_last,out_user;
wire [31:0] expired;
wire [127:0] dropped;
reg out_ready=0;
integer output_fd;
genvar p;
generate for(p=0;p<4;p=p+1) begin: source
    reg [79:0] stimulus[0:CYCLES-1];
    initial begin
        case(p)
            0:$readmemh("input0.mem",stimulus);
            1:$readmemh("input1.mem",stimulus);
            2:$readmemh("input2.mem",stimulus);
            3:$readmemh("input3.mem",stimulus);
        endcase
    end
    assign data[p*64+:64]=stimulus[cycle][63:0];
    assign keep[p*8+:8]=stimulus[cycle][71:64];
    assign last[p]=stimulus[cycle][72];
    assign valid[p]=!rst && stimulus[cycle][73];
    assign user[p]=stimulus[cycle][74];
end endgenerate
axis_udp_batch_aggregator #(.TIMEOUT_CYCLES(50000)) dut (
    .clk(clk),.rst(rst),.s_axis_tdata(data),.s_axis_tkeep(keep),.s_axis_tvalid(valid),
    .s_axis_tready(ready),.s_axis_tlast(last),.s_axis_tuser(user),
    .m_axis_tdata(out_data),.m_axis_tkeep(out_keep),.m_axis_tvalid(out_valid),
    .m_axis_tready(out_ready),.m_axis_tlast(out_last),.m_axis_tuser(out_user),
    .expired_batches(expired),.dropped_frames(dropped)
);
reg held=0;
reg [63:0] held_data;
reg held_last;
initial begin
    output_fd=$fopen("output.txt","w");
    repeat(8) @(negedge clk);
    rst=0;
end
always @(negedge clk) begin
    if(!rst) begin
        cycle<=cycle+1;
        out_ready <= cycle%13<10 && !(cycle>=19000 && cycle<19500);
    end
end
always @(posedge clk) if(!rst) begin
    if(held && (!out_valid || out_data!==held_data || out_last!==held_last)) $fatal(1,"AXIS changed under backpressure");
    held<=out_valid && !out_ready;held_data<=out_data;held_last<=out_last;
    if(out_valid && out_ready) begin
        if(out_keep!==8'hff || out_user!==0) $fatal(1,"invalid output flags");
        $fdisplay(output_fd,"%016h %d",out_data,out_last);
    end
    if(cycle==CYCLES-2) begin
        $display("COUNTERS expired=%d dropped=%h",expired,dropped);
        if(expired<1 || dropped==0) $fatal(1,"missing negative-test events");
        $fclose(output_fd);$finish;
    end
end
endmodule
