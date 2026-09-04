`timescale 1ns/1ps

module test_axis_udp_batch_aggregator;
    localparam ROUNDS = 2;
    reg clk = 0, rst = 1;
    always #1.666 clk = ~clk;

    reg [63:0] s_data[0:3];
    reg [7:0] s_keep[0:3];
    reg [3:0] s_valid = 0, s_last = 0, s_user = 0;
    wire [3:0] s_ready;
    wire [63:0] m_data;
    wire [7:0] m_keep;
    wire m_valid, m_last;
    reg m_ready = 1;
    wire [3:0] m_dest;
    wire m_user;

    axis_udp_batch_aggregator dut (
        .clk(clk), .rst(rst),
        .s_axis_0_tdata(s_data[0]), .s_axis_0_tkeep(s_keep[0]), .s_axis_0_tvalid(s_valid[0]), .s_axis_0_tready(s_ready[0]), .s_axis_0_tlast(s_last[0]), .s_axis_0_tuser(s_user[0]), .s_axis_0_overflow(1'b0),
        .s_axis_1_tdata(s_data[1]), .s_axis_1_tkeep(s_keep[1]), .s_axis_1_tvalid(s_valid[1]), .s_axis_1_tready(s_ready[1]), .s_axis_1_tlast(s_last[1]), .s_axis_1_tuser(s_user[1]), .s_axis_1_overflow(1'b0),
        .s_axis_2_tdata(s_data[2]), .s_axis_2_tkeep(s_keep[2]), .s_axis_2_tvalid(s_valid[2]), .s_axis_2_tready(s_ready[2]), .s_axis_2_tlast(s_last[2]), .s_axis_2_tuser(s_user[2]), .s_axis_2_overflow(1'b0),
        .s_axis_3_tdata(s_data[3]), .s_axis_3_tkeep(s_keep[3]), .s_axis_3_tvalid(s_valid[3]), .s_axis_3_tready(s_ready[3]), .s_axis_3_tlast(s_last[3]), .s_axis_3_tuser(s_user[3]), .s_axis_3_overflow(1'b0),
        .m_axis_tdata(m_data), .m_axis_tkeep(m_keep), .m_axis_tvalid(m_valid),
        .m_axis_tready(m_ready), .m_axis_tlast(m_last), .m_axis_tdest(m_dest), .m_axis_tuser(m_user)
    );

    function automatic [63:0] network_word(input [63:0] value);
        network_word = {value[7:0],value[15:8],value[23:16],value[31:24],
                        value[39:32],value[47:40],value[55:48],value[63:56]};
    endfunction

    task automatic send_batch(input integer port);
        integer beat, rec, item;
        reg [63:0] word, value;
        begin
            for (beat = 0; beat <= 5+ROUNDS*65; beat = beat+1) begin
                word = 0;
                if (beat == 1) begin word[39:32]=8'h08; word[47:40]=0; word[55:48]=8'h45; end
                if (beat == 2) begin word[55:48]=4; word[63:56]=17; end
                if (beat == 4) begin word[39:32]=8'h23; word[47:40]=8'h45; end
                if (beat == 5) begin
                    word[23:16]=8'ha4; word[31:24]=8'h16; word[39:32]=4;
                    word[47:40]=ROUNDS; word[55:48]=0; word[63:56]=0;
                end
                if (beat >= 6) begin
                    rec = (beat-6)/65;
                    item = (beat-6)%65;
                    if (item == 0) begin word[7:0]=8'h12; word[15:8]=rec; end
                    else begin
                        value = port*1000 + rec*100 + item-1;
                        word = network_word(value);
                    end
                end
                @(negedge clk);
                s_data[port] = word; s_keep[port] = 8'hff;
                s_valid[port] = 1; s_last[port] = beat == 5+ROUNDS*65;
                while (!s_ready[port]) @(negedge clk);
            end
            @(negedge clk); s_valid[port]=0; s_last[port]=0;
        end
    endtask

    integer out_beat = 0, out_rec, out_item;
    reg [63:0] decoded, expected;
    always @(posedge clk) begin
        if (!rst && m_valid && m_ready) begin
            if (out_beat >= 6) begin
                out_rec = (out_beat-6)/65;
                out_item = (out_beat-6)%65;
                if (out_item == 0) begin
                    if (m_data[7:0] !== 8'h12 || m_data[15:8] !== out_rec[7:0])
                        $fatal(1, "round header mismatch beat=%0d data=%h", out_beat, m_data);
                end else begin
                    decoded = network_word(m_data);
                    expected = 6000 + out_rec*400 + (out_item-1)*4;
                    if (decoded !== expected)
                        $fatal(1, "sum mismatch rec=%0d item=%0d got=%0d expected=%0d", out_rec, out_item-1, decoded, expected);
                end
            end
            if (m_last) begin
                if (out_beat != 5+ROUNDS*65) $fatal(1, "bad output length");
                $display("PASS: %0d-round four-worker batch", ROUNDS);
                $finish;
            end
            out_beat = out_beat+1;
        end
    end

    initial begin
        integer n;
        for (n=0; n<4; n=n+1) begin s_data[n]=0; s_keep[n]=8'hff; end
        repeat (8) @(posedge clk); rst=0;
        fork
            send_batch(0); send_batch(1); send_batch(2); send_batch(3);
        join
        repeat (10000) @(posedge clk);
        $fatal(1, "timeout");
    end
endmodule
