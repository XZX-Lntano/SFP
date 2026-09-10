set root [file normalize [file join [file dirname [info script]] ..]]
set slots 256
if {$argc > 0} {set slots [lindex $argv 0]}
set out [file join $root "进程间计算" reports]
file mkdir $out
create_project -in_memory -part xczu9eg-ffvb1156-2-e
set_param general.maxThreads 4
read_verilog [file join $root corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v]
# OOC resource/timing estimate; full-board place-and-route remains required.
synth_design -top axis_udp_batch_aggregator -mode out_of_context -part xczu9eg-ffvb1156-2-e -generic SLOTS=$slots
create_clock -period 3.333 [get_ports clk]
opt_design
report_utilization -hierarchical -file [file join $out batch_${slots}_hierarchy.rpt]
report_utilization -file [file join $out batch_${slots}_utilization.rpt]
report_timing_summary -file [file join $out batch_${slots}_synth_timing.rpt]
write_checkpoint -force [file join $out batch_${slots}.dcp]
