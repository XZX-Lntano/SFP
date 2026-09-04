open_project fpga.xpr
set old_file [get_files -quiet */axis_udp_pair_aggregator.v]
if {[llength $old_file]} {
    remove_files $old_file
}
set batch_file [file normalize ../rtl/axis_udp_batch_aggregator.v]
if {![llength [get_files -quiet $batch_file]]} {
    add_files -fileset sources_1 $batch_file
}
set_property file_type SystemVerilog [get_files $batch_file]
update_compile_order -fileset sources_1
synth_design -rtl -name batch_rtl_check -top fpga -part xczu9eg-ffvb1156-2-e
report_utilization -file batch_rtl_utilization.rpt
close_project
