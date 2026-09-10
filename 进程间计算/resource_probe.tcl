set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root "进程间计算" reports]
file mkdir $out
create_project -in_memory -part xczu9eg-ffvb1156-2-e
set part [get_parts xczu9eg-ffvb1156-2-e]
report_property $part -file [file join $out part.txt]
read_verilog [file join $root corundum/fpga/lib/axis/rtl/axis_fifo.v]
read_verilog [file join $root corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_pair_aggregator.v]
synth_design -top axis_udp_pair_aggregator -mode out_of_context -part xczu9eg-ffvb1156-2-e
create_clock -period 3.333 [get_ports clk]
report_utilization -file [file join $out aggregator_baseline_utilization.rpt]
report_timing_summary -file [file join $out aggregator_baseline_timing.rpt]
